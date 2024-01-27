// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVICES_BIN_DRIVER_HOST_DEFAULTS_H_
#define SRC_DEVICES_BIN_DRIVER_HOST_DEFAULTS_H_

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>

namespace internal {

extern const device_power_state_info_t kDeviceDefaultPowerStates[2];
extern const device_performance_state_info_t kDeviceDefaultPerfStates[1];
extern const std::array<fuchsia_device::wire::SystemPowerStateInfo,
                        fuchsia_device_manager::wire::kMaxSystemPowerStates>
    kDeviceDefaultStateMapping;

}  // namespace internal

#endif  // SRC_DEVICES_BIN_DRIVER_HOST_DEFAULTS_H_
