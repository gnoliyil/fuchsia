// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/drivers/test/device_host.h"

#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/sync/cpp/completion.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <zircon/system/public/zircon/compiler.h>

#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "src/lib/fsl/io/device_watcher.h"
#include "src/lib/fxl/strings/concatenate.h"
#include "src/media/audio/drivers/test/test_base.h"

namespace media::audio::drivers::test {

// TODO(fxbug.dev/65580): Previous implementation used value-parameterized testing. Consider
// reverting to this, moving AddDevices to a function called at static initialization time. If we
// cannot access cmdline flags at that time, this would force us to always register admin tests,
// skipping them at runtime based on the cmdline flag.

extern void RegisterBasicTestsForDevice(const DeviceEntry& device_entry);
extern void RegisterAdminTestsForDevice(const DeviceEntry& device_entry,
                                        bool expect_audio_core_not_connected);
extern void RegisterPositionTestsForDevice(const DeviceEntry& device_entry,
                                           bool expect_audio_core_not_connected,
                                           bool enable_position_tests);

static const struct {
  const char* path;
  DriverType driver_type;
} kAudioDevNodes[] = {
    {.path = "/dev/class/audio-input", .driver_type = DriverType::StreamConfigInput},
    {.path = "/dev/class/audio-output", .driver_type = DriverType::StreamConfigOutput},
    {.path = "/dev/class/dai", .driver_type = DriverType::Dai},
    {.path = "/dev/class/codec", .driver_type = DriverType::Codec},
    {.path = "/dev/class/audio-composite", .driver_type = DriverType::Composite},
};

// Our thread and dispatcher must exist during the entirety of test execution; create it now.
DeviceHost::DeviceHost() : device_loop_(async::Loop(&kAsyncLoopConfigNeverAttachToThread)) {
  device_loop_.StartThread("AddVadAndDetectDevices");
}
DeviceHost::~DeviceHost() { QuitDeviceLoop(); }

// Post a task to our thread to detect and add all devices, so that testing can begin.
void DeviceHost::AddDevices(bool devfs_only, bool no_virtual_audio) {
  libsync::Completion done;
  async::PostTask(device_loop_.dispatcher(), [this, &done, devfs_only, no_virtual_audio]() {
    DetectDevices(devfs_only, no_virtual_audio);
    done.Signal();
  });
  // If we hang indefinitely here, the test execution environment will eventually timeout.
  done.Wait();
}

// Set up DeviceWatchers to detect audio devices.
//
// First, detect audio devices that were already in devfs when we started the detection process.
//
// Following this, (optionally) add any virtual_audio devices and rely on our previously-installed
// device watchers to detect them. (NOTE: subsequent device arrivals/departures that happen outside
// the control of this suite are treated as immediate failures.)
//
// We then (optionally) add an instance of the Bluetooth audio device library.
//
// Note that with the current design, we must keep the DeviceWatchers alive so that each device's
// fuchsia_io::Directory is not dropped. This means that the DeviceWatcher callback might run after
// this method exits. This requires us to use class member `device_enumeration_complete_` to signal
// that these subsequent device-detection callbacks should trigger immediate failures instead of
// treating this like another device to be tested.
void DeviceHost::DetectDevices(bool devfs_only, bool no_virtual_audio) {
  // This is guarded by `device_enumeration_complete_` which we set before we exit, but we give this
  // variable static scope to avoid future issues.
  static DeviceType dev_type = DeviceType::BuiltIn;

  // Ensure that an initial devfs enumeration pass completes before creating the next watcher.
  // This is only accessed by the idle_callback, which we explicitly await before we exit. Just in
  // case the callback can subsequently run for some reason, we give this variable static scope.
  static bool initial_enumeration_done;

  // Set up the device watchers. If any fail, automatically stop monitoring all device sources.
  // First, we add any preexisting ("built-in") devices.
  for (const auto& devnode : kAudioDevNodes) {
    initial_enumeration_done = false;
    auto watcher = fsl::DeviceWatcher::CreateWithIdleCallback(
        devnode.path,
        [this, driver_type = devnode.driver_type](const fidl::ClientEnd<fuchsia_io::Directory>& dir,
                                                  const std::string& filename) {
          ASSERT_FALSE(device_enumeration_complete_)
              << "Unexpected audio device detection occurred after test suite configuration";

          FX_LOGS(TRACE) << "dir handle " << dir.channel().get() << " for '" << filename << "' ("
                         << dev_type << " " << driver_type << ")";
          device_entries().insert({dir, filename, driver_type, dev_type});
        },
        []() { initial_enumeration_done = true; }, device_loop_.dispatcher());

    if (watcher == nullptr) {
      ASSERT_FALSE(watcher == nullptr)
          << "AudioDriver::TestBase failed creating DeviceWatcher for '" << devnode.path << "'.";
    }

    // If we hang indefinitely here, the test execution environment will eventually timeout.
    while (!initial_enumeration_done) {
      device_loop_.RunUntilIdle();
    }
    ASSERT_TRUE(initial_enumeration_done)
        << "DeviceWatcher did not finish initial enumeration, for " << dev_type << "/"
        << devnode.driver_type;

    // We must save this so each device's fidl::ClientEnd<fuchsia_io::Directory is not dropped.
    device_watchers().emplace_back(std::move(watcher));
  }

  // Then, if enabled, enable virtual_audio instances and wait for their detection.
  // By reusing the watchers we've already configured, we detect each preexisting device only once.
  if (!no_virtual_audio) {
    auto device_count = device_entries().size();
    dev_type = DeviceType::Virtual;
    AddVirtualDevices();

    // If we hang indefinitely here, the test execution environment will eventually timeout.
    while (device_entries().size() < device_count + kNumVirtualAudioDevicesToAdd) {
      device_loop_.RunUntilIdle();
    }
    ASSERT_GE(device_entries().size(), device_count + kNumVirtualAudioDevicesToAdd)
        << "DeviceWatcher timed out, for " << dev_type << " devices";
  }

  // If any subsequent device detections occur, we consider these errors.
  device_enumeration_complete_ = true;

  // And finally, unless expressly excluded, manually add a device entry for the Bluetooth audio
  // library, to validate admin functions even if AudioCore has connected to "real" audio drivers.
  if (!devfs_only) {
    device_entries().insert({{}, "A2DP", DriverType::StreamConfigOutput, DeviceType::A2DP});
  }
}

// Optionally called during DetectDevices. Create virtual_audio instances (StreamConfig input and
// output, Dai input and output, and Composite) using the default configuration settings (which
// should pass all tests).
void DeviceHost::AddVirtualDevices() {
  const std::string kControlNodePath =
      fxl::Concatenate({"/dev/", fuchsia::virtualaudio::CONTROL_NODE_NAME});
  zx_status_t status = fdio_service_connect(kControlNodePath.c_str(),
                                            controller_.NewRequest().TakeChannel().release());
  ASSERT_EQ(status, ZX_OK) << "fdio_service_connect failed";

  uint32_t num_inputs = -1, num_outputs = -1, num_unspecified_direction = -1;
  status = controller_->GetNumDevices(&num_inputs, &num_outputs, &num_unspecified_direction);
  ASSERT_EQ(status, ZX_OK) << "GetNumDevices failed";
  ASSERT_TRUE(controller_.is_bound()) << "virtualaudio::Control did not stay bound";
  ASSERT_EQ(num_inputs, 0u) << num_inputs << " virtual_audio inputs already exist (should be 0)";
  ASSERT_EQ(num_outputs, 0u) << num_outputs << " virtual_audio outputs already exist (should be 0)";
  ASSERT_EQ(num_unspecified_direction, 0u)
      << num_outputs
      << " virtual_audio devices with unspecified direction already exist (should be 0)";

  AddVirtualDevice(true, fuchsia::virtualaudio::DeviceType::STREAM_CONFIG, stream_config_input_);
  AddVirtualDevice(false, fuchsia::virtualaudio::DeviceType::STREAM_CONFIG, stream_config_output_);
  AddVirtualDevice(true, fuchsia::virtualaudio::DeviceType::DAI, dai_input_);
  AddVirtualDevice(false, fuchsia::virtualaudio::DeviceType::DAI, dai_output_);
  // No direction support in composite devices, is_input = true is unused.
  AddVirtualDevice(true, fuchsia::virtualaudio::DeviceType::COMPOSITE, composite_);
}

void DeviceHost::AddVirtualDevice(bool is_input,
                                  const fuchsia::virtualaudio::DeviceType device_type,
                                  fuchsia::virtualaudio::DevicePtr& device_ptr) {
  const char* direction = is_input ? "input" : "output";
  const char* type;

  switch (device_type) {
    case fuchsia::virtualaudio::DeviceSpecific::Tag::kStreamConfig:
      type = "StreamConfig";
      break;
    case fuchsia::virtualaudio::DeviceSpecific::Tag::kDai:
      type = "Dai";
      break;
    case fuchsia::virtualaudio::DeviceSpecific::Tag::kCodec:
      type = "Codec";
      break;
    case fuchsia::virtualaudio::DeviceSpecific::Tag::kComposite:
      type = "Composite";
      break;
    default:
      ZX_ASSERT(0);
  }
  fuchsia::virtualaudio::Direction configuration_direction;
  configuration_direction.set_is_input(is_input);
  fuchsia::virtualaudio::Control_GetDefaultConfiguration_Result config_result;
  zx_status_t status = controller_->GetDefaultConfiguration(
      device_type, std::move(configuration_direction), &config_result);
  ASSERT_EQ(status, ZX_OK) << "virtualaudio::Control::GetDefaultConfiguration (" << type << " "
                           << direction << ") failed";
  ASSERT_FALSE(config_result.is_err()) << "Failed to GetDefaultConfiguration for (" << type << " "
                                       << direction << ") device: " << config_result.err();

  fuchsia::virtualaudio::Configuration config = std::move(config_result.response().config);
  fuchsia::virtualaudio::Control_AddDevice_Result result;
  status = controller_->AddDevice(std::move(config),
                                  device_ptr.NewRequest(device_loop_.dispatcher()), &result);

  ASSERT_EQ(status, ZX_OK) << "virtualaudio::Control::AddDevice (" << type << " " << direction
                           << ") failed";
  ASSERT_FALSE(result.is_err()) << "Failed to add " << type << " " << direction
                                << " device: " << result.err();
  device_ptr.set_error_handler([type, direction](zx_status_t error) {
    FAIL() << "virtualaudio::Device (" << type << " " << direction << ") disconnected: " << error;
  });
}

// Create testcase instances for each device entry.
void DeviceHost::RegisterTests(bool expect_audio_core_not_connected, bool enable_position_tests) {
  for (auto& device_entry : device_entries()) {
    RegisterBasicTestsForDevice(device_entry);
    RegisterAdminTestsForDevice(device_entry, expect_audio_core_not_connected);
    RegisterPositionTestsForDevice(device_entry, expect_audio_core_not_connected,
                                   enable_position_tests);
  }
}

// Testing is complete. Clean up our virtual audio devices and shut down our loop.
zx_status_t DeviceHost::QuitDeviceLoop() {
  if (shutting_down_) {
    return ZX_OK;
  }
  shutting_down_ = true;

  if (device_loop_.GetState() == ASYNC_LOOP_SHUTDOWN) {
    return ZX_OK;
  }

  libsync::Completion done;
  async::PostTask(device_loop_.dispatcher(), [this, &done]() {
    stream_config_input_.set_error_handler(nullptr);
    stream_config_output_.set_error_handler(nullptr);
    dai_input_.set_error_handler(nullptr);
    dai_output_.set_error_handler(nullptr);
    composite_.set_error_handler(nullptr);

    if (controller_.is_bound()) {
      zx_status_t status = controller_->RemoveAll();
      ASSERT_EQ(status, ZX_OK) << "Final RemoveAll failed";

      uint32_t input_count = -1, output_count = -1, unspecified_direction_count = -1;
      do {
        status =
            controller_->GetNumDevices(&input_count, &output_count, &unspecified_direction_count);
        ASSERT_EQ(status, ZX_OK) << "After final RemoveAll, GetNumDevices failed";
      } while (input_count != 0 || output_count != 0 || unspecified_direction_count != 0);
    }

    device_loop_.RunUntilIdle();
    done.Signal();
  });

  zx_status_t status = done.Wait(zx::sec(10));
  device_loop_.Shutdown();

  return status;
}

}  // namespace media::audio::drivers::test
