// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_X86_INCLUDE_ACPI_H_
#define SRC_DEVICES_BOARD_DRIVERS_X86_INCLUDE_ACPI_H_

#include <lib/ddk/device.h>
#include <zircon/compiler.h>

#include "src/devices/board/lib/acpi/acpi-impl.h"
#include "src/devices/board/lib/acpi/acpi.h"
#include "src/devices/board/lib/acpi/manager.h"
#include "x86.h"

zx_status_t publish_acpi_devices(acpi::Manager* manager, zx_device_t* parent,
                                 zx_device_t* acpi_root);
zx_status_t acpi_suspend(zx_device_t* device, uint8_t requested_state, bool enable_wake,
                         uint8_t suspend_reason, uint8_t* out_state);

#endif  // SRC_DEVICES_BOARD_DRIVERS_X86_INCLUDE_ACPI_H_
