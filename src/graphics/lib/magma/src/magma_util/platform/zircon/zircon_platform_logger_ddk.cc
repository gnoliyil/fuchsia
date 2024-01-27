// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/debug.h>
#include <stdarg.h>
#include <stdio.h>

#include "platform_logger.h"

namespace magma {

void PlatformLogger::LogVa(LogLevel level, const char* file, int line, const char* fmt,
                           va_list args) {
  // TODO: Propogate file and line via caller.
  switch (level) {
    case PlatformLogger::LOG_ERROR:
      zxlogvf(ERROR, file, line, fmt, args);
      return;
    case PlatformLogger::LOG_WARNING:
      zxlogvf(WARNING, file, line, fmt, args);
      return;
    case PlatformLogger::LOG_INFO:
      zxlogvf(INFO, file, line, fmt, args);
      return;
  }
}

}  // namespace magma
