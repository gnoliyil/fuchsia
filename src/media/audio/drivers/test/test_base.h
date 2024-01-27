// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_TEST_TEST_BASE_H_
#define SRC_MEDIA_AUDIO_DRIVERS_TEST_TEST_BASE_H_

#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <lib/async-loop/default.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/device/audio.h>

#include "src/media/audio/drivers/test/audio_device_enumerator_stub.h"
#include "src/media/audio/lib/test/test_fixture.h"

namespace media::audio::drivers::test {

// We enable top-level methods (e.g. TestBase::RequestFormats, BasicTest::RequestStreamProperties,
// AdminTest::RequestBuffer) to skip or produce multiple errors and then cause a test case to
// exit-early once they return, even if no fatal errors were triggered.
// Gtest defines NO macro for this case -- only ASSERT_NO_FATAL_FAILURE -- so we define our own.
// Macro definition in headers is discouraged (at best), but this is used in local test code only.
#define ASSERT_NO_FAILURE_OR_SKIP(statement, ...)          \
  do {                                                     \
    statement;                                             \
    if (TestBase::HasFailure() || TestBase::IsSkipped()) { \
      return;                                              \
    }                                                      \
  } while (0)

enum DeviceType : uint16_t { Input = 0, Output = 1 };

struct DeviceEntry {
  // `index() == 0` means A2DP.
  std::variant<std::monostate, fidl::UnownedClientEnd<fuchsia_io::Directory>> dir;
  std::string filename;
  DeviceType dev_type;

  bool isA2DP() const { return dir.index() == 0; }

  bool operator<(const DeviceEntry& rhs) const {
    return std::tie(dir, filename, dev_type) < std::tie(rhs.dir, rhs.filename, rhs.dev_type);
  }
};

// Used in registering separate test case instances for each enumerated device
//
// See googletest/docs/advanced.md for details
//
// Devices are displayed in the 'audio-output/000' format, or simply the filename, if the
// special dir_fd value is observed (an example might be 'Bluetooth-A2DP' for Bluetooth devices).
std::string inline DevNameForEntry(const DeviceEntry& device_entry) {
  if (device_entry.isA2DP()) {
    return device_entry.filename;
  }

  return std::string(device_entry.dev_type == DeviceType::Input ? "audio-input" : "audio-output") +
         "/" + device_entry.filename;
}
std::string inline TestNameForEntry(const std::string& test_class_name,
                                    const DeviceEntry& device_entry) {
  return DevNameForEntry(device_entry) + ":" + test_class_name;
}

class TestBase : public media::audio::test::TestFixture {
 public:
  explicit TestBase(const DeviceEntry& device_entry) : device_entry_(device_entry) {}

 protected:
  void SetUp() override;
  void TearDown() override;

  void ConnectToDevice(const DeviceEntry& device_entry);
  void ConnectToBluetoothDevice();
  void CreateStreamConfigFromChannel(
      fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig> channel);

  const DeviceEntry& device_entry() const { return device_entry_; }
  void set_device_type(DeviceType device_type) {}
  DeviceType device_type() const { return device_entry_.dev_type; }

  // "Basic" (stream-config channel) tests and "Admin" (ring-buffer channel) tests both need to know
  // the supported formats, so this is implemented in the shared base class.
  void RequestFormats();

  // TODO(fxbug.dev/83972): Consider a more functional style when validating formats
  const std::vector<fuchsia::hardware::audio::PcmSupportedFormats>& pcm_formats() const {
    return pcm_formats_;
  }

  fidl::InterfacePtr<fuchsia::hardware::audio::StreamConfig>& stream_config() {
    return stream_config_;
  }

  const fuchsia::hardware::audio::PcmFormat& min_format() const { return min_format_; }
  const fuchsia::hardware::audio::PcmFormat& max_format() const { return max_format_; }
  static void LogFormat(const fuchsia::hardware::audio::PcmFormat& format,
                        const std::string& tag = {});

  void WaitForError(zx::duration wait_duration = kWaitForErrorDuration) {
    RunLoopWithTimeoutOrUntil([]() { return HasFailure() || IsSkipped(); }, wait_duration);
  }

 private:
  static constexpr zx::duration kWaitForErrorDuration = zx::msec(100);

  void SetMinMaxFormats();

  std::optional<component_testing::RealmRoot> realm_;
  fuchsia::component::BinderPtr audio_binder_;

  const DeviceEntry& device_entry_;

  fidl::InterfacePtr<fuchsia::hardware::audio::StreamConfig> stream_config_;

  std::vector<fuchsia::hardware::audio::PcmSupportedFormats> pcm_formats_;

  fuchsia::hardware::audio::PcmFormat min_format_;
  fuchsia::hardware::audio::PcmFormat max_format_;
};

}  // namespace media::audio::drivers::test

#endif  // SRC_MEDIA_AUDIO_DRIVERS_TEST_TEST_BASE_H_
