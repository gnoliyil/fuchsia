// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/input/injection/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/testing/enclosing_environment.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/ui/input/testing/fake_input_report_device/fake.h"
#include "src/ui/input/testing/fake_input_report_device/reports_reader.h"

namespace {

// Max timeout in failure cases.
// Set this as low as you can that still works across all test platforms.
constexpr zx::duration kTimeout = zx::min(5);

// Common services for each test.
const std::map<std::string, std::string> LocalServices() {
  return {
      // Test-only variants of the input pipeline and root presenter are included in this tests's
      // package for component hermeticity, and to avoid reading /dev/class/input-report. Reading
      // the input device driver in a test can cause conflicts with real input devices.
      {"fuchsia.input.injection.InputDeviceRegistry",
       "fuchsia-pkg://fuchsia.com/mediabuttons-integration-tests-ip#meta/input-pipeline.cmx"},
      {"fuchsia.ui.policy.DeviceListenerRegistry",
       "fuchsia-pkg://fuchsia.com/mediabuttons-integration-tests-ip#meta/input-pipeline.cmx"},
      {"fuchsia.ui.pointerinjector.configuration.Setup",
       "fuchsia-pkg://fuchsia.com/mediabuttons-integration-tests-ip#meta/root_presenter.cmx"},
      // Scenic protocols.
      {"fuchsia.ui.scenic.Scenic",
       "fuchsia-pkg://fuchsia.com/mediabuttons-integration-tests-ip#meta/scenic.cmx"},
      // Misc protocols.
      {"fuchsia.cobalt.LoggerFactory",
       "fuchsia-pkg://fuchsia.com/mock_cobalt#meta/mock_cobalt.cmx"},
      {"fuchsia.hardware.display.Provider",
       "fuchsia-pkg://fuchsia.com/fake-hardware-display-controller-provider#meta/hdcp.cmx"},
  };
}

// Allow these global services from outside the test environment.
const std::vector<std::string> GlobalServices() {
  return {"fuchsia.vulkan.loader.Loader", "fuchsia.sysmem.Allocator",
          "fuchsia.scheduler.ProfileProvider"};
}

// This implements the MediaButtonsListener class. Its purpose is to test that MediaButton Events
// are actually sent out to the Listeners.
class ButtonsListenerImpl : public fuchsia::ui::policy::MediaButtonsListener {
 public:
  ButtonsListenerImpl(
      fidl::InterfaceRequest<fuchsia::ui::policy::MediaButtonsListener> listener_request,
      fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event)
      : listener_binding_(this, std::move(listener_request)), on_event_(std::move(on_event)) {}

 private:
  // |MediaButtonsListener|
  void OnMediaButtonsEvent(fuchsia::ui::input::MediaButtonsEvent event) override {
    FX_NOTREACHED() << "unused";
  }

  // |MediaButtonsListener|
  void OnEvent(fuchsia::ui::input::MediaButtonsEvent event, OnEventCallback callback) override {
    on_event_(event);
    callback();
  }

  fidl::Binding<fuchsia::ui::policy::MediaButtonsListener> listener_binding_;
  fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event_;
};

class MediaButtonsListenerTest : public sys::testing::TestWithEnvironment {
 protected:
  explicit MediaButtonsListenerTest() {
    auto services = sys::testing::EnvironmentServices::Create(real_env());
    zx_status_t is_ok = ZX_OK;

    // Add common services.
    for (const auto& [name, url] : LocalServices()) {
      is_ok = services->AddServiceWithLaunchInfo({.url = url}, name);
      FX_CHECK(is_ok == ZX_OK) << "Failed to add service " << name;
    }

    // Enable services from outside this test.
    for (const auto& service : GlobalServices()) {
      is_ok = services->AllowParentService(service);
      FX_CHECK(is_ok == ZX_OK) << "Failed to add service " << service;
    }

    test_env_ = CreateNewEnclosingEnvironment("media-buttons-test-ip", std::move(services));

    WaitForEnclosingEnvToStart(test_env_.get());

    FX_VLOGS(1) << "Created test environment.";

    // Post a "just in case" quit task, if the test hangs.
    async::PostDelayedTask(
        dispatcher(),
        [] { FX_LOGS(FATAL) << "\n\n>> Test did not complete in time, terminating.  <<\n\n"; },
        kTimeout);
  }

  ~MediaButtonsListenerTest() override {
    FX_CHECK(injection_count_ > 0) << "injection expected but didn't happen.";
  }

  sys::testing::EnclosingEnvironment* test_env() { return test_env_.get(); }

