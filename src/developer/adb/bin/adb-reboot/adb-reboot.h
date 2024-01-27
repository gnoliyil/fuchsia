// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_ADB_BIN_ADB_REBOOT_ADB_REBOOT_H_
#define SRC_DEVELOPER_ADB_BIN_ADB_REBOOT_ADB_REBOOT_H_

#include <fidl/fuchsia.hardware.adb/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <zircon/types.h>

#include <optional>
#include <string>

namespace adb_reboot {

// Provides reboot service to adb daemon.
class AdbReboot : public fidl::WireServer<fuchsia_hardware_adb::Provider> {
 public:
  explicit AdbReboot(fidl::ClientEnd<fuchsia_io::Directory> svc, async_dispatcher_t* dispatcher)
      : svc_(std::move(svc)), dispatcher_(dispatcher) {}

  // fuchsia_hardware_adb::Provider method.
  void ConnectToService(ConnectToServiceRequestView request,
                        ConnectToServiceCompleter::Sync& completer) override;

 private:
  // Helper methods.
  zx_status_t Reboot(std::optional<std::string> args);

  // Post reboot task onto the scheduler so that we can reply to the adb client before shutting
  // down.
  template <typename RebootFunction>
  zx_status_t ScheduleReboot(RebootFunction f);

  fidl::ClientEnd<fuchsia_io::Directory> svc_;
  async_dispatcher_t* dispatcher_;
};

}  // namespace adb_reboot

#endif  // SRC_DEVELOPER_ADB_BIN_ADB_REBOOT_ADB_REBOOT_H_
