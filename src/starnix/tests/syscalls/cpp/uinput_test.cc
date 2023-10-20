// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>

#include <gtest/gtest.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "src/starnix/tests/syscalls/cpp/test_helper.h"

namespace {

class UinputTest : public ::testing::Test {
 public:
  void SetUp() override {
    if (getuid() != 0) {
      GTEST_SKIP() << "Can only be run as root.";
    }

    uinput_fd_ = test_helper::ScopedFD(open("/dev/uinput", O_RDWR));
    ASSERT_TRUE(uinput_fd_.is_valid())
        << "open(\"/dev/uinput\") failed: " << strerror(errno) << "(" << errno << ")";
  }

 protected:
  test_helper::ScopedFD uinput_fd_;
};

TEST_F(UinputTest, UiSetEvbit) {
  int res = ioctl(uinput_fd_.get(), UI_SET_EVBIT, EV_KEY);
  EXPECT_EQ(res, -1);
}

TEST_F(UinputTest, UiSetKeybit) {
  int res = ioctl(uinput_fd_.get(), UI_SET_KEYBIT, KEY_SPACE);
  EXPECT_EQ(res, -1);
}

TEST_F(UinputTest, UiDevSetup) {
  struct uinput_setup usetup;
  memset(&usetup, 0, sizeof(usetup));
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor = 0x18d1;
  usetup.id.product = 0x0002;
  strcpy(usetup.name, "Example device");

  int res = ioctl(uinput_fd_.get(), UI_DEV_SETUP, KEY_SPACE);
  EXPECT_EQ(res, -1);
}

TEST_F(UinputTest, UiDevCreate) {
  int res = ioctl(uinput_fd_.get(), UI_DEV_CREATE);
  EXPECT_EQ(res, -1);
}

TEST_F(UinputTest, UiDevDestroy) {
  int res = ioctl(uinput_fd_.get(), UI_DEV_DESTROY);
  EXPECT_EQ(res, -1);
}

}  // namespace
