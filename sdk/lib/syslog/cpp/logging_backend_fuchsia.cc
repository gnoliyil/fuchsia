// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fuchsia/diagnostics/stream/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/executor.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/stdcompat/variant.h>
#include <lib/sync/completion.h>
#include <lib/syslog/cpp/log_level.h>
#include <lib/syslog/cpp/logging_backend.h>
#include <lib/syslog/cpp/logging_backend_fuchsia_globals.h>
#include <lib/syslog/streams/cpp/encode.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>

#include <fstream>
#include <iostream>
#include <sstream>

#include "lib/syslog/cpp/logging_backend_fuchsia_private.h"
#include "lib/syslog/cpp/macros.h"
#include "lib/syslog/streams/cpp/fields.h"

namespace syslog_backend {

// Returns true if we are running in the DDK.
// This is used to customize logging behavior for drivers
// and related driver functionality.
bool fx_log_compat_no_interest_listener();

bool HasStructuredBackend() { return true; }

using log_word_t = uint64_t;

zx_koid_t GetKoid(zx_handle_t handle) {
  zx_info_handle_basic_t info;
  // We need to use _zx_object_get_info to avoid breaking the driver ABI.
  // fake_ddk can fake out this method, which results in us deadlocking
  // when used in certain drivers because the fake doesn't properly pass-through
  // to the real syscall in this case.
  zx_status_t status =
      _zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  return status == ZX_OK ? info.koid : ZX_KOID_INVALID;
}

static zx_koid_t pid = GetKoid(zx_process_self());
static thread_local zx_koid_t tid = GetCurrentThreadKoid();

// Represents a slice of a buffer of type T.
template <typename T>
class DataSlice {
 public:
  DataSlice(T* ptr, WordOffset<T> slice) : ptr_(ptr), slice_(slice) {}

  T& operator[](WordOffset<T> offset) {
    offset.AssertValid();
    return ptr_[offset];
  }

  const T& operator[](WordOffset<T> offset) const { return ptr_[offset.unsafe_get()]; }

  WordOffset<T> slice() { return slice_; }

  T* data() { return ptr_; }

 private:
  T* ptr_;
  WordOffset<T> slice_;
};

static DataSlice<const char> SliceFromString(const std::string& string) {
  return DataSlice<const char>(
      string.data(), WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(string.size())));
}

template <typename T, size_t size>
static DataSlice<const T> SliceFromArray(const T (&array)[size]) {
  return DataSlice<const T>(array, size);
}

template <size_t size>
static DataSlice<const char> SliceFromArray(const char (&array)[size]) {
  return DataSlice<const char>(
      array, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(size - 1)));
}

static constexpr int WORD_SIZE = sizeof(log_word_t);  // See sdk/lib/syslog/streams/cpp/encode.cc
class DataBuffer {
 public:
  static constexpr auto kBufferSize = (1 << 15) / WORD_SIZE;
  void Write(const log_word_t* data, WordOffset<log_word_t> length) {
    for (size_t i = 0; i < length.unsafe_get(); i++) {
      buffer_[cursor_.unsafe_get() + i] = data[i];
    }
    cursor_ = cursor_ + length;
  }

  WordOffset<log_word_t> WritePadded(const void* msg, const ByteOffset& length) {
    auto retval = WritePaddedInternal(&buffer_[cursor_.unsafe_get()], msg, length);
    cursor_ = cursor_ + retval;
    return retval;
  }

  template <typename T>
  void Write(const T& data) {
    static_assert(sizeof(T) >= sizeof(log_word_t));
    static_assert(alignof(T) >= sizeof(log_word_t));
    Write(reinterpret_cast<const log_word_t*>(&data), cursor_ + (sizeof(T) / sizeof(log_word_t)));
  }

  DataSlice<log_word_t> GetSlice() { return DataSlice<log_word_t>(buffer_, cursor_); }

 private:
  WordOffset<log_word_t> cursor_;
  log_word_t buffer_[kBufferSize];
};

