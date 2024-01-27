// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.nand/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zircon/types.h>

#include <utility>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramnand.h>

namespace ramdevice_client {

__EXPORT
zx_status_t RamNand::Create(fuchsia_hardware_nand::wire::RamNandInfo config,
                            std::optional<RamNand>* out) {
  zx::result ctl = component::Connect<fuchsia_hardware_nand::RamNandCtl>(kBasePath);
  if (ctl.is_error()) {
    fprintf(stderr, "could not connect to RamNandCtl: %s\n", ctl.status_string());
    return ctl.status_value();
  }

  const fidl::WireResult result = fidl::WireCall(ctl.value())->CreateDevice(std::move(config));
  if (!result.ok()) {
    fprintf(stderr, "could not create ram_nand device: %s\n", result.status_string());
    return result.status();
  }
  const fidl::WireResponse response = result.value();
  if (zx_status_t status = response.status; status != ZX_OK) {
    fprintf(stderr, "could not create ram_nand device: %s\n", zx_status_get_string(status));
    return status;
  }
  const std::string name(response.name.get());

  fbl::unique_fd ram_nand_ctl(open(kBasePath, O_RDONLY | O_DIRECTORY));
  if (!ram_nand_ctl) {
    fprintf(stderr, "Could not open ram_nand_ctl: %s\n", strerror(errno));
    return ZX_ERR_INTERNAL;
  }

  zx::result channel = device_watcher::RecursiveWaitForFile(ram_nand_ctl.get(), name.c_str());
  if (channel.is_error()) {
    fprintf(stderr, "could not open ram_nand at '%s': %s\n", name.c_str(), channel.status_string());
    return channel.error_value();
  }

  *out = RamNand(fidl::ClientEnd<fuchsia_device::Controller>(std::move(channel.value())),
                 fbl::String::Concat({kBasePath, "/", name}), name);

  return ZX_OK;
}

__EXPORT
RamNand::~RamNand() {
  if (unbind) {
    const fidl::WireResult result = fidl::WireCall(controller_)->ScheduleUnbind();
    if (!result.ok()) {
      fprintf(stderr, "Could not unbind ram_nand: %s\n", result.FormatDescription().c_str());
      return;
    }
    const fit::result response = result.value();
    if (response.is_error()) {
      fprintf(stderr, "Could not unbind ram_nand: %s\n",
              zx_status_get_string(response.error_value()));
    }
  }
}

}  // namespace ramdevice_client
