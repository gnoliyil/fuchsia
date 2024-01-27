// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_TESTS_UTILS_SCENIC_REALM_BUILDER_H_
#define SRC_UI_SCENIC_TESTS_UTILS_SCENIC_REALM_BUILDER_H_

#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>

namespace integration_tests {

using ProtocolName = std::string;

// Configs required to launch a client which exposes |fuchsia.ui.app.ViewProvider|.
struct ViewProviderConfig {
  // Name of the ViewProvider component.
  std::string name;

  // URL for the manifest of the component.
  std::string component_url;
};

struct RealmBuilderArgs {
  bool use_flatland = true;
  std::optional<ViewProviderConfig> view_provider_config;
};

// Helper class for building a scenic realm. The scenic realm consists of three
// components:
//   * Scenic
//   * Mock Cobalt
//   * Fake Display Provider
// This class sets up the component topology and routes protocols between the test
// manager and its child components.
//
// The realm builder library is used to construct a realm during runtime with a
// topology as follows:
//       test_manager
//            |
//     <test component>
//            |
//       <realm root>
//            |          <-Test realm
// ----------------------------
//     /      |     \    <-Scenic realm
//  Scenic  Cobalt  Hdcp
class ScenicRealmBuilder {
 public:
  ScenicRealmBuilder(RealmBuilderArgs args = {});

  // Routes |protocol| from the realm root returned by |Build()| to the test fixtures
  // component. Should be used only for the protocols which are required by the test component.
  // |protocol| must be exposed by one of the components inside the scenic realm.
  ScenicRealmBuilder& AddRealmProtocol(const ProtocolName& protocol);

  // Builds the realm with the provided components and routes and returns the realm root.
  component_testing::RealmRoot Build();

 private:
  // Adds child components in the scenic realm and routes required protocols from the
  // test_manager to the realm.
  ScenicRealmBuilder& Init(RealmBuilderArgs args);

  component_testing::RealmBuilder realm_builder_;
};

}  // namespace integration_tests

#endif  // SRC_UI_SCENIC_TESTS_UTILS_SCENIC_REALM_BUILDER_H_
