// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/ddk/platform-defs.h>
#include <lib/driver/devicetree/testing/board-test-helper.h>

#include <zxtest/zxtest.h>

namespace example_board {

namespace {

const zbi_platform_id_t kPlatformId = []() {
  zbi_platform_id_t plat_id = {};
  plat_id.vid = PDEV_VID_TEST;
  plat_id.pid = PDEV_PID_TEST;
  strcpy(plat_id.board_name, "example-devicetree");
  return plat_id;
}();

}

class ExampleBoardTest : public zxtest::Test {
 public:
  ExampleBoardTest()
      : board_test_("/pkg/test-data/basic-properties.dtb", kPlatformId, loop_.dispatcher()) {
    loop_.StartThread("test-realm");
    board_test_.SetupRealm();
  }

 protected:
  async::Loop loop_{&kAsyncLoopConfigNoAttachToCurrentThread};
  fdf_devicetree::testing::BoardTestHelper board_test_;
};

TEST_F(ExampleBoardTest, DevicetreeEnumeration) {
  std::vector<std::string> device_node_paths = {
      "sys/platform/pt",
      "sys/platform/00:00:32",
      "sys/platform/00:00:32:1",
  };
  ASSERT_TRUE(board_test_.StartRealm().is_ok());
  ASSERT_TRUE(board_test_.WaitOnDevices(device_node_paths).is_ok());
}

}  // namespace example_board