struct RecordState {
  RecordState()
      : arg_size(WordOffset<log_word_t>::FromByteOffset(
            ByteOffset::FromBuffer(0, sizeof(LogBuffer::data)))),
        current_key_size(ByteOffset::FromBuffer(0, sizeof(LogBuffer::data))),
        cursor(WordOffset<log_word_t>::FromByteOffset(
            ByteOffset::FromBuffer(0, sizeof(LogBuffer::data)))) {}
  // Header of the record itself
  uint64_t* header;
  syslog::LogSeverity log_severity;
  // True if we're logging from a driver.
  bool is_legacy_backend = false;
  syslog::LogSeverity raw_severity;
  ::fuchsia::diagnostics::Severity severity;
  // arg_size in words
  WordOffset<log_word_t> arg_size;
  zx_handle_t socket = ZX_HANDLE_INVALID;
  // key_size in bytes
  ByteOffset current_key_size;
  // Header of the current argument being encoded
  uint64_t* current_header_position = 0;
  uint32_t dropped_count = 0;
  // Current position (in 64-bit words) into the buffer.
  WordOffset<log_word_t> cursor;
  // True if encoding was successful, false otherwise
  bool encode_success = true;
  // Message string -- valid if severity is FATAL. For FATAL
  // logs the caller is responsible for ensuring the string
  // is valid for the duration of the call (which our macros
  // will ensure for current users). This must be a
  // const char* as the type has to be trivially destructable.
  // This will leak on usage, as the process will crash shortly afterwards.
  const char* maybe_fatal_string;
  static RecordState* CreatePtr(LogBuffer* buffer) {
    return reinterpret_cast<RecordState*>(&buffer->record_state);
  }
  size_t PtrToIndex(void* ptr) const {
    return reinterpret_cast<size_t>(static_cast<uint8_t*>(ptr)) - reinterpret_cast<size_t>(header);
  }
};
static_assert(sizeof(RecordState) <= sizeof(LogBuffer::record_state));
static_assert(std::alignment_of<RecordState>() == sizeof(uint64_t));

// Used for accessing external data buffers provided by clients.
// Used by the Encoder to do in-place encoding of data
class ExternalDataBuffer {
 public:
  explicit ExternalDataBuffer(LogBuffer* buffer)
      : buffer_(&buffer->data[0]), cursor_(RecordState::CreatePtr(buffer)->cursor) {}

  ExternalDataBuffer(log_word_t* data, size_t length, WordOffset<log_word_t>& cursor)
      : buffer_(data), cursor_(cursor) {}
  __WARN_UNUSED_RESULT bool Write(const log_word_t* data, WordOffset<log_word_t> length) {
    if (!cursor_.in_bounds(length)) {
      return false;
    }
    for (size_t i = 0; i < length.unsafe_get(); i++) {
      buffer_[(cursor_ + i).unsafe_get()] = data[i];
    }
    cursor_ = cursor_ + length;
    return true;
  }

  __WARN_UNUSED_RESULT bool WritePadded(const void* msg, const ByteOffset& byte_count,
                                        WordOffset<log_word_t>* written) {
    assert(written != nullptr);
    WordOffset<log_word_t> word_count = cursor_.begin().AddPadded(byte_count);
    if (!cursor_.in_bounds(word_count)) {
      return false;
    }
    auto retval = WritePaddedInternal(buffer_ + cursor_.unsafe_get(), msg, byte_count);
    cursor_ = cursor_ + retval;
    *written = retval;
    return true;
  }

  template <typename T>
  __WARN_UNUSED_RESULT bool Write(const T& data) {
    static_assert(sizeof(T) >= sizeof(log_word_t));
    static_assert(alignof(T) >= sizeof(log_word_t));
    return Write(reinterpret_cast<const log_word_t*>(&data),
                 WordOffset<log_word_t>::FromByteOffset(
                     ByteOffset::Unbounded((sizeof(T) / sizeof(log_word_t)) * sizeof(log_word_t))));
  }

  uint64_t* data() { return buffer_ + cursor_.unsafe_get(); }

  DataSlice<log_word_t> GetSlice() { return DataSlice<log_word_t>(buffer_, cursor_); }

