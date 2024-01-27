// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "sc_stage_1_passkey.h"

#include <optional>

#include "lib/fit/function.h"
#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/common/uint128.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/delegate.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/sc_stage_1.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/util.h"

namespace bt::sm {

namespace {
const size_t kMaxPasskeyBitLocation = 19;

uint8_t GetPasskeyBit(uint32_t passkey, size_t passkey_bit_location) {
  uint32_t masked_passkey = passkey & (1 << passkey_bit_location);
  // These values come from the spec f4 section (V5.1 Vol. 3 Part H Section 2.2.7)
  return (masked_passkey == 0) ? 0x80 : 0x81;
}

}  // namespace

ScStage1Passkey::ScStage1Passkey(PairingPhase::Listener::WeakPtr listener, Role role,
                                 UInt256 local_pub_key_x, UInt256 peer_pub_key_x,
                                 PairingMethod method, PairingChannel::WeakPtr sm_chan,
                                 Stage1CompleteCallback on_complete)
    : listener_(std::move(listener)),
      role_(role),
      local_public_key_x_(local_pub_key_x),
      peer_public_key_x_(peer_pub_key_x),
      method_(method),
      passkey_bit_location_(0),
      sent_local_confirm_(false),
      peer_confirm_(std::nullopt),
      sent_local_rand_(false),
      peer_rand_(std::nullopt),
      sm_chan_(std::move(sm_chan)),
      on_complete_(std::move(on_complete)),
      weak_self_(this) {
  BT_ASSERT(method == PairingMethod::kPasskeyEntryDisplay ||
            method == PairingMethod::kPasskeyEntryInput);
}

void ScStage1Passkey::Run() {
  auto self = weak_self_.GetWeakPtr();
  auto passkey_responder = [self](std::optional<uint32_t> passkey) {
    if (!self.is_alive()) {
      bt_log(TRACE, "sm", "passkey for expired callback");
      return;
    }
    if (!passkey.has_value()) {
      self->on_complete_(fit::error(ErrorCode::kPasskeyEntryFailed));
      return;
    }
    self->passkey_ = passkey;
    self->StartBitExchange();
  };
  if (method_ == PairingMethod::kPasskeyEntryDisplay) {
    // Randomly generate a 6 digit passkey.
    uint32_t passkey;
    random_generator()->GetInt<uint32_t>(passkey, /*exclusive_upper_bound=*/1'000'000);
    listener_->DisplayPasskey(passkey, Delegate::DisplayMethod::kPeerEntry,
                              [responder = std::move(passkey_responder), passkey](bool confirm) {
                                std::optional<uint32_t> passkey_response = passkey;
                                if (!confirm) {
                                  bt_log(WARN, "sm", "passkey entry display rejected by user");
                                  passkey_response = std::nullopt;
                                }
                                bt_log(INFO, "sm", "SC passkey entry display accepted by user");
                                responder(passkey_response);
                              });
  } else {  // method_ == kPasskeyEntryInput
    listener_->RequestPasskey([responder = std::move(passkey_responder)](int64_t passkey) {
      std::optional<uint32_t> passkey_response = passkey;
      if (passkey >= 1000000 || passkey < 0) {
        bt_log(WARN, "sm", "rejecting passkey entry input: %s",
               passkey >= 1000000 ? "passkey has > 6 digits" : "user rejected");
        passkey_response = std::nullopt;
      }
      bt_log(INFO, "sm", "SC passkey entry display (passkey: %ld) accepted by user", passkey);
      responder(passkey_response);
    });
  }
}

void ScStage1Passkey::StartBitExchange() {
  BT_ASSERT(passkey_.has_value());
  // The passkey is 6 digits i.e. representable in 2^20 bits. Attempting to exchange > 20 bits
  // indicates a programmer error.
  BT_ASSERT(passkey_bit_location_ <= kMaxPasskeyBitLocation);
  local_rand_ = Random<UInt128>();
  sent_local_confirm_ = sent_local_rand_ = false;

  if (role_ == Role::kInitiator || peer_confirm_.has_value()) {
    // The initiator always sends the pairing confirm first. The only situation where we should
    // have received a peer confirm before StartBitExchange is if, as responder in the first bit
    // exchange, we receive the peer initiator's confirm while waiting for local user input.
    BT_ASSERT((role_ == Role::kInitiator && !peer_confirm_.has_value()) ||
              passkey_bit_location_ == 0);
    SendPairingConfirm();
  }
  // As responder, we wait for the peer confirm before taking any action.
}

void ScStage1Passkey::SendPairingConfirm() {
  BT_ASSERT(!sent_local_confirm_);
  BT_ASSERT(passkey_.has_value());
  if (role_ == Role::kResponder) {
    BT_ASSERT(peer_confirm_.has_value());
  }

  uint8_t current_passkey_bit = GetPasskeyBit(*passkey_, passkey_bit_location_);
  std::optional<UInt128> maybe_confirm =
      util::F4(local_public_key_x_, peer_public_key_x_, local_rand_, current_passkey_bit);
  if (!maybe_confirm.has_value()) {
    bt_log(WARN, "sm", "could not calculate confirm value in SC Stage 1 Passkey Entry");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  local_confirm_ = *maybe_confirm;
  sm_chan_->SendMessage(kPairingConfirm, local_confirm_);
  sent_local_confirm_ = true;
}

void ScStage1Passkey::OnPairingConfirm(PairingConfirmValue confirm) {
  if (peer_confirm_.has_value()) {
    bt_log(WARN, "sm", "received multiple Pairing Confirm values in one SC Passkey Entry cycle");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  if (sent_local_rand_ || peer_rand_.has_value() ||
      (!sent_local_confirm_ && role_ == Role::kInitiator)) {
    bt_log(WARN, "sm", "received Pairing Confirm out of order in SC Passkey Entry");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  peer_confirm_ = confirm;
  if (role_ == Role::kInitiator) {
    SendPairingRandom();
  } else if (passkey_.has_value()) {
    // As responder, it's possible to receive a confirm value while waiting for the passkey. If
    // that occurs, the local confirm will be sent by StartBitExchange.
    SendPairingConfirm();
  }
}

void ScStage1Passkey::SendPairingRandom() {
  BT_ASSERT(sent_local_confirm_ && peer_confirm_.has_value());
  BT_ASSERT(!sent_local_rand_);
  if (role_ == Role::kResponder) {
    BT_ASSERT(peer_rand_.has_value());
  }
  sm_chan_->SendMessage(kPairingRandom, local_rand_);
  sent_local_rand_ = true;
}

void ScStage1Passkey::OnPairingRandom(PairingRandomValue rand) {
  if (!sent_local_confirm_ || !peer_confirm_.has_value()) {
    bt_log(WARN, "sm", "received Pairing Random before confirm value sent");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  if (peer_rand_.has_value()) {
    bt_log(WARN, "sm", "received multiple Pairing Random values in one SC Passkey Entry cycle");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  if (role_ == Role::kInitiator && !sent_local_rand_) {
    bt_log(WARN, "sm", "received peer random out of order");
    on_complete_(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }

  uint8_t current_passkey_bit = GetPasskeyBit(*passkey_, passkey_bit_location_);
  std::optional<PairingConfirmValue> maybe_confirm_check =
      util::F4(peer_public_key_x_, local_public_key_x_, rand, current_passkey_bit);
  if (!maybe_confirm_check.has_value()) {
    bt_log(WARN, "sm", "unable to calculate SC confirm check value");
    on_complete_(fit::error(ErrorCode::kConfirmValueFailed));
    return;
  }
  if (*maybe_confirm_check != *peer_confirm_) {
    bt_log(WARN, "sm", "peer SC confirm value did not match check, aborting");
    on_complete_(fit::error(ErrorCode::kConfirmValueFailed));
    return;
  }
  peer_rand_ = rand;
  if (role_ == Role::kResponder) {
    SendPairingRandom();
  }
  // After the random exchange completes, this round of passkey bit agreement is over.
  FinishBitExchange();
}

void ScStage1Passkey::FinishBitExchange() {
  BT_ASSERT(sent_local_confirm_);
  BT_ASSERT(peer_confirm_.has_value());
  BT_ASSERT(sent_local_rand_);
  BT_ASSERT(peer_rand_.has_value());
  passkey_bit_location_++;
  if (passkey_bit_location_ <= kMaxPasskeyBitLocation) {
    peer_confirm_ = std::nullopt;
    peer_rand_ = std::nullopt;
    StartBitExchange();
    return;
  }
  // If we've exchanged bits 0-19 (20 bits), stage 1 is complete and we notify the callback.
  auto [initiator_rand, responder_rand] = util::MapToRoles(local_rand_, *peer_rand_, role_);
  UInt128 passkey_array{0};
  // Copy little-endian uint32 passkey to the UInt128 array needed for Stage 2
  auto little_endian_passkey = htole32(*passkey_);
  std::memcpy(passkey_array.data(), &little_endian_passkey, sizeof(uint32_t));
  on_complete_(fit::ok(Output{.initiator_r = passkey_array,
                              .responder_r = passkey_array,
                              .initiator_rand = initiator_rand,
                              .responder_rand = responder_rand}));
}

}  // namespace bt::sm
