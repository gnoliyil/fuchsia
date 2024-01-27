// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.driver.test/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

TEST(DdkFallbackTest, DriverWasLoaded) {
  fbl::unique_fd dev(open("/dev", O_RDONLY));
  ASSERT_TRUE(dev);

  zx::result channel =
      device_watcher::RecursiveWaitForFile(dev.get(), "sys/test/ddk-fallback-test-device-0");
  ASSERT_EQ(channel.status_value(), ZX_OK);
}

int main(int argc, char **argv) {
  // Setup DriverTestRealm.
  auto client_end = component::Connect<fuchsia_driver_test::Realm>();
  if (!client_end.is_ok()) {
    return 1;
  }
  fidl::WireSyncClient client{std::move(*client_end)};

  fidl::Arena allocator;
  auto response = client->Start(fuchsia_driver_test::wire::RealmArgs(allocator));
  if (response.status() != ZX_OK) {
    return 1;
  }
  if (response->is_error()) {
    return 1;
  }

  // Run the tests.
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
