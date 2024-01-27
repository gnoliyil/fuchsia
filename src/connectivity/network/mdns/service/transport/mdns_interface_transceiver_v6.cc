// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/network/mdns/service/transport/mdns_interface_transceiver_v6.h"

#include <arpa/inet.h>
#include <errno.h>
#include <lib/syslog/cpp/macros.h>
#include <sys/socket.h>

#include "src/connectivity/network/mdns/service/common/mdns_addresses.h"

namespace mdns {

MdnsInterfaceTransceiverV6::MdnsInterfaceTransceiverV6(inet::IpAddress address,
                                                       const std::string& name, uint32_t id,
                                                       Media media)
    : MdnsInterfaceTransceiver(address, name, id, media) {}

MdnsInterfaceTransceiverV6::~MdnsInterfaceTransceiverV6() {}

int MdnsInterfaceTransceiverV6::SetOptionDisableMulticastLoop() {
  int param = 0;
  int result =
      setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &param, sizeof(param));
  if (result < 0) {
    if (errno == ENOPROTOOPT) {
      FX_LOGS(WARNING) << "fxbug.dev/20688 IPV6_MULTICAST_LOOP not supported "
                          "(ENOPROTOOPT), continuing anyway";
      result = 0;
    } else {
      FX_LOGS(ERROR) << "Failed to set socket option IPV6_MULTICAST_LOOP, " << strerror(errno);
    }
  }

  return result;
}

int MdnsInterfaceTransceiverV6::SetOptionJoinMulticastGroup() {
  ipv6_mreq param;
  param.ipv6mr_multiaddr = MdnsAddresses::v6_multicast().as_sockaddr_in6().sin6_addr;
  param.ipv6mr_interface = id();
  int result = setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_JOIN_GROUP, &param, sizeof(param));
  if (result < 0) {
    if (errno == ENODEV) {
      FX_LOGS(WARNING) << "fxbug.dev/21173 IPV6_JOIN_GROUP returned ENODEV, mDNS will "
                          "not communicate via IPV6";
    } else {
      FX_LOGS(ERROR) << "Failed to set socket option IPV6_JOIN_GROUP, " << strerror(errno);
    }
  }

  return result;
}

int MdnsInterfaceTransceiverV6::SetOptionOutboundInterface() {
  uint32_t id = this->id();
  int result = setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_MULTICAST_IF, &id, sizeof(id));
  if (result < 0) {
    if (errno == EADDRNOTAVAIL) {
      // This is expected when the interface is removed as we try to use it. We still return
      // result < 0, because we don't want to use this interface.
      FX_LOGS(WARNING) << "Failed to set socket option IPV6_MULTICAST_IF, " << strerror(errno);
    } else {
      FX_LOGS(ERROR) << "Failed to set socket option IPV6_MULTICAST_IF, " << strerror(errno);
    }
  }

  return result;
}

int MdnsInterfaceTransceiverV6::SetOptionUnicastTtl() {
  int param = kTimeToLive_;
  int result =
      setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &param, sizeof(param));
  if (result < 0) {
    if (errno == ENOPROTOOPT) {
      // TODO(fxbug.dev/41357): remove the bug reference when the bug is fixed.
      FX_LOGS(WARNING)
          << "fxbug.dev/41357: IPV6_UNICAST_HOPS not supported (ENOPROTOOPT), continuing anyway";
      result = 0;
    } else {
      FX_LOGS(ERROR) << "Failed to set socket option IPV6_UNICAST_HOPS, " << strerror(errno);
    }
  }

  return result;
}

int MdnsInterfaceTransceiverV6::SetOptionMulticastTtl() {
  int param = kTimeToLive_;
  int result =
      setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &param, sizeof(param));
  if (result < 0) {
    if (errno == ENOPROTOOPT) {
      FX_LOGS(WARNING) << "IPV6_MULTICAST_HOPS not supported (ENOPROTOOPT), continuing anyway";
      result = 0;
    } else {
      FX_LOGS(ERROR) << "Failed to set socket option IPV6_MULTICAST_HOPS, " << strerror(errno);
    }
  }

  return result;
}

int MdnsInterfaceTransceiverV6::SetOptionFamilySpecific() {
  // Receive V6 packets only.
  int param = 1;
  int result = setsockopt(socket_fd().get(), IPPROTO_IPV6, IPV6_V6ONLY, &param, sizeof(param));
  if (result < 0) {
    FX_LOGS(ERROR) << "Failed to set socket option IPV6_V6ONLY, " << strerror(errno);
    return false;
  }

  return result;
}

int MdnsInterfaceTransceiverV6::Bind() {
  int result = bind(socket_fd().get(), MdnsAddresses::v6_bind().as_sockaddr(),
                    MdnsAddresses::v6_bind().socklen());
  if (result < 0) {
    FX_LOGS(ERROR) << "Failed to bind socket to V6 address, " << strerror(errno);
  }

  return result;
}

ssize_t MdnsInterfaceTransceiverV6::SendTo(const void* buffer, size_t size,
                                           const inet::SocketAddress& address) {
  if (address == MdnsAddresses::v4_multicast()) {
    // |v4_multicast| indicates multicast, meaning V6 multicast in this case.
    return sendto(socket_fd().get(), buffer, size, 0, MdnsAddresses::v6_multicast().as_sockaddr(),
                  MdnsAddresses::v6_multicast().socklen());
  }

  return sendto(socket_fd().get(), buffer, size, 0, address.as_sockaddr(), address.socklen());
}

}  // namespace mdns
