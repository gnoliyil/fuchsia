// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "legacy_low_energy_advertiser.h"

#include <endian.h>
#include <lib/async/default.h>

#include "src/connectivity/bluetooth/core/bt-host/common/advertising_data.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/util.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/sequential_command_runner.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::hci {

LegacyLowEnergyAdvertiser::~LegacyLowEnergyAdvertiser() {
  // This object is probably being destroyed because the stack is shutting down, in which case the
  // HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }
  StopAdvertising();
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildEnablePacket(
    const DeviceAddress& address, pw::bluetooth::emboss::GenericEnableParam enable) {
  constexpr size_t kPayloadSize = sizeof(hci_spec::LESetAdvertisingEnableCommandParams);
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(hci_spec::kLESetAdvertisingEnable, kPayloadSize);
  packet->mutable_payload<hci_spec::LESetAdvertisingEnableCommandParams>()->advertising_enable =
      enable;
  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildSetAdvertisingData(
    const DeviceAddress& address, const AdvertisingData& data, AdvFlags flags) {
  auto packet = CommandPacket::New(hci_spec::kLESetAdvertisingData,
                                   sizeof(hci_spec::LESetAdvertisingDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();

  auto params = packet->mutable_payload<hci_spec::LESetAdvertisingDataCommandParams>();
  params->adv_data_length = static_cast<uint8_t>(data.CalculateBlockSize(/*include_flags=*/true));

  MutableBufferView adv_view(params->adv_data, params->adv_data_length);
  data.WriteBlock(&adv_view, flags);

  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildSetScanResponse(
    const DeviceAddress& address, const AdvertisingData& scan_rsp) {
  auto packet = CommandPacket::New(hci_spec::kLESetScanResponseData,
                                   sizeof(hci_spec::LESetScanResponseDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();

  auto params = packet->mutable_payload<hci_spec::LESetScanResponseDataCommandParams>();
  params->scan_rsp_data_length = static_cast<uint8_t>(scan_rsp.CalculateBlockSize());

  MutableBufferView scan_data_view(params->scan_rsp_data, sizeof(params->scan_rsp_data));
  scan_rsp.WriteBlock(&scan_data_view, std::nullopt);

  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress& address, hci_spec::LEAdvertisingType type,
    hci_spec::LEOwnAddressType own_address_type, AdvertisingIntervalRange interval) {
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(hci_spec::kLESetAdvertisingParameters,
                         sizeof(hci_spec::LESetAdvertisingParametersCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();

  auto params = packet->mutable_payload<hci_spec::LESetAdvertisingParametersCommandParams>();
  params->adv_interval_min = htole16(interval.min());
  params->adv_interval_max = htole16(interval.max());
  params->adv_type = type;
  params->own_address_type = own_address_type;
  params->adv_channel_map = hci_spec::kLEAdvertisingChannelAll;
  params->adv_filter_policy = hci_spec::LEAdvFilterPolicy::kAllowAll;

  // We don't support directed advertising yet, so leave peer_address and peer_address_type as 0x00
  // (|packet| parameters are initialized to zero above).

  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    const DeviceAddress& address) {
  auto packet = CommandPacket::New(hci_spec::kLESetAdvertisingData,
                                   sizeof(hci_spec::LESetAdvertisingDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildUnsetScanResponse(
    const DeviceAddress& address) {
  auto packet = CommandPacket::New(hci_spec::kLESetScanResponseData,
                                   sizeof(hci_spec::LESetScanResponseDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  return packet;
}

std::unique_ptr<CommandPacket> LegacyLowEnergyAdvertiser::BuildRemoveAdvertisingSet(
    const DeviceAddress& address) {
  constexpr size_t kPayloadSize = sizeof(hci_spec::LESetAdvertisingEnableCommandParams);
  auto packet = CommandPacket::New(hci_spec::kLESetAdvertisingEnable, kPayloadSize);
  auto params = packet->mutable_payload<hci_spec::LESetAdvertisingEnableCommandParams>();
  params->advertising_enable = pw::bluetooth::emboss::GenericEnableParam::DISABLE;
  return packet;
}

static std::unique_ptr<CommandPacket> BuildReadAdvertisingTxPower() {
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(hci_spec::kLEReadAdvertisingChannelTxPower);
  return packet;
}

void LegacyLowEnergyAdvertiser::StartAdvertising(const DeviceAddress& address,
                                                 const AdvertisingData& data,
                                                 const AdvertisingData& scan_rsp,
                                                 AdvertisingOptions options,
                                                 ConnectionCallback connect_callback,
                                                 ResultFunction<> result_callback) {
  fit::result<HostError> result = CanStartAdvertising(address, data, scan_rsp, options);
  if (result.is_error()) {
    result_callback(ToResult(result.error_value()));
    return;
  }

  if (IsAdvertising() && !IsAdvertising(address)) {
    bt_log(INFO, "hci-le", "already advertising (only one advertisement supported at a time)");
    result_callback(ToResult(HostError::kNotSupported));
    return;
  }

  if (IsAdvertising()) {
    bt_log(DEBUG, "hci-le", "updating existing advertisement");
  }

  // Midst of a TX power level read - send a cancel over the previous status callback.
  if (staged_params_.has_value()) {
    auto result_cb = std::move(staged_params_.value().result_callback);
    result_cb(ToResult(HostError::kCanceled));
  }

  // If the TX Power level is requested, then stage the parameters for the read operation.
  // If there already is an outstanding TX Power Level read request, return early.
  // Advertising on the outstanding call will now use the updated |staged_params_|.
  if (options.include_tx_power_level) {
    AdvertisingData data_copy;
    data.Copy(&data_copy);

    AdvertisingData scan_rsp_copy;
    scan_rsp.Copy(&scan_rsp_copy);

    staged_params_ = StagedParams{address,
                                  options.interval,
                                  options.flags,
                                  std::move(data_copy),
                                  std::move(scan_rsp_copy),
                                  std::move(connect_callback),
                                  std::move(result_callback)};

    if (starting_ && hci_cmd_runner().IsReady()) {
      return;
    }
  }

  if (!hci_cmd_runner().IsReady()) {
    bt_log(DEBUG, "hci-le",
           "canceling advertising start/stop sequence due to new advertising request");
    // Abort any remaining commands from the current stop sequence. If we got
    // here then the controller MUST receive our request to disable advertising,
    // so the commands that we send next will overwrite the current advertising
    // settings and re-enable it.
    hci_cmd_runner().Cancel();
  }

  starting_ = true;

  // If the TX Power Level is requested, read it from the controller, update the data buf, and
  // proceed with starting advertising.
  //
  // If advertising was canceled during the TX power level read (either |starting_| was
  // reset or the |result_callback| was moved), return early.
  if (options.include_tx_power_level) {
    auto power_cb = [this](auto, const hci::EventPacket& event) mutable {
      BT_ASSERT(staged_params_.has_value());
      if (!starting_ || !staged_params_.value().result_callback) {
        bt_log(INFO, "hci-le", "Advertising canceled during TX Power Level read.");
        return;
      }

      if (hci_is_error(event, WARN, "hci-le", "read TX power level failed")) {
        staged_params_.value().result_callback(event.ToResult());
        staged_params_ = {};
        starting_ = false;
        return;
      }

      const auto& params =
          event.return_params<hci_spec::LEReadAdvertisingChannelTxPowerReturnParams>();

      // Update the advertising and scan response data with the TX power level.
      auto staged_params = std::move(staged_params_.value());
      staged_params.data.SetTxPower(params->tx_power);
      if (staged_params.scan_rsp.CalculateBlockSize()) {
        staged_params.scan_rsp.SetTxPower(params->tx_power);
      }
      // Reset the |staged_params_| as it is no longer in use.
      staged_params_ = {};

      StartAdvertisingInternal(
          staged_params.address, staged_params.data, staged_params.scan_rsp, staged_params.interval,
          staged_params.flags, std::move(staged_params.connect_callback),
          [this,
           result_callback = std::move(staged_params.result_callback)](const Result<>& result) {
            starting_ = false;
            result_callback(result);
          });
    };

    hci()->command_channel()->SendCommand(BuildReadAdvertisingTxPower(), std::move(power_cb));
    return;
  }

  StartAdvertisingInternal(
      address, data, scan_rsp, options.interval, options.flags, std::move(connect_callback),
      [this, result_callback = std::move(result_callback)](const Result<>& result) {
        starting_ = false;
        result_callback(result);
      });
}

void LegacyLowEnergyAdvertiser::StopAdvertising() {
  LowEnergyAdvertiser::StopAdvertising();
  starting_ = false;
}

void LegacyLowEnergyAdvertiser::StopAdvertising(const DeviceAddress& address) {
  if (!hci_cmd_runner().IsReady()) {
    hci_cmd_runner().Cancel();
  }

  LowEnergyAdvertiser::StopAdvertisingInternal(address);
  starting_ = false;
}

void LegacyLowEnergyAdvertiser::OnIncomingConnection(
    hci_spec::ConnectionHandle handle, pw::bluetooth::emboss::ConnectionRole role,
    const DeviceAddress& peer_address, const hci_spec::LEConnectionParameters& conn_params) {
  static DeviceAddress identity_address = DeviceAddress(DeviceAddress::Type::kLEPublic, {0});

  // We use the identity address as the local address if we aren't advertising. If we aren't
  // advertising, this is obviously wrong. However, the link will be disconnected in that case
  // before it can propagate to higher layers.
  DeviceAddress local_address = identity_address;
  if (IsAdvertising()) {
    local_address = connection_callbacks().begin()->first;
  }

  CompleteIncomingConnection(handle, role, local_address, peer_address, conn_params);
}

}  // namespace bt::hci
