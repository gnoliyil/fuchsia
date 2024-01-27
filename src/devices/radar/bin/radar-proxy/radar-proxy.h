// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_RADAR_BIN_RADAR_PROXY_RADAR_PROXY_H_
#define SRC_DEVICES_RADAR_BIN_RADAR_PROXY_RADAR_PROXY_H_

#include <fidl/fuchsia.hardware.radar/cpp/fidl.h>
#include <lib/fit/function.h>

#include <filesystem>
#include <string>

#include "src/lib/fsl/io/device_watcher.h"

namespace radar {

class RadarDeviceConnector {
 public:
  // Called on zero or more eligible radar devices. If true is returned, the device is usable and
  // the callback should not be invoked again. Otherwise, the device is not usable, and the search
  // for a suitable device should continue.
  using ConnectDeviceCallback =
      fit::function<bool(fidl::ClientEnd<fuchsia_hardware_radar::RadarBurstReaderProvider>)>;

  // Synchronously connects to the given radar device and passes the client end to the callback. The
  // callback is not called if the connection could not be made. The callback return value is
  // ignored.
  virtual void ConnectToRadarDevice(int dir_fd, const std::filesystem::path& path,
                                    ConnectDeviceCallback connect_device) = 0;

  // Calls ConnectToRadarDevice() on available devices until the callback returns true.
  virtual void ConnectToFirstRadarDevice(ConnectDeviceCallback connect_device) = 0;
};

class RadarProxy : public fidl::Server<fuchsia_hardware_radar::RadarBurstReaderProvider> {
 public:
  static constexpr char kRadarDeviceDirectory[] = "/dev/class/radar";

  RadarProxy()
      : device_watcher_(fsl::DeviceWatcher::Create(
            kRadarDeviceDirectory,
            [&](int dir_fd, const std::string& filename) { DeviceAdded(dir_fd, filename); })) {}

  // Called by a DeviceWatcher when /dev/class/radar has a new device, and for each existing device
  // during construction.
  virtual void DeviceAdded(int dir_fd, const std::string& filename) = 0;

 private:
  std::unique_ptr<fsl::DeviceWatcher> device_watcher_;
};

}  // namespace radar

#endif  // SRC_DEVICES_RADAR_BIN_RADAR_PROXY_RADAR_PROXY_H_