 private:
  // Start of buffer
  log_word_t* buffer_ = nullptr;
  // Current location in buffer (in words)
  WordOffset<log_word_t>& cursor_;
};

template <typename T>
class Encoder {
 public:
  explicit Encoder(T& buffer) { buffer_ = &buffer; }

  void Begin(RecordState& state, zx_time_t timestamp, ::fuchsia::diagnostics::Severity severity) {
    state.severity = severity;
    state.header = buffer_->data();
    log_word_t empty_header = 0;
    state.encode_success &= buffer_->Write(empty_header);
    state.encode_success &= buffer_->Write(timestamp);
  }

  void FlushPreviousArgument(RecordState& state) { state.arg_size.reset(); }

  void AppendArgumentKey(RecordState& state, DataSlice<const char> key) {
    FlushPreviousArgument(state);
    auto header_position = buffer_->data();
    log_word_t empty_header = 0;
    state.encode_success &= buffer_->Write(empty_header);
    WordOffset<log_word_t> s_size =
        WordOffset<log_word_t>::FromByteOffset(ByteOffset::Unbounded(0));
    state.encode_success &= buffer_->WritePadded(key.data(), key.slice().ToByteOffset(), &s_size);
    state.arg_size = s_size + 1;  // offset by 1 for the header
    state.current_key_size = key.slice().ToByteOffset();
    state.current_header_position = header_position;
  }

  uint64_t ComputeArgHeader(RecordState& state, int type) {
    return ArgumentFields::Type::Make(type) |
           ArgumentFields::SizeWords::Make(state.arg_size.unsafe_get()) |
           ArgumentFields::NameRefVal::Make(state.current_key_size.unsafe_get()) |
           ArgumentFields::NameRefMSB::Make(state.current_key_size.unsafe_get() > 0 ? 1 : 0) |
           ReservedFields::Value::Make(0);
  }

  void AppendArgumentValue(RecordState& state, int64_t value) {
    int type = 3;
    state.encode_success &= buffer_->Write(value);
    state.arg_size = state.arg_size + 1;
    *state.current_header_position = ComputeArgHeader(state, type);
  }

  void AppendArgumentValue(RecordState& state, uint64_t value) {
    int type = 4;
    state.encode_success &= buffer_->Write(value);
    state.arg_size = state.arg_size + 1;
    *state.current_header_position = ComputeArgHeader(state, type);
  }

  void AppendArgumentValue(RecordState& state, double value) {
    int type = 5;
    state.encode_success &= buffer_->Write(value);
    state.arg_size = state.arg_size + 1;
    *state.current_header_position = ComputeArgHeader(state, type);
  }

  void AppendArgumentValue(RecordState& state, DataSlice<const char> string) {
    int type = 6;
    WordOffset<log_word_t> written =
        WordOffset<log_word_t>::FromByteOffset(ByteOffset::Unbounded(0));
    state.encode_success &=
        buffer_->WritePadded(string.data(), string.slice().ToByteOffset(), &written);
    state.arg_size = state.arg_size + written;
    uint64_t value_ref =
        string.slice().unsafe_get() > 0 ? (1 << 15) | string.slice().unsafe_get() : 0;
    *state.current_header_position =
        ComputeArgHeader(state, type) | StringArgumentFields::ValueRef::Make(value_ref);
  }

  void AppendArgumentValue(RecordState& state, bool value) {
    int type = 9;
    *state.current_header_position = ComputeArgHeader(state, type) |
                                     BoolArgumentFields::Value::Make(static_cast<uint64_t>(value));
  }

  void End(RecordState& state) {
    // See src/lib/diagnostics/stream/rust/src/lib.rs
    constexpr auto kTracingFormatLogRecordType = 9;
    FlushPreviousArgument(state);
    uint64_t header =
        HeaderFields::Type::Make(kTracingFormatLogRecordType) |
        HeaderFields::SizeWords::Make(static_cast<size_t>(buffer_->data() - state.header)) |
        HeaderFields::Reserved::Make(0) | HeaderFields::Severity::Make(state.severity);
    *state.header = header;
  }

