// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/common/host_util.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>

#include <mach-o/dyld.h>
#endif

namespace zxdb {

std::string GetSelfPath() {
  std::string result;
#if defined(__APPLE__)
  // Executable path can have relative references ("..") depending on how the
  // app was launched.
  uint32_t length = 0;
  _NSGetExecutablePath(nullptr, &length);
  result.resize(length);
  _NSGetExecutablePath(result.data(), &length);
  result.resize(length - 1);  // Length included terminator.
#elif defined(__linux__)
  // The realpath() call below will resolve the symbolic link.
  result.assign("/proc/self/exe");
#else
#error Write this for your platform.
#endif

  char fullpath[PATH_MAX];
  return std::string(realpath(result.c_str(), fullpath));
}

char** GetEnviron() {
#if defined(__APPLE__)
  return *_NSGetEnviron();
#elif defined(__linux__)
  return environ;
#else
#error Write this for your platform.
#endif
}

}  // namespace zxdb
