// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/fidl/host_server.h"

#include <fuchsia/bluetooth/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl_test_base.h>
#include <lib/zx/channel.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "adapter_test_fixture.h"
#include "fuchsia/bluetooth/host/cpp/fidl.h"
#include "helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/fidl/helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/fake_adapter_test_fixture.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/gap.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_address_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt/fake_layer.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_l2cap.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_peer.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"

namespace fuchsia::bluetooth {
// Make PeerIds equality comparable for advanced testing matchers. ADL rules mandate the namespace.
bool operator==(const PeerId& a, const PeerId& b) { return fidl::Equals(a, b); }
}  // namespace fuchsia::bluetooth

namespace bthost {
namespace {

// Limiting the de-scoped aliases here helps test cases be more specific about whether they're using
// FIDL names or bt-host internal names.
using bt::LowerBits;
using bt::StaticByteBuffer;
using bt::UpperBits;
using bt::l2cap::testing::FakeChannel;
using bt::sm::AuthReq;
using bt::sm::KeyDistGen;
using bt::testing::FakePeer;

namespace fbt = fuchsia::bluetooth;
namespace fsys = fuchsia::bluetooth::sys;

const bt::PeerId kTestId(1);
const bt::DeviceAddress kLeTestAddr(bt::DeviceAddress::Type::kLEPublic, {0x01, 0, 0, 0, 0, 0});
const bt::DeviceAddress kBredrTestAddr(bt::DeviceAddress::Type::kBREDR, {0x01, 0, 0, 0, 0, 0});

const fbt::Address kTestFidlAddrPublic{fbt::AddressType::PUBLIC, {1, 0, 0, 0, 0, 0}};
const fbt::Address kTestFidlAddrRandom{fbt::AddressType::RANDOM,
                                       {0x55, 0x44, 0x33, 0x22, 0x11, 0b11000011}};
const fbt::Address kTestFidlAddrResolvable{fbt::AddressType::RANDOM,
                                           {0x55, 0x44, 0x33, 0x22, 0x11, 0b01000011}};
const fbt::Address kTestFidlAddrNonResolvable{fbt::AddressType::RANDOM,
                                              {0x55, 0x44, 0x33, 0x22, 0x11, 0x00}};

class MockFidlPairingDelegate : public fsys::testing::PairingDelegate_TestBase {
 public:
  using PairingRequestCallback =
      fit::function<void(fsys::Peer, fsys::PairingMethod, uint32_t, OnPairingRequestCallback)>;
  using PairingCompleteCallback = fit::function<void(fbt::PeerId, bool)>;
  using RemoteKeypressCallback = fit::function<void(fbt::PeerId, fsys::PairingKeypress)>;

  MockFidlPairingDelegate(fidl::InterfaceRequest<PairingDelegate> request,
                          async_dispatcher_t* dispatcher)
      : binding_(this, std::move(request), dispatcher) {}

  ~MockFidlPairingDelegate() override = default;

  void OnPairingRequest(fsys::Peer device, fsys::PairingMethod method, uint32_t displayed_passkey,
                        OnPairingRequestCallback callback) override {
    pairing_request_cb_(std::move(device), method, displayed_passkey, std::move(callback));
  }

  void OnPairingComplete(fbt::PeerId id, bool success) override {
    pairing_complete_cb_(id, success);
  }

  void OnRemoteKeypress(fbt::PeerId id, fsys::PairingKeypress keypress) override {
    remote_keypress_cb_(id, keypress);
  }

  void set_pairing_request_cb(PairingRequestCallback cb) { pairing_request_cb_ = std::move(cb); }
  void set_pairing_complete_cb(PairingCompleteCallback cb) { pairing_complete_cb_ = std::move(cb); }
  void set_remote_keypress_cb(RemoteKeypressCallback cb) { remote_keypress_cb_ = std::move(cb); }

 private:
  void NotImplemented_(const std::string& name) override {
    FAIL() << name << " is not implemented";
  }

  fidl::Binding<PairingDelegate> binding_;
  PairingRequestCallback pairing_request_cb_;
  PairingCompleteCallback pairing_complete_cb_;
  RemoteKeypressCallback remote_keypress_cb_;
};

class HostServerTest : public bthost::testing::AdapterTestFixture {
 public:
  HostServerTest() = default;
  ~HostServerTest() override = default;

  void SetUp() override {
    AdapterTestFixture::SetUp();

    gatt_ = take_gatt();
    ResetHostServer();
  }

