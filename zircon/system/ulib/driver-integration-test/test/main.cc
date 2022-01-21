// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <lib/ddk/platform-defs.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/status.h>

#include <sdk/lib/device-watcher/cpp/device-watcher.h>
#include <zxtest/zxtest.h>

namespace {

using device_watcher::RecursiveWaitForFile;
using driver_integration_test::IsolatedDevmgr;

const board_test::DeviceEntry kDeviceEntry = []() {
  board_test::DeviceEntry entry = {};
  strcpy(entry.name, "test-device");
  entry.vid = PDEV_VID_TEST;
  entry.pid = PDEV_PID_TEST;
  return entry;
}();

TEST(DriverIntegrationTest, EnumerationTest) {
  IsolatedDevmgr::Args args;
  args.device_list.push_back(kDeviceEntry);

  IsolatedDevmgr devmgr;
  ASSERT_OK(IsolatedDevmgr::Create(&args, &devmgr));

  fbl::unique_fd fd;
  ASSERT_OK(RecursiveWaitForFile(devmgr.devfs_root(), "sys/platform", &fd));

  EXPECT_OK(RecursiveWaitForFile(devmgr.devfs_root(),
                                 "sys/platform/platform-passthrough/test-board", &fd));

  EXPECT_OK(RecursiveWaitForFile(devmgr.devfs_root(), "sys/platform/11:18:0/test-device", &fd));
}

}  // namespace