 private:
  T* buffer_;
};

const size_t kMaxTags = 4;  // Legacy from ulib/syslog. Might be worth rethinking.
const char kMessageFieldName[] = "message";
const char kPrintfFieldName[] = "printf";
const char kVerbosityFieldName[] = "verbosity";
const char kPidFieldName[] = "pid";
const char kTidFieldName[] = "tid";
const char kDroppedLogsFieldName[] = "dropped_logs";
const char kTagFieldName[] = "tag";
const char kFileFieldName[] = "file";
const char kLineFieldName[] = "line";

class GlobalStateLock;
class LogState {
 public:
  static void Set(const syslog::LogSettings& settings, const GlobalStateLock& lock);
  static void Set(const syslog::LogSettings& settings,
                  const std::initializer_list<std::string>& tags, const GlobalStateLock& lock);
  void set_severity_handler(void (*callback)(void* context, syslog::LogSeverity severity),
                            void* context) {
    handler_ = callback;
    handler_context_ = context;
  }

  syslog::LogSeverity min_severity() const { return min_severity_; }

  const std::string* tags() const { return tags_; }
  size_t tag_count() const { return num_tags_; }
  // Allowed to be const because descriptor_ is mutable
  cpp17::variant<zx::socket, std::ofstream>& descriptor() const { return descriptor_; }

  void HandleInterest();

  void Connect();
  void ConnectAsync();
  struct Task : public async_task_t {
    LogState* context;
    sync_completion_t completion;
  };
  static void RunTask(async_dispatcher_t* dispatcher, async_task_t* task, zx_status_t status) {
    Task* callback = static_cast<Task*>(task);
    callback->context->ConnectAsync();
    sync_completion_signal(&callback->completion);
  }

  // Thunk to initialize logging and allocate HLCPP objects
  // which are "thread-hostile" and cannot be allocated on a remote thread.
  void InitializeAsyncTask() {
    Task task = {};
    task.deadline = 0;
    task.handler = RunTask;
    task.context = this;
    async_post_task(executor_->dispatcher(), &task);
    sync_completion_wait(&task.completion, ZX_TIME_INFINITE);
  }

 private:
  LogState(const syslog::LogSettings& settings, const std::initializer_list<std::string>& tags);
  bool WriteLogToFile(std::ofstream* file_ptr, zx_time_t time, zx_koid_t pid, zx_koid_t tid,
                      syslog::LogSeverity severity, const char* file_name, unsigned int line,
                      const char* tag, const char* condition, const std::string& msg) const;
  fuchsia::logger::LogSinkPtr log_sink_;
  void (*handler_)(void* context, syslog::LogSeverity severity);
  void* handler_context_;
  async::Loop loop_;
  std::optional<async::Executor> executor_;
  std::atomic<syslog::LogSeverity> min_severity_;
  const syslog::LogSeverity default_severity_;
  mutable cpp17::variant<zx::socket, std::ofstream> descriptor_ = zx::socket();
  std::string tags_[kMaxTags];
  std::string tag_str_;
  size_t num_tags_ = 0;
  async_dispatcher_t* interest_listener_dispatcher_;
  bool serve_interest_listener_;
  bool wait_for_initial_interest_;
  // Handle to a fuchsia.logger.LogSink instance.
  zx_handle_t provided_log_sink_ = ZX_HANDLE_INVALID;
};

// Global state lock. In order to mutate the LogState through SetStateLocked
// and GetStateLocked you must hold this capability.
// Do not directly use the C API. The C API exists solely
// to expose globals as a shared library.
// If the logger is not initialized, this will implicitly init the logger.
class GlobalStateLock {
 public:
  GlobalStateLock() {
    AcquireState();
    if (!GetStateLocked()) {
      LogState::Set(syslog::LogSettings(), *this);
    }
  }

  // Retrieves the global state
  syslog_backend::LogState* operator->() const { return GetStateLocked(); }

  // Sets the global state
  void Set(syslog_backend::LogState* state) const { SetStateLocked(state); }

