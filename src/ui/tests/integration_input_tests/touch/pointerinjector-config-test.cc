// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/input/injection/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/session.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <zircon/status.h>
#include <zircon/types.h>
#include <zircon/utc.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <test/accessibility/cpp/fidl.h>
#include <test/touch/cpp/fidl.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/input/testing/fake_input_report_device/fake.h"
#include "src/ui/input/testing/fake_input_report_device/reports_reader.h"
#include "src/ui/testing/ui_test_manager/ui_test_manager.h"

// This test exercises the pointer injector code in the context of Input Pipeline and a real Scenic
// client. It is a multi-component test, and carefully avoids sleeping or polling for component
// coordination.
// - It runs real (Root Presenter + Input Pipeline | Scene Manager) components, and a real Scenic
// component.
// - It uses a fake display controller; the physical device is unused.
//
// Components involved
// - This test program
// - Root Presenter (with separate Input Pipeline) or Scene Manager
// - Scenic
// - Child view, a Scenic client
//
// Touch dispatch path
// - Test program's injection -> Input Pipeline -> Scenic -> Child view
//
// Setup sequence
// - The test sets up this view hierarchy:
//   - Top level scene, owned by Root Presenter.
//   - Child view, owned by the ui client.
// - The test waits for a Scenic event that verifies the child has UI content in the scene graph.
// - The test injects input into Input Pipeline, emulating a display's touch report.
// - Input Pipeline dispatches the touch event to Scenic, which in turn dispatches it to the child.
// - The child receives the touch event and reports back to the test over a custom test-only FIDL.
// - Test waits for the child to report a touch; when the test receives the report, the test quits
//   successfully.
//
// This test uses the realm_builder library to construct the topology of components
// and routes services between them. For v2 components, every test driver component
// sits as a child of test_manager in the topology. Thus, the topology of a test
// driver component such as this one looks like this:
//
//     test_manager
//         |
//   pointerinjector-config-test-ip.cml (this component)
//
// With the usage of the realm_builder library, we construct a realm during runtime
// and then extend the topology to look like:
//
//    test_manager
//         |
//   pointerinjector-config-test-ip.cml (this component)
//         |
//   <created realm root>
//      /      \
//   scenic  input-pipeline
//
// For more information about testing v2 components and realm_builder,
// visit the following links:
//
// Testing: https://fuchsia.dev/fuchsia-src/concepts/testing/v2
// Realm Builder: https://fuchsia.dev/fuchsia-src/development/components/v2/realm_builder

namespace {

using test::touch::ResponseListener;
using ScenicEvent = fuchsia::ui::scenic::Event;
using GfxEvent = fuchsia::ui::gfx::Event;

// Types imported for the realm_builder library.
using component_testing::ChildRef;
using component_testing::LocalComponent;
using component_testing::LocalComponentHandles;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;
using RealmBuilder = component_testing::RealmBuilder;

// Alias for Component child name as provided to Realm Builder.
using ChildName = std::string;

// Alias for Component Legacy URL as provided to Realm Builder.
using LegacyUrl = std::string;

// Max timeout in failure cases.
// Set this as low as you can that still works across all test platforms.
constexpr zx::duration kTimeout = zx::min(5);

constexpr auto kTouchScreenMaxDim = 1000;
constexpr auto kTouchScreenMinDim = -1000;

// Maximum distance between two view coordinates so that they are considered equal.
constexpr auto kViewCoordinateEpsilon = 0.01;

// The type used to measure UTC time. The integer value here does not matter so
// long as it differs from the ZX_CLOCK_MONOTONIC=0 defined by Zircon.
using time_utc = zx::basic_time<1>;

constexpr auto kMockResponseListener = "response_listener";

constexpr auto kTapRetryInterval = zx::sec(1);

enum class TapLocation { kTopLeft };

// This component implements the test.touch.ResponseListener protocol
// and the interface for a RealmBuilder LocalComponent. A LocalComponent
// is a component that is implemented here in the test, as opposed to elsewhere
// in the system. When it's inserted to the realm, it will act like a proper
// component. This is accomplished, in part, because the realm_builder
// library creates the necessary plumbing. It creates a manifest for the component
// and routes all capabilities to and from it.
class ResponseListenerServer : public ResponseListener, public LocalComponent {
 public:
  explicit ResponseListenerServer(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

  // |test::touch::ResponseListener|
  void Respond(test::touch::PointerData pointer_data) override {
    FX_CHECK(respond_callback_) << "Expected callback to be set for test.touch.Respond().";
    respond_callback_(std::move(pointer_data));
  }

  // |LocalComponent::Start|
  // When the component framework requests for this component to start, this
  // method will be invoked by the realm_builder library.
  void Start(std::unique_ptr<LocalComponentHandles> local_handles) override {
    // When this component starts, add a binding to the test.touch.ResponseListener
    // protocol to this component's outgoing directory.
    FX_CHECK(local_handles->outgoing()->AddPublicService(
                 fidl::InterfaceRequestHandler<test::touch::ResponseListener>([this](auto request) {
                   bindings_.AddBinding(this, std::move(request), dispatcher_);
                 })) == ZX_OK);
    local_handles_.emplace_back(std::move(local_handles));
  }

  void SetRespondCallback(fit::function<void(test::touch::PointerData)> callback) {
    respond_callback_ = std::move(callback);
  }

 private:
  async_dispatcher_t* dispatcher_ = nullptr;
  std::vector<std::unique_ptr<LocalComponentHandles>> local_handles_;
  fidl::BindingSet<test::touch::ResponseListener> bindings_;
  fit::function<void(test::touch::PointerData)> respond_callback_;
};

struct PointerInjectorConfigTestData {
  int display_rotation;

