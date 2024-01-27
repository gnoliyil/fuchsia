// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_SYSLOG_FX_LOGGER_H_
#define ZIRCON_SYSTEM_ULIB_SYSLOG_FX_LOGGER_H_

#include <lib/syslog/logger.h>
#include <lib/zx/process.h>
#include <lib/zx/socket.h>
#include <lib/zx/thread.h>
#include <zircon/status.h>

#include <atomic>

#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>

struct fx_logger {
 public:
  // If tags or ntags are out of bound, this constructor will not fail but it
  // will not store all the tags and global tag behaviour would be undefined.
  // So they should be validated before calling this constructor.
  explicit fx_logger(const fx_logger_config_t* config, bool structured) {
    pid_ = []() {
      zx_info_handle_basic_t info;
      zx_status_t status = zx_object_get_info(zx_process_self(), ZX_INFO_HANDLE_BASIC, &info,
                                              sizeof(info), nullptr, nullptr);
      ZX_DEBUG_ASSERT_MSG(status == ZX_OK, "zx_object_get_info(zx_process_self(), ...): %s",
                          zx_status_get_string(status));
      return info.koid;
    }();
    dropped_logs_.store(0, std::memory_order_relaxed);
    Reconfigure(config, structured);
    if (GetLogConnectionStatus() == ZX_ERR_BAD_STATE) {
      ActivateFallback(-1);
    }
  }

  ~fx_logger() = default;

  template <typename Callback>
  void GetTags(const Callback& callback) {
    fbl::AutoLock tag_lock(&tags_mutex_);
    for (auto tag : tags_) {
      callback(tag);
    }
  }

  zx_status_t VLogWrite(fx_log_severity_t severity, const char* tag, const char* format,
                        va_list args, const char* file = nullptr, uint32_t line = 0) {
    return VLogWrite(severity, tag, file, line, format, args, true);
  }

  zx_status_t LogWrite(fx_log_severity_t severity, const char* tag, const char* msg,
                       const char* file = nullptr, uint32_t line = 0) {
    return VLogWrite(severity, tag, file, line, msg, va_list{}, false);
  }

  zx_status_t SetSeverity(fx_log_severity_t log_severity) {
    if (log_severity > FX_LOG_FATAL) {
      return ZX_ERR_INVALID_ARGS;
    }
    severity_.store(log_severity, std::memory_order_relaxed);
    return ZX_OK;
  }

  fx_log_severity_t GetSeverity() { return severity_.load(std::memory_order_relaxed); }

  void ActivateFallback(int fallback_fd);

  zx_status_t Reconfigure(const fx_logger_config_t* config, bool is_structured);

  zx_status_t GetLogConnectionStatus();

  // Set the log connection for this logger using a handle which
  // is presumed to come from a socket connection with the logging
  // service.
  void SetLogConnection(zx_handle_t handle);

 private:
  zx_status_t VLogWrite(fx_log_severity_t severity, const char* tag, const char* file,
                        uint32_t line, const char* msg, va_list args, bool perform_format);

  zx_status_t VLogWriteToSocket(fx_log_severity_t severity, const char* tag, const char* file,
                                uint32_t line, const char* msg, va_list args, bool perform_format);

  zx_status_t VLogWriteToFd(int fd, fx_log_severity_t severity, const char* tag, const char* file,
                            uint32_t line, const char* msg, va_list args, bool perform_format);

  zx_status_t SetTags(const char* const* tags, size_t ntags);

  zx_koid_t pid_;
  std::atomic<fx_log_severity_t> severity_;
  std::atomic<uint32_t> dropped_logs_;
  std::atomic<int> logger_fd_ = -1;
  zx::socket socket_;
  fbl::Vector<fbl::String> tags_ __TA_GUARDED(tags_mutex_);

  // This field is just used to close fd when
  // logger object goes out of scope
  fbl::unique_fd fd_to_close_;

  // string representation to print in fallback mode
  fbl::String tagstr_;

  // Mutex protecting tags
  fbl::Mutex tags_mutex_;

  fbl::Mutex logger_mutex_;

  // True if structured logging was requested by the user,
  // false otherwise. Note that this may be false,
  // and we could still be using structured logs
  // like in the case of drivers.
  bool is_structured_ = false;
};

#endif  // ZIRCON_SYSTEM_ULIB_SYSLOG_FX_LOGGER_H_
