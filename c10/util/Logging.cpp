#include <c10/util/Backtrace.h>
#include <c10/util/Flags.h>
#include <c10/util/Lazy.h>
#include <c10/util/Logging.h>
#ifdef FBCODE_CAFFE2
#include <folly/synchronization/SanitizeThread.h>
#endif

#ifndef _WIN32
#include <sys/time.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <iostream>

// Common code that we use regardless of whether we use glog or not.

C10_DEFINE_bool(
    caffe2_use_fatal_for_enforce,
    false,
    "If set true, when CAFFE_ENFORCE is not met, abort instead "
    "of throwing an exception.");

namespace c10 {

namespace {
std::function<::c10::Backtrace()>& GetFetchStackTrace() {
  static std::function<::c10::Backtrace()> func = []() {
    return get_lazy_backtrace(/*frames_to_skip=*/1);
  };
  return func;
}
} // namespace

void SetStackTraceFetcher(std::function<::c10::Backtrace()> fetcher) {
  GetFetchStackTrace() = std::move(fetcher);
}

void SetStackTraceFetcher(std::function<string()> fetcher) {
  SetStackTraceFetcher([fetcher = std::move(fetcher)] {
    return std::make_shared<PrecomputedLazyValue<std::string>>(fetcher());
  });
}

void ThrowEnforceNotMet(
    const char* file,
    const int line,
    const char* condition,
    const std::string& msg,
    const void* caller) {
  c10::Error e(file, line, condition, msg, GetFetchStackTrace()(), caller);
  if (FLAGS_caffe2_use_fatal_for_enforce) {
    LOG(FATAL) << e.msg();
  }
  throw std::move(e);
}

void ThrowEnforceNotMet(
    const char* file,
    const int line,
    const char* condition,
    const char* msg,
    const void* caller) {
  ThrowEnforceNotMet(file, line, condition, std::string(msg), caller);
}

void ThrowEnforceFiniteNotMet(
    const char* file,
    const int line,
    const char* condition,
    const std::string& msg,
    const void* caller) {
  throw c10::EnforceFiniteError(
      file, line, condition, msg, GetFetchStackTrace()(), caller);
}

void ThrowEnforceFiniteNotMet(
    const char* file,
    const int line,
    const char* condition,
    const char* msg,
    const void* caller) {
  ThrowEnforceFiniteNotMet(file, line, condition, std::string(msg), caller);
}

namespace {

class PyTorchStyleBacktrace : public OptimisticLazyValue<std::string> {
 public:
  PyTorchStyleBacktrace(SourceLocation source_location)
      : backtrace_(GetFetchStackTrace()()), source_location_(source_location) {}

 private:
  std::string compute() const override {
    return str(
        "Exception raised from ",
        source_location_,
        " (most recent call first):\n",
        backtrace_->get());
  }