  float clip_scale = 1.f;
  float clip_translation_x = 0.f;
  float clip_translation_y = 0.f;

  // expected location of the pointer event, in client view space, where the
  // range of the X and Y axes is [0, 1]
  float expected_x;
  float expected_y;
};

using PointerInjectorConfigTestParams =
    std::tuple<ui_testing::UITestRealm::SceneOwnerType, PointerInjectorConfigTestData>;

class PointerInjectorConfigTest
    : public gtest::RealLoopFixture,
      public testing::WithParamInterface<PointerInjectorConfigTestParams> {
 protected:
  PointerInjectorConfigTest() = default;
  ~PointerInjectorConfigTest() override {
    FX_CHECK(injection_count_ > 0) << "injection expected but didn't happen.";
  }

  void SetUp() override {
    // Post a "just in case" quit task, if the test hangs.
    async::PostDelayedTask(
        dispatcher(),
        [] { FX_LOGS(FATAL) << "\n\n>> Test did not complete in time, terminating.  <<\n\n"; },
        kTimeout);

    auto [scene_owner, test_data] = GetParam();

    ui_testing::UITestRealm::Config config;
    config.display_rotation = test_data.display_rotation;
    config.scene_owner = scene_owner;
    config.use_input = true;
    config.accessibility_owner = ui_testing::UITestRealm::AccessibilityOwnerType::FAKE;
    config.ui_to_client_services = {fuchsia::ui::scenic::Scenic::Name_};
    ui_test_manager_ = std::make_unique<ui_testing::UITestManager>(std::move(config));

    // Assemble realm.
    BuildRealm();

    // Get the display dimensions.
    FX_LOGS(INFO) << "Waiting for scenic display info";
    scenic_ = realm_exposed_services()->Connect<fuchsia::ui::scenic::Scenic>();
    scenic_->GetDisplayInfo([this](fuchsia::ui::gfx::DisplayInfo display_info) {
      display_width_ = display_info.width_in_px;
      display_height_ = display_info.height_in_px;
      FX_LOGS(INFO) << "Got display_width = " << display_width_
                    << " and display_height = " << display_height_;
    });
    RunLoopUntil([this] { return display_width_ != 0 && display_height_ != 0; });

    // Register input injection device.
    FX_LOGS(INFO) << "Registering input injection device";
    RegisterInjectionDevice();

    // Launch client view, and wait until it's rendering to proceed with the test.
    ui_test_manager_->InitializeScene();
    RunLoopUntil([this]() { return ui_test_manager_->ClientViewIsRendering(); });

    realm_exposed_services_->Connect<test::accessibility::Magnifier>(
        this->fake_magnifier_.NewRequest());
  }

  // Waits for one or more pointer events; calls QuitLoop once one meets expectations.
  void WaitForAResponseMeetingExpectations(float expected_x, float expected_y,
                                           const std::string& component_name) {
    response_listener()->SetRespondCallback(
        [this, expected_x, expected_y, component_name](test::touch::PointerData pointer_data) {
          FX_LOGS(INFO) << "Client received tap at (" << pointer_data.local_x() << ", "
                        << pointer_data.local_y() << ").";
          FX_LOGS(INFO) << "Expected tap is at approximately (" << expected_x << ", " << expected_y
                        << ").";

          // Allow for minor rounding differences in coordinates.
          EXPECT_EQ(pointer_data.component_name(), component_name);
          if (abs(pointer_data.local_x() - expected_x) <= kViewCoordinateEpsilon &&
              abs(pointer_data.local_y() - expected_y) <= kViewCoordinateEpsilon) {
            response_listener()->SetRespondCallback([](test::touch::PointerData ignored) {});
            QuitLoop();
          }
        });
  }

  void RegisterInjectionDevice() {
    registry_ = realm_exposed_services()->Connect<fuchsia::input::injection::InputDeviceRegistry>();
    registry_.set_error_handler([](zx_status_t status) {
      FX_LOGS(ERROR) << "Input device registry error: " << zx_status_get_string(status);
    });
    // Create a FakeInputDevice
    fake_input_device_ = std::make_unique<fake_input_report_device::FakeInputDevice>(
        input_device_ptr_.NewRequest(), dispatcher());

    // Set descriptor
    auto device_descriptor = std::make_unique<fuchsia::input::report::DeviceDescriptor>();
    auto touch = device_descriptor->mutable_touch()->mutable_input();
    touch->set_touch_type(fuchsia::input::report::TouchType::TOUCHSCREEN);
    touch->set_max_contacts(10);

    fuchsia::input::report::Axis axis;
    axis.unit.type = fuchsia::input::report::UnitType::NONE;
    axis.unit.exponent = 0;
    axis.range.min = kTouchScreenMinDim;
    axis.range.max = kTouchScreenMaxDim;

    fuchsia::input::report::ContactInputDescriptor contact;
    contact.set_position_x(axis);
    contact.set_position_y(axis);
    contact.set_pressure(axis);

    touch->mutable_contacts()->push_back(std::move(contact));

    fake_input_device_->SetDescriptor(std::move(device_descriptor));

    // Register the FakeInputDevice
    registry_->Register(std::move(input_device_ptr_));
    FX_LOGS(INFO) << "Registered touchscreen with x touch range = (-1000, 1000) "
                  << "and y touch range = (-1000, 1000).";
  }

  zx::basic_time<ZX_CLOCK_MONOTONIC> TapTopLeft() {
    // Inject directly into Input Pipeline, using fuchsia.input.injection FIDLs.

    // Set InputReports to inject. One contact at the center of the top left quadrant, followed
    // by no contacts.
    fuchsia::input::report::ContactInputReport contact_input_report;
    contact_input_report.set_contact_id(1);

    auto [scene_owner, test_data] = GetParam();

    // Inject one input report, then a conclusion (empty) report.
    switch (test_data.display_rotation) {
      case 0:
        contact_input_report.set_position_x(-500);
        contact_input_report.set_position_y(-500);
        break;
      case 90:
        // The /config/data/display_rotation (90) specifies how many degrees to rotate the
        // presentation child view, counter-clockwise, in a right-handed coordinate system. Thus,
        // the user observes the child view to rotate *clockwise* by that amount (90).
        contact_input_report.set_position_x(500);
        contact_input_report.set_position_y(-500);
        break;
      default:
        FX_NOTREACHED();
    }

    fuchsia::input::report::TouchInputReport touch_input_report;
    auto contacts = touch_input_report.mutable_contacts();
    contacts->push_back(std::move(contact_input_report));

    fuchsia::input::report::InputReport input_report;
    input_report.set_touch(std::move(touch_input_report));

    std::vector<fuchsia::input::report::InputReport> input_reports;
    input_reports.push_back(std::move(input_report));

    fuchsia::input::report::TouchInputReport remove_touch_input_report;
    fuchsia::input::report::InputReport remove_input_report;
    remove_input_report.set_touch(std::move(remove_touch_input_report));
    input_reports.push_back(std::move(remove_input_report));
    fake_input_device_->SetReports(std::move(input_reports));

    ++injection_count_;
    FX_LOGS(INFO) << "*** Tap injected, count: " << injection_count_;
    return RealNow<zx::basic_time<ZX_CLOCK_MONOTONIC>>();
  }

  // Try injecting a tap every `kTapRetryInterval` until the test completes.
  void TryInjectRepeatedly(TapLocation tap_location,
                           zx::basic_time<ZX_CLOCK_MONOTONIC> input_injection_time) {
    input_injection_time = TapTopLeft();
    async::PostDelayedTask(
        dispatcher(),
        [this, tap_location, input_injection_time] {
          TryInjectRepeatedly(tap_location, input_injection_time);
        },
        kTapRetryInterval);
  }

  void SetClipSpaceTransform(float scale, float x, float y) {
    fake_magnifier_->SetMagnification(scale, x, y);
  }

  // Guaranteed to be initialized after SetUp().
  float display_width() const { return static_cast<float>(display_width_); }
  float display_height() const { return static_cast<float>(display_height_); }

  sys::ServiceDirectory* realm_exposed_services() { return realm_exposed_services_.get(); }
  Realm* realm() { return realm_.get(); }

  ResponseListenerServer* response_listener() { return response_listener_.get(); }

 private:
  void BuildRealm() {
    FX_LOGS(INFO) << "Building realm";
    realm_ = std::make_unique<Realm>(ui_test_manager_->AddSubrealm());

    // Key part of service setup: have this test component vend the
    // |ResponseListener| service in the constructed realm.
    response_listener_ = std::make_unique<ResponseListenerServer>(dispatcher());
    realm()->AddLocalChild(kMockResponseListener, response_listener_.get());

    realm()->AddChild(kCppGfxClient, kCppGfxClientUrl);

    realm()->AddRoute({.capabilities = {Protocol{fuchsia::ui::app::ViewProvider::Name_}},
                       .source = ChildRef{kCppGfxClient},
                       .targets = {ParentRef()}});
    realm()->AddRoute({.capabilities = {Protocol{test::touch::ResponseListener::Name_}},
                       .source = ChildRef{kMockResponseListener},
                       .targets = {ChildRef{kCppGfxClient}}});
    realm()->AddRoute({.capabilities = {Protocol{fuchsia::ui::scenic::Scenic::Name_}},
                       .source = ParentRef(),
                       .targets = {ChildRef{kCppGfxClient}}});

    ui_test_manager_->BuildRealm();

    realm_exposed_services_ = ui_test_manager_->CloneExposedServicesDirectory();
  }

  template <typename TimeT>
  TimeT RealNow();

  template <>
  zx::time RealNow() {
    return zx::clock::get_monotonic();
  }

  template <>
  time_utc RealNow() {
    zx::unowned_clock utc_clock(zx_utc_reference_get());
    zx_time_t now;
    FX_CHECK(utc_clock->read(&now) == ZX_OK);
    return time_utc(now);
  }

  std::unique_ptr<ui_testing::UITestManager> ui_test_manager_;
  std::unique_ptr<sys::ServiceDirectory> realm_exposed_services_;
  std::unique_ptr<Realm> realm_;

  std::unique_ptr<ResponseListenerServer> response_listener_;

  fuchsia::input::injection::InputDeviceRegistryPtr registry_;
  std::unique_ptr<fake_input_report_device::FakeInputDevice> fake_input_device_;
  fuchsia::input::report::InputDevicePtr input_device_ptr_;

  int injection_count_ = 0;

  fuchsia::ui::scenic::ScenicPtr scenic_;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;

  test::accessibility::MagnifierSyncPtr fake_magnifier_;

  static constexpr auto kCppGfxClient = "gfx_client";
  static constexpr auto kCppGfxClientUrl = "#meta/touch-gfx-client.cm";
};

// Declare test data.
// In all these tests, we tap the center of the top left quadrant of the
// physical display (after rotation), and verify that the client view gets a
// pointer event with the expected coordinates.

// No changes to display rotation or clip space
constexpr PointerInjectorConfigTestData kTestDataBaseCase = {
    .display_rotation = 0, .expected_x = 1.f / 4.f, .expected_y = 1.f / 4.f};

// Test scale by a factor of 2.
// Intuitive argument for these expected coordinates:
// Here we've zoomed into the center of the client view, scaling it up by 2x. So, the touch point
// will have 'migrated' halfway towards the center of the client view:  3/8 instead of 1/4.
constexpr PointerInjectorConfigTestData kTestDataScale = {
    .display_rotation = 0, .clip_scale = 2.f, .expected_x = 3.f / 8.f, .expected_y = 3.f / 8.f};

// Test display rotation by 90 degrees.
// In this case, rotation shouldn't affect what the client view sees.
constexpr PointerInjectorConfigTestData kTestDataRotateAndScale = {
    .display_rotation = 90, .clip_scale = 2.f, .expected_x = 3.f / 8.f, .expected_y = 3.f / 8.f};

// Test scaling and translation.
constexpr float kScale = 3.f;
constexpr float kTranslationX = -0.2f;
constexpr float kTranslationY = 0.1f;
constexpr PointerInjectorConfigTestData kTestDataScaleAndTranslate = {
    .display_rotation = 0,
    .clip_scale = kScale,
    .clip_translation_x = kTranslationX,
    .clip_translation_y = kTranslationY,
    // Terms: 'Original position' + 'movement due to scale' + 'movement due to translation'
    .expected_x = 0.25f + 0.25f * (1.f - 1.f / kScale) - kTranslationX / 2.f / kScale,
    .expected_y = 0.25f + 0.25f * (1.f - 1.f / kScale) - kTranslationY / 2.f / kScale};

// Test scaling, translation, and rotation at once.
//
// Here, the translation does affect what the client view sees, so we have to account for it.
// This is what the translation looks like in client view coordinates, where it's rotated 90
// degrees.
constexpr float kClientViewTranslationX = kTranslationY;
constexpr float kClientViewTranslationY = -kTranslationX;
constexpr PointerInjectorConfigTestData kTestDataScaleTranslateRotate = {
    .display_rotation = 90,
    .clip_scale = kScale,
    .clip_translation_x = kTranslationX,
    .clip_translation_y = kTranslationY,
    // Same formula as before, but with different transform values.
    .expected_x = 0.25f + 0.25f * (1.f - 1.f / kScale) - kClientViewTranslationX / 2.f / kScale,
    .expected_y = 0.25f + 0.25f * (1.f - 1.f / kScale) - kClientViewTranslationY / 2.f / kScale};

INSTANTIATE_TEST_SUITE_P(
    PointerInjectorConfigTestWithParams, PointerInjectorConfigTest,
    ::testing::Combine(::testing::Values(ui_testing::UITestRealm::SceneOwnerType::ROOT_PRESENTER,
                                         ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER),
                       ::testing::Values(kTestDataBaseCase, kTestDataScale, kTestDataRotateAndScale,
                                         kTestDataScaleAndTranslate,
                                         kTestDataScaleTranslateRotate)));

TEST_P(PointerInjectorConfigTest, CppGfxClientTapTest) {
  auto [scene_owner, test_data] = GetParam();

  FX_LOGS(INFO) << "Starting test with params: display_rotation=" << test_data.display_rotation
                << ", clip_scale=" << test_data.clip_scale
                << ", clip_translation_x=" << test_data.clip_translation_x
                << ", clip_translation_y=" << test_data.clip_translation_y
                << ", expected_x=" << test_data.expected_x
                << ", expected_y=" << test_data.expected_y;

  // Use `ZX_CLOCK_MONOTONIC` to avoid complications due to wall-clock time changes.
  zx::basic_time<ZX_CLOCK_MONOTONIC> input_injection_time(0);

  SetClipSpaceTransform(test_data.clip_scale, test_data.clip_translation_x,
                        test_data.clip_translation_y);

  TryInjectRepeatedly(TapLocation::kTopLeft, input_injection_time);

  switch (test_data.display_rotation) {
    case 0:
      WaitForAResponseMeetingExpectations(
          /*expected_x=*/display_width() * test_data.expected_x,
          /*expected_y=*/display_height() * test_data.expected_y,
          /*component_name=*/"touch-gfx-client");
      break;
    case 90:
      WaitForAResponseMeetingExpectations(
          /*expected_x=*/display_height() * test_data.expected_x,
          /*expected_y=*/display_width() * test_data.expected_y,
          /*component_name=*/"touch-gfx-client");
      break;
    default:
      FX_NOTREACHED();
  }

  RunLoop();
}

}  // namespace
