// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests should run without any network interface (except loopback).

#include <arpa/inet.h>
#include <limits.h>
#include <sys/utsname.h>

#include <gtest/gtest.h>

#include "src/connectivity/network/tests/os.h"

namespace {

class NoNetworkTest : public testing::Test {
 protected:
  void SetUp() override {
    if (!kIsFuchsia) {
      GTEST_SKIP() << "NoNetworkTest can only run in a loopback-only environment";
    }
  }
};

TEST_F(NoNetworkTest, NonBlockingConnectHostV4) {
  int connfd;
  ASSERT_GE(connfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), 0) << strerror(errno);

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  ASSERT_EQ(inet_pton(AF_INET, "192.168.0.1", &addr.sin_addr), 1) << strerror(errno);
  addr.sin_port = htons(10000);

  ASSERT_EQ(connect(connfd, (const struct sockaddr*)&addr, sizeof(addr)), -1);
  ASSERT_EQ(errno, EHOSTUNREACH) << strerror(errno);

  ASSERT_EQ(close(connfd), 0) << strerror(errno);
}

TEST_F(NoNetworkTest, NonBlockingConnectHostV6) {
  int connfd;
  ASSERT_GE(connfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0), 0) << strerror(errno);

  struct sockaddr_in6 addr;
  addr.sin6_family = AF_INET6;
  ASSERT_EQ(inet_pton(AF_INET6, "fc00::1", &addr.sin6_addr), 1) << strerror(errno);
  addr.sin6_port = htons(10000);

  ASSERT_EQ(connect(connfd, (const struct sockaddr*)&addr, sizeof(addr)), -1);
  ASSERT_EQ(errno, EHOSTUNREACH) << strerror(errno);

  ASSERT_EQ(close(connfd), 0) << strerror(errno);
}

TEST_F(NoNetworkTest, NonBlockingConnectNetV4) {
  int connfd;
  ASSERT_GE(connfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), 0) << strerror(errno);

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  // multicast address
  ASSERT_EQ(inet_pton(AF_INET, "224.0.0.0", &addr.sin_addr), 1) << strerror(errno);
  addr.sin_port = htons(10000);

  ASSERT_EQ(connect(connfd, (const struct sockaddr*)&addr, sizeof(addr)), -1);
  ASSERT_EQ(errno, ENETUNREACH) << strerror(errno);

  ASSERT_EQ(close(connfd), 0) << strerror(errno);
}

TEST_F(NoNetworkTest, NonBlockingConnectNetV6) {
  int connfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(connfd, 0) << "socket failed: " << strerror(errno);

  struct sockaddr_in6 addr;
  addr.sin6_family = AF_INET6;
  // linklocal address
  ASSERT_EQ(inet_pton(AF_INET6, "fe80::1", &addr.sin6_addr), 1) << strerror(errno);
  addr.sin6_port = htons(10000);

  ASSERT_EQ(connect(connfd, (const struct sockaddr*)&addr, sizeof(addr)), -1);
  ASSERT_EQ(errno, ENETUNREACH) << strerror(errno);

  ASSERT_EQ(close(connfd), 0) << strerror(errno);
}

TEST_F(NoNetworkTest, SendToNetV4) {
  int fd;
  ASSERT_TRUE(fd = socket(AF_INET, SOCK_DGRAM, 0)) << strerror(errno);
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  // unroutable (documentation-only) address
  inet_pton(AF_INET, "203.0.113.1", &addr.sin_addr);
  addr.sin_port = htons(10000);
  char bytes[1];
  EXPECT_EQ(
      sendto(fd, bytes, sizeof(bytes), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
      -1);
  EXPECT_EQ(errno, ENETUNREACH) << strerror(errno);
}

}  // namespace