  void RegisterInjectionDevice() {
    registry_ = test_env()->ConnectToService<fuchsia::input::injection::InputDeviceRegistry>();

    // Create a FakeInputDevice
    fake_input_device_ = std::make_unique<fake_input_report_device::FakeInputDevice>(
        input_device_ptr_.NewRequest(), dispatcher());

    // Set descriptor
    auto device_descriptor = std::make_unique<fuchsia::input::report::DeviceDescriptor>();
    auto consumer_controls = device_descriptor->mutable_consumer_control()->mutable_input();
    consumer_controls->set_buttons({
        fuchsia::input::report::ConsumerControlButton::CAMERA_DISABLE,
        fuchsia::input::report::ConsumerControlButton::MIC_MUTE,
        fuchsia::input::report::ConsumerControlButton::PAUSE,
        fuchsia::input::report::ConsumerControlButton::VOLUME_UP,
        fuchsia::input::report::ConsumerControlButton::VOLUME_DOWN,
    });

    fake_input_device_->SetDescriptor(std::move(device_descriptor));

    // Register the FakeInputDevice
    registry_->Register(std::move(input_device_ptr_));
  }

  // Inject directly into Input Pipeline, using fuchsia.input.injection FIDLs.
  void InjectInput() {
    // Set InputReports to inject: one report with buttons pressed, followed by a report with no
    // buttons pressed.
    fuchsia::input::report::ConsumerControlInputReport cc_input_report;
    cc_input_report.set_pressed_buttons({
        fuchsia::input::report::ConsumerControlButton::CAMERA_DISABLE,
        fuchsia::input::report::ConsumerControlButton::MIC_MUTE,
        fuchsia::input::report::ConsumerControlButton::PAUSE,
        fuchsia::input::report::ConsumerControlButton::VOLUME_UP,
    });
    fuchsia::input::report::InputReport input_report;
    input_report.set_consumer_control(std::move(cc_input_report));

    std::vector<fuchsia::input::report::InputReport> input_reports;
    input_reports.push_back(std::move(input_report));

    fuchsia::input::report::ConsumerControlInputReport remove_cc_input_report;
    remove_cc_input_report.set_pressed_buttons({});

    fuchsia::input::report::InputReport remove_input_report;
    remove_input_report.set_consumer_control(std::move(remove_cc_input_report));
    input_reports.push_back(std::move(remove_input_report));

    // Inject the reports.
    fake_input_device_->SetReports(std::move(input_reports));

    ++injection_count_;
  }

 private:
  std::unique_ptr<sys::testing::EnclosingEnvironment> test_env_;
  fuchsia::input::injection::InputDeviceRegistryPtr registry_;
  std::unique_ptr<fake_input_report_device::FakeInputDevice> fake_input_device_;
  fuchsia::input::report::InputDevicePtr input_device_ptr_;
  uint32_t injection_count_ = 0;
};

TEST_F(MediaButtonsListenerTest, MediaButtonsWithCallback) {
  RegisterInjectionDevice();

  // Callback to save the observed media button event.
  std::optional<fuchsia::ui::input::MediaButtonsEvent> observed_event;
  fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event =
      [&observed_event](const fuchsia::ui::input::MediaButtonsEvent& observed) {
        observed_event = fidl::Clone(observed);
      };

  // Register the MediaButtons listener against Input Pipeline.
  fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> listener_handle;
  auto button_listener_impl =
      std::make_unique<ButtonsListenerImpl>(listener_handle.NewRequest(), std::move(on_event));

  auto input_pipeline = test_env()->ConnectToService<fuchsia::ui::policy::DeviceListenerRegistry>();
  input_pipeline.set_error_handler([](zx_status_t status) {
    FX_LOGS(FATAL) << "Lost connection to Input Pipeline: " << zx_status_get_string(status);
  });
  input_pipeline->RegisterListener(std::move(listener_handle), [this] { InjectInput(); });

  RunLoopUntil([&observed_event] { return observed_event.has_value(); });

  ASSERT_TRUE(observed_event->has_volume());
  EXPECT_EQ(observed_event->volume(), 1);

  ASSERT_TRUE(observed_event->has_mic_mute());
  EXPECT_TRUE(observed_event->mic_mute());

  ASSERT_TRUE(observed_event->has_pause());
  EXPECT_TRUE(observed_event->pause());

  ASSERT_TRUE(observed_event->has_camera_disable());
  EXPECT_TRUE(observed_event->camera_disable());
}

}  // namespace
