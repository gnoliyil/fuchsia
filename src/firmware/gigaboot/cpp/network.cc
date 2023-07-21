// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "network.h"

#include <efi/protocol/managed-network.h>
#include <gigaboot/cpp/utils.h>

namespace gigaboot {

namespace {

// Given an efi_mac_addr, return a 6 byte octet "real" MAC address.
MacAddr mac_addr_from_efi_mac_addr(efi_mac_addr const& addr) {
  MacAddr eth;
  static_assert(sizeof(addr.addr) >= eth.size(), "Error: source MAC is too small");
  std::copy(std::cbegin(addr.addr), std::cbegin(addr.addr) + eth.size(), std::begin(eth));
  return eth;
}

// Given a "real" MAC address, return an efi_mac_addr.
efi_mac_addr efi_mac_addr_from_mac_addr(MacAddr addr) {
  efi_mac_addr eth = {};
  static_assert(addr.size() <= sizeof(eth.addr), "Error: source MAC is too large");
  std::copy(addr.cbegin(), addr.cend(), std::begin(eth.addr));
  return eth;
}

// Returns the first functional network interface found, configured to support unicast receive,
// or nullptr if there is no such interface.
EfiProtocolPtr<efi_managed_network_protocol> FindNetworkIntf() {
  auto managed_handles = EfiLocateHandleBufferByProtocol<efi_managed_network_protocol>();
  if (managed_handles.is_error()) {
    return nullptr;
  }

  for (auto handle : managed_handles->AsSpan()) {
    auto managed_proto = EfiOpenProtocol<efi_managed_network_protocol>(handle);
    if (managed_proto.is_error()) {
      continue;
    }

    efi_managed_network_config_data mnp_config_data;
    efi_simple_network_mode snp_mode_data;
    if (managed_proto->GetModeData(managed_proto.value().get(), &mnp_config_data, &snp_mode_data) !=
        EFI_SUCCESS) {
      continue;
    }

    if (!snp_mode_data.MediaPresent) {
      continue;
    }

    // Note: this managed network instance may already have been started by the TCP6 stack.
    // If that's the case, it should already have unicast receive enabled, and we don't
    // want to stop traffic to enable it.
    if (!mnp_config_data.EnableUnicastReceive) {
      if (managed_proto->Configure(managed_proto.value().get(), nullptr) != EFI_SUCCESS) {
        continue;
      }

      mnp_config_data.EnableUnicastReceive = true;
      if (managed_proto->Configure(managed_proto.value().get(), &mnp_config_data) != EFI_SUCCESS) {
        continue;
      }
    }

    return std::move(managed_proto.value());
  }

  return nullptr;
}

}  // namespace

fit::result<efi_status, EthernetAgent> EthernetAgent::Create() {
  EfiProtocolPtr<efi_managed_network_protocol> mnp = FindNetworkIntf();
  if (!mnp) {
    return fit::error(EFI_NO_MEDIA);
  }

  efi_managed_network_config_data mnp_data;
  efi_simple_network_mode snp_data;
  if (efi_status res = mnp->GetModeData(mnp.get(), &mnp_data, &snp_data); res != EFI_SUCCESS) {
    return fit::error(res);
  }

  MacAddr mac_addr = mac_addr_from_efi_mac_addr(snp_data.CurrentAddress);
  return fit::ok(EthernetAgent(std::move(mnp), mac_addr));
}

fit::result<efi_status> EthernetAgent::SendV6Frame(
    MacAddr dst, cpp20::span<const uint8_t> data, efi_event callback,
    efi_managed_network_sync_completion_token* token) {
  if (!token) {
    return fit::error(EFI_INVALID_PARAMETER);
  }

  // Note: the efi_mac_addr struct is a 32 byte array, while real MAC addresses are 6 octets.
  // There is no obvious explanation for this discrepancy in the UEFI docs.
  // It's much more convenient to deal with normal sized MAC addresses, though,
  // so we convert to efi_mac_addr only when necessary.
  efi_mac_addr dest = efi_mac_addr_from_mac_addr(dst);
  efi_mac_addr src = efi_mac_addr_from_mac_addr(mac_addr_);
  efi_managed_network_transmit_data tx_data = {
      .DestinationAddress = &dest,
      .SourceAddress = &src,
      .ProtocolType = 0x86DD,  // IPv6 EtherType
      .DataLength = static_cast<uint32_t>(data.size()),
      .HeaderLength = 0,
      .FragmentCount = 1,
      .FragmentTable =
          {
              {
                  .FragmentLength = static_cast<uint32_t>(data.size()),
                  .FragmentBuffer = const_cast<uint8_t*>(data.data()),
              },
          },
  };

  *token = {.Event = callback, .Packet = {.TxData = &tx_data}};
  if (efi_status res = net_proto_->Transmit(net_proto_.get(), token); res != EFI_SUCCESS) {
    return fit::error(res);
  }

  return fit::ok();
}

}  // namespace gigaboot
