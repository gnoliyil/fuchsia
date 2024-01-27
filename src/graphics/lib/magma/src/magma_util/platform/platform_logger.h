// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_LOGGER_H
#define PLATFORM_LOGGER_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAGMA_LOG(level, ...) \
  magma::PlatformLogger::Log(magma::PlatformLogger::LOG_##level, __FILE__, __LINE__, __VA_ARGS__)

namespace magma {

class PlatformLogger {
 public:
  enum LogLevel { LOG_ERROR, LOG_WARNING, LOG_INFO };

  __attribute__((format(printf, 4, 5))) static void Log(PlatformLogger::LogLevel level,
                                                        const char* file, int line, const char* msg,
                                                        ...) {
    va_list args;
    va_start(args, msg);
    LogVa(level, file, line, msg, args);
    va_end(args);
  }

  static void LogVa(PlatformLogger::LogLevel level, const char* file, int line, const char* msg,
                    va_list args);
};

}  // namespace magma

#endif  // PLATFORM_LOGGER_H
