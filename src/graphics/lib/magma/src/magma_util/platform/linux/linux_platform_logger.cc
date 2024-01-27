// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdarg.h>
#include <stdio.h>

#include "platform_logger.h"
#include "platform_logger_provider.h"

namespace magma {

bool PlatformLoggerProvider::IsInitialized() { return true; }

bool PlatformLoggerProvider::Initialize(std::unique_ptr<PlatformHandle> handle) { return true; }

static void print_level(PlatformLogger::LogLevel level) {
  switch (level) {
    case PlatformLogger::LOG_ERROR:
      printf("[ERROR] ");
      break;
    case PlatformLogger::LOG_WARNING:
      printf("[WARNING] ");
      break;
    case PlatformLogger::LOG_INFO:
      printf("[INFO] ");
      break;
  }
}

void PlatformLogger::LogVa(LogLevel level, const char* file, int line, const char* msg,
                           va_list args) {
  print_level(level);
  printf("%s:%d ", file, line);
  vprintf(msg, args);
  printf("\n");
}

}  // namespace magma
