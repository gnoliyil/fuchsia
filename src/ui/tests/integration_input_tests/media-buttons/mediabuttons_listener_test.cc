// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/input/injection/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <fuchsia/scheduler/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/tracing/provider/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/vulkan/loader/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/input/testing/fake_input_report_device/fake.h"
#include "src/ui/input/testing/fake_input_report_device/reports_reader.h"
#include "src/ui/testing/ui_test_manager/ui_test_manager.h"

namespace {
// Types imported for the realm_builder library.
using component_testing::ChildRef;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;
using RealmBuilder = component_testing::RealmBuilder;

// Max timeout in failure cases.
// Set this as low as you can that still works across all test platforms.
constexpr zx::duration kTimeout = zx::min(5);

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

class MediaButtonsListenerTest
    : public gtest::RealLoopFixture,
      public ::testing::WithParamInterface<ui_testing::UITestRealm::SceneOwnerType> {
 protected:
  MediaButtonsListenerTest() = default;

  ~MediaButtonsListenerTest() override {
    FX_CHECK(injection_count_ > 0) << "injection expected but didn't happen.";
  }

  void SetUp() override {
    // Post a "just in case" quit task, if the test hangs.
    async::PostDelayedTask(
        dispatcher(),
        [] { FX_LOGS(FATAL) << "\n\n>> Test did not complete in time, terminating.  <<\n\n"; },
        kTimeout);

    ui_testing::UITestRealm::Config config;
    config.scene_owner = GetParam();
    config.use_input = true;
    ui_test_manager_.emplace(std::move(config));

    ui_test_manager_->BuildRealm();
    realm_exposed_services_ = ui_test_manager_->CloneExposedServicesDirectory();
  }

  void TearDown() override {
    bool complete = false;
    ui_test_manager_->TeardownRealm(
        [&](fit::result<fuchsia::component::Error> result) { complete = true; });
    RunLoopUntil([&]() { return complete; });
  }

  void RegisterInjectionDevice() {
    registry_ = realm_exposed_services()->Connect<fuchsia::input::injection::InputDeviceRegistry>();

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
  void InjectInput(fuchsia::input::report::ConsumerControlInputReport cc_input_report) {
    fuchsia::input::report::InputReport input_report;
    input_report.set_consumer_control(std::move(cc_input_report));

    std::vector<fuchsia::input::report::InputReport> input_reports;
    input_reports.push_back(std::move(input_report));
    fake_input_device_->SetReports(std::move(input_reports));

    ++injection_count_;
  }

  sys::ServiceDirectory* realm_exposed_services() { return realm_exposed_services_.get(); }
  const std::optional<Realm>& realm() const { return realm_; }

 private:
  std::optional<ui_testing::UITestManager> ui_test_manager_;
  std::unique_ptr<sys::ServiceDirectory> realm_exposed_services_;
  std::optional<Realm> realm_;

  fuchsia::input::injection::InputDeviceRegistryPtr registry_;
  std::unique_ptr<fake_input_report_device::FakeInputDevice> fake_input_device_;
  fuchsia::input::report::InputDevicePtr input_device_ptr_;
  uint32_t injection_count_ = 0;
};

INSTANTIATE_TEST_SUITE_P(MediaButtonsListenerTestWithParams, MediaButtonsListenerTest,
                         ::testing::Values(ui_testing::UITestRealm::SceneOwnerType::ROOT_PRESENTER,
                                           ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER));
TEST_P(MediaButtonsListenerTest, MediaButtonsWithCallback) {
  RegisterInjectionDevice();
  // Callbacks to save the observed media button event.
  std::optional<fuchsia::ui::input::MediaButtonsEvent> observed_event_first_listener;
  fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event_first =
      [&observed_event_first_listener](const fuchsia::ui::input::MediaButtonsEvent& observed) {
        observed_event_first_listener = fidl::Clone(observed);
      };
  std::optional<fuchsia::ui::input::MediaButtonsEvent> observed_event_second_listener;
  fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event_second =
      [&observed_event_second_listener](const fuchsia::ui::input::MediaButtonsEvent& observed) {
        observed_event_second_listener = fidl::Clone(observed);
      };
  std::optional<fuchsia::ui::input::MediaButtonsEvent> observed_event_third_listener;
  fit::function<void(const fuchsia::ui::input::MediaButtonsEvent&)> on_event_third =
      [&observed_event_third_listener](const fuchsia::ui::input::MediaButtonsEvent& observed) {
        observed_event_third_listener = fidl::Clone(observed);
      };

  // Register the MediaButtons listeners against Input Pipeline and inject an event with pressed
  // buttons.
  fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> first_listener_handle;
  auto first_button_listener_impl = std::make_unique<ButtonsListenerImpl>(
      first_listener_handle.NewRequest(), std::move(on_event_first));
  fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> second_listener_handle;
  auto second_button_listener_impl = std::make_unique<ButtonsListenerImpl>(
      second_listener_handle.NewRequest(), std::move(on_event_second));
  fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> third_listener_handle;
  auto third_button_listener_impl = std::make_unique<ButtonsListenerImpl>(
      third_listener_handle.NewRequest(), std::move(on_event_third));

  auto listener_registry =
      realm_exposed_services()->Connect<fuchsia::ui::policy::DeviceListenerRegistry>();
  listener_registry.set_error_handler([](zx_status_t status) {
    FX_LOGS(FATAL) << "Lost connection to DeviceListenerRegistry: " << zx_status_get_string(status);
  });
  fuchsia::input::report::ConsumerControlInputReport first_report;
  first_report.set_pressed_buttons({
      fuchsia::input::report::ConsumerControlButton::CAMERA_DISABLE,
      fuchsia::input::report::ConsumerControlButton::MIC_MUTE,
      fuchsia::input::report::ConsumerControlButton::PAUSE,
      fuchsia::input::report::ConsumerControlButton::VOLUME_UP,
  });
  listener_registry->RegisterListener(std::move(first_listener_handle), [] {});
  listener_registry->RegisterListener(std::move(second_listener_handle), [] {});
  listener_registry->RegisterListener(std::move(third_listener_handle), [] {});
  InjectInput(std::move(first_report));
  RunLoopUntil([&observed_event_first_listener, &observed_event_second_listener,
                &observed_event_third_listener] {
    return observed_event_first_listener.has_value() &&
           (observed_event_second_listener.has_value() &&
            observed_event_third_listener.has_value());
  });

  ASSERT_TRUE(observed_event_first_listener->has_volume());
  EXPECT_EQ(observed_event_first_listener->volume(), 1);
  ASSERT_TRUE(observed_event_first_listener->has_mic_mute());
  EXPECT_TRUE(observed_event_first_listener->mic_mute());
  ASSERT_TRUE(observed_event_first_listener->has_pause());
  EXPECT_TRUE(observed_event_first_listener->pause());
  ASSERT_TRUE(observed_event_first_listener->has_camera_disable());
  EXPECT_TRUE(observed_event_first_listener->camera_disable());

  ASSERT_TRUE(observed_event_second_listener->has_volume());
  EXPECT_EQ(observed_event_second_listener->volume(), 1);
  ASSERT_TRUE(observed_event_second_listener->has_mic_mute());
  EXPECT_TRUE(observed_event_second_listener->mic_mute());
  ASSERT_TRUE(observed_event_second_listener->has_pause());
  EXPECT_TRUE(observed_event_second_listener->pause());
  ASSERT_TRUE(observed_event_second_listener->has_camera_disable());
  EXPECT_TRUE(observed_event_second_listener->camera_disable());

  ASSERT_TRUE(observed_event_third_listener->has_volume());
  EXPECT_EQ(observed_event_third_listener->volume(), 1);
  ASSERT_TRUE(observed_event_third_listener->has_mic_mute());
  EXPECT_TRUE(observed_event_third_listener->mic_mute());
  ASSERT_TRUE(observed_event_third_listener->has_pause());
  EXPECT_TRUE(observed_event_third_listener->pause());
  ASSERT_TRUE(observed_event_third_listener->has_camera_disable());
  EXPECT_TRUE(observed_event_third_listener->camera_disable());

  // Inject a second event that represents releasing the pressed buttons.
  observed_event_first_listener.reset();
  observed_event_second_listener.reset();
  observed_event_third_listener.reset();
  fuchsia::input::report::ConsumerControlInputReport second_report;
  second_report.set_pressed_buttons({});
  InjectInput(std::move(second_report));
  RunLoopUntil([&observed_event_first_listener, &observed_event_second_listener,
                &observed_event_third_listener] {
    return observed_event_first_listener.has_value() &&
           (observed_event_second_listener.has_value() &&
            observed_event_third_listener.has_value());
  });

  ASSERT_TRUE(observed_event_first_listener->has_volume());
  EXPECT_EQ(observed_event_first_listener->volume(), 0);
  ASSERT_TRUE(observed_event_first_listener->has_mic_mute());
  EXPECT_FALSE(observed_event_first_listener->mic_mute());
  ASSERT_TRUE(observed_event_first_listener->has_pause());
  EXPECT_FALSE(observed_event_first_listener->pause());
  ASSERT_TRUE(observed_event_first_listener->has_camera_disable());
  EXPECT_FALSE(observed_event_first_listener->camera_disable());

  ASSERT_TRUE(observed_event_second_listener->has_volume());
  EXPECT_EQ(observed_event_second_listener->volume(), 0);
  ASSERT_TRUE(observed_event_second_listener->has_mic_mute());
  EXPECT_FALSE(observed_event_second_listener->mic_mute());
  ASSERT_TRUE(observed_event_second_listener->has_pause());
  EXPECT_FALSE(observed_event_second_listener->pause());
  ASSERT_TRUE(observed_event_second_listener->has_camera_disable());
  EXPECT_FALSE(observed_event_second_listener->camera_disable());

  ASSERT_TRUE(observed_event_third_listener->has_volume());
  EXPECT_EQ(observed_event_third_listener->volume(), 0);
  ASSERT_TRUE(observed_event_third_listener->has_mic_mute());
  EXPECT_FALSE(observed_event_third_listener->mic_mute());
  ASSERT_TRUE(observed_event_third_listener->has_pause());
  EXPECT_FALSE(observed_event_third_listener->pause());
  ASSERT_TRUE(observed_event_third_listener->has_camera_disable());
  EXPECT_FALSE(observed_event_third_listener->camera_disable());

  // Drop endpoint of second listener to close channel.
  second_button_listener_impl = {};

  // Inject a third event that should result in second listener being unregistered since it's
  // channel is now closed.
  observed_event_first_listener.reset();
  observed_event_second_listener.reset();
  observed_event_third_listener.reset();
  fuchsia::input::report::ConsumerControlInputReport third_report;
  third_report.set_pressed_buttons({fuchsia::input::report::ConsumerControlButton::VOLUME_UP});
  InjectInput(std::move(third_report));
  RunLoopUntil([&observed_event_first_listener, &observed_event_third_listener] {
    return observed_event_first_listener.has_value() && observed_event_third_listener.has_value();
  });

  ASSERT_TRUE(observed_event_first_listener->has_volume());
  EXPECT_EQ(observed_event_first_listener->volume(), 1);
  ASSERT_TRUE(observed_event_third_listener->has_volume());
  EXPECT_EQ(observed_event_third_listener->volume(), 1);
  ASSERT_FALSE(observed_event_second_listener.has_value());
}

}  // namespace
