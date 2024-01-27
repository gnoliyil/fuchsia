// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>

#include "src/connectivity/network/tests/os.h"
// <net/if.h> doesn't contain the full list of interface flags on Linux.
#if defined(__linux__)
#include <linux/if.h>
#else
#include <net/if.h>
#endif

#include <algorithm>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace {

uint8_t count_prefix(const uint8_t* mask, size_t len) {
  uint8_t l = 0;
  for (size_t i = 0; i < len; i++) {
    auto m = mask[i];
    for (size_t j = 0; j < sizeof(mask); j++) {
      if (m) {
        l++;
        m <<= 1;
      } else {
        return l;
      }
    }
  }
  return l;
}

TEST(GetIfAddrsTest, GetIfAddrsTest) {
  const uint32_t unsupported_flags = IFF_BROADCAST | IFF_DEBUG | IFF_POINTOPOINT | IFF_NOTRAILERS |
                                     IFF_NOARP | IFF_ALLMULTI | IFF_MASTER | IFF_SLAVE |
                                     IFF_MULTICAST | IFF_PORTSEL | IFF_AUTOMEDIA | IFF_DYNAMIC |
                                     IFF_LOWER_UP | IFF_DORMANT | IFF_ECHO;

  // Fields of this tuple are: interface_name, address, prefix_length, scope_id, flags.
  using InterfaceAddress = std::tuple<std::string, std::string, uint8_t, uint32_t, uint32_t>;

  std::vector<InterfaceAddress> want_ifaddrs{
      std::make_tuple("lo", "127.0.0.1", 8, 0, IFF_LOOPBACK | IFF_UP | IFF_RUNNING),
      std::make_tuple("lo", "::1", 128, 0, IFF_LOOPBACK | IFF_UP | IFF_RUNNING),
  };
  const size_t lo_addrs_count = want_ifaddrs.size();

  if (kIsFuchsia) {
    want_ifaddrs.push_back(std::make_tuple("ep1", "192.168.0.1", 20, 0, IFF_UP | IFF_RUNNING));
    want_ifaddrs.push_back(std::make_tuple("ep2", "192.168.0.2", 15, 0, IFF_UP | IFF_RUNNING));
    want_ifaddrs.push_back(std::make_tuple("ep3", "fe80::1", 64, 4, IFF_UP | IFF_RUNNING));
    want_ifaddrs.push_back(std::make_tuple("ep4", "1234::5:6:7:8", 120, 0, IFF_UP | IFF_RUNNING));
  }

  std::vector<InterfaceAddress> unknown_link_local_addrs, other_addrs;
  constexpr size_t link_local_addrs_per_external_interface = 1;
  const size_t want_unknown_link_local_addrs =
      link_local_addrs_per_external_interface * (want_ifaddrs.size() - lo_addrs_count);

  struct ifaddrs* ifaddr;
  ASSERT_EQ(getifaddrs(&ifaddr), 0) << strerror(errno);
  for (auto it = ifaddr; it != nullptr; it = it->ifa_next) {
    const auto if_name = std::string(it->ifa_name);
    // Only loopback is consistent on host environments.
    if (!kIsFuchsia && if_name != "lo") {
      continue;
    }

    switch (it->ifa_addr->sa_family) {
      case AF_INET: {
        struct sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
        char sin_addr_buf[INET_ADDRSTRLEN];
        const char* sin_addr =
            inet_ntop(AF_INET, &addr_in->sin_addr, sin_addr_buf, INET_ADDRSTRLEN);

        const sockaddr_in* netmask = reinterpret_cast<sockaddr_in*>(it->ifa_netmask);
        const uint8_t prefix_len =
            count_prefix(reinterpret_cast<const uint8_t*>(&netmask->sin_addr.s_addr), 4);

        other_addrs.push_back(std::make_tuple(if_name, std::string(sin_addr), prefix_len, 0,
                                              it->ifa_flags & ~unsupported_flags));
        break;
      }
      case AF_INET6: {
        struct sockaddr_in6* addr_in6 = reinterpret_cast<sockaddr_in6*>(it->ifa_addr);
        char sin6_addr_buf[INET6_ADDRSTRLEN];
        const char* sin6_addr =
            inet_ntop(AF_INET6, &(addr_in6->sin6_addr), sin6_addr_buf, INET6_ADDRSTRLEN);

        const sockaddr_in6* netmask = reinterpret_cast<sockaddr_in6*>(it->ifa_netmask);
        const uint8_t prefix_len = count_prefix(netmask->sin6_addr.s6_addr, 16);

        const std::string sin6_addr_str = std::string(sin6_addr);

        const bool is_known_addr = std::any_of(want_ifaddrs.begin(), want_ifaddrs.end(),
                                               [sin6_addr_str](const InterfaceAddress& ifaddr) {
                                                 return std::get<1>(ifaddr) == sin6_addr_str;
                                               });

        InterfaceAddress if_addr =
            std::make_tuple(if_name, sin6_addr_str, prefix_len, addr_in6->sin6_scope_id,
                            it->ifa_flags & ~unsupported_flags);

        if (IN6_IS_ADDR_LINKLOCAL(addr_in6->sin6_addr.s6_addr) && !is_known_addr) {
          unknown_link_local_addrs.push_back(std::move(if_addr));
        } else {
          other_addrs.push_back(std::move(if_addr));
        }

        break;
      }
      case AF_PACKET:
        // Ignore AF_PACKET addresses because raw sockets are not supported on Fuchsia.
        continue;
      default:
        GTEST_FAIL() << "unexpected address family " << it->ifa_addr->sa_family;
    }
  }
  freeifaddrs(ifaddr);

  EXPECT_THAT(other_addrs, testing::UnorderedElementsAreArray(want_ifaddrs));
  EXPECT_THAT(unknown_link_local_addrs, testing::SizeIs(want_unknown_link_local_addrs));
}

}  // namespace
