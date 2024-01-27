// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fake_l2cap.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"

namespace bt {

using l2cap::testing::FakeChannel;

namespace l2cap::testing {
namespace {

// Use plausible ERTM parameters that do not necessarily match values in production. See Core Spec
// v5.0 Vol 3, Part A, Sec 5.4 for meanings.
constexpr uint8_t kErtmNFramesInTxWindow = 32;
constexpr uint8_t kErtmMaxTransmissions = 8;
constexpr uint16_t kMaxTxPduPayloadSize = 1024;

}  // namespace

bool FakeL2cap::IsLinkConnected(hci_spec::ConnectionHandle handle) const {
  auto link_iter = links_.find(handle);
  if (link_iter == links_.end()) {
    return false;
  }
  return link_iter->second.connected;
}

void FakeL2cap::TriggerLEConnectionParameterUpdate(
    hci_spec::ConnectionHandle handle, const hci_spec::LEPreferredConnectionParameters& params) {
  LinkData& link_data = ConnectedLinkData(handle);
  link_data.le_conn_param_cb(params);
}

void FakeL2cap::ExpectOutboundL2capChannel(hci_spec::ConnectionHandle handle, l2cap::PSM psm,
                                           l2cap::ChannelId id, l2cap::ChannelId remote_id,
                                           l2cap::ChannelParameters params) {
  LinkData& link_data = GetLinkData(handle);
  ChannelData chan_data;
  chan_data.local_id = id;
  chan_data.remote_id = remote_id;
  chan_data.params = params;
  link_data.expected_outbound_conns[psm].push(chan_data);
}

bool FakeL2cap::TriggerInboundL2capChannel(hci_spec::ConnectionHandle handle, l2cap::PSM psm,
                                           l2cap::ChannelId id, l2cap::ChannelId remote_id,
                                           uint16_t max_tx_sdu_size) {
  LinkData& link_data = ConnectedLinkData(handle);
  auto cb_iter = registered_services_.find(psm);

  // No service registered for the PSM.
  if (cb_iter == registered_services_.end()) {
    return false;
  }

  l2cap::ChannelCallback& cb = cb_iter->second.channel_cb;
  auto chan_params = cb_iter->second.channel_params;
  auto mode = chan_params.mode.value_or(l2cap::ChannelMode::kBasic);
  auto max_rx_sdu_size = chan_params.max_rx_sdu_size.value_or(l2cap::kDefaultMTU);
  auto channel_info = l2cap::ChannelInfo::MakeBasicMode(max_rx_sdu_size, max_tx_sdu_size);
  if (mode == l2cap::ChannelMode::kEnhancedRetransmission) {
    channel_info = l2cap::ChannelInfo::MakeEnhancedRetransmissionMode(
        max_rx_sdu_size, max_tx_sdu_size, /*n_frames_in_tx_window=*/kErtmNFramesInTxWindow,
        /*max_transmissions=*/kErtmMaxTransmissions,
        /*max_tx_pdu_payload_size=*/kMaxTxPduPayloadSize);
  }

  auto chan = OpenFakeChannel(&link_data, id, remote_id, channel_info);
  cb(chan->GetWeakPtr());

  return true;
}

void FakeL2cap::TriggerLinkError(hci_spec::ConnectionHandle handle) {
  LinkData& link_data = ConnectedLinkData(handle);
  link_data.link_error_cb();
}

void FakeL2cap::AddACLConnection(hci_spec::ConnectionHandle handle,
                                 pw::bluetooth::emboss::ConnectionRole role,
                                 l2cap::LinkErrorCallback link_error_cb,
                                 l2cap::SecurityUpgradeCallback security_cb) {
  RegisterInternal(handle, role, bt::LinkType::kACL, std::move(link_error_cb));
}

ChannelManager::LEFixedChannels FakeL2cap::AddLEConnection(
    hci_spec::ConnectionHandle handle, pw::bluetooth::emboss::ConnectionRole role,
    l2cap::LinkErrorCallback link_error_cb,
    l2cap::LEConnectionParameterUpdateCallback conn_param_cb,
    l2cap::SecurityUpgradeCallback security_cb) {
  LinkData* data = RegisterInternal(handle, role, bt::LinkType::kLE, std::move(link_error_cb));
  data->le_conn_param_cb = std::move(conn_param_cb);

  // Open the ATT and SMP fixed channels.
  auto att = OpenFakeFixedChannel(data, l2cap::kATTChannelId);
  auto smp = OpenFakeFixedChannel(data, l2cap::kLESMPChannelId);
  return LEFixedChannels{.att = att->GetWeakPtr(), .smp = smp->GetWeakPtr()};
}

void FakeL2cap::RemoveConnection(hci_spec::ConnectionHandle handle) { links_.erase(handle); }

void FakeL2cap::AssignLinkSecurityProperties(hci_spec::ConnectionHandle handle,
                                             sm::SecurityProperties security) {
  // TODO(armansito): implement
}

void FakeL2cap::RequestConnectionParameterUpdate(
    hci_spec::ConnectionHandle handle, hci_spec::LEPreferredConnectionParameters params,
    l2cap::ConnectionParameterUpdateRequestCallback request_cb) {
  bool response = connection_parameter_update_request_responder_
                      ? connection_parameter_update_request_responder_(handle, params)
                      : true;
  // Simulate async response.
  async::PostTask(async_get_default_dispatcher(), std::bind(std::move(request_cb), response));
}

void FakeL2cap::OpenL2capChannel(hci_spec::ConnectionHandle handle, l2cap::PSM psm,
                                 l2cap::ChannelParameters params, l2cap::ChannelCallback cb) {
  LinkData& link_data = ConnectedLinkData(handle);
  auto psm_it = link_data.expected_outbound_conns.find(psm);

  BT_DEBUG_ASSERT_MSG(psm_it != link_data.expected_outbound_conns.end() && !psm_it->second.empty(),
                      "Unexpected outgoing L2CAP connection (PSM %#.4x)", psm);

  auto chan_data = psm_it->second.front();
  psm_it->second.pop();

  auto mode = params.mode.value_or(l2cap::ChannelMode::kBasic);
  auto max_rx_sdu_size = params.max_rx_sdu_size.value_or(l2cap::kMaxMTU);

  BT_ASSERT_MSG(chan_data.params == params,
                "Didn't receive expected L2CAP channel parameters (expected: %s, found: %s)",
                bt_str(chan_data.params), bt_str(params));

  auto channel_info = l2cap::ChannelInfo::MakeBasicMode(max_rx_sdu_size, l2cap::kDefaultMTU);
  if (mode == l2cap::ChannelMode::kEnhancedRetransmission) {
    channel_info = l2cap::ChannelInfo::MakeEnhancedRetransmissionMode(
        max_rx_sdu_size, l2cap::kDefaultMTU, /*n_frames_in_tx_window=*/kErtmNFramesInTxWindow,
        /*max_transmissions=*/kErtmMaxTransmissions,
        /*max_tx_pdu_payload_size=*/kMaxTxPduPayloadSize);
  }

  auto fake_chan =
      OpenFakeChannel(&link_data, chan_data.local_id, chan_data.remote_id, channel_info);
  l2cap::Channel::WeakPtr chan;
  if (fake_chan.is_alive()) {
    chan = fake_chan->GetWeakPtr();
  }

  // Simulate async channel creation process.
  async::PostTask(async_get_default_dispatcher(),
                  [cb = std::move(cb), chan = std::move(chan)]() { cb(chan); });
}

bool FakeL2cap::RegisterService(l2cap::PSM psm, l2cap::ChannelParameters params,
                                l2cap::ChannelCallback channel_callback) {
  BT_DEBUG_ASSERT(registered_services_.count(psm) == 0);
  registered_services_.emplace(psm, ServiceInfo(params, std::move(channel_callback)));
  return true;
}

void FakeL2cap::UnregisterService(l2cap::PSM psm) { registered_services_.erase(psm); }

FakeL2cap::~FakeL2cap() {
  for (auto& link_it : links_) {
    for (auto& psm_it : link_it.second.expected_outbound_conns) {
      BT_DEBUG_ASSERT_MSG(psm_it.second.empty(), "didn't receive expected connection on PSM %#.4x",
                          psm_it.first);
    }
  }
}

FakeL2cap::LinkData* FakeL2cap::RegisterInternal(hci_spec::ConnectionHandle handle,
                                                 pw::bluetooth::emboss::ConnectionRole role,
                                                 bt::LinkType link_type,
                                                 l2cap::LinkErrorCallback link_error_cb) {
  auto& data = GetLinkData(handle);
  BT_DEBUG_ASSERT_MSG(!data.connected, "connection handle re-used (handle: %#.4x)", handle);

  data.connected = true;
  data.role = role;
  data.type = link_type;
  data.link_error_cb = std::move(link_error_cb);

  return &data;
}

FakeChannel::WeakPtr FakeL2cap::OpenFakeChannel(LinkData* link, l2cap::ChannelId id,
                                                l2cap::ChannelId remote_id,
                                                l2cap::ChannelInfo info) {
  FakeChannel::WeakPtr chan;
  if (!simulate_open_channel_failure_) {
    auto channel = std::make_unique<FakeChannel>(id, remote_id, link->handle, link->type, info);
    chan = channel->AsWeakPtr();
    channel->SetLinkErrorCallback(link->link_error_cb.share());
    link->channels_.emplace(id, std::move(channel));
  }

  if (chan_cb_) {
    chan_cb_(chan);
  }

  return chan;
}

FakeChannel::WeakPtr FakeL2cap::OpenFakeFixedChannel(LinkData* link, l2cap::ChannelId id) {
  return OpenFakeChannel(link, id, id);
}

FakeL2cap::LinkData& FakeL2cap::GetLinkData(hci_spec::ConnectionHandle handle) {
  auto [it, inserted] = links_.try_emplace(handle);
  auto& data = it->second;
  if (inserted) {
    data.connected = false;
    data.handle = handle;
  }
  return data;
}

FakeL2cap::LinkData& FakeL2cap::ConnectedLinkData(hci_spec::ConnectionHandle handle) {
  auto link_iter = links_.find(handle);
  BT_DEBUG_ASSERT_MSG(link_iter != links_.end(), "fake link not found (handle: %#.4x)", handle);
  BT_DEBUG_ASSERT_MSG(link_iter->second.connected, "fake link not connected yet (handle: %#.4x)",
                      handle);
  return link_iter->second;
}

}  // namespace l2cap::testing
}  // namespace bt