  ::c10::Backtrace backtrace_;
  SourceLocation source_location_;
};

} // namespace

// PyTorch-style error message
// (This must be defined here for access to GetFetchStackTrace)
Error::Error(SourceLocation source_location, std::string msg)
    : Error(
          std::move(msg),
          std::make_shared<PyTorchStyleBacktrace>(source_location)) {}

using APIUsageLoggerType = std::function<void(const std::string&)>;
using APIUsageMetadataLoggerType = std::function<void(
    const std::string&,
    const std::map<std::string, std::string>& metadata_map)>;
using DDPUsageLoggerType = std::function<void(const DDPLoggingData&)>;

namespace {
bool IsAPIUsageDebugMode() {
  const char* val = getenv("PYTORCH_API_USAGE_STDERR");
  return val && *val; // any non-empty value
}

void APIUsageDebug(const string& event) {
  // use stderr to avoid messing with glog
  std::cerr << "PYTORCH_API_USAGE " << event << std::endl;
}

APIUsageLoggerType* GetAPIUsageLogger() {
  static APIUsageLoggerType func =
      IsAPIUsageDebugMode() ? &APIUsageDebug : [](const string&) {};
  return &func;
}

APIUsageMetadataLoggerType* GetAPIUsageMetadataLogger() {
  static APIUsageMetadataLoggerType func =
      [](const std::string&,
         const std::map<std::string, std::string>& metadata_map) {};
  return &func;
}

DDPUsageLoggerType* GetDDPUsageLogger() {
  static DDPUsageLoggerType func = [](const DDPLoggingData&) {};
  return &func;
}
} // namespace

void SetAPIUsageLogger(std::function<void(const std::string&)> logger) {
  TORCH_CHECK(logger);
  *GetAPIUsageLogger() = std::move(logger);
}

void SetAPIUsageMetadataLogger(
    std::function<void(
        const std::string&,
        const std::map<std::string, std::string>& metadata_map)> logger) {
  TORCH_CHECK(logger);
  *GetAPIUsageMetadataLogger() = std::move(logger);
}

void SetPyTorchDDPUsageLogger(
    std::function<void(const DDPLoggingData&)> logger) {
  TORCH_CHECK(logger);
  *GetDDPUsageLogger() = std::move(logger);
}

static int64_t GLOBAL_RANK = -1;

int64_t GetGlobalRank() {
  return GLOBAL_RANK;
}

void SetGlobalRank(int64_t rank) {
  GLOBAL_RANK = rank;
}

void LogAPIUsage(const std::string& event) try {
  if (auto logger = GetAPIUsageLogger())
    (*logger)(event);
} catch (std::bad_function_call&) {
  // static destructor race
}

void LogAPIUsageMetadata(
    const std::string& context,
    const std::map<std::string, std::string>& metadata_map) try {
  if (auto logger = GetAPIUsageMetadataLogger())
    (*logger)(context, metadata_map);
} catch (std::bad_function_call&) {
  // static destructor race
}

void LogPyTorchDDPUsage(const DDPLoggingData& ddpData) try {
  if (auto logger = GetDDPUsageLogger())
    (*logger)(ddpData);
} catch (std::bad_function_call&) {
  // static destructor race
}

namespace detail {
bool LogAPIUsageFakeReturn(const std::string& event) try {
  if (auto logger = GetAPIUsageLogger())
    (*logger)(event);
  return true;
} catch (std::bad_function_call&) {
  // static destructor race
  return true;
}

namespace {

void setLogLevelFlagFromEnv();

} // namespace
} // namespace detail
} // namespace c10

// This backward compatibility flags are in order to deal with cases where
// Caffe2 are not built with glog, but some init flags still pass in these
// flags. They may go away in the future.
C10_DEFINE_int32(minloglevel, 0, "Equivalent to glog minloglevel");
C10_DEFINE_int32(v, 0, "Equivalent to glog verbose");
C10_DEFINE_bool(logtostderr, false, "Equivalent to glog logtostderr");

#ifdef ANDROID
#include <android/log.h>
#endif // ANDROID

C10_DEFINE_int(
    caffe2_log_level,
    c10::GLOG_WARNING,
    "The minimum log level that caffe2 will output.");

