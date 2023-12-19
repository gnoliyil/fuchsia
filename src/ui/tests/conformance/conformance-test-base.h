// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_TESTS_CONFORMANCE_CONFORMANCE_TEST_BASE_H_
#define SRC_UI_TESTS_CONFORMANCE_CONFORMANCE_TEST_BASE_H_

#include <fuchsia/testing/harness/cpp/fidl.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/zx/channel.h>

#include <cstdlib>
#include <iostream>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace ui_conformance_test_base {

/// ConformanceTest use realm_proxy to connect test realm.
class ConformanceTest : public gtest::RealLoopFixture {
 public:
  ConformanceTest() = default;
  ~ConformanceTest() override = default;

  /// SetUp connect test realm so test can use realm_proxy_ to access.
  void SetUp() override;

  /// Connect to the FIDL protocol which served from the realm proxy use default served path if no
  /// name passed in.
  template <typename Interface>
  fidl::SynchronousInterfacePtr<Interface> ConnectSyncIntoRealm(
      const std::string& service_path = Interface::Name_) {
    fidl::SynchronousInterfacePtr<Interface> ptr;

    fuchsia::testing::harness::RealmProxy_ConnectToNamedProtocol_Result result;
    if (realm_proxy_->ConnectToNamedProtocol(service_path, ptr.NewRequest().TakeChannel(),
                                             &result) != ZX_OK) {
      std::cerr << "ConnectToNamedProtocol(" << service_path << ", " << Interface::Name_
                << ") failed." << std::endl;
      std::abort();
    }
    return std::move(ptr);
  }

 private:
  fuchsia::testing::harness::RealmProxySyncPtr realm_proxy_;
};

}  // namespace ui_conformance_test_base

#endif  // SRC_UI_TESTS_CONFORMANCE_CONFORMANCE_TEST_BASE_H_
