// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_host/driver_host.h"

#include <lib/async-loop/default.h>

#include <zxtest/zxtest.h>

#include "src/devices/bin/driver_host/driver_host_context.h"
#include "src/devices/bin/driver_host/zx_device.h"

namespace {

TEST(DriverHostTest, MkDevpath) {
  DriverHostContext ctx(&kAsyncLoopConfigNoAttachToCurrentThread);
  fbl::RefPtr<zx_driver> drv;
  ASSERT_OK(zx_driver::Create("test", ctx.inspect().drivers(), &drv));

  auto driver = Driver::Create(drv.get());
  ASSERT_OK(driver.status_value());

  fbl::RefPtr<zx_device> dev;
  constexpr char device_name[] = "device-name";

  ASSERT_OK(zx_device::Create(&ctx, device_name, *std::move(driver), &dev));
  dev->vnode.reset();

  auto result = mkdevpath(*dev, nullptr, 0);
  EXPECT_STREQ("", result);

  std::vector<char> buf;
  result = mkdevpath(*dev, buf.data(), buf.size());
  EXPECT_STREQ("", result);

  buf.resize(sizeof(device_name));
  result = mkdevpath(*dev, buf.data(), buf.size());
  EXPECT_STREQ(device_name, result);

  buf.resize(sizeof(device_name) * 2);
  result = mkdevpath(*dev, buf.data(), buf.size());
  EXPECT_STREQ(device_name, result);

  buf.resize(sizeof(device_name) / 2);
  result = mkdevpath(*dev, buf.data(), buf.size());
  EXPECT_STREQ("...", result);
}

}  // namespace