namespace c10 {

void initLogging() {
  detail::setLogLevelFlagFromEnv();
}

bool InitCaffeLogging(int* argc, char** argv) {
  // When doing InitCaffeLogging, we will assume that caffe's flag parser has
  // already finished.
  if (*argc == 0)
    return true;
  if (!c10::CommandLineFlagsHasBeenParsed()) {
    std::cerr << "InitCaffeLogging() has to be called after "
                 "c10::ParseCommandLineFlags. Modify your program to make sure "
                 "of this."
              << std::endl;
    return false;
  }
  if (FLAGS_caffe2_log_level > GLOG_FATAL) {
    std::cerr << "The log level of Caffe2 has to be no larger than GLOG_FATAL("
              << GLOG_FATAL << "). Capping it to GLOG_FATAL." << std::endl;
    FLAGS_caffe2_log_level = GLOG_FATAL;
  }
  return true;
}

void UpdateLoggingLevelsFromFlags() {}

void ShowLogInfoToStderr() {
  FLAGS_caffe2_log_level = GLOG_INFO;
}

MessageLogger::MessageLogger(const char* file, int line, int severity)
    : severity_(severity) {
  if (severity_ < FLAGS_caffe2_log_level) {
    // Nothing needs to be logged.
    return;
  }
#ifdef ANDROID
  tag_ = "native";
#else // !ANDROID
  tag_ = "";
#endif // ANDROID

  time_t rawtime = 0;
  time(&rawtime);

#ifndef _WIN32
  struct tm raw_timeinfo = {0};
  struct tm* timeinfo = &raw_timeinfo;
  localtime_r(&rawtime, timeinfo);
#else
  // is thread safe on Windows
  struct tm* timeinfo = localtime(&rawtime);
#endif

#ifndef _WIN32
  // Get the current nanoseconds since epoch
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long ns = ts.tv_nsec;
#else
  long ns = 0;
#endif

  if (GLOBAL_RANK != -1) {
    stream_ << "[rank" << GLOBAL_RANK << "]:";
  }
  stream_ << "[" << CAFFE2_SEVERITY_PREFIX[std::min(4, GLOG_FATAL - severity_)]
          << (timeinfo->tm_mon + 1) * 100 + timeinfo->tm_mday
          << std::setfill('0') << " " << std::setw(2) << timeinfo->tm_hour
          << ":" << std::setw(2) << timeinfo->tm_min << ":" << std::setw(2)
          << timeinfo->tm_sec << "." << std::setw(9) << ns << " "
          << c10::detail::StripBasename(std::string(file)) << ":" << line
          << "] ";
}

// Output the contents of the stream to the proper channel on destruction.
MessageLogger::~MessageLogger() {
  if (severity_ < FLAGS_caffe2_log_level) {
    // Nothing needs to be logged.
    return;
  }
  stream_ << "\n";
#ifdef ANDROID
  static const int android_log_levels[] = {
      ANDROID_LOG_FATAL, // LOG_FATAL
      ANDROID_LOG_ERROR, // LOG_ERROR
      ANDROID_LOG_WARN, // LOG_WARNING
      ANDROID_LOG_INFO, // LOG_INFO
      ANDROID_LOG_DEBUG, // VLOG(1)
      ANDROID_LOG_VERBOSE, // VLOG(2) .. VLOG(N)
  };
  int android_level_index = GLOG_FATAL - std::min(GLOG_FATAL, severity_);
  int level = android_log_levels[std::min(android_level_index, 5)];
  // Output the log string the Android log at the appropriate level.
  __android_log_print(level, tag_, "%s", stream_.str().c_str());
  // Indicate termination if needed.
  if (severity_ == GLOG_FATAL) {
    __android_log_print(ANDROID_LOG_FATAL, tag_, "terminating.\n");
  }
#else // !ANDROID
  if (severity_ >= FLAGS_caffe2_log_level) {
    // If not building on Android, log all output to std::cerr.
    std::cerr << stream_.str();
    // Simulating the glog default behavior: if the severity is above INFO,
    // we flush the stream so that the output appears immediately on std::cerr.
    // This is expected in some of our tests.
    if (severity_ > GLOG_INFO) {
      std::cerr << std::flush;
    }
  }
#endif // ANDROID
  if (severity_ == GLOG_FATAL) {
    DealWithFatal();
  }
}

} // namespace c10

namespace c10::detail {
namespace {

void setLogLevelFlagFromEnv() {
  const char* level_str = std::getenv("TORCH_CPP_LOG_LEVEL");

  // Not set, fallback to the default level (i.e. WARNING).
  std::string level{level_str != nullptr ? level_str : ""};
  if (level.empty()) {
    return;
  }

  std::transform(
      level.begin(), level.end(), level.begin(), [](unsigned char c) {
        return toupper(c);
      });

  if (level == "0" || level == "INFO") {
    FLAGS_caffe2_log_level = 0;

    return;
  }
  if (level == "1" || level == "WARNING") {
    FLAGS_caffe2_log_level = 1;

    return;
  }
  if (level == "2" || level == "ERROR") {
    FLAGS_caffe2_log_level = 2;

    return;
  }
  if (level == "3" || level == "FATAL") {
    FLAGS_caffe2_log_level = 3;

    return;
  }

  std::cerr
      << "`TORCH_CPP_LOG_LEVEL` environment variable cannot be parsed. Valid values are "
         "`INFO`, `WARNING`, `ERROR`, and `FATAL` or their numerical equivalents `0`, `1`, "
         "`2`, and `3`."
      << std::endl;
}

} // namespace
} // namespace c10::detail
