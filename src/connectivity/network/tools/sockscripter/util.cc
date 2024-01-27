// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util.h"

#include <net/if.h>

#include <climits>

#include "netinet/in.h"

// Helper function to convert a string containing a decimal number to an integer using strtol.
bool str2int(const std::string& in_str, int* out_int) {
  if (in_str.empty()) {
    return false;
  }

  if (!std::isdigit(in_str[0]) && in_str[0] != '-') {
    return false;
  }

  long val_long = strtol(in_str.c_str(), nullptr, 10);
  if (val_long == LONG_MAX || val_long == LONG_MIN) {
    return false;
  }

  if (val_long >= INT_MAX || val_long <= INT_MIN) {
    return false;
  }

  *out_int = static_cast<int>(val_long);
  return true;
}

bool getFlagInt(const std::string& in_str, int* out_int) {
  if (in_str.empty()) {
    return false;
  }

  if (in_str[0] == '0') {
    *out_int = 0;
    return true;
  } else if (in_str[0] == '1') {
    *out_int = 1;
    return true;
  }
  return false;
}

const char* GetDomainName(int domain) {
  switch (domain) {
    case AF_INET:
      return "IPv4";
    case AF_INET6:
      return "IPv6";
    default:
      return "UNKNOWN";
  }
}

const char* GetTypeName(int type) {
  switch (type) {
    case SOCK_DGRAM:
      return "DGRAM";
    case SOCK_STREAM:
      return "STREAM";
    case SOCK_RAW:
      return "RAW";
    default:
      return "UNKNOWN";
  }
}

const char* GetProtoName(int proto) {
  switch (proto) {
    case IPPROTO_TCP:
      return "TCP";
    case IPPROTO_UDP:
      return "UDP";
    case IPPROTO_ICMP:
      return "ICMP";
    case IPPROTO_ICMPV6:
      return "ICMPv6";
    default:
      return "UNKNOWN";
  }
}
