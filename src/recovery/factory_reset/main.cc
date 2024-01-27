// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <iostream>

#include "src/recovery/factory_reset/factory_reset.h"

int main(int argc, const char** argv) {
  zx::result dev = component::Connect<fuchsia_io::Directory>("/dev");
  if (dev.is_error()) {
    std::cerr << "failed to open '/dev': " << dev.status_string();
    return -1;
  }

  zx::result admin = component::Connect<fuchsia_hardware_power_statecontrol::Admin>();
  if (admin.is_error()) {
    std::cerr << "failed to connect to fuchsia.hardware.power.statecontrol/Admin: "
              << admin.status_string();
    return -1;
  }

  zx::result fshost = component::Connect<fuchsia_fshost::Admin>();
  if (fshost.is_error()) {
    std::cerr << "failed to connect to fuchsia.fshost/Admin: " << fshost.status_string();
    return -1;
  }

  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  component::OutgoingDirectory outgoing(loop.dispatcher());

  factory_reset::FactoryReset factory_reset(loop.dispatcher(), std::move(dev).value(),
                                            std::move(admin.value()), std::move(fshost.value()));
  fidl::ServerBindingGroup<fuchsia_recovery::FactoryReset> bindings;
  if (zx::result<> result = outgoing.AddUnmanagedProtocol<fuchsia_recovery::FactoryReset>(
          bindings.CreateHandler(&factory_reset, loop.dispatcher(), fidl::kIgnoreBindingClosure));
      result.is_error()) {
    std::cerr << "failed to expose fuchsia.recovery/FactoryReset: " << result.status_string();
    return -1;
  }

  if (zx_status_t status = loop.Run(); status != ZX_OK) {
    std::cerr << "failed to run async loop: " << zx_status_get_string(status);
    return -1;
  }
  return 0;
}
