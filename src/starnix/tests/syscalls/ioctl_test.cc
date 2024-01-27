// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

namespace {

constexpr char kLoopbackIfName[] = "lo";
constexpr char kUnknownIfName[] = "unknown";

class IoctlTest : public ::testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  }

 protected:
  fbl::unique_fd fd;
};

struct IoctlInvalidTestCase {
  uint16_t req;
  uint16_t family;
  const char* name;
  uint8_t data;
  int expected_errno;
};

class IoctlInvalidTest : public IoctlTest,
                         public ::testing::WithParamInterface<IoctlInvalidTestCase> {};

TEST_P(IoctlInvalidTest, InvalidRequest) {
  const auto [req, family, name, data, expected_errno] = GetParam();

  ifreq ifr;
  ifr.ifr_addr = {.sa_family = family}, ifr.ifr_addr.sa_data[0] = data;
  strncpy(ifr.ifr_name, name, IFNAMSIZ);

  ASSERT_EQ(ioctl(fd.get(), req, &ifr), -1);
  EXPECT_EQ(errno, expected_errno);
}

INSTANTIATE_TEST_SUITE_P(IoctlInvalidTest, IoctlInvalidTest,
                         ::testing::Values(
                             // TODO(https://fxbug.dev/129547): Check for ENODEV.
                             IoctlInvalidTestCase{
                                 .req = SIOCGIFADDR,
                                 .family = AF_INET,
                                 .name = kUnknownIfName,
                                 .data = 0,
                                 .expected_errno = ENOENT,
                             },
                             IoctlInvalidTestCase{
                                 .req = SIOCGIFADDR,
                                 .family = AF_INET6,
                                 .name = kLoopbackIfName,
                                 .data = 0,
                                 .expected_errno = EINVAL,
                             },
                             // TODO(https://fxbug.dev/129547): Check for ENODEV.
                             IoctlInvalidTestCase{
                                 .req = SIOCSIFADDR,
                                 .family = AF_INET,
                                 .name = kUnknownIfName,
                                 .data = 0,
                                 .expected_errno = ENOENT,
                             },
                             IoctlInvalidTestCase{
                                 .req = SIOCSIFADDR,
                                 .family = AF_INET6,
                                 .name = kLoopbackIfName,
                                 .data = 0,
                                 .expected_errno = EINVAL,
                             }));

void GetIfAddr(fbl::unique_fd& fd, in_addr_t expected_addr) {
  ifreq ifr;
  ifr.ifr_addr = {.sa_family = AF_INET}, strncpy(ifr.ifr_name, kLoopbackIfName, IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFADDR, &ifr), 0) << strerror(errno);

  EXPECT_EQ(strncmp(ifr.ifr_name, kLoopbackIfName, IFNAMSIZ), 0);
  sockaddr_in* s = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
  EXPECT_EQ(s->sin_family, AF_INET);
  EXPECT_EQ(s->sin_port, 0);
  EXPECT_EQ(ntohl(s->sin_addr.s_addr), expected_addr);
}

TEST_F(IoctlTest, SIOCGIFADDR_Success) { ASSERT_NO_FATAL_FAILURE(GetIfAddr(fd, INADDR_LOOPBACK)); }

void SetIfAddr(fbl::unique_fd& fd, in_addr_t addr) {
  ifreq ifr;
  *(reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)) = sockaddr_in{
      .sin_family = AF_INET,
      .sin_addr = {.s_addr = addr},
  };
  strncpy(ifr.ifr_name, kLoopbackIfName, IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCSIFADDR, &ifr), 0) << strerror(errno);

  ASSERT_NO_FATAL_FAILURE(GetIfAddr(fd, addr));
}

TEST_F(IoctlTest, SIOCSIFADDR_Success) {
  ASSERT_NO_FATAL_FAILURE(SetIfAddr(fd, INADDR_ANY));
  ASSERT_NO_FATAL_FAILURE(SetIfAddr(fd, INADDR_LOOPBACK));
}

}  // namespace
