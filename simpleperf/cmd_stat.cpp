/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "command.h"
#include "environment.h"
#include "event_attr.h"
#include "event_fd.h"
#include "event_selection_set.h"
#include "event_type.h"
#include "IOEventLoop.h"
#include "utils.h"
#include "workload.h"

namespace {

static std::vector<std::string> default_measured_event_types{
    "cpu-cycles",   "stalled-cycles-frontend", "stalled-cycles-backend",
    "instructions", "branch-instructions",     "branch-misses",
    "task-clock",   "context-switches",        "page-faults",
};

struct CounterSum {
  uint64_t value = 0;
  uint64_t time_enabled = 0;
  uint64_t time_running = 0;
};

struct CounterSummary {
  std::string type_name;
  std::string modifier;
  uint32_t group_id;
  uint64_t count;
  double scale;
  std::string readable_count;
  std::string comment;
  bool auto_generated;

  CounterSummary(const std::string& type_name, const std::string& modifier,
                 uint32_t group_id, uint64_t count, double scale,
                 bool auto_generated, bool csv)
      : type_name(type_name),
        modifier(modifier),
        group_id(group_id),
        count(count),
        scale(scale),
        auto_generated(auto_generated) {
    readable_count = ReadableCountValue(csv);
  }

  bool IsMonitoredAtTheSameTime(const CounterSummary& other) const {
    // Two summaries are monitored at the same time if they are in the same
    // group or are monitored all the time.
    if (group_id == other.group_id) {
      return true;
    }
    return IsMonitoredAllTheTime() && other.IsMonitoredAllTheTime();
  }

  std::string Name() const {
    if (modifier.empty()) {
      return type_name;
    }
    return type_name + ":" + modifier;
  }

  bool IsMonitoredAllTheTime() const {
    // If an event runs all the time it is enabled (by not sharing hardware
    // counters with other events), the scale of its summary is usually within
    // [1, 1 + 1e-5]. By setting SCALE_ERROR_LIMIT to 1e-5, We can identify
    // events monitored all the time in most cases while keeping the report
    // error rate <= 1e-5.
    constexpr double SCALE_ERROR_LIMIT = 1e-5;
    return (fabs(scale - 1.0) < SCALE_ERROR_LIMIT);
  }

