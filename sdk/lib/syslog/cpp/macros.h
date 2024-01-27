// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYSLOG_CPP_MACROS_H_
#define LIB_SYSLOG_CPP_MACROS_H_

#if defined(__Fuchsia__)
#include <zircon/types.h>
#endif

#include <lib/syslog/cpp/log_level.h>

#include <atomic>
#include <functional>
#include <limits>
#include <sstream>
#include <vector>

namespace syslog_backend {

struct LogBuffer;

#define WEAK __attribute__((weak))

#ifdef __Fuchsia__
WEAK void BeginRecordWithSocket(LogBuffer* buffer, fuchsia_logging::LogSeverity severity,
                                const char* file_name, unsigned int line, const char* msg,
                                const char* condition, zx_handle_t socket);
WEAK void SetInterestChangedListener(void (*callback)(void* context,
                                                      fuchsia_logging::LogSeverity severity),
                                     void* context);
#endif
WEAK void BeginRecord(LogBuffer* buffer, fuchsia_logging::LogSeverity severity, const char* file,
                      unsigned int line, const char* msg, const char* condition);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, const char* value);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, const char* value, size_t value_length);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, int64_t value);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, uint64_t value);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, double value);

WEAK void WriteKeyValue(LogBuffer* buffer, const char* key, bool value);

WEAK bool FlushRecord(LogBuffer* buffer);

WEAK bool HasStructuredBackend();

#undef WEAK

template <typename... Args>
constexpr size_t ArgsSize(Args... args) {
  return sizeof...(args);
}

template <typename... Args>
struct Tuplet {
  std::tuple<Args...> tuple;
  size_t size;
  constexpr Tuplet(std::tuple<Args...> tuple, size_t size) : tuple(tuple), size(size) {}
};

template <typename Key, typename Value>
struct KeyValue {
  Key key;
  Value value;
  constexpr KeyValue(Key key, Value value) : key(key), value(value) {}
};

template <size_t i, size_t size>
constexpr bool ILessThanSize() {
  return i < size;
}

template <bool expr>
constexpr bool Not() {
  return !expr;
}

// Opaque structure representing the backend encode state.
// This structure only has meaning to the backend and application code shouldn't
// touch these values.
struct LogBuffer {
  // Max size of log buffer. This number may change as additional fields
  // are added to the internal encoding state. It is based on trial-and-error
  // and is adjusted when compilation fails due to it not being large enough.
  static constexpr auto kBufferSize = (1 << 15) / 8;
  // Additional storage for internal log state.
  static constexpr auto kStateSize = 18;
  // Record state (for keeping track of backend-specific details)
  uint64_t record_state[kStateSize];
  // Log data (used by the backend to encode the log into). The format
  // for this is backend-specific.
  uint64_t data[kBufferSize];

  // Does nothing (enabled when we hit the last parameter that the user passed into us)
  template <size_t i, size_t size, typename... T,
            typename std::enable_if<Not<ILessThanSize<i, size>()>(), int>::type = 0>
  void Encode(Tuplet<T...> value) {}

  // Encodes an int8
  void Encode(KeyValue<const char*, int8_t> value) {
    Encode(KeyValue<const char*, int64_t>(value.key, value.value));
  }

  // Encodes an int16
  void Encode(KeyValue<const char*, int16_t> value) {
    Encode(KeyValue<const char*, int64_t>(value.key, value.value));
  }

  // Encodes an int32
  void Encode(KeyValue<const char*, int32_t> value) {
    Encode(KeyValue<const char*, int64_t>(value.key, value.value));
  }