  void ResetHostServer() {
    fidl::InterfaceHandle<fuchsia::bluetooth::host::Host> host_handle;
    host_server_ = std::make_unique<HostServer>(host_handle.NewRequest().TakeChannel(),
                                                adapter()->AsWeakPtr(), gatt_->GetWeakPtr());
    host_.Bind(std::move(host_handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();

    host_ = nullptr;
    host_server_ = nullptr;
    gatt_ = nullptr;

    AdapterTestFixture::TearDown();
  }

 protected:
  HostServer* host_server() const { return host_server_.get(); }

  fuchsia::bluetooth::host::Host* host_client() const { return host_.get(); }

  // Mutable reference to the Host client interface pointer.
  fuchsia::bluetooth::host::HostPtr& host_client_ptr() { return host_; }

  // Create and bind a MockFidlPairingDelegate and attach it to the HostServer under test. It is
  // heap-allocated to permit its explicit destruction.
  [[nodiscard]] std::unique_ptr<MockFidlPairingDelegate> SetMockFidlPairingDelegate(
      fsys::InputCapability input_capability, fsys::OutputCapability output_capability) {
    using ::testing::StrictMock;
    fidl::InterfaceHandle<fsys::PairingDelegate> pairing_delegate_handle;
    auto pairing_delegate = std::make_unique<MockFidlPairingDelegate>(
        pairing_delegate_handle.NewRequest(), dispatcher());
    host_client()->SetPairingDelegate(input_capability, output_capability,
                                      std::move(pairing_delegate_handle));

    // Wait for the Access/SetPairingDelegate message to process.
    RunLoopUntilIdle();
    return pairing_delegate;
  }

  bt::gap::Peer* AddFakePeer(const bt::DeviceAddress& address) {
    bt::gap::Peer* peer = adapter()->peer_cache()->NewPeer(address, /*connectable=*/true);
    BT_ASSERT(peer);
    BT_ASSERT(peer->temporary());

    test_device()->AddPeer(std::make_unique<FakePeer>(address));

    return peer;
  }

  using ConnectResult = fpromise::result<void, fsys::Error>;
  std::optional<ConnectResult> ConnectFakePeer(bt::PeerId id) {
    std::optional<ConnectResult> result;
    host_client()->Connect(fbt::PeerId{id.value()},
                           [&](ConnectResult _result) { result = _result; });
    RunLoopUntilIdle();
    return result;
  }

  std::tuple<bt::gap::Peer*, FakeChannel::WeakPtr> CreateAndConnectFakePeer(
      bool connect_le = true) {
    auto address = connect_le ? kLeTestAddr : kBredrTestAddr;
    bt::gap::Peer* peer = AddFakePeer(address);

    // This is to capture the channel created during the Connection process
    FakeChannel::WeakPtr fake_chan = FakeChannel::WeakPtr();
    l2cap()->set_channel_callback(
        [&fake_chan](FakeChannel::WeakPtr new_fake_chan) { fake_chan = std::move(new_fake_chan); });

    auto connect_result = ConnectFakePeer(peer->identifier());

    if (!connect_result || connect_result->is_error()) {
      peer = nullptr;
      fake_chan.reset();
    }
    return std::make_tuple(peer, fake_chan);
  }

  // Calls the RestoreBonds method and verifies that the callback is run with the expected output.
  void TestRestoreBonds(std::vector<fsys::BondingData> bonds,
                        std::vector<fsys::BondingData> expected) {
    bool called = false;
    host_server()->RestoreBonds(std::move(bonds), [&](auto errors) {
      called = true;
      ASSERT_EQ(expected.size(), errors.size());
      for (size_t i = 0; i < errors.size(); i++) {
        SCOPED_TRACE(i);
        EXPECT_TRUE(fidl::Equals(errors[i], expected[i]));
      }
    });
    EXPECT_TRUE(called);
  }

 private:
  std::unique_ptr<HostServer> host_server_;
  std::unique_ptr<bt::gatt::GATT> gatt_;
  fuchsia::bluetooth::host::HostPtr host_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(HostServerTest);
};

// The main role of this sub-suite is improved test object lifecycle management (see TearDown for
// more details). An additional convenience it provides is fake peer/channel and mock pairing
// delegate setup, which all tests of the full pairing stack need.
class HostServerPairingTest : public HostServerTest {
 public:
  void SetUp() override {
    HostServerTest::SetUp();
    NewPairingTest(fsys::InputCapability::NONE, fsys::OutputCapability::NONE);
  }

  void NewPairingTest(fsys::InputCapability input_cap, fsys::OutputCapability output_cap,
                      bool is_le = true) {
    pairing_delegate_ = SetMockFidlPairingDelegate(input_cap, output_cap);
    if (!fake_peer_ || !fake_chan_.is_alive()) {
      ASSERT_FALSE(fake_peer_);
      ASSERT_FALSE(fake_chan_.is_alive());
      std::tie(fake_peer_, fake_chan_) = CreateAndConnectFakePeer(is_le);
      ASSERT_TRUE(fake_peer_);
      ASSERT_TRUE(fake_chan_.is_alive());
      ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, fake_peer_->le()->connection_state());
    }
  }

  // With the base HostServerTest, it is too easy to set up callbacks related to fake channels or
  // the mock pairing delegate that lead to unexpected failure callbacks, or worse, use-after-
  // frees. These failures mostly stem from the Host server notifying the client upon pairing
  // delegate destruction, which is not important behavior for many tests.
  void TearDown() override {
    fake_chan_->SetSendCallback(/*callback=*/nullptr, /*dispatcher=*/nullptr);
    host_client_ptr().Unbind();
    HostServerTest::TearDown();
  }

  bt::gap::Peer* peer() { return fake_peer_; }
  FakeChannel::WeakPtr fake_chan() { return fake_chan_; }

 private:
  std::unique_ptr<MockFidlPairingDelegate> pairing_delegate_;
  bt::gap::Peer* fake_peer_ = nullptr;
  FakeChannel::WeakPtr fake_chan_;
};

// Constructs a vector of a fidl::Clone'able data type that contains a copy of the input |data|.
// This allows move-only FIDL types to be re-used in test cases that need to refer to such data.
//
// Returns an empty vector if |data| could not be copied, e.g. because it contains handles that
// cannot be duplicated.
template <typename T>
std::vector<T> MakeClonedVector(const T& data) {
  std::vector<T> output;
  T clone;

  zx_status_t status = fidl::Clone(data, &clone);
  EXPECT_EQ(ZX_OK, status);
  if (status == ZX_OK) {
    output.push_back(std::move(clone));
  }

  return output;
}

// Construct bonding data structure for testing using the given ID and address and an empty LE bond
// structure.
fsys::BondingData MakeTestBond(bt::PeerId id, fbt::Address address) {
  fsys::BondingData bond;
  bond.set_identifier(fbt::PeerId{id.value()});
  bond.set_address(address);
  bond.set_le_bond(fsys::LeBondData());
  return bond;
}

TEST_F(HostServerTest, FidlIoCapabilitiesMapToHostIoCapability) {
  // Isolate HostServer's private bt::gap::PairingDelegate implementation.
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());