 private:
  std::string ReadableCountValue(bool csv) {
    if (type_name == "cpu-clock" || type_name == "task-clock") {
      // Convert nanoseconds to milliseconds.
      double value = count / 1e6;
      return android::base::StringPrintf("%lf(ms)", value);
    } else {
      // Convert big numbers to human friendly mode. For example,
      // 1000000 will be converted to 1,000,000.
      std::string s = android::base::StringPrintf("%" PRIu64, count);
      if (csv) {
        return s;
      } else {
        for (size_t i = s.size() - 1, j = 1; i > 0; --i, ++j) {
          if (j == 3) {
            s.insert(s.begin() + i, ',');
            j = 0;
          }
        }
        return s;
      }
    }
  }
};

static const std::unordered_map<std::string_view, std::pair<std::string_view, std::string_view>>
    COMMON_EVENT_RATE_MAP = {
        {"cache-misses", {"cache-references", "miss rate"}},
        {"branch-misses", {"branch-instructions", "miss rate"}},
};

static const std::unordered_map<std::string_view, std::pair<std::string_view, std::string_view>>
    ARM_EVENT_RATE_MAP = {
        // Refer to "D6.10.5 Meaningful ratios between common microarchitectural events" in ARMv8
        // specification.
        {"raw-l1i-cache-refill", {"raw-l1i-cache", "level 1 instruction cache refill rate"}},
        {"raw-l1i-tlb-refill", {"raw-l1i-tlb", "level 1 instruction TLB refill rate"}},
        {"raw-l1d-cache-refill", {"raw-l1d-cache", "level 1 data or unified cache refill rate"}},
        {"raw-l1d-tlb-refill", {"raw-l1d-tlb", "level 1 data or unified TLB refill rate"}},
        {"raw-l2d-cache-refill", {"raw-l2d-cache", "level 2 data or unified cache refill rate"}},
        {"raw-l2i-cache-refill", {"raw-l2i-cache", "level 2 instruction cache refill rate"}},
        {"raw-l3d-cache-refill", {"raw-l3d-cache", "level 3 data or unified cache refill rate"}},
        {"raw-l2d-tlb-refill", {"raw-l2d-tlb", "level 2 data or unified TLB refill rate"}},
        {"raw-l2i-tlb-refill", {"raw-l2i-tlb", "level 2 instruction TLB refill rate"}},
        {"raw-bus-access", {"raw-bus-cycles", "bus accesses per cycle"}},
        {"raw-ll-cache-miss", {"raw-ll-cache", "last level data or unified cache refill rate"}},
        {"raw-dtlb-walk", {"raw-l1d-tlb", "data TLB miss rate"}},
        {"raw-itlb-walk", {"raw-l1i-tlb", "instruction TLB miss rate"}},
        {"raw-ll-cache-miss-rd", {"raw-ll-cache-rd", "memory read operation miss rate"}},
        {"raw-remote-access-rd",
         {"raw-remote-access", "read accesses to another socket in a multi-socket system"}},
        // Refer to "Table K3-2 Relationship between REFILL events and associated access events" in
        // ARMv8 specification.
        {"raw-l1d-cache-refill-rd", {"raw-l1d-cache-rd", "level 1 cache refill rate, read"}},
        {"raw-l1d-cache-refill-wr", {"raw-l1d-cache-wr", "level 1 cache refill rate, write"}},
        {"raw-l1d-tlb-refill-rd", {"raw-l1d-tlb-rd", "level 1 TLB refill rate, read"}},
        {"raw-l1d-tlb-refill-wr", {"raw-l1d-tlb-wr", "level 1 TLB refill rate, write"}},
        {"raw-l2d-cache-refill-rd", {"raw-l2d-cache-rd", "level 2 data cache refill rate, read"}},
        {"raw-l2d-cache-refill-wr", {"raw-l2d-cache-wr", "level 2 data cache refill rate, write"}},
        {"raw-l2d-tlb-refill-rd", {"raw-l2d-tlb-rd", "level 2 data TLB refill rate, read"}},
};

class CounterSummaries {
 public:
  explicit CounterSummaries(bool csv) : csv_(csv) {}
  std::vector<CounterSummary>& Summaries() { return summaries_; }

  const CounterSummary* FindSummary(const std::string& type_name,
                                    const std::string& modifier) {
    for (const auto& s : summaries_) {
      if (s.type_name == type_name && s.modifier == modifier) {
        return &s;
      }
    }
    return nullptr;
  }

  // If we have two summaries monitoring the same event type at the same time,
  // that one is for user space only, and the other is for kernel space only;
  // then we can automatically generate a summary combining the two results.
  // For example, a summary of branch-misses:u and a summary for branch-misses:k
  // can generate a summary of branch-misses.
  void AutoGenerateSummaries() {
    for (size_t i = 0; i < summaries_.size(); ++i) {
      const CounterSummary& s = summaries_[i];
      if (s.modifier == "u") {
        const CounterSummary* other = FindSummary(s.type_name, "k");
        if (other != nullptr && other->IsMonitoredAtTheSameTime(s)) {
          if (FindSummary(s.type_name, "") == nullptr) {
            Summaries().emplace_back(s.type_name, "", s.group_id, s.count + other->count, s.scale,
                                     true, csv_);
          }
        }
      }
    }
  }

  void GenerateComments(double duration_in_sec) {
    for (auto& s : summaries_) {
      s.comment = GetCommentForSummary(s, duration_in_sec);
    }
  }

