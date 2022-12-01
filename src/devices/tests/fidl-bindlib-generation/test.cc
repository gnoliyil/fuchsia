// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/device-watcher/cpp/device-watcher.h>

#include <gtest/gtest.h>

TEST(SimpleDriverTestRealmTest, DriversExist) {
  ASSERT_EQ(
      device_watcher::RecursiveWaitForFile("/dev/sys/test/fidl_bindlib_generation").status_value(),
      ZX_OK);
  ASSERT_EQ(device_watcher::RecursiveWaitForFile("/dev/sys/test/fidl_bindlib_generation/child")
                .status_value(),
            ZX_OK);
}
