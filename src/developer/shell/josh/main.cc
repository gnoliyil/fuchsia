// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>

#include "src/developer/shell/josh/console/console.h"

int main(int argc, char* argv[]) {
  fuchsia_logging::SetTags({"josh"});

  return shell::ConsoleMain(argc, const_cast<const char**>(argv));
}