  void Show(FILE* fp) {
    size_t count_column_width = 0;
    size_t name_column_width = 0;
    size_t comment_column_width = 0;
    for (auto& s : summaries_) {
      count_column_width =
          std::max(count_column_width, s.readable_count.size());
      name_column_width = std::max(name_column_width, s.Name().size());
      comment_column_width = std::max(comment_column_width, s.comment.size());
    }

    for (auto& s : summaries_) {
      if (csv_) {
        fprintf(fp, "%s,%s,%s,(%.0lf%%)%s\n", s.readable_count.c_str(),
                s.Name().c_str(), s.comment.c_str(), 1.0 / s.scale * 100,
                (s.auto_generated ? " (generated)," : ","));
      } else {
        fprintf(fp, "  %*s  %-*s   # %-*s  (%.0lf%%)%s\n",
                static_cast<int>(count_column_width), s.readable_count.c_str(),
                static_cast<int>(name_column_width), s.Name().c_str(),
                static_cast<int>(comment_column_width), s.comment.c_str(),
                1.0 / s.scale * 100, (s.auto_generated ? " (generated)" : ""));
      }
    }
  }

 private:
  std::string GetCommentForSummary(const CounterSummary& s,
                                   double duration_in_sec) {
    char sap_mid;
    if (csv_) {
      sap_mid = ',';
    } else {
      sap_mid = ' ';
    }
    if (s.type_name == "task-clock") {
      double run_sec = s.count / 1e9;
      double used_cpus = run_sec / (duration_in_sec / s.scale);
      return android::base::StringPrintf("%lf%ccpus used", used_cpus, sap_mid);
    }
    if (s.type_name == "cpu-clock") {
      return "";
    }
    if (s.type_name == "cpu-cycles") {
      double running_time_in_sec;
      if (!FindRunningTimeForSummary(s, &running_time_in_sec)) {
        return "";
      }
      double hz = s.count / (running_time_in_sec / s.scale);
      return android::base::StringPrintf("%lf%cGHz", hz / 1e9, sap_mid);
    }
    if (s.type_name == "instructions" && s.count != 0) {
      const CounterSummary* other = FindSummary("cpu-cycles", s.modifier);
      if (other != nullptr && other->IsMonitoredAtTheSameTime(s)) {
        double cpi = static_cast<double>(other->count) / s.count;
        return android::base::StringPrintf("%lf%ccycles per instruction", cpi,
                                           sap_mid);
      }
    }
    std::string rate_comment = GetRateComment(s, sap_mid);
    if (!rate_comment.empty()) {
      return rate_comment;
    }
    double running_time_in_sec;
    if (!FindRunningTimeForSummary(s, &running_time_in_sec)) {
      return "";
    }
    double rate = s.count / (running_time_in_sec / s.scale);
    if (rate > 1e9) {
      return android::base::StringPrintf("%.3lf%cG/sec", rate / 1e9, sap_mid);
    }
    if (rate > 1e6) {
      return android::base::StringPrintf("%.3lf%cM/sec", rate / 1e6, sap_mid);
    }
    if (rate > 1e3) {
      return android::base::StringPrintf("%.3lf%cK/sec", rate / 1e3, sap_mid);
    }
    return android::base::StringPrintf("%.3lf%c/sec", rate, sap_mid);
  }