  // Retrieves the global state
  syslog_backend::LogState* operator*() const { return GetStateLocked(); }

  ~GlobalStateLock() { ReleaseState(); }
};

static syslog::LogSeverity IntoLogSeverity(fuchsia::diagnostics::Severity severity) {
  switch (severity) {
    case fuchsia::diagnostics::Severity::TRACE:
      return syslog::LOG_TRACE;
    case fuchsia::diagnostics::Severity::DEBUG:
      return syslog::LOG_DEBUG;
    case fuchsia::diagnostics::Severity::INFO:
      return syslog::LOG_INFO;
    case fuchsia::diagnostics::Severity::WARN:
      return syslog::LOG_WARNING;
    case fuchsia::diagnostics::Severity::ERROR:
      return syslog::LOG_ERROR;
    case fuchsia::diagnostics::Severity::FATAL:
      return syslog::LOG_FATAL;
  }
}

void LogState::HandleInterest() {
  log_sink_->WaitForInterestChange(
      [=](fuchsia::logger::LogSink_WaitForInterestChange_Result interest_result) {
        auto interest = std::move(interest_result.response().data);
        if (!interest.has_min_severity()) {
          min_severity_ = default_severity_;
        } else {
          min_severity_ = IntoLogSeverity(interest.min_severity());
        }
        handler_(handler_context_, min_severity_);
        HandleInterest();
      });
}

void LogState::ConnectAsync() {
  zx::channel logger, logger_request;
  if (zx::channel::create(0, &logger, &logger_request) != ZX_OK) {
    return;
  }
  if (provided_log_sink_ == ZX_HANDLE_INVALID) {
    // TODO(https://fxbug.dev/75214): Support for custom names.
    if (fdio_service_connect("/svc/fuchsia.logger.LogSink", logger_request.release()) != ZX_OK) {
      return;
    }
  } else {
    logger = zx::channel(provided_log_sink_);
    provided_log_sink_ = ZX_HANDLE_INVALID;
  }

  if (wait_for_initial_interest_) {
    fuchsia::logger::LogSinkSyncPtr sync_log_sink;
    sync_log_sink.Bind(std::move(logger));
    fuchsia::logger::LogSink_WaitForInterestChange_Result interest_result;
    sync_log_sink->WaitForInterestChange(&interest_result);
    auto interest = std::move(interest_result.response().data);
    if (!interest.has_min_severity()) {
      min_severity_ = default_severity_;
    } else {
      min_severity_ = IntoLogSeverity(interest.min_severity());
    }
    handler_(handler_context_, min_severity_);
    logger = sync_log_sink.Unbind().TakeChannel();
  }

  if (log_sink_.Bind(std::move(logger), loop_.dispatcher()) != ZX_OK) {
    return;
  }
  zx::socket local, remote;
  if (zx::socket::create(ZX_SOCKET_DATAGRAM, &local, &remote) != ZX_OK) {
    return;
  }
  log_sink_->ConnectStructured(std::move(remote));
  HandleInterest();
  descriptor_ = std::move(local);
}

void LogState::Connect() {
  // Always disable the interest listener for the DDK.
  // Once structured logging is available in the SDK
  // this may unblock support for this.
  // For now -- we have no way to properly support
  // this functionality for drivers.
  if (fx_log_compat_no_interest_listener()) {
    serve_interest_listener_ = false;
  }
  if (serve_interest_listener_) {
    if (!interest_listener_dispatcher_) {
      loop_.StartThread("log-interest-listener-thread");
    } else {
      executor_.emplace(interest_listener_dispatcher_);
    }
    handler_ = [](void* ctx, syslog::LogSeverity severity) {};
    InitializeAsyncTask();
  } else {
    zx::channel logger, logger_request;
    if (provided_log_sink_ == ZX_HANDLE_INVALID) {
      if (zx::channel::create(0, &logger, &logger_request) != ZX_OK) {
        return;
      }
      // TODO(https://fxbug.dev/75214): Support for custom names.
      if (fdio_service_connect("/svc/fuchsia.logger.LogSink", logger_request.release()) != ZX_OK) {
        return;
      }
    } else {
      logger = zx::channel(provided_log_sink_);
    }
    ::fuchsia::logger::LogSink_SyncProxy logger_client(std::move(logger));
    zx::socket local, remote;
    if (zx::socket::create(ZX_SOCKET_DATAGRAM, &local, &remote) != ZX_OK) {
      return;
    }
    if (logger_client.ConnectStructured(std::move(remote))) {
      return;
    }

    descriptor_ = std::move(local);
  }
}

