// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bredr_interrogator.h"

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/command_channel.h"

namespace bt::gap {

BrEdrInterrogator::BrEdrInterrogator(Peer::WeakPtr peer, hci_spec::ConnectionHandle handle,
                                     hci::CommandChannel::WeakPtr cmd_channel)
    : peer_(std::move(peer)),
      peer_id_(peer_->identifier()),
      handle_(handle),
      cmd_runner_(std::move(cmd_channel)),
      weak_self_(this) {
  BT_ASSERT(peer_.is_alive());
}

void BrEdrInterrogator::Start(ResultCallback callback) {
  callback_ = std::move(callback);

  if (!peer_.is_alive() || !peer_->bredr()) {
    Complete(ToResult(HostError::kFailed));
    return;
  }

  if (!peer_->name()) {
    QueueRemoteNameRequest();
  }

  if (!peer_->version()) {
    QueueReadRemoteVersionInformation();
  }

  if (!peer_->features().HasPage(0)) {
    QueueReadRemoteFeatures();
  } else if (peer_->features().HasBit(/*page=*/0, hci_spec::LMPFeature::kExtendedFeatures)) {
    QueueReadRemoteExtendedFeatures(/*page=*/1);
  }

  if (!cmd_runner_.HasQueuedCommands()) {
    Complete(fit::ok());
    return;
  }

  cmd_runner_.RunCommands([this](hci::Result<> result) { Complete(result); });
}

void BrEdrInterrogator::Cancel() {
  if (!cmd_runner_.IsReady()) {
    cmd_runner_.Cancel();
  }
}

void BrEdrInterrogator::Complete(hci::Result<> result) {
  if (!callback_) {
    return;
  }

  auto self = weak_self_.GetWeakPtr();

  // callback may destroy this object
  callback_(result);

  if (self.is_alive() && !cmd_runner_.IsReady()) {
    cmd_runner_.Cancel();
  }
}

void BrEdrInterrogator::QueueRemoteNameRequest() {
  pw::bluetooth::emboss::PageScanRepetitionMode mode =
      pw::bluetooth::emboss::PageScanRepetitionMode::R0_;
  if (peer_->bredr()->page_scan_repetition_mode()) {
    mode = *peer_->bredr()->page_scan_repetition_mode();
  }

  auto packet =
      hci::EmbossCommandPacket::New<pw::bluetooth::emboss::RemoteNameRequestCommandWriter>(
          hci_spec::kRemoteNameRequest);
  auto params = packet.view_t();
  params.bd_addr().CopyFrom(peer_->address().value().view());
  params.page_scan_repetition_mode().Write(mode);
  if (peer_->bredr()->clock_offset()) {
    params.clock_offset().valid().Write(true);
    const uint16_t offset = peer_->bredr()->clock_offset().value();
    params.clock_offset().clock_offset().Write(offset);
  }

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (hci_is_error(event, WARN, "gap-bredr", "remote name request failed")) {
      return;
    }
    bt_log(TRACE, "gap-bredr", "name request complete (peer id: %s)", bt_str(peer_id_));
    const auto& params = event.params<hci_spec::RemoteNameRequestCompleteEventParams>();
    const auto remote_name_end = std::find(params.remote_name, std::end(params.remote_name), '\0');
    peer_->RegisterName(std::string(params.remote_name, remote_name_end),
                        Peer::NameSource::kNameDiscoveryProcedure);
  };

  bt_log(TRACE, "gap-bredr", "sending name request (peer id: %s)", bt_str(peer_->identifier()));
  cmd_runner_.QueueCommand(std::move(packet), std::move(cmd_cb), /*wait=*/false,
                           hci_spec::kRemoteNameRequestCompleteEventCode, {hci_spec::kInquiry});
}