  std::string GetRateComment(const CounterSummary& s, char sep) {
    std::string_view miss_event_name = s.type_name;
    std::string event_name;
    std::string rate_desc;
    if (auto it = COMMON_EVENT_RATE_MAP.find(miss_event_name); it != COMMON_EVENT_RATE_MAP.end()) {
      event_name = it->second.first;
      rate_desc = it->second.second;
    }
    if (event_name.empty() && (GetBuildArch() == ARCH_ARM || GetBuildArch() == ARCH_ARM64)) {
      if (auto it = ARM_EVENT_RATE_MAP.find(miss_event_name); it != ARM_EVENT_RATE_MAP.end()) {
        event_name = it->second.first;
        rate_desc = it->second.second;
      }
    }
    if (event_name.empty() && android::base::ConsumeSuffix(&miss_event_name, "-misses")) {
      event_name = std::string(miss_event_name) + "s";
      rate_desc = "miss rate";
    }
    if (!event_name.empty()) {
      const CounterSummary* other = FindSummary(event_name, s.modifier);
      if (other != nullptr && other->IsMonitoredAtTheSameTime(s) && other->count != 0) {
        double miss_rate = static_cast<double>(s.count) / other->count;
        return android::base::StringPrintf("%f%%%c%s", miss_rate * 100, sep, rate_desc.c_str());
      }
    }
    return "";
  }