void SetInterestChangedListener(void (*callback)(void* context, syslog::LogSeverity severity),
                                void* context) {
  GlobalStateLock log_state;
  log_state->set_severity_handler(callback, context);
}

cpp17::string_view StripDots(cpp17::string_view path) {
  while (strncmp(path.data(), "../", 3) == 0) {
    path = path.substr(3);
  }
  return path;
}

void BeginRecordInternal(LogBuffer* buffer, syslog::LogSeverity severity, const char* file_name,
                         unsigned int line, const char* msg, const char* condition, bool is_printf,
                         zx_handle_t socket) {
  // Ensure we have log state
  GlobalStateLock log_state;
  cpp17::optional<int8_t> raw_severity;
  // Optional so no allocation overhead
  // occurs if condition isn't set.
  std::optional<std::string> modified_msg;
  if (condition) {
    std::stringstream s;
    s << "Check failed: " << condition << ". ";
    if (msg) {
      s << msg;
    }
    modified_msg = s.str();
    if (severity == syslog::LOG_FATAL) {
      // We're crashing -- so leak the string in order to prevent
      // use-after-free of the maybe_fatal_string.
      // We need this to prevent a use-after-free in FlushRecord.
      msg = new char[modified_msg->size() + 1];
      strcpy(const_cast<char*>(msg), modified_msg->c_str());
    } else {
      msg = modified_msg->data();
    }
  }
  // Validate that the severity matches the FIDL definition in
  // sdk/fidl/fuchsia.diagnostics/severity.fidl.
  if ((severity % 0x10) || (severity > 0x60) || (severity < 0x10)) {
    raw_severity = severity;
    severity = syslog::LOG_DEBUG;
  }
  zx_time_t time = zx_clock_get_monotonic();
  auto* state = RecordState::CreatePtr(buffer);
  RecordState& record = *state;
  // Invoke the constructor of RecordState to construct a valid RecordState
  // inside the LogBuffer.
  new (state) RecordState;
  state->socket = socket;
  if (severity == syslog::LOG_FATAL) {
    state->maybe_fatal_string = msg;
  }
  record.is_legacy_backend = socket != ZX_HANDLE_INVALID;
  state->raw_severity = raw_severity.value_or(severity);
  state->log_severity = severity;
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.Begin(*state, time, ::fuchsia::diagnostics::Severity(severity));
  if (is_printf) {
    encoder.AppendArgumentKey(record, SliceFromArray(kPrintfFieldName));
    encoder.AppendArgumentValue(record, static_cast<uint64_t>(0));
  }
  encoder.AppendArgumentKey(record, SliceFromArray(kPidFieldName));
  encoder.AppendArgumentValue(record, static_cast<uint64_t>(pid));
  encoder.AppendArgumentKey(record, SliceFromArray(kTidFieldName));
  encoder.AppendArgumentValue(record, static_cast<uint64_t>(tid));

  auto dropped_count = GetAndResetDropped();
  record.dropped_count = dropped_count;
  if (raw_severity) {
    encoder.AppendArgumentKey(record, SliceFromString(kVerbosityFieldName));
    encoder.AppendArgumentValue(record, static_cast<int64_t>(raw_severity.value()));
  }
  if (dropped_count) {
    encoder.AppendArgumentKey(record, SliceFromString(kDroppedLogsFieldName));
    encoder.AppendArgumentValue(record, static_cast<uint64_t>(dropped_count));
  }
  for (size_t i = 0; i < log_state->tag_count(); i++) {
    encoder.AppendArgumentKey(record, SliceFromString(kTagFieldName));
    encoder.AppendArgumentValue(record, SliceFromString(log_state->tags()[i]));
  }
  if (msg) {
    encoder.AppendArgumentKey(record, SliceFromString(kMessageFieldName));
    encoder.AppendArgumentValue(record, SliceFromString(msg));
  }
  if (file_name) {
    encoder.AppendArgumentKey(record, SliceFromString(kFileFieldName));
    encoder.AppendArgumentValue(record, SliceFromString(StripDots(file_name).data()));
  }
  encoder.AppendArgumentKey(record, SliceFromString(kLineFieldName));
  encoder.AppendArgumentValue(record, static_cast<uint64_t>(line));
}

