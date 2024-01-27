// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "emulated_peer.h"

#include <fuchsia/bluetooth/test/cpp/fidl.h>

#include "src/connectivity/bluetooth/hci/virtual/log.h"

namespace fbt = fuchsia::bluetooth;
namespace ftest = fuchsia::bluetooth::test;

namespace bt_hci_virtual {
namespace {

bt::DeviceAddress::Type LeAddressTypeFromFidl(fbt::AddressType type) {
  return (type == fbt::AddressType::RANDOM) ? bt::DeviceAddress::Type::kLERandom
                                            : bt::DeviceAddress::Type::kLEPublic;
}

bt::DeviceAddress LeAddressFromFidl(const fbt::Address& address) {
  return bt::DeviceAddress(LeAddressTypeFromFidl(address.type), address.bytes);
}

pw::bluetooth::emboss::ConnectionRole ConnectionRoleFromFidl(fbt::ConnectionRole role) {
  switch (role) {
    case fbt::ConnectionRole::LEADER:
      return pw::bluetooth::emboss::ConnectionRole::CENTRAL;
    case fbt::ConnectionRole::FOLLOWER:
      [[fallthrough]];
    default:
      break;
  }
  return pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;
}

}  // namespace

// static
EmulatedPeer::Result EmulatedPeer::NewLowEnergy(
    ftest::LowEnergyPeerParameters parameters,
    fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
    bt::testing::FakeController* fake_controller) {
  ZX_DEBUG_ASSERT(request);
  ZX_DEBUG_ASSERT(fake_controller);

  if (!parameters.has_address()) {
    logf(ERROR, "A fake peer address is mandatory!\n");
    return fpromise::error(ftest::EmulatorPeerError::PARAMETERS_INVALID);
  }

  bt::BufferView adv, scan_response;
  if (parameters.has_advertisement()) {
    adv = bt::BufferView(parameters.advertisement().data);
  }
  if (parameters.has_scan_response()) {
    scan_response = bt::BufferView(parameters.scan_response().data);
  }

  auto address = LeAddressFromFidl(parameters.address());
  bool connectable = parameters.has_connectable() && parameters.connectable();
  bool scannable = scan_response.size() != 0u;

  // TODO(armansito): We should consider splitting bt::testing::FakePeer into separate types for
  // BR/EDR and LE transport emulation logic.
  auto peer = std::make_unique<bt::testing::FakePeer>(address, connectable, scannable);
  peer->SetAdvertisingData(adv);
  if (scannable) {
    peer->SetScanResponse(/*should_batch_reports=*/false, scan_response);
  }

  if (!fake_controller->AddPeer(std::move(peer))) {
    logf(ERROR, "A fake LE peer with given address already exists: %s\n",
         address.ToString().c_str());
    return fpromise::error(ftest::EmulatorPeerError::ADDRESS_REPEATED);
  }

  return fpromise::ok(std::unique_ptr<EmulatedPeer>(
      new EmulatedPeer(address, std::move(request), fake_controller)));
}

// static
EmulatedPeer::Result EmulatedPeer::NewBredr(
    ftest::BredrPeerParameters parameters,
    fidl::InterfaceRequest<fuchsia::bluetooth::test::Peer> request,
    bt::testing::FakeController* fake_controller) {
  ZX_DEBUG_ASSERT(request);
  ZX_DEBUG_ASSERT(fake_controller);

  if (!parameters.has_address()) {
    logf(ERROR, "A fake peer address is mandatory!\n");
    return fpromise::error(ftest::EmulatorPeerError::PARAMETERS_INVALID);
  }

  auto address = bt::DeviceAddress(bt::DeviceAddress::Type::kBREDR, parameters.address().bytes);
  bool connectable = parameters.has_connectable() && parameters.connectable();

  // TODO(armansito): We should consider splitting bt::testing::FakePeer into separate types for
  // BR/EDR and LE transport emulation logic.
  auto peer = std::make_unique<bt::testing::FakePeer>(address, connectable, false);
  if (parameters.has_device_class()) {
    peer->set_class_of_device(bt::DeviceClass(parameters.device_class().value));
  }
  if (parameters.has_service_definition()) {
    std::vector<bt::sdp::ServiceRecord> recs;
    for (const auto& defn : parameters.service_definition()) {
      auto rec = bthost::fidl_helpers::ServiceDefinitionToServiceRecord(defn);
      if (rec.is_ok()) {
        recs.emplace_back(std::move(rec.value()));
      }
    }
    bt::l2cap::ChannelParameters params;
    auto NopConnectCallback = [](auto /*channel*/, const bt::sdp::DataElement&) {};
    peer->sdp_server()->server()->RegisterService(std::move(recs), params, NopConnectCallback);
  }

  if (!fake_controller->AddPeer(std::move(peer))) {
    logf(ERROR, "A fake BR/EDR peer with given address already exists: %s\n",
         address.ToString().c_str());
    return fpromise::error(ftest::EmulatorPeerError::ADDRESS_REPEATED);
  }

  return fpromise::ok(std::unique_ptr<EmulatedPeer>(
      new EmulatedPeer(address, std::move(request), fake_controller)));
}

EmulatedPeer::EmulatedPeer(bt::DeviceAddress address, fidl::InterfaceRequest<ftest::Peer> request,
                           bt::testing::FakeController* fake_controller)
    : address_(address), fake_controller_(fake_controller), binding_(this, std::move(request)) {
  ZX_DEBUG_ASSERT(fake_controller_);
  ZX_DEBUG_ASSERT(binding_.is_bound());

  binding_.set_error_handler(fit::bind_member<&EmulatedPeer::OnChannelClosed>(this));
}

EmulatedPeer::~EmulatedPeer() { CleanUp(); }

void EmulatedPeer::AssignConnectionStatus(ftest::HciError status,
                                          AssignConnectionStatusCallback callback) {
  logf(TRACE, "EmulatedPeer.AssignConnectionStatus\n");

  auto peer = fake_controller_->FindPeer(address_);
  if (peer) {
    peer->set_connect_response(static_cast<pw::bluetooth::emboss::StatusCode>(status));
  }

  callback();
}

void EmulatedPeer::EmulateLeConnectionComplete(fbt::ConnectionRole role) {
  logf(TRACE, "EmulatedPeer.EmulateLeConnectionComplete\n");
  fake_controller_->ConnectLowEnergy(address_, ConnectionRoleFromFidl(role));
}

void EmulatedPeer::EmulateDisconnectionComplete() {
  logf(TRACE, "EmulatedPeer.EmulateDisconnectionComplete\n");
  fake_controller_->Disconnect(address_);
}

void EmulatedPeer::WatchConnectionStates(WatchConnectionStatesCallback callback) {
  logf(TRACE, "EmulatedPeer.WatchConnectionState\n");
  connection_state_getter_.Watch(std::move(callback));
}

void EmulatedPeer::UpdateConnectionState(bool connected) {
  connection_state_getter_.Add(connected ? ftest::ConnectionState::CONNECTED
                                         : ftest::ConnectionState::DISCONNECTED);
}

void EmulatedPeer::OnChannelClosed(zx_status_t status) {
  logf(TRACE, "EmulatedPeer channel closed\n");
  NotifyChannelClosed();
}

void EmulatedPeer::CleanUp() { fake_controller_->RemovePeer(address_); }

void EmulatedPeer::NotifyChannelClosed() {
  if (closed_callback_) {
    closed_callback_();
  }
}

}  // namespace bt_hci_virtual