  bool FindRunningTimeForSummary(const CounterSummary& summary, double* running_time_in_sec) {
    for (auto& s : summaries_) {
      if ((s.type_name == "task-clock" || s.type_name == "cpu-clock") &&
          s.IsMonitoredAtTheSameTime(summary) && s.count != 0u) {
        *running_time_in_sec = s.count / 1e9;
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<CounterSummary> summaries_;
  bool csv_;
};

// devfreq may use performance counters to calculate memory latency (as in
// drivers/devfreq/arm-memlat-mon.c). Hopefully we can get more available counters by asking devfreq
// to not use the memory latency governor temporarily.
class DevfreqCounters {
 public:
  bool Use() {
    if (!IsRoot()) {
      LOG(ERROR) << "--use-devfreq-counters needs root permission to set devfreq governors";
      return false;
    }
    std::string devfreq_dir = "/sys/class/devfreq/";
    for (auto& name : GetSubDirs(devfreq_dir)) {
      std::string governor_path = devfreq_dir + name + "/governor";
      if (IsRegularFile(governor_path)) {
        std::string governor;
        if (!android::base::ReadFileToString(governor_path, &governor)) {
          LOG(ERROR) << "failed to read " << governor_path;
          return false;
        }
        governor = android::base::Trim(governor);
        if (governor == "mem_latency") {
          if (!android::base::WriteStringToFile("performance", governor_path)) {
            PLOG(ERROR) << "failed to write " << governor_path;
            return false;
          }
          mem_latency_governor_paths_.emplace_back(std::move(governor_path));
        }
      }
    }
    return true;
  }

  ~DevfreqCounters() {
    for (auto& path : mem_latency_governor_paths_) {
      android::base::WriteStringToFile("mem_latency", path);
    }
  }

 private:
  std::vector<std::string> mem_latency_governor_paths_;
};

class StatCommand : public Command {
 public:
  StatCommand()
      : Command("stat", "gather performance counter information",
                // clang-format off
"Usage: simpleperf stat [options] [command [command-args]]\n"
"       Gather performance counter information of running [command].\n"
"       And -a/-p/-t option can be used to change target of counter information.\n"
"-a           Collect system-wide information.\n"
#if defined(__ANDROID__)
"--app package_name    Profile the process of an Android application.\n"
"                      On non-rooted devices, the app must be debuggable,\n"
"                      because we use run-as to switch to the app's context.\n"
#endif
"--cpu cpu_item1,cpu_item2,...\n"
"                 Collect information only on the selected cpus. cpu_item can\n"
"                 be a cpu number like 1, or a cpu range like 0-3.\n"
"--csv            Write report in comma separate form.\n"
"--duration time_in_sec  Monitor for time_in_sec seconds instead of running\n"
"                        [command]. Here time_in_sec may be any positive\n"
"                        floating point number.\n"
"--interval time_in_ms   Print stat for every time_in_ms milliseconds.\n"
"                        Here time_in_ms may be any positive floating point\n"
"                        number. Simpleperf prints total values from the\n"
"                        starting point. But this can be changed by\n"
"                        --interval-only-values.\n"
"--interval-only-values  Print numbers of events happened in each interval.\n"
"-e event1[:modifier1],event2[:modifier2],...\n"
"                 Select a list of events to count. An event can be:\n"
"                   1) an event name listed in `simpleperf list`;\n"
"                   2) a raw PMU event in rN format. N is a hex number.\n"
"                      For example, r1b selects event number 0x1b.\n"
"                 Modifiers can be added to define how the event should be\n"
"                 monitored. Possible modifiers are:\n"
"                   u - monitor user space events only\n"
"                   k - monitor kernel space events only\n"
"--group event1[:modifier],event2[:modifier2],...\n"
"             Similar to -e option. But events specified in the same --group\n"
"             option are monitored as a group, and scheduled in and out at the\n"
"             same time.\n"
"--no-inherit     Don't stat created child threads/processes.\n"
"-o output_filename  Write report to output_filename instead of standard output.\n"
"-p pid1,pid2,... Stat events on existing processes. Mutually exclusive with -a.\n"
"-t tid1,tid2,... Stat events on existing threads. Mutually exclusive with -a.\n"
#if defined(__ANDROID__)
"--use-devfreq-counters    On devices with Qualcomm SOCs, some hardware counters may be used\n"
"                          to monitor memory latency (in drivers/devfreq/arm-memlat-mon.c),\n"
"                          making fewer counters available to users. This option asks devfreq\n"
"                          to temporarily release counters by replacing memory-latency governor\n"
"                          with performance governor. It affects memory latency during profiling,\n"
"                          and may cause wedged power if simpleperf is killed in between.\n"
#endif
"--verbose        Show result in verbose mode.\n"
#if 0
// Below options are only used internally and shouldn't be visible to the public.
"--in-app         We are already running in the app's context.\n"
"--tracepoint-events file_name   Read tracepoint events from [file_name] instead of tracefs.\n"
"--out-fd <fd>    Write output to a file descriptor.\n"
"--stop-signal-fd <fd>   Stop stating when fd is readable.\n"
#endif
                // clang-format on
                ),
        verbose_mode_(false),
        system_wide_collection_(false),
        child_inherit_(true),
        duration_in_sec_(0),
        interval_in_ms_(0),
        interval_only_values_(false),
        event_selection_set_(true),
        csv_(false),
        in_app_context_(false) {
    // Die if parent exits.
    prctl(PR_SET_PDEATHSIG, SIGHUP, 0, 0, 0);
  }

  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args,
                    std::vector<std::string>* non_option_args);
  bool AddDefaultMeasuredEventTypes();
  void SetEventSelectionFlags();
  bool ShowCounters(const std::vector<CountersInfo>& counters,
                    double duration_in_sec, FILE* fp);

  bool verbose_mode_;
  bool system_wide_collection_;
  bool child_inherit_;
  double duration_in_sec_;
  double interval_in_ms_;
  bool interval_only_values_;
  std::vector<CounterSum> last_sum_values_;
  std::vector<int> cpus_;
  EventSelectionSet event_selection_set_;
  std::string output_filename_;
  android::base::unique_fd out_fd_;
  bool csv_;
  std::string app_package_name_;
  bool in_app_context_;
  android::base::unique_fd stop_signal_fd_;
  bool use_devfreq_counters_ = false;
};

bool StatCommand::Run(const std::vector<std::string>& args) {
  if (!CheckPerfEventLimit()) {
    return false;
  }

  // 1. Parse options, and use default measured event types if not given.
  std::vector<std::string> workload_args;
  if (!ParseOptions(args, &workload_args)) {
    return false;
  }
  if (!app_package_name_.empty() && !in_app_context_) {
    if (!IsRoot()) {
      return RunInAppContext(app_package_name_, "stat", args, workload_args.size(),
                             output_filename_, !event_selection_set_.GetTracepointEvents().empty());
    }
  }
  DevfreqCounters devfreq_counters;
  if (use_devfreq_counters_) {
    if (!devfreq_counters.Use()) {
      return false;
    }
  }
  if (event_selection_set_.empty()) {
    if (!AddDefaultMeasuredEventTypes()) {
      return false;
    }
  }
  SetEventSelectionFlags();

  // 2. Create workload.
  std::unique_ptr<Workload> workload;
  if (!workload_args.empty()) {
    workload = Workload::CreateWorkload(workload_args);
    if (workload == nullptr) {
      return false;
    }
  }
  bool need_to_check_targets = false;
  if (system_wide_collection_) {
    event_selection_set_.AddMonitoredThreads({-1});
  } else if (!event_selection_set_.HasMonitoredTarget()) {
    if (workload != nullptr) {
      event_selection_set_.AddMonitoredProcesses({workload->GetPid()});
      event_selection_set_.SetEnableOnExec(true);
    } else if (!app_package_name_.empty()) {
      std::set<pid_t> pids = WaitForAppProcesses(app_package_name_);
      event_selection_set_.AddMonitoredProcesses(pids);
    } else {
      LOG(ERROR)
          << "No threads to monitor. Try `simpleperf help stat` for help\n";
      return false;
    }
  } else {
    need_to_check_targets = true;
  }

  // 3. Open perf_event_files and output file if defined.
  if (!system_wide_collection_ && cpus_.empty()) {
    cpus_.push_back(-1);  // Monitor on all cpus.
  }
  if (!event_selection_set_.OpenEventFiles(cpus_)) {
    return false;
  }
  std::unique_ptr<FILE, decltype(&fclose)> fp_holder(nullptr, fclose);
  if (!output_filename_.empty()) {
    fp_holder.reset(fopen(output_filename_.c_str(), "we"));
    if (fp_holder == nullptr) {
      PLOG(ERROR) << "failed to open " << output_filename_;
      return false;
    }
  } else if (out_fd_ != -1) {
    fp_holder.reset(fdopen(out_fd_.release(), "we"));
    if (fp_holder == nullptr) {
      PLOG(ERROR) << "failed to write output.";
      return false;
    }
  }
  FILE* fp = fp_holder ? fp_holder.get() : stdout;

  // 4. Add signal/periodic Events.
  IOEventLoop* loop = event_selection_set_.GetIOEventLoop();
  if (interval_in_ms_ != 0) {
    if (!loop->UsePreciseTimer()) {
      return false;
    }
  }
  std::chrono::time_point<std::chrono::steady_clock> start_time;
  std::vector<CountersInfo> counters;
  if (need_to_check_targets && !event_selection_set_.StopWhenNoMoreTargets()) {
    return false;
  }
  auto exit_loop_callback = [loop]() {
    return loop->ExitLoop();
  };
  if (!loop->AddSignalEvents({SIGCHLD, SIGINT, SIGTERM, SIGHUP}, exit_loop_callback)) {
    return false;
  }
  if (stop_signal_fd_ != -1) {
    if (!loop->AddReadEvent(stop_signal_fd_, exit_loop_callback)) {
      return false;
    }
  }
  if (duration_in_sec_ != 0) {
    if (!loop->AddPeriodicEvent(SecondToTimeval(duration_in_sec_), exit_loop_callback)) {
      return false;
    }
  }
  auto print_counters = [&]() {
      auto end_time = std::chrono::steady_clock::now();
      if (!event_selection_set_.ReadCounters(&counters)) {
        return false;
      }
      double duration_in_sec =
      std::chrono::duration_cast<std::chrono::duration<double>>(end_time -
                                                                start_time)
      .count();
      if (!ShowCounters(counters, duration_in_sec, fp)) {
        return false;
      }
      return true;
  };

  if (interval_in_ms_ != 0) {
    if (!loop->AddPeriodicEvent(SecondToTimeval(interval_in_ms_ / 1000.0),
                                print_counters)) {
      return false;
    }
  }

  // 5. Count events while workload running.
  start_time = std::chrono::steady_clock::now();
  if (workload != nullptr && !workload->Start()) {
    return false;
  }
  if (!loop->RunLoop()) {
    return false;
  }

  // 6. Read and print counters.
  if (interval_in_ms_ == 0) {
    return print_counters();
  }
  return true;
}

bool StatCommand::ParseOptions(const std::vector<std::string>& args,
                               std::vector<std::string>* non_option_args) {
  std::set<pid_t> tid_set;
  size_t i;
  for (i = 0; i < args.size() && args[i].size() > 0 && args[i][0] == '-'; ++i) {
    if (args[i] == "-a") {
      system_wide_collection_ = true;
    } else if (args[i] == "--app") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      app_package_name_ = args[i];
    } else if (args[i] == "--cpu") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      cpus_ = GetCpusFromString(args[i]);
    } else if (args[i] == "--csv") {
      csv_ = true;
    } else if (args[i] == "--duration") {
      if (!GetDoubleOption(args, &i, &duration_in_sec_, 1e-9)) {
        return false;
      }
    } else if (args[i] == "--interval") {
      if (!GetDoubleOption(args, &i, &interval_in_ms_, 1e-9)) {
        return false;
      }
    } else if (args[i] == "--interval-only-values") {
      interval_only_values_ = true;
    } else if (args[i] == "-e") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::vector<std::string> event_types = android::base::Split(args[i], ",");
      for (auto& event_type : event_types) {
        if (!event_selection_set_.AddEventType(event_type)) {
          return false;
        }
      }
    } else if (args[i] == "--group") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::vector<std::string> event_types = android::base::Split(args[i], ",");
      if (!event_selection_set_.AddEventGroup(event_types)) {
        return false;
      }
    } else if (args[i] == "--in-app") {
      in_app_context_ = true;
    } else if (args[i] == "--no-inherit") {
      child_inherit_ = false;
    } else if (args[i] == "-o") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      output_filename_ = args[i];
    } else if (args[i] == "--out-fd") {
      int fd;
      if (!GetUintOption(args, &i, &fd)) {
        return false;
      }
      out_fd_.reset(fd);
    } else if (args[i] == "-p") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::set<pid_t> pids;
      if (!GetValidThreadsFromThreadString(args[i], &pids)) {
        return false;
      }
      event_selection_set_.AddMonitoredProcesses(pids);
    } else if (args[i] == "--stop-signal-fd") {
      int fd;
      if (!GetUintOption(args, &i, &fd)) {
        return false;
      }
      stop_signal_fd_.reset(fd);
    } else if (args[i] == "-t") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      std::set<pid_t> tids;
      if (!GetValidThreadsFromThreadString(args[i], &tids)) {
        return false;
      }
      event_selection_set_.AddMonitoredThreads(tids);
    } else if (args[i] == "--tracepoint-events") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      if (!SetTracepointEventsFilePath(args[i])) {
        return false;
      }