  // Getter should be safe to call when no PairingDelegate assigned.
  EXPECT_EQ(bt::sm::IOCapability::kNoInputNoOutput, host_pairing_delegate->io_capability());

  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);
  EXPECT_EQ(bt::sm::IOCapability::kKeyboardDisplay, host_pairing_delegate->io_capability());
}

TEST_F(HostServerTest, HostCompletePairingCallsFidlOnPairingComplete) {
  using namespace ::testing;

  // Isolate HostServer's private bt::gap::PairingDelegate implementation.
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  // fuchsia::bluetooth::PeerId has no equality operator
  std::optional<fbt::PeerId> actual_id;
  fidl_pairing_delegate->set_pairing_complete_cb(
      [&actual_id](fbt::PeerId id, Unused) { actual_id = id; });
  auto id = bt::PeerId(0xc0decafe);
  host_pairing_delegate->CompletePairing(
      id, fit::error(bt::Error(bt::sm::ErrorCode::kConfirmValueFailed)));

  // Wait for the PairingDelegate/OnPairingComplete message to process.
  RunLoopUntilIdle();

  EXPECT_TRUE(actual_id.has_value());
  EXPECT_EQ(id.value(), actual_id.value().value);
}

TEST_F(HostServerTest, HostConfirmPairingRequestsConsentPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, fsys::PairingMethod method,
                                uint32_t displayed_passkey,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        EXPECT_EQ(method, fsys::PairingMethod::CONSENT);
        EXPECT_EQ(displayed_passkey, 0u);
        callback(/*accept=*/true, /*entered_passkey=*/0u);
      });

  bool confirm_cb_value = false;
  bt::gap::PairingDelegate::ConfirmCallback confirm_cb = [&confirm_cb_value](bool confirmed) {
    confirm_cb_value = confirmed;
  };
  host_pairing_delegate->ConfirmPairing(peer->identifier(), std::move(confirm_cb));

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_value);
}

TEST_F(HostServerTest, HostDisplayPasskeyRequestsPasskeyDisplayOrNumericComparisonPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  // This call should use PASSKEY_DISPLAY to request that the user perform peer passkey entry.
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, fsys::PairingMethod method,
                                uint32_t displayed_passkey,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        EXPECT_EQ(method, fsys::PairingMethod::PASSKEY_DISPLAY);
        EXPECT_EQ(displayed_passkey, 12345u);
        callback(/*accept=*/false, /*entered_passkey=*/0u);
      });

  bool confirm_cb_called = false;
  auto confirm_cb = [&confirm_cb_called](bool confirmed) {
    EXPECT_FALSE(confirmed);
    confirm_cb_called = true;
  };
  using DisplayMethod = bt::gap::PairingDelegate::DisplayMethod;
  host_pairing_delegate->DisplayPasskey(peer->identifier(), 12345, DisplayMethod::kPeerEntry,
                                        confirm_cb);

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_called);

  // This call should use PASSKEY_COMPARISON to request that the user compare the passkeys shown on
  // the local and peer devices.
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, fsys::PairingMethod method,
                                uint32_t displayed_passkey,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        EXPECT_EQ(method, fsys::PairingMethod::PASSKEY_COMPARISON);
        EXPECT_EQ(displayed_passkey, 12345u);
        callback(/*accept=*/false, /*entered_passkey=*/0u);
      });

  confirm_cb_called = false;
  host_pairing_delegate->DisplayPasskey(peer->identifier(), 12345, DisplayMethod::kComparison,
                                        confirm_cb);

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_called);
}

TEST_F(HostServerTest, HostRequestPasskeyRequestsPasskeyEntryPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  std::optional<int64_t> passkey_response;
  auto response_cb = [&passkey_response](int64_t passkey) { passkey_response = passkey; };

  // The first request is rejected and should receive a negative passkey value, regardless what was
  // passed over FIDL (i.e. 12345).
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, fsys::PairingMethod method,
                                uint32_t displayed_passkey,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        EXPECT_EQ(method, fsys::PairingMethod::PASSKEY_ENTRY);
        EXPECT_EQ(displayed_passkey, 0u);
        callback(/*accept=*/false, /*entered_passkey=*/12345);
      });

  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_LT(passkey_response.value(), 0);

  // The second request should be accepted with the passkey set to "0".
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/true, /*entered_passkey=*/0);
      });

  passkey_response.reset();
  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_EQ(0, passkey_response.value());

  // The third request should be accepted with the passkey set to "12345".
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                fsys::PairingDelegate::OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/true, /*entered_passkey=*/12345);
      });

  passkey_response.reset();
  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_EQ(12345, passkey_response.value());
}

