// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_TESTS_DEVICE_HOST_H_
#define SRC_MEDIA_AUDIO_DRIVERS_TESTS_DEVICE_HOST_H_

#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>

#include <atomic>
#include <optional>

#include "src/lib/fsl/io/device_watcher.h"
#include "src/media/audio/drivers/tests/test_base.h"

namespace media::audio::drivers::test {

class DeviceHost {
 public:
  DeviceHost();
  ~DeviceHost();

  // Post a task to our thread to detect and add all devices, so that driver testing can begin.
  void AddDevices(bool devfs_only, bool no_virtual_audio);

  // Create testcase instances for each device entry, based on the passed-in configuration.
  void RegisterTests(bool expect_audio_core_not_connected, bool enable_position_tests);

  // Testing is complete. Clean up our virtual audio devices and shut down our loop.
  zx_status_t QuitDeviceLoop();

 private:
  // Detect devfs-based audio devices, optionally adding device entries for a2dp and virtual_audio.
  void DetectDevices(bool devfs_only, bool no_virtual_audio);

  // Optionally called during DetectDevices. Create virtual_audio instances for each device type
  // using the default configurations settings (which should always pass all tests).
  void AddVirtualDevices();
  void AddVirtualDevice(fuchsia::virtualaudio::DeviceType device_type,
                        fuchsia::virtualaudio::DevicePtr& device_ptr,
                        std::optional<bool> is_input = std::nullopt);

  std::set<DeviceEntry>& device_entries() { return device_entries_; }
  std::vector<std::unique_ptr<fsl::DeviceWatcher>>& device_watchers() { return device_watchers_; }

  async::Loop device_loop_;
  std::set<DeviceEntry> device_entries_;
  std::vector<std::unique_ptr<fsl::DeviceWatcher>> device_watchers_;

  // While the test suite is running, we spawn a number of virtual_audio driver instances.
  static constexpr size_t kNumVirtualAudioDevicesToAdd = 5;
  fuchsia::virtualaudio::ControlSyncPtr controller_ = nullptr;
  fuchsia::virtualaudio::DevicePtr composite_ = nullptr;
  fuchsia::virtualaudio::DevicePtr dai_input_ = nullptr;
  fuchsia::virtualaudio::DevicePtr dai_output_ = nullptr;
  fuchsia::virtualaudio::DevicePtr stream_config_input_ = nullptr;
  fuchsia::virtualaudio::DevicePtr stream_config_output_ = nullptr;

  bool shutting_down_ = false;
  std::atomic_bool device_enumeration_complete_ = false;
};

}  // namespace media::audio::drivers::test

#endif  // SRC_MEDIA_AUDIO_DRIVERS_TESTS_DEVICE_HOST_H_