#if defined(__ANDROID__)
    } else if (args[i] == "--use-devfreq-counters") {
      use_devfreq_counters_ = true;
#endif
    } else if (args[i] == "--verbose") {
      verbose_mode_ = true;
    } else {
      ReportUnknownOption(args, i);
      return false;
    }
  }

  if (system_wide_collection_ && event_selection_set_.HasMonitoredTarget()) {
    LOG(ERROR) << "Stat system wide and existing processes/threads can't be "
                  "used at the same time.";
    return false;
  }
  if (system_wide_collection_ && !IsRoot()) {
    LOG(ERROR) << "System wide profiling needs root privilege.";
    return false;
  }

  non_option_args->clear();
  for (; i < args.size(); ++i) {
    non_option_args->push_back(args[i]);
  }
  return true;
}

bool StatCommand::AddDefaultMeasuredEventTypes() {
  for (auto& name : default_measured_event_types) {
    // It is not an error when some event types in the default list are not
    // supported by the kernel.
    const EventType* type = FindEventTypeByName(name);
    if (type != nullptr &&
        IsEventAttrSupported(CreateDefaultPerfEventAttr(*type))) {
      if (!event_selection_set_.AddEventType(name)) {
        return false;
      }
    }
  }
  if (event_selection_set_.empty()) {
    LOG(ERROR) << "Failed to add any supported default measured types";
    return false;
  }
  return true;
}