TEST_F(HostServerTest, SysDelegateInvokesCallbackMultipleTimesIgnored) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockFidlPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  using OnPairingRequestCallback = fsys::PairingDelegate::OnPairingRequestCallback;
  OnPairingRequestCallback fidl_passkey_req_cb = nullptr, fidl_confirm_req_cb = nullptr;
  fidl_pairing_delegate->set_pairing_request_cb(
      [id = peer->identifier(), &fidl_passkey_req_cb, &fidl_confirm_req_cb](
          fsys::Peer peer, fsys::PairingMethod method, auto /*ignore*/,
          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        if (method == fsys::PairingMethod::PASSKEY_ENTRY) {
          fidl_passkey_req_cb = std::move(callback);
        } else if (method == fsys::PairingMethod::CONSENT) {
          fidl_confirm_req_cb = std::move(callback);
        } else {
          FAIL() << "unexpected pairing request method!";
        }
      });

  size_t passkey_req_cb_count = 0, confirm_req_cb_count = 0;
  auto passkey_response_cb = [&passkey_req_cb_count](int64_t /*ignore*/) {
    passkey_req_cb_count++;
    ;
  };
  auto confirm_req_cb = [&confirm_req_cb_count](bool /*ignore*/) { confirm_req_cb_count++; };

  host_pairing_delegate->RequestPasskey(peer->identifier(), passkey_response_cb);
  host_pairing_delegate->ConfirmPairing(peer->identifier(), confirm_req_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_passkey_req_cb);
  ASSERT_TRUE(fidl_confirm_req_cb);

  ASSERT_EQ(0u, passkey_req_cb_count);
  ASSERT_EQ(0u, confirm_req_cb_count);

  fidl_passkey_req_cb(true, 12345);
  fidl_confirm_req_cb(true, 0);
  RunLoopUntilIdle();
  ASSERT_EQ(1u, passkey_req_cb_count);
  ASSERT_EQ(1u, confirm_req_cb_count);

  fidl_passkey_req_cb(true, 456789);
  fidl_confirm_req_cb(true, 0);
  RunLoopUntilIdle();
  ASSERT_EQ(1u, passkey_req_cb_count);
  ASSERT_EQ(1u, confirm_req_cb_count);
}

TEST_F(HostServerTest, WatchState) {
  std::optional<fsys::HostInfo> info;
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_id());
  ASSERT_TRUE(info->has_technology());
  ASSERT_TRUE(info->has_local_name());
  ASSERT_TRUE(info->has_discoverable());
  ASSERT_TRUE(info->has_discovering());
  ASSERT_TRUE(info->has_addresses());

  EXPECT_EQ(adapter()->identifier().value(), info->id().value);
  EXPECT_EQ(fsys::TechnologyType::DUAL_MODE, info->technology());
  EXPECT_EQ("fuchsia", info->local_name());
  EXPECT_FALSE(info->discoverable());
  EXPECT_FALSE(info->discovering());
  EXPECT_EQ(fbt::AddressType::PUBLIC, info->addresses()[0].type);
  EXPECT_TRUE(
      ContainersEqual(adapter()->state().controller_address.bytes(), info->addresses()[0].bytes));
}

TEST_F(HostServerTest, WatchDiscoveryState) {
  std::optional<fsys::HostInfo> info;

  // Make initial watch call so that subsequent calls remain pending.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  info.reset();

  // Watch for updates.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());

  host_server()->StartDiscovery([](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discovering());
  EXPECT_TRUE(info->discovering());

  info.reset();
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());
  host_server()->StopDiscovery();
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discovering());
  EXPECT_FALSE(info->discovering());
}