void BeginRecordPrintf(LogBuffer* buffer, syslog::LogSeverity severity, const char* file_name,
                       unsigned int line, const char* msg) {
  BeginRecordInternal(buffer, severity, file_name, line, msg, nullptr, true /* is_printf */,
                      ZX_HANDLE_INVALID);
}

void BeginRecord(LogBuffer* buffer, syslog::LogSeverity severity, const char* file_name,
                 unsigned int line, const char* msg, const char* condition) {
  BeginRecordInternal(buffer, severity, file_name, line, msg, condition, false /* is_printf */,
                      ZX_HANDLE_INVALID);
}

void BeginRecordWithSocket(LogBuffer* buffer, syslog::LogSeverity severity, const char* file_name,
                           unsigned int line, const char* msg, const char* condition,
                           zx_handle_t socket) {
  BeginRecordInternal(buffer, severity, file_name, line, msg, condition, false /* is_printf */,
                      socket);
}

void WriteKeyValue(LogBuffer* buffer, const char* key, const char* value) {
  WriteKeyValue(buffer, key, value, strlen(value));
}

void WriteKeyValue(LogBuffer* buffer, const char* key, const char* value, size_t value_length) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.AppendArgumentKey(
      *state, DataSlice<const char>(
                  key, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(strlen(key)))));
  encoder.AppendArgumentValue(
      *state, DataSlice<const char>(value, WordOffset<const char>::FromByteOffset(
                                               ByteOffset::Unbounded(value_length))));
}

void WriteKeyValue(LogBuffer* buffer, const char* key, int64_t value) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.AppendArgumentKey(
      *state, DataSlice<const char>(
                  key, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(strlen(key)))));
  encoder.AppendArgumentValue(*state, value);
}

void WriteKeyValue(LogBuffer* buffer, const char* key, uint64_t value) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.AppendArgumentKey(
      *state, DataSlice<const char>(
                  key, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(strlen(key)))));
  encoder.AppendArgumentValue(*state, value);
}

void WriteKeyValue(LogBuffer* buffer, const char* key, double value) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.AppendArgumentKey(
      *state, DataSlice<const char>(
                  key, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(strlen(key)))));
  encoder.AppendArgumentValue(*state, value);
}

void WriteKeyValue(LogBuffer* buffer, const char* key, bool value) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.AppendArgumentKey(
      *state, DataSlice<const char>(
                  key, WordOffset<const char>::FromByteOffset(ByteOffset::Unbounded(strlen(key)))));
  encoder.AppendArgumentValue(*state, value);
}

void EndRecord(LogBuffer* buffer) {
  auto* state = RecordState::CreatePtr(buffer);
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  encoder.End(*state);
}

bool FlushRecord(LogBuffer* buffer) {
  GlobalStateLock log_state;
  auto* state = RecordState::CreatePtr(buffer);
  if (!state->encode_success) {
    return false;
  }
  ExternalDataBuffer external_buffer(buffer);
  Encoder<ExternalDataBuffer> encoder(external_buffer);
  auto slice = external_buffer.GetSlice();
  if ((state->log_severity < log_state->min_severity()) && !state->is_legacy_backend) {
    if (state->log_severity != state->raw_severity) {
      // Assume we're supposed to generate a log here.
      // For custom severities, macros.h is supposed to filter for us.
    } else {
      return true;
    }
  }
  auto socket = state->socket == ZX_HANDLE_INVALID
                    ? cpp17::get<zx::socket>(log_state->descriptor()).get()
                    : state->socket;
  auto status =
      zx_socket_write(socket, 0, slice.data(), slice.slice().ToByteOffset().unsafe_get(), nullptr);
  if (status != ZX_OK) {
    AddDropped(state->dropped_count + 1);
  }
  if (state->log_severity == syslog::LOG_FATAL) {
    std::cerr << state->maybe_fatal_string << std::endl;
    abort();
  }
  return status != ZX_ERR_BAD_STATE && status != ZX_ERR_PEER_CLOSED &&
         state->log_severity != syslog::LOG_FATAL;
}