void StatCommand::SetEventSelectionFlags() {
  event_selection_set_.SetInherit(child_inherit_);
}

bool StatCommand::ShowCounters(const std::vector<CountersInfo>& counters,
                               double duration_in_sec, FILE* fp) {
  if (csv_) {
    fprintf(fp, "Performance counter statistics,\n");
  } else {
    fprintf(fp, "Performance counter statistics:\n\n");
  }

  if (verbose_mode_) {
    for (auto& counters_info : counters) {
      for (auto& counter_info : counters_info.counters) {
        if (csv_) {
          fprintf(fp, "%s,tid,%d,cpu,%d,count,%" PRIu64 ",time_enabled,%" PRIu64
                      ",time running,%" PRIu64 ",id,%" PRIu64 ",\n",
                  counters_info.event_name.c_str(), counter_info.tid,
                  counter_info.cpu, counter_info.counter.value,
                  counter_info.counter.time_enabled,
                  counter_info.counter.time_running, counter_info.counter.id);
        } else {
          fprintf(fp,
                  "%s(tid %d, cpu %d): count %" PRIu64 ", time_enabled %" PRIu64
                  ", time running %" PRIu64 ", id %" PRIu64 "\n",
                  counters_info.event_name.c_str(), counter_info.tid,
                  counter_info.cpu, counter_info.counter.value,
                  counter_info.counter.time_enabled,
                  counter_info.counter.time_running, counter_info.counter.id);
        }
      }
    }
  }

  bool counters_always_available = true;
  CounterSummaries summaries(csv_);
  for (size_t i = 0; i < counters.size(); ++i) {
    const CountersInfo& counters_info = counters[i];
    CounterSum sum;
    for (auto& counter_info : counters_info.counters) {
      sum.value += counter_info.counter.value;
      sum.time_enabled += counter_info.counter.time_enabled;
      sum.time_running += counter_info.counter.time_running;
    }
    if (interval_only_values_) {
      if (last_sum_values_.size() < counters.size()) {
        last_sum_values_.resize(counters.size());
      }
      CounterSum tmp = sum;
      sum.value -= last_sum_values_[i].value;
      sum.time_enabled -= last_sum_values_[i].time_enabled;
      sum.time_running -= last_sum_values_[i].time_running;
      last_sum_values_[i] = tmp;
    }

    double scale = 1.0;
    if (sum.time_running < sum.time_enabled && sum.time_running != 0) {
      scale = static_cast<double>(sum.time_enabled) / sum.time_running;
    }
    summaries.Summaries().emplace_back(counters_info.event_name, counters_info.event_modifier,
                                       counters_info.group_id, sum.value, scale, false, csv_);
    counters_always_available &= summaries.Summaries().back().IsMonitoredAllTheTime();
  }
  summaries.AutoGenerateSummaries();
  summaries.GenerateComments(duration_in_sec);
  summaries.Show(fp);

  if (csv_)
    fprintf(fp, "Total test time,%lf,seconds,\n", duration_in_sec);
  else
    fprintf(fp, "\nTotal test time: %lf seconds.\n", duration_in_sec);

  if (!counters_always_available) {
    LOG(WARNING) << "Some hardware counters are not always available (scale < 100%). "
                 << "Try --use-devfreq-counters if on a rooted device.";
  }
  return true;
}

}  // namespace

void RegisterStatCommand() {
  RegisterCommand("stat",
                  [] { return std::unique_ptr<Command>(new StatCommand); });
}
