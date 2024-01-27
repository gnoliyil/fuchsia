// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/component/incoming/cpp/service_client.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>

#include "adb-shell.h"

int main(int argc, char** argv) {
  syslog::SetTags({"adb"});

  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);

  zx::result svc = component::OpenServiceRoot();
  if (svc.is_error()) {
    FX_LOGS(ERROR) << "Failed to connect to service root: " << svc.status_string();
    return -1;
  }

  auto config = adb_shell_config::Config::TakeFromStartupHandle();

  auto outgoing = component::OutgoingDirectory(loop.dispatcher());

  zx::result result = outgoing.ServeFromStartupInfo();
  if (result.is_error()) {
    FX_LOGS(ERROR) << "Failed to serve outgoing directory: " << result.status_string();
    return -1;
  }

  auto adb_shell =
      std::make_unique<adb_shell::AdbShell>(std::move(*svc), loop.dispatcher(), config);
  result = outgoing.AddProtocol<fuchsia_hardware_adb::Provider>(std::move(adb_shell));
  if (result.is_error()) {
    FX_LOGS(ERROR) << "Failed to serve outgoing directory: " << result.status_string();
    return -1;
  }

  return loop.Run();
}
