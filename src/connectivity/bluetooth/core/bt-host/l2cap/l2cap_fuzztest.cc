// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzzer/FuzzedDataProvider.h>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test_double_base.h"

namespace bt::testing {

// ACL Buffer Info
constexpr size_t kMaxDataPacketLength = 64;
// Ensure outbound ACL packets aren't queued.
constexpr size_t kMaxPacketCount = 1000;

// If the packet size is too large, we consume too much of the fuzzer data per packet without much
// benefit.
constexpr uint16_t kMaxAclPacketSize = 100;

constexpr hci_spec::ConnectionHandle kHandle = 0x0001;

// Don't toggle connection too often or else l2cap won't get very far.
constexpr float kToggleConnectionChance = 0.04;

class FuzzerController : public ControllerTestDoubleBase, public WeakSelf<FuzzerController> {
 public:
  FuzzerController() : WeakSelf(this) {}
  ~FuzzerController() override = default;

 private:
  // Controller overrides:
  void SendCommand(pw::span<const std::byte> command) override {}
  void SendAclData(pw::span<const std::byte> data) override {}
  void SendScoData(pw::span<const std::byte> data) override {}
};

// Reuse ControllerTest test fixture code even though we're not using gtest.
using TestingBase = ControllerTest<FuzzerController>;
class DataFuzzTest : public TestingBase {
 public:
  DataFuzzTest(const uint8_t* data, size_t size) : data_(data, size), connection_(false) {
    TestingBase::SetUp();
    const auto bredr_buffer_info = hci::DataBufferInfo(kMaxDataPacketLength, kMaxPacketCount);
    InitializeACLDataChannel(bredr_buffer_info);

    channel_manager_ = l2cap::ChannelManager::Create(transport()->acl_data_channel(),
                                                     transport()->command_channel(),
                                                     /*random_channel_ids=*/true);
  }

  ~DataFuzzTest() override {
    channel_manager_ = nullptr;
    TestingBase::TearDown();
  }

  void TestBody() override {
    RegisterService();

    while (data_.remaining_bytes() > 0) {
      bool run_loop = data_.ConsumeBool();
      if (run_loop) {
        RunLoopUntilIdle();
      }

      if (!SendAclPacket()) {
        break;
      }

      if (data_.ConsumeProbability<float>() < kToggleConnectionChance) {
        ToggleConnection();
      }
    }

    RunLoopUntilIdle();
  }

  bool SendAclPacket() {
    if (data_.remaining_bytes() < sizeof(uint64_t)) {
      return false;
    }
    // Consumes 8 bytes.
    auto packet_size = data_.ConsumeIntegralInRange<uint16_t>(
        sizeof(hci_spec::ACLDataHeader),
        std::min(static_cast<size_t>(kMaxAclPacketSize), data_.remaining_bytes()));

    auto packet_data = data_.ConsumeBytes<uint8_t>(packet_size);
    if (packet_data.size() < packet_size) {
      // Check if we ran out of fuzzer data.
      return false;
    }

    MutableBufferView packet_view(packet_data.data(), packet_data.size());

    // Use correct length so packets aren't rejected for invalid length.
    packet_view.AsMutable<hci_spec::ACLDataHeader>().data_total_length =
        htole16(packet_view.size() - sizeof(hci_spec::ACLDataHeader));

    // Use correct connection handle so packets aren't rejected/queued for invalid handle.
    uint16_t handle_and_flags =
        packet_view.ReadMember<&hci_spec::ACLDataHeader::handle_and_flags>();
    handle_and_flags &= 0xF000;  // Keep flags, clear handle.
    handle_and_flags |= kHandle;
    packet_view.AsMutable<hci_spec::ACLDataHeader>().handle_and_flags = handle_and_flags;

    auto status = test_device()->SendACLDataChannelPacket(packet_view);
    BT_ASSERT(status == ZX_OK);
    return true;
  }

  void RegisterService() {
    channel_manager_->RegisterService(
        l2cap::kAVDTP, l2cap::ChannelParameters(), [this](l2cap::Channel::WeakPtr chan) {
          if (!chan.is_alive()) {
            return;
          }
          chan->Activate(/*rx_callback=*/[](auto) {}, /*closed_callback=*/
                         [this, id = chan->id()] { channels_.erase(id); });
          channels_.emplace(chan->id(), std::move(chan));
        });
  }

  void ToggleConnection() {
    if (connection_) {
      acl_data_channel()->UnregisterLink(kHandle);
      channel_manager_->RemoveConnection(kHandle);
      connection_ = false;
      return;
    }

    acl_data_channel()->RegisterLink(kHandle, bt::LinkType::kACL);
    channel_manager_->AddACLConnection(
        kHandle, hci_spec::ConnectionRole::CENTRAL, /*link_error_callback=*/[] {},
        /*security_callback=*/[](auto, auto, auto) {});
    connection_ = true;
  }

 private:
  FuzzedDataProvider data_;
  std::unique_ptr<l2cap::ChannelManager> channel_manager_;
  bool connection_;
  std::unordered_map<l2cap::ChannelId, l2cap::Channel::WeakPtr> channels_;
};

}  // namespace bt::testing

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  bt::testing::DataFuzzTest fuzz(data, size);
  fuzz.TestBody();
  return 0;
}