TEST_F(HostServerTest, WatchDiscoverableState) {
  std::optional<fsys::HostInfo> info;

  // Make initial watch call so that subsequent calls remain pending.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  info.reset();

  // Watch for updates.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());

  host_server()->SetDiscoverable(/*discoverable=*/true, [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discoverable());
  EXPECT_TRUE(info->discoverable());

  info.reset();
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());
  host_server()->SetDiscoverable(/*discoverable=*/false, [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discoverable());
  EXPECT_FALSE(info->discoverable());
}

TEST_F(HostServerPairingTest, InitiatePairingLeDefault) {
  const StaticByteBuffer kExpected(
      0x01,  // code: "Pairing Request"
      0x04,  // IO cap.: KeyboardDisplay
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
      0x10,                                        // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey | KeyDistGen::kLinkKey  // responder keys
  );

  // IOCapabilities must be KeyboardDisplay to support default MITM pairing request.
  NewPairingTest(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan()->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fpromise::result<void, fsys::Error>> pair_result;
  fsys::PairingOptions opts;
  host_client()->Pair(fbt::PeerId{peer()->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxbug.dev/886): We don't have a good mechanism for driving pairing to completion without
  // faking the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up
  // to the FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(HostServerPairingTest, InitiatePairingLeEncrypted) {
  const StaticByteBuffer kExpected(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,                                        // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey | KeyDistGen::kLinkKey  // responder keys
  );

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan()->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fpromise::result<void, fsys::Error>> pair_result;
  fsys::PairingOptions opts;
  opts.set_le_security_level(fsys::PairingSecurityLevel::ENCRYPTED);
  host_client()->Pair(fbt::PeerId{peer()->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxbug.dev/886): We don't have a good mechanism for driving pairing to completion without
  // faking the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up
  // to the FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(HostServerPairingTest, InitiatePairingNonBondableLe) {
  const StaticByteBuffer kExpected(0x01,  // code: "Pairing Request"
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   0x00,  // initiator keys: none
                                   0x00   // responder keys: none
  );

  // IOCapabilities must be KeyboardDisplay to support default MITM pairing request.
  NewPairingTest(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan()->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fpromise::result<void, fsys::Error>> pair_result;
  fsys::PairingOptions opts;
  opts.set_bondable_mode(fsys::BondableMode::NON_BONDABLE);
  host_client()->Pair(fbt::PeerId{peer()->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxbug.dev/886): We don't have a good mechanism for driving pairing to completion without
  // faking the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up
  // to the FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(HostServerTest, InitiateBrEdrPairingLePeerFails) {
  auto [peer, fake_chan] = CreateAndConnectFakePeer();
  ASSERT_TRUE(peer);
  ASSERT_TRUE(fake_chan.is_alive());
  ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, peer->le()->connection_state());

  std::optional<fpromise::result<void, fsys::Error>> pair_result;
  fsys::PairingOptions opts;
  // Set pairing option with classic
  opts.set_transport(fsys::TechnologyType::CLASSIC);
  auto pair_cb = [&](auto result) {
    ASSERT_TRUE(result.is_err());
    pair_result = std::move(result);
  };
  host_client()->Pair(fbt::PeerId{peer->identifier().value()}, std::move(opts), std::move(pair_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(pair_result);
  ASSERT_EQ(pair_result->error(), fsys::Error::PEER_NOT_FOUND);
}

TEST_F(HostServerTest, WatchPeersHangsOnFirstCallWithNoExistingPeers) {
  // By default the peer cache contains no entries when HostServer is first constructed. The first
  // call to WatchPeers should hang.
  bool replied = false;
  host_server()->WatchPeers([&](auto, auto) { replied = true; });
  EXPECT_FALSE(replied);
}

TEST_F(HostServerTest, WatchPeersRepliesOnFirstCallWithExistingPeers) {
  [[maybe_unused]] bt::gap::Peer* peer =
      adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ResetHostServer();

  // The first call to WatchPeers immediately resolves with the contents of the peer cache.
  bool replied = false;
  host_server()->WatchPeers([&](auto updated, auto removed) {
    EXPECT_EQ(1u, updated.size());
    EXPECT_TRUE(removed.empty());
    replied = true;
  });
  EXPECT_TRUE(replied);
}

TEST_F(HostServerTest, WatchPeersHandlesNonEnumeratedAppearanceInPeer) {
  using namespace ::testing;
  bt::gap::Peer* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);
  bt::AdvertisingData adv_data;

  // Invalid appearance.
  adv_data.SetAppearance(0xFFFFu);
  bt::DynamicByteBuffer write_buf(adv_data.CalculateBlockSize(/*include_flags=*/true));
  ASSERT_TRUE(adv_data.WriteBlock(&write_buf, bt::AdvFlag::kLEGeneralDiscoverableMode));
  peer->MutLe().SetAdvertisingData(/*rssi=*/0, write_buf, zx::time());

  ResetHostServer();

  bool replied = false;
  host_client()->WatchPeers([&](auto updated, [[maybe_unused]] auto removed) {
    // Client should still receive updates to this peer.
    replied = true;
    const fbt::PeerId id = {peer->identifier().value()};
    ASSERT_THAT(updated, Contains(Property(&fsys::Peer::id, id)));
    EXPECT_FALSE(updated.front().has_appearance());
  });
  RunLoopUntilIdle();
  EXPECT_TRUE(replied);
}

TEST_F(HostServerTest, WatchPeersStateMachine) {
  std::optional<std::vector<fsys::Peer>> updated;
  std::optional<std::vector<fbt::PeerId>> removed;

  // Initial watch call hangs as the cache is empty.
  host_server()->WatchPeers([&](auto updated_arg, auto removed_arg) {
    updated = std::move(updated_arg);
    removed = std::move(removed_arg);
  });
  ASSERT_FALSE(updated.has_value());
  ASSERT_FALSE(removed.has_value());

  // Adding a new peer should resolve the hanging get.
  bt::gap::Peer* peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(updated.has_value());
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(1u, updated->size());
  EXPECT_TRUE(fidl::Equals(fidl_helpers::PeerToFidl(*peer), (*updated)[0]));
  EXPECT_TRUE(removed->empty());
  updated.reset();
  removed.reset();

  // The next call should hang.
  host_server()->WatchPeers([&](auto updated_arg, auto removed_arg) {
    updated = std::move(updated_arg);
    removed = std::move(removed_arg);
  });
  ASSERT_FALSE(updated.has_value());
  ASSERT_FALSE(removed.has_value());

  // Removing the peer should resolve the hanging get.
  auto peer_id = peer->identifier();
  [[maybe_unused]] auto result = adapter()->peer_cache()->RemoveDisconnectedPeer(peer_id);
  ASSERT_TRUE(updated.has_value());
  ASSERT_TRUE(removed.has_value());
  EXPECT_TRUE(updated->empty());
  EXPECT_EQ(1u, removed->size());
  EXPECT_TRUE(fidl::Equals(fbt::PeerId{peer_id.value()}, (*removed)[0]));
}

TEST_F(HostServerTest, WatchPeersUpdatedThenRemoved) {
  // Add then remove a peer. The watcher should only report the removal.
  bt::PeerId id;
  {
    bt::gap::Peer* peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
    id = peer->identifier();

    // |peer| becomes a dangling pointer after the call to RemoveDisconnectedPeer. We scoped the
    // binding of |peer| so that it doesn't exist beyond this point.
    [[maybe_unused]] auto result = adapter()->peer_cache()->RemoveDisconnectedPeer(id);
  }

  bool replied = false;
  host_server()->WatchPeers([&replied, id](auto updated, auto removed) {
    EXPECT_TRUE(updated.empty());
    EXPECT_EQ(1u, removed.size());
    EXPECT_TRUE(fidl::Equals(fbt::PeerId{id.value()}, removed[0]));
    replied = true;
  });
  EXPECT_TRUE(replied);
}

TEST_F(HostServerTest, SetLeSecurityMode) {
  // Set the HostServer to SecureConnectionsOnly mode first
  host_client()->SetLeSecurityMode(fsys::LeSecurityMode::SECURE_CONNECTIONS_ONLY);
  RunLoopUntilIdle();
  ASSERT_EQ(fidl_helpers::LeSecurityModeFromFidl(fsys::LeSecurityMode::SECURE_CONNECTIONS_ONLY),
            adapter()->le()->security_mode());

  // Set the HostServer back to Mode 1 and verify that the change takes place
  host_client()->SetLeSecurityMode(fsys::LeSecurityMode::MODE_1);
  RunLoopUntilIdle();
  ASSERT_EQ(fidl_helpers::LeSecurityModeFromFidl(fsys::LeSecurityMode::MODE_1),
            adapter()->le()->security_mode());
}

TEST_F(HostServerTest, ConnectLowEnergy) {
  bt::gap::Peer* peer = AddFakePeer(kLeTestAddr);
  EXPECT_EQ(bt::gap::TechnologyType::kLowEnergy, peer->technology());

  auto result = ConnectFakePeer(peer->identifier());
  ASSERT_TRUE(result);
  ASSERT_FALSE(result->is_error());

  EXPECT_FALSE(peer->bredr());
  ASSERT_TRUE(peer->le());
  EXPECT_TRUE(peer->le()->connected());

  // bt-host should only attempt to connect the LE transport.
  EXPECT_EQ(1, test_device()->le_create_connection_command_count());
  EXPECT_EQ(0, test_device()->acl_create_connection_command_count());
}

TEST_F(HostServerTest, ConnectBredr) {
  bt::gap::Peer* peer = AddFakePeer(kBredrTestAddr);
  EXPECT_EQ(bt::gap::TechnologyType::kClassic, peer->technology());

  auto result = ConnectFakePeer(peer->identifier());
  ASSERT_TRUE(result);
  ASSERT_FALSE(result->is_error());

  EXPECT_FALSE(peer->le());
  ASSERT_TRUE(peer->bredr());

  // bt-host should only attempt to connect the BR/EDR transport.
  EXPECT_EQ(0, test_device()->le_create_connection_command_count());
  EXPECT_EQ(1, test_device()->acl_create_connection_command_count());
}

TEST_F(HostServerTest, ConnectDualMode) {
  // Initialize the peer with data for both transport types.
  bt::gap::Peer* peer = AddFakePeer(kBredrTestAddr);
  peer->MutLe();
  ASSERT_TRUE(peer->le());
  peer->MutBrEdr();
  ASSERT_TRUE(peer->bredr());
  EXPECT_EQ(bt::gap::TechnologyType::kDualMode, peer->technology());

  auto result = ConnectFakePeer(peer->identifier());
  ASSERT_TRUE(result);
  ASSERT_FALSE(result->is_error());

  // bt-host should only attempt to connect the BR/EDR transport.
  EXPECT_FALSE(peer->le()->connected());
  EXPECT_EQ(0, test_device()->le_create_connection_command_count());
  EXPECT_EQ(1, test_device()->acl_create_connection_command_count());
}

TEST_F(HostServerTest, RestoreBondsErrorDataMissing) {
  fsys::BondingData bond;

  // Empty bond.
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // ID missing.
  bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  bond.clear_identifier();
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // Address missing.
  bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  bond.clear_address();
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // Transport data missing.
  bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  bond.clear_le_bond();
  bond.clear_bredr_bond();
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // Transport data missing keys.
  bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));
}

TEST_F(HostServerTest, RestoreBondsInvalidAddress) {
  // LE Random address on dual-mode or BR/EDR-only bond should not be supported.
  fsys::BondingData bond = MakeTestBond(kTestId, kTestFidlAddrRandom);
  bond.set_bredr_bond(fsys::BredrBondData());
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // BR/EDR only
  bond.clear_le_bond();
  TestRestoreBonds(MakeClonedVector(bond), MakeClonedVector(bond));

  // Resolvable Private address should not be supported
  fsys::BondingData resolvable_bond = MakeTestBond(kTestId, kTestFidlAddrResolvable);
  TestRestoreBonds(MakeClonedVector(resolvable_bond), MakeClonedVector(resolvable_bond));

  // Non-resolvable Private address should not be supported
  fsys::BondingData non_resolvable_bond = MakeTestBond(kTestId, kTestFidlAddrNonResolvable);
  TestRestoreBonds(MakeClonedVector(non_resolvable_bond), MakeClonedVector(non_resolvable_bond));
}

TEST_F(HostServerTest, RestoreBondsLeOnlySuccess) {
  fsys::BondingData bond = MakeTestBond(kTestId, kTestFidlAddrRandom);
  auto ltk =
      fsys::Ltk{.key =
                    fsys::PeerKey{
                        .security =
                            fsys::SecurityProperties{
                                .authenticated = true,
                                .secure_connections = true,
                                .encryption_key_size = 16,
                            },
                        .data =
                            fsys::Key{
                                .value = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
                            },
                    },
                .ediv = 0,
                .rand = 0};
  fsys::LeBondData le;
  le.set_peer_ltk(ltk);
  le.set_local_ltk(ltk);
  bond.set_le_bond(std::move(le));

  // This should succeed.
  TestRestoreBonds(MakeClonedVector(bond), {} /* no errors expected */);

  auto* peer = adapter()->peer_cache()->FindById(kTestId);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(peer->le());
  EXPECT_FALSE(peer->bredr());
  EXPECT_EQ(bt::DeviceAddress::Type::kLERandom, peer->address().type());
}

TEST_F(HostServerTest, RestoreBondsBredrOnlySuccess) {
  fsys::BondingData bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  bond.clear_le_bond();

  fsys::BredrBondData bredr;
  bredr.set_link_key(fsys::PeerKey{
      .security =
          fsys::SecurityProperties{
              .authenticated = true,
              .secure_connections = true,
              .encryption_key_size = 16,
          },
      .data =
          fsys::Key{
              .value = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
          },
  });
  constexpr bt::UUID kServiceId = bt::sdp::profile::kAudioSink;
  bredr.set_services({fidl_helpers::UuidToFidl(kServiceId)});
  bond.set_bredr_bond(std::move(bredr));

  // This should succeed.
  TestRestoreBonds(MakeClonedVector(bond), {} /* no errors expected */);

  auto* peer = adapter()->peer_cache()->FindById(kTestId);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->bredr());
  EXPECT_THAT(peer->bredr()->services(), ::testing::ElementsAre(kServiceId));
  EXPECT_FALSE(peer->le());
  EXPECT_EQ(bt::DeviceAddress::Type::kBREDR, peer->address().type());
}

TEST_F(HostServerTest, RestoreBondsDualModeSuccess) {
  fsys::BondingData bond = MakeTestBond(kTestId, kTestFidlAddrPublic);
  auto key = fsys::PeerKey{
      .security =
          fsys::SecurityProperties{
              .authenticated = true,
              .secure_connections = true,
              .encryption_key_size = 16,
          },
      .data =
          fsys::Key{
              .value = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
          },
  };
  auto ltk = fsys::Ltk{.key = key, .ediv = 0, .rand = 0};
  fsys::LeBondData le;
  le.set_peer_ltk(ltk);
  le.set_local_ltk(ltk);
  bond.set_le_bond(std::move(le));

  fsys::BredrBondData bredr;
  bredr.set_link_key(key);
  constexpr bt::UUID kServiceId = bt::sdp::profile::kAudioSink;
  bredr.set_services({fidl_helpers::UuidToFidl(kServiceId)});
  bond.set_bredr_bond(std::move(bredr));

  // This should succeed.
  TestRestoreBonds(MakeClonedVector(bond), {} /* no errors expected */);

  auto* peer = adapter()->peer_cache()->FindById(kTestId);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(peer->le());
  ASSERT_TRUE(peer->bredr());
  EXPECT_THAT(peer->bredr()->services(), ::testing::ElementsAre(kServiceId));
  EXPECT_EQ(bt::DeviceAddress::Type::kBREDR, peer->address().type());
}

TEST_F(HostServerTest, SetHostData) {
  EXPECT_FALSE(adapter()->le()->irk());

  fsys::Key irk{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
  fsys::HostData data;
  data.set_irk(irk);

  host_server()->SetLocalData(std::move(data));
  ASSERT_TRUE(adapter()->le()->irk());
  EXPECT_EQ(irk.value, adapter()->le()->irk().value());
}

TEST_F(HostServerTest, OnNewBondingData) {
  const std::string kTestName = "florp";
  const bt::UInt128 kTestKeyValue{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const bt::sm::SecurityProperties kTestSecurity(bt::sm::SecurityLevel::kSecureAuthenticated, 16,
                                                 /*secure_connections=*/true);
  const bt::sm::LTK kTestLtk(kTestSecurity, bt::hci_spec::LinkKey(kTestKeyValue, 0, 0));
  const fsys::PeerKey kTestKeyFidl{
      .security =
          fsys::SecurityProperties{
              .authenticated = true,
              .secure_connections = true,
              .encryption_key_size = 16,
          },
      .data = fsys::Key{.value = kTestKeyValue},
  };
  const fsys::Ltk kTestLtkFidl{.key = kTestKeyFidl, .ediv = 0, .rand = 0};

  std::optional<fsys::BondingData> data;
  host_client_ptr().events().OnNewBondingData = [&](auto _data) { data = std::move(_data); };

  auto* peer = adapter()->peer_cache()->NewPeer(kBredrTestAddr, /*connectable=*/true);
  peer->RegisterName(kTestName);
  adapter()->peer_cache()->StoreLowEnergyBond(peer->identifier(),
                                              bt::sm::PairingData{.peer_ltk = {kTestLtk}});
  RunLoopUntilIdle();

  ASSERT_TRUE(data);
  ASSERT_TRUE(data->has_identifier());
  ASSERT_TRUE(data->has_local_address());
  ASSERT_TRUE(data->has_address());
  ASSERT_TRUE(data->has_name());

  EXPECT_TRUE(fidl::Equals((fbt::Address{fbt::AddressType::PUBLIC, std::array<uint8_t, 6>{0}}),
                           data->local_address()));
  EXPECT_TRUE(fidl::Equals(kTestFidlAddrPublic, data->address()));
  EXPECT_EQ(kTestName, data->name());

  ASSERT_TRUE(data->has_le_bond());
  EXPECT_FALSE(data->has_bredr_bond());

  ASSERT_TRUE(data->le_bond().has_peer_ltk());
  EXPECT_FALSE(data->le_bond().has_local_ltk());
  EXPECT_FALSE(data->le_bond().has_irk());
  EXPECT_FALSE(data->le_bond().has_csrk());
  EXPECT_TRUE(fidl::Equals(kTestLtkFidl, data->le_bond().peer_ltk()));

  // Add BR/EDR data.
  data.reset();
  adapter()->peer_cache()->StoreBrEdrBond(kBredrTestAddr, kTestLtk);
  RunLoopUntilIdle();

  ASSERT_TRUE(data);
  ASSERT_TRUE(data->has_identifier());
  ASSERT_TRUE(data->has_local_address());
  ASSERT_TRUE(data->has_address());
  ASSERT_TRUE(data->has_name());

  EXPECT_TRUE(fidl::Equals((fbt::Address{fbt::AddressType::PUBLIC, std::array<uint8_t, 6>{0}}),
                           data->local_address()));
  EXPECT_TRUE(fidl::Equals(kTestFidlAddrPublic, data->address()));
  EXPECT_EQ(kTestName, data->name());

  ASSERT_TRUE(data->has_le_bond());
  ASSERT_TRUE(data->le_bond().has_peer_ltk());
  EXPECT_FALSE(data->le_bond().has_local_ltk());
  EXPECT_FALSE(data->le_bond().has_irk());
  EXPECT_FALSE(data->le_bond().has_csrk());
  EXPECT_TRUE(fidl::Equals(kTestLtkFidl, data->le_bond().peer_ltk()));

  ASSERT_TRUE(data->has_bredr_bond());
  ASSERT_TRUE(data->bredr_bond().has_link_key());
  EXPECT_TRUE(fidl::Equals(kTestKeyFidl, data->bredr_bond().link_key()));
}

TEST_F(HostServerTest, EnableBackgroundScan) {
  host_server()->EnableBackgroundScan(true);
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE, test_device()->le_scan_state().scan_type);

  host_server()->EnableBackgroundScan(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
}

TEST_F(HostServerTest, EnableBackgroundScanTwiceAtSameTime) {
  host_server()->EnableBackgroundScan(true);
  host_server()->EnableBackgroundScan(true);
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE, test_device()->le_scan_state().scan_type);

  host_server()->EnableBackgroundScan(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
}

TEST_F(HostServerTest, EnableBackgroundScanTwiceSequentially) {
  host_server()->EnableBackgroundScan(true);
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE, test_device()->le_scan_state().scan_type);

  host_server()->EnableBackgroundScan(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE, test_device()->le_scan_state().scan_type);

  host_server()->EnableBackgroundScan(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
}

TEST_F(HostServerTest, CancelEnableBackgroundScan) {
  host_server()->EnableBackgroundScan(true);
  host_server()->EnableBackgroundScan(false);

  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  host_server()->EnableBackgroundScan(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
}

TEST_F(HostServerTest, DisableBackgroundScan) {
  host_server()->EnableBackgroundScan(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
}

TEST_F(HostServerTest, EnableBackgroundScanFailsToStart) {
  test_device()->SetDefaultCommandStatus(bt::hci_spec::kLESetScanEnable,
                                         pw::bluetooth::emboss::StatusCode::CONTROLLER_BUSY);
  host_server()->EnableBackgroundScan(true);
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  RunLoopUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);

  test_device()->ClearDefaultCommandStatus(bt::hci_spec::kLESetScanEnable);
  host_server()->EnableBackgroundScan(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
}

class HostServerTestFakeAdapter : public bt::gap::testing::FakeAdapterTestFixture {
 public:
  HostServerTestFakeAdapter() = default;
  ~HostServerTestFakeAdapter() override = default;

  void SetUp() override {
    FakeAdapterTestFixture::SetUp();
    gatt_ = std::make_unique<bt::gatt::testing::FakeLayer>();
    fidl::InterfaceHandle<fuchsia::bluetooth::host::Host> host_handle;
    host_server_ = std::make_unique<HostServer>(host_handle.NewRequest().TakeChannel(),
                                                adapter()->AsWeakPtr(), gatt_->GetWeakPtr());
    host_.Bind(std::move(host_handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();
    host_ = nullptr;
    host_server_ = nullptr;
    gatt_ = nullptr;
    FakeAdapterTestFixture::TearDown();
  }

 protected:
  HostServer* host_server() const { return host_server_.get(); }

  fuchsia::bluetooth::host::Host* host_client() const { return host_.get(); }

  fuchsia::bluetooth::host::HostPtr& host_client_ptr() { return host_; }

 private:
  std::unique_ptr<HostServer> host_server_;
  fuchsia::bluetooth::host::HostPtr host_;
  std::unique_ptr<bt::gatt::GATT> gatt_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(HostServerTestFakeAdapter);
};

TEST_F(HostServerTestFakeAdapter, SetLocalNameNotifiesWatchState) {
  std::vector<fsys::HostInfo> info;
  // Consume initial state value.
  host_client()->WatchState([&](auto value) { info.push_back(std::move(value)); });
  RunLoopUntilIdle();
  EXPECT_EQ(info.size(), 1u);
  // Second watch state will hang until state is updated.
  host_client()->WatchState([&](auto value) { info.push_back(std::move(value)); });
  RunLoopUntilIdle();
  EXPECT_EQ(info.size(), 1u);

  int cb_count = 0;
  host_client()->SetLocalName("test", [&](auto result) {
    EXPECT_TRUE(result.is_response());
    cb_count++;
  });
  RunLoopUntilIdle();
  EXPECT_EQ(cb_count, 1);
  EXPECT_EQ(adapter()->local_name(), "test");
  ASSERT_EQ(info.size(), 2u);
  ASSERT_TRUE(info.back().has_local_name());
  EXPECT_EQ(info.back().local_name(), "test");
}

TEST_F(HostServerTestFakeAdapter, WatchAddressesState) {
  std::optional<fsys::HostInfo> info;

  // Make an initial watch call so that subsequent calls remain pending.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  info.reset();

  // Next request to watch should hang and not produce a result.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());

  host_server()->EnablePrivacy(/*enabled=*/true);
  RunLoopUntilIdle();
  // The LE address change is an asynchronous operation. The state watcher should only update when
  // the address changes.
  EXPECT_FALSE(info.has_value());
  // Simulate a change in random LE address.
  auto resolvable_address =
      bt::DeviceAddress(bt::DeviceAddress::Type::kLERandom, {0x55, 0x44, 0x33, 0x22, 0x11, 0x43});
  adapter()->fake_le()->UpdateRandomAddress(resolvable_address);
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_addresses());
  // Both the public and private addresses should be reported.
  EXPECT_EQ(info->addresses().size(), 2u);
  EXPECT_TRUE(
      ContainersEqual(adapter()->state().controller_address.bytes(), info->addresses()[0].bytes));
  EXPECT_EQ(fbt::AddressType::RANDOM, info->addresses()[1].type);
  EXPECT_TRUE(ContainersEqual(adapter()->le()->CurrentAddress().value().bytes(),
                              info->addresses()[1].bytes));

  info.reset();
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());
  // Disabling privacy is a synchronous operation - the random LE address should no longer be used.
  host_server()->EnablePrivacy(/*enabled=*/false);
  RunLoopUntilIdle();

  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_addresses());
  // Only the public address should be reported.
  EXPECT_EQ(info->addresses().size(), 1u);
  EXPECT_TRUE(
      ContainersEqual(adapter()->state().controller_address.bytes(), info->addresses()[0].bytes));
}

}  // namespace
}  // namespace bthost