void BrEdrInterrogator::QueueReadRemoteFeatures() {
  auto packet = hci::EmbossCommandPacket::New<
      pw::bluetooth::emboss::ReadRemoteSupportedFeaturesCommandWriter>(
      hci_spec::kReadRemoteSupportedFeatures);
  packet.view_t().connection_handle().Write(handle_);

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (hci_is_error(event, WARN, "gap-bredr", "read remote supported features failed")) {
      return;
    }
    bt_log(TRACE, "gap-bredr", "remote features request complete (peer id: %s)", bt_str(peer_id_));
    const auto& params =
        event.view().payload<hci_spec::ReadRemoteSupportedFeaturesCompleteEventParams>();
    peer_->SetFeaturePage(0, le64toh(params.lmp_features));

    if (peer_->features().HasBit(0, hci_spec::LMPFeature::kExtendedFeatures)) {
      peer_->set_last_page_number(1);
      QueueReadRemoteExtendedFeatures(/*page=*/1);
    }
  };

  bt_log(TRACE, "gap-bredr", "asking for supported features (peer id: %s)", bt_str(peer_id_));
  cmd_runner_.QueueCommand(std::move(packet), std::move(cmd_cb), /*wait=*/false,
                           hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
}

void BrEdrInterrogator::QueueReadRemoteExtendedFeatures(uint8_t page) {
  auto packet =
      hci::EmbossCommandPacket::New<pw::bluetooth::emboss::ReadRemoteExtendedFeaturesCommandWriter>(
          hci_spec::kReadRemoteExtendedFeatures);
  auto params = packet.view_t();
  params.connection_handle().Write(handle_);
  params.page_number().Write(page);

  auto cmd_cb = [this, page](const auto& event) {
    if (hci_is_error(event, WARN, "gap-bredr", "read remote extended features failed (peer id: %s)",
                     bt_str(peer_id_))) {
      return;
    }
    const auto& params =
        event.view().template payload<hci_spec::ReadRemoteExtendedFeaturesCompleteEventParams>();

    bt_log(TRACE, "gap-bredr",
           "got extended features page %u, max page %u (requested page: %u, peer id: %s)",
           params.page_number, params.max_page_number, page, bt_str(peer_id_));

    peer_->SetFeaturePage(params.page_number, le64toh(params.lmp_features));

    if (params.page_number != page) {
      bt_log(INFO, "gap-bredr", "requested page %u and got page %u, giving up (peer: %s)", page,
             params.page_number, bt_str(peer_id_));
      peer_->set_last_page_number(0);
      return;
    }

    // NOTE: last page number will be capped at 2
    peer_->set_last_page_number(params.max_page_number);

    if (page < peer_->features().last_page_number()) {
      QueueReadRemoteExtendedFeatures(page + 1);
    }
  };

  bt_log(TRACE, "gap-bredr", "requesting extended features page %u (peer id: %s)", page,
         bt_str(peer_id_));
  cmd_runner_.QueueCommand(std::move(packet), std::move(cmd_cb), /*wait=*/false,
                           hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode);
}

void BrEdrInterrogator::QueueReadRemoteVersionInformation() {
  auto packet =
      hci::EmbossCommandPacket::New<pw::bluetooth::emboss::ReadRemoteVersionInfoCommandWriter>(
          hci_spec::kReadRemoteVersionInfo);
  packet.view_t().connection_handle().Write(handle_);

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (hci_is_error(event, WARN, "gap", "read remote version info failed")) {
      return;
    }
    bt_log(TRACE, "gap", "read remote version info completed (peer id: %s)", bt_str(peer_id_));
    const auto params = event.params<hci_spec::ReadRemoteVersionInfoCompleteEventParams>();
    peer_->set_version(params.lmp_version, params.manufacturer_name, params.lmp_subversion);
  };

  bt_log(TRACE, "gap", "asking for version info (peer id: %s)", bt_str(peer_id_));
  cmd_runner_.QueueCommand(std::move(packet), std::move(cmd_cb), /*wait=*/false,
                           hci_spec::kReadRemoteVersionInfoCompleteEventCode);
}

}  // namespace bt::gap