bool LogState::WriteLogToFile(std::ofstream* file_ptr, zx_time_t time, zx_koid_t pid, zx_koid_t tid,
                              syslog::LogSeverity severity, const char* file_name,
                              unsigned int line, const char* tag, const char* condition,
                              const std::string& msg) const {
  auto& file = *file_ptr;
  file << "[" << std::setw(5) << std::setfill('0') << time / 1000000000UL << "." << std::setw(6)
       << (time / 1000UL) % 1000000UL << std::setw(0) << "][" << pid << "][" << tid << "][";

  auto& tag_str = tag_str_;
  file << tag_str;

  if (tag) {
    if (!tag_str.empty()) {
      file << ", ";
    }

    file << tag;
  }

  file << "] ";

  switch (severity) {
    case syslog::LOG_TRACE:
      file << "TRACE";
      break;
    case syslog::LOG_DEBUG:
      file << "DEBUG";
      break;
    case syslog::LOG_INFO:
      file << "INFO";
      break;
    case syslog::LOG_WARNING:
      file << "WARNING";
      break;
    case syslog::LOG_ERROR:
      file << "ERROR";
      break;
    case syslog::LOG_FATAL:
      file << "FATAL";
      break;
    default:
      file << "VLOG(" << (syslog::LOG_INFO - severity) << ")";
  }

  file << ": [" << file_name << "(" << line << ")] " << msg << std::endl;

  return severity != syslog::LOG_FATAL;
}

void LogState::Set(const syslog::LogSettings& settings, const GlobalStateLock& lock) {
  Set(settings, {}, lock);
}

void LogState::Set(const syslog::LogSettings& settings,
                   const std::initializer_list<std::string>& tags, const GlobalStateLock& lock) {
  auto old = *lock;
  lock.Set(new LogState(settings, tags));
  if (old) {
    delete old;
  }
}

LogState::LogState(const syslog::LogSettings& in_settings,
                   const std::initializer_list<std::string>& tags)
    : loop_(&kAsyncLoopConfigNeverAttachToThread),
      executor_(loop_.dispatcher()),
      min_severity_(in_settings.min_log_level),
      default_severity_(in_settings.min_log_level),
      wait_for_initial_interest_(in_settings.wait_for_initial_interest) {
  syslog::LogSettings settings = in_settings;
  interest_listener_dispatcher_ =
      static_cast<async_dispatcher_t*>(settings.single_threaded_dispatcher);
  serve_interest_listener_ = !settings.disable_interest_listener;
  min_severity_ = in_settings.min_log_level;

  std::ostringstream tag_str;

  for (auto& tag : tags) {
    if (num_tags_) {
      tag_str << ", ";
    }
    tag_str << tag;
    tags_[num_tags_++] = tag;
    if (num_tags_ >= kMaxTags)
      break;
  }

  tag_str_ = tag_str.str();
  provided_log_sink_ = in_settings.log_sink;
  Connect();
}

void SetLogSettings(const syslog::LogSettings& settings) {
  GlobalStateLock lock;
  LogState::Set(settings, lock);
}

void SetLogSettings(const syslog::LogSettings& settings,
                    const std::initializer_list<std::string>& tags) {
  GlobalStateLock lock;
  LogState::Set(settings, tags, lock);
}

syslog::LogSeverity GetMinLogLevel() {
  GlobalStateLock lock;
  return lock->min_severity();
}

}  // namespace syslog_backend