  // Encodes an int64
  void Encode(KeyValue<const char*, int64_t> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

#ifdef __APPLE__
  // Encodes a size_t. On Apple Clang, size_t is a special type.
  void Encode(KeyValue<const char*, size_t> value) {
    syslog_backend::WriteKeyValue(this, value.key, static_cast<int64_t>(value.value));
  }
#endif

  // Encodes an uint8_t
  void Encode(KeyValue<const char*, uint8_t> value) {
    Encode(KeyValue<const char*, uint64_t>(value.key, value.value));
  }

  // Encodes an uint16_t
  void Encode(KeyValue<const char*, uint16_t> value) {
    Encode(KeyValue<const char*, uint64_t>(value.key, value.value));
  }

  // Encodes a uint32_t
  void Encode(KeyValue<const char*, uint32_t> value) {
    Encode(KeyValue<const char*, uint64_t>(value.key, value.value));
  }

  // Encodes an uint64
  void Encode(KeyValue<const char*, uint64_t> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes a NULL-terminated C-string.
  void Encode(KeyValue<const char*, const char*> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes a NULL-terminated C-string.
  void Encode(KeyValue<const char*, char*> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes a C++ std::string.
  void Encode(KeyValue<const char*, std::string> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value.data(), value.value.size());
  }

  // Encodes a C++ std::string_view.
  void Encode(KeyValue<const char*, std::string_view> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value.data(), value.value.size());
  }

  // Encodes a double floating point value
  void Encode(KeyValue<const char*, double> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes a floating point value
  void Encode(KeyValue<const char*, float> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes a boolean value
  void Encode(KeyValue<const char*, bool> value) {
    syslog_backend::WriteKeyValue(this, value.key, value.value);
  }

  // Encodes an arbitrary list of values recursively.
  template <size_t i, size_t size, typename... T,
            typename std::enable_if<ILessThanSize<i, size>(), int>::type = 0>
  void Encode(Tuplet<T...> value) {
    auto val = std::get<i>(value.tuple);
    Encode(val);
    Encode<i + 1, size>(value);
  }
};
}  // namespace syslog_backend

namespace fuchsia_logging {

template <typename... LogArgs>
constexpr syslog_backend::Tuplet<LogArgs...> Args(LogArgs... values) {
  return syslog_backend::Tuplet<LogArgs...>(std::make_tuple(values...), sizeof...(values));
}

template <typename Key, typename Value>
constexpr syslog_backend::KeyValue<Key, Value> KeyValueInternal(Key key, Value value) {
  return syslog_backend::KeyValue<Key, Value>(key, value);
}

// Used to denote a key-value pair for use in structured logging API calls.
// This macro exists solely to improve readability of calls to FX_SLOG
#define KV(a, b) a, b

template <typename Msg, typename... KeyValuePairs>
struct LogValue {
  constexpr LogValue(Msg msg, syslog_backend::Tuplet<KeyValuePairs...> kvps)
      : msg(msg), kvps(kvps) {}
  // FIXME(fxbug.dev/106574): With hwasan, or asan without stack-to-heap promotion for
  // detecting use-after-returns, we can encounter a stack overflow in blobfs when bringing
  // up one of the drivers needed for networking. This largely has to do with the LogBuffer
  // which is 32kB and inlining can cause 4 of them to be allocated in a single frame. Without
  // hwasan/asan, stack coloring can merge these to use the same stack space, but hwasan/asan
  // both prevent this. It's still desirable to not have such a large object on the stack, so
  // until we come up with a better API for using this or figure out sanitizers and stack coloring,
  // we can temporarily work around the stack overflow by disabling inlining for this function.
  __attribute__((__noinline__)) void LogNew(::fuchsia_logging::LogSeverity severity,
                                            const char* file, unsigned int line,
                                            const char* condition) const {
    syslog_backend::LogBuffer buffer;
    syslog_backend::BeginRecord(&buffer, severity, file, line, msg, condition);
    // https://bugs.llvm.org/show_bug.cgi?id=41093 -- Clang loses constexpr
    // even though this should be constexpr here.
    buffer.Encode<0, sizeof...(KeyValuePairs)>(kvps);
    syslog_backend::FlushRecord(&buffer);
  }

  Msg msg;
  syslog_backend::Tuplet<KeyValuePairs...> kvps;
};

class LogMessageVoidify {
 public:
  void operator&(std::ostream&) {}
};

class LogMessage {
 public:
  LogMessage(LogSeverity severity, const char* file, int line, const char* condition,
             const char* tag
#if defined(__Fuchsia__)
             ,
             zx_status_t status = std::numeric_limits<zx_status_t>::max()
#endif
  );
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  std::ostringstream stream_;
  const LogSeverity severity_;
  const char* file_;
  const int line_;
  const char* condition_;
  const char* tag_;
#if defined(__Fuchsia__)
  const zx_status_t status_;
#endif
};

// LogFirstNState is used by the macro FX_LOGS_FIRST_N below.
class LogFirstNState {
 public:
  bool ShouldLog(uint32_t n);

 private:
  std::atomic<uint32_t> counter_{0};
};

// Gets the FX_VLOGS default verbosity level.
uint8_t GetVlogVerbosity();

// Returns true if |severity| is at or above the current minimum log level.
// LOG_FATAL and above is always true.
bool ShouldCreateLogMessage(LogSeverity severity);

}  // namespace fuchsia_logging

#define FX_LOG_STREAM(severity, tag)                                                            \
  ::fuchsia_logging::LogMessage(::fuchsia_logging::LOG_##severity, __FILE__, __LINE__, nullptr, \
                                tag)                                                            \
      .stream()

#define FX_LOG_STREAM_STATUS(severity, status, tag)                                             \
  ::fuchsia_logging::LogMessage(::fuchsia_logging::LOG_##severity, __FILE__, __LINE__, nullptr, \
                                tag, status)                                                    \
      .stream()

#define FX_LAZY_STREAM(stream, condition) \
  !(condition) ? (void)0 : ::fuchsia_logging::LogMessageVoidify() & (stream)

#define FX_EAT_STREAM_PARAMETERS(ignored)                                                       \
  true || (ignored)                                                                             \
      ? (void)0                                                                                 \
      : ::fuchsia_logging::LogMessageVoidify() &                                                \
            ::fuchsia_logging::LogMessage(::fuchsia_logging::LOG_FATAL, 0, 0, nullptr, nullptr) \
                .stream()

#define FX_LOG_IS_ON(severity) \
  (::fuchsia_logging::ShouldCreateLogMessage(::fuchsia_logging::LOG_##severity))

#define FX_LOGS(severity) FX_LOGST(severity, nullptr)

#define FX_LOGST(severity, tag) FX_LAZY_STREAM(FX_LOG_STREAM(severity, tag), FX_LOG_IS_ON(severity))

#if defined(__Fuchsia__)
#define FX_PLOGST(severity, tag, status) \
  FX_LAZY_STREAM(FX_LOG_STREAM_STATUS(severity, status, tag), FX_LOG_IS_ON(severity))
#define FX_PLOGS(severity, status) FX_PLOGST(severity, nullptr, status)
#endif

// Writes a message to the global logger, the first |n| times that any callsite
// of this macro is invoked. |n| should be a positive integer literal.
// |severity| is one of INFO, WARNING, ERROR, FATAL
//
// Implementation notes:
// The outer for loop is a trick to allow us to introduce a new scope and
// introduce the variable |do_log| into that scope. It executes exactly once.
//
// The inner for loop is a trick to allow us to introduce a new scope and
// introduce the static variable |internal_state| into that new scope. It
// executes either zero or one times.
//
// C++ does not allow us to introduce two new variables into a single for loop
// scope and we need |do_log| so that the inner for loop doesn't execute twice.
#define FX_FIRST_N(n, log_statement)                              \
  for (bool do_log = true; do_log; do_log = false)                \
    for (static ::fuchsia_logging::LogFirstNState internal_state; \
         do_log && internal_state.ShouldLog(n); do_log = false)   \
  log_statement
#define FX_LOGS_FIRST_N(severity, n) FX_FIRST_N(n, FX_LOGS(severity))
#define FX_LOGST_FIRST_N(severity, n, tag) FX_FIRST_N(n, FX_LOGST(severity, tag))

#define FX_CHECK(condition) FX_CHECKT(condition, nullptr)

#define FX_CHECKT(condition, tag)                                                                \
  FX_LAZY_STREAM(::fuchsia_logging::LogMessage(::fuchsia_logging::LOG_FATAL, __FILE__, __LINE__, \
                                               #condition, tag)                                  \
                     .stream(),                                                                  \
                 !(condition))

// The VLOG macros log with translated verbosities

// Get the severity corresponding to the given verbosity. Note that
// verbosity relative to the default severity and can be thought of
// as incrementally "more vebose than" the baseline.
fuchsia_logging::LogSeverity GetSeverityFromVerbosity(uint8_t verbosity);

#define FX_VLOG_IS_ON(verbose_level) (verbose_level <= ::fuchsia_logging::GetVlogVerbosity())

#define FX_VLOG_STREAM(verbose_level, tag)                                                   \
  ::fuchsia_logging::LogMessage(GetSeverityFromVerbosity(verbose_level), __FILE__, __LINE__, \
                                nullptr, tag)                                                \
      .stream()

#define FX_VLOGS(verbose_level) \
  FX_LAZY_STREAM(FX_VLOG_STREAM(verbose_level, nullptr), FX_VLOG_IS_ON(verbose_level))

#define FX_VLOGST(verbose_level, tag) \
  FX_LAZY_STREAM(FX_VLOG_STREAM(verbose_level, tag), FX_VLOG_IS_ON(verbose_level))

#ifndef NDEBUG
#define FX_DLOGS(severity) FX_LOGS(severity)
#define FX_DVLOGS(verbose_level) FX_VLOGS(verbose_level)
#define FX_DCHECK(condition) FX_CHECK(condition)
#else
#define FX_DLOGS(severity) FX_EAT_STREAM_PARAMETERS(true)
#define FX_DVLOGS(verbose_level) FX_EAT_STREAM_PARAMETERS(true)
#define FX_DCHECK(condition) FX_EAT_STREAM_PARAMETERS(condition)
#endif

#define FX_NOTREACHED() FX_DCHECK(false)

#define FX_NOTIMPLEMENTED() FX_LOGS(ERROR) << "Not implemented in: " << __PRETTY_FUNCTION__

template <typename Msg, typename... Args>
static auto MakeValue(Msg msg, syslog_backend::Tuplet<Args...> args) {
  return fuchsia_logging::LogValue<Msg, Args...>(msg, args);
}

template <size_t i, size_t size, typename... Values, typename... Tuple,
          typename std::enable_if<syslog_backend::Not<syslog_backend::ILessThanSize<i, size>()>(),
                                  int>::type = 0>
static auto MakeKV(std::tuple<Values...> value, std::tuple<Tuple...> tuple) {
  return syslog_backend::Tuplet<Tuple...>(tuple, size);
}

template <size_t i, size_t size, typename... Values, typename... Tuple,
          typename std::enable_if<syslog_backend::ILessThanSize<i, size>(), int>::type = 0>
static auto MakeKV(std::tuple<Values...> value, std::tuple<Tuple...> tuple) {
  // Key at index i, value at index i+1
  auto k = std::get<i>(value);
  auto v = std::get<i + 1>(value);
  auto new_tuple = std::tuple_cat(tuple, std::make_tuple(fuchsia_logging::KeyValueInternal(k, v)));
  return MakeKV<i + 2, size, Values...>(value, new_tuple);
}

template <typename... Args, typename... EmptyTuple>
static auto MakeKV(std::tuple<Args...> args, std::tuple<EmptyTuple...> start_tuple) {
  return MakeKV<0, sizeof...(Args), Args..., EmptyTuple...>(args, start_tuple);
}

template <typename... Args>
static auto MakeKV(std::tuple<Args...> args) {
  return MakeKV(args, std::make_tuple());
}

template <typename Msg, typename... Args>
static void fx_slog_internal(fuchsia_logging::LogSeverity flag, const char* file, int line, Msg msg,
                             Args... args) {
  MakeValue(msg, MakeKV<Args...>(std::make_tuple(args...))).LogNew(flag, file, line, nullptr);
}

#define FX_SLOG_ETC(flag, args...)                         \
  do {                                                     \
    if (::fuchsia_logging::ShouldCreateLogMessage(flag)) { \
      fx_slog_internal(flag, __FILE__, __LINE__, args);    \
    }                                                      \
  } while (0)

#define FX_SLOG(flag, msg...) FX_SLOG_ETC(::fuchsia_logging::LOG_##flag, msg)

#endif  // LIB_SYSLOG_CPP_MACROS_H_
