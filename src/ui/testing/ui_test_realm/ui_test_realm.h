// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_TESTING_UI_TEST_REALM_UI_TEST_REALM_H_
#define SRC_UI_TESTING_UI_TEST_REALM_UI_TEST_REALM_H_

#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/sys/cpp/service_directory.h>

#include <memory>
#include <optional>

#include "src/lib/fxl/macros.h"

namespace ui_testing {

// DPR constants.
constexpr auto kDefaultDevicePixelRatio = 1.f;
constexpr auto kMediumResolutionDevicePixelRatio = 1.25f;
constexpr auto kHighResolutionDevicePixelRatio = 2.f;

// Library class to manage test realm on behalf of UI integration test clients.
//
// TEST REALM
//
// UITestRealm own a RealmBuilder realm encapsulating the relevant portion of
// the UI stack. The realm comprises two main parts:
//
//   (1) The ui layer component. This component runs the portion of the UI stack
//       specified by the client via the UITestRealm::Config argument passed
//       to the UITestRealm constructor. This portion of the realm is
//       (mostly) specified statically in //src/ui/testing/ui_test_manager/meta.
//   (2) The client subrealm. This subrealm is a RealmBuilder Realm, owned and
//       configured by the client, containing any additional test-specific
//       components.
//
// The component topology of the test is:
//
//                        test_manager
//                       /            \
//         test fixture component      realm builder server
//                   /
//          ui test realm root
//           /             \
//   client subrealm     (ui layer components)
//          |
//    (test-specific
//      components)
//
// Clients can configure the scene owner, which specifies which ui layer
// component to use (layers are specified statically in
// //src/ui/testing/ui_test_manager/meta). Clients can also specify the set of
// ui services that must be routed to the client subrealm, and the set of client
// services that must be exposed out of the top-level realm. UITestRealm will
// configure all necessary routes between the ui layer component, the client
// subrealm, and the top-level realm.
//
// CLIENT SUBREALM
//
// The client can specify the test-specific portion of the component topology
// within its own subrealm (see AddSubrealm() below). Important notes on the client
// subrealm:
//
//   (1) A client subrealm should NOT contain any UI services (scenic, scene
//       manager, text manager, or a11y manager).
//   (2) A client MUST expose fuchsia.ui.app.ViewProvider from its subrealm if
//       specifies a scene owner.
//   (3) Clients can consume required ui services from ParentRef(), provided
//       they request those services in Config::ui_to_client_services.
//
// INPUT
//
// UITestManager enables configurations with or without input.
//
// * If Config::use_scene_owner = true and Config::use_input = true, then
//   UITestManager assumes input pipeline will own input for the test scene
//   (via scene manager).
// * If Config::use_scene_owner = false and Config::use_input = true, then
//   UITestManager will expose raw scenic input APIs out of the test realm.
// * If clients set Config::use_input = false, then UITestManager will not
//   any input APIs out of the test realm.
//
// ACCESSIBILITY
//
// UITestManager enables configurations without accessibility, and also allows
// clients to opt into using a real or fake a11y manager. In general, clients
// should not request accessibility unless it's explicitly required. For cases
// where accessibility is required, clients should prefer using the fake a11y
// manager for tests that require a11y services, but do not test a11y
// functionality (e.g. tests that run a chromium view). Clients should only use
// a real a11y manager for tests that explicitly exercise accessibility-specific
// behavior.
class UITestRealm {
 public:
  enum class AccessibilityOwnerType {
    // Use the fake a11y manager. Clients should prefer using the fake a11y
    // manager for tests that require a11y services, but do not test a11y
    // functionality (e.g. tests that run a chromium client).
    FAKE = 1,

    // Use the real a11y manager. Clients should only use the real a11y manager
    // for tests that exercise accessibility-specific functionality.
    REAL = 2,
  };

  struct Config {
    // Specifies whether scene manager owns the root of the scene.
    // If false, then no scene owner will be present in the test realm.
    //
    // UITestManager assumes that scene manager serves input via input pipeline
    // in addition to managing scene ownership. Therefore it exposes the
    // following services out of the test from the top-level realm:
    //   * fuchsia.input.injection.InputDeviceRegistry
    //   * fuchsia.ui.policy.DeviceListenerRegistry
    //   * fuchsia.ui.pointerinjector.configuration.Setup
    //
    // Furthermore, if |use_scene_owner| is true, the client promises to expose
    // fuchsia.ui.app.ViewProvider from its subrealm.
    //
    // If |use_scene_owner| is false, the top-level realm exposes the raw scenic
    // input API:
    //   * fuchsia.ui.pointerinjector.Registry
    bool use_scene_owner = false;

    // Specifies the entity that owns accessibility in the test realm, if any.
    // If std::nullopt, then no a11y services will be present in the test realm.
    std::optional<AccessibilityOwnerType> accessibility_owner;

    // List of ui services required by components in the client subrealm.
    // UITestManager will route these services from the ui layer component to the
    // client subrealm.
    std::vector<std::string> ui_to_client_services;

    // List of capabilities to pass-through from the parent to the client subrealm.
    std::vector<component_testing::Capability> passthrough_capabilities;

    // List of non-ui services the test manager needs to expose to the test fixture.
    // By specifying services here, the client promises to expose them from its subrealm.
    std::vector<std::string> exposed_client_services;

    // List of client realm services to route to the ui layer component.
    //
    // *** Use cases for this field are ~very~ rare.
    // *** This optoin will NOT be available to OOT clients.
    std::vector<std::string> client_to_ui_services;

    // Clockwise display rotation, in degrees. Display rotation MUST be a multiple of 90 degrees.
    int display_rotation = 0;

    // Device pixel ratio for the fake display.
    //
    // Must result in integer logical display dimensions.
    float device_pixel_ratio = kDefaultDevicePixelRatio;

    // Indicates which graphics composition API to use (true -> flatland, false
    // -> gfx).
    bool use_flatland = false;

    // Idle threshold minutes for the activity service.
    int idle_threshold_minutes = 1;
  };

  explicit UITestRealm(Config config);
  ~UITestRealm() = default;

  // Adds a child to the realm under construction, and returns the new child.
  // Must NOT be called after BuildRealm().
  component_testing::Realm AddSubrealm();

  // Calls realm_builder_.Build();
  void Build();

  // Calls realm_builder_.Teardown();
  void Teardown(component_testing::ScopedChild::TeardownCallback on_teardown_complete);

  // Returns a clone of the realm's exposed services directory.
  // Clients should call this method once, and retain the handle returned.
  //
  // MUST be called AFTER Build().
  std::unique_ptr<sys::ServiceDirectory> CloneExposedServicesDirectory();

  const std::optional<component_testing::RealmRoot>& realm_root() const { return realm_root_; }

  const Config& config() { return config_; }

 private:
  // Helper methods to configure the test realm.
  void ConfigureClientSubrealm();
  void ConfigureAccessibility();
  void RouteConfigData();
  void ConfigureSceneOwner();

  // Helper method to route a set of services from the specified source to the
  // spceified targets.
  void RouteServices(std::vector<std::string> services, component_testing::Ref source,
                     std::vector<component_testing::Ref> targets);

  // Helper method to determine the component url used to instantiate the base
  // UI realm.
  std::string CalculateBaseRealmUrl();

  Config config_;
  component_testing::RealmBuilder realm_builder_ =
      component_testing::RealmBuilder::CreateFromRelativeUrl(CalculateBaseRealmUrl());
  std::optional<component_testing::RealmRoot> realm_root_;

  // Some tests may not need a dedicated subrealm. Those clients will not call
  // AddSubrealm(), so UITestManager will crash if it tries to add routes
  // to/from the missing subrealm.
  //
  // NOTE: This piece of state is temporary, and can be removed once the client
  // owns a full RealmBuilder instance, as opposed to a child realm.
  bool has_client_subrealm_ = false;

  // Add state as necessary.

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(UITestRealm);
};

}  // namespace ui_testing

#endif  // SRC_UI_TESTING_UI_TEST_REALM_UI_TEST_REALM_H_
