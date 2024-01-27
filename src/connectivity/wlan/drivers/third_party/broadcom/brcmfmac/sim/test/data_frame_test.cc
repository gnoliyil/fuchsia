// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <zircon/errors.h>

#include <array>
#include <cstdint>
#include <vector>

#include "fuchsia/hardware/network/driver/c/banjo.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_utils.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/macaddr.h"

// infrastructure BSS diagram:
//        ap
//       /  \
//      /    \
// brcmfmac   client (the test)
//
// "Client" in the context of this test often refers to the test, which may act as either
// a destination of an Rx from the driver or a source of a Tx to the driver.
// In the traditional sense of the meaning, both the driver and the test are clients to the ap.
namespace wlan::brcmfmac {
namespace {

constexpr zx::duration kSimulatedClockDuration = zx::sec(10);

}  // namespace

// Some default AP and association request values
constexpr wlan_channel_t kDefaultChannel = {
    .primary = 9, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
constexpr simulation::WlanTxInfo kDefaultTxInfo = {.channel = kDefaultChannel};
constexpr cssid_t kApSsid = {.len = 15, .data = "Fuchsia Fake AP"};
const common::MacAddr kApBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
constexpr uint8_t kIes[] = {
    // SSID
    0x00, 0x0f, 'F', 'u', 'c', 'h', 's', 'i', 'a', ' ', 'F', 'a', 'k', 'e', ' ', 'A', 'P',
    // Supported rates
    0x01, 0x08, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    // DS parameter set - channel 157
    0x03, 0x01, 0x9d,
    // DTIM
    0x05, 0x04, 0x00, 0x01, 0x00, 0x00,
    // Power constraint
    0x20, 0x01, 0x03,
    // HT capabilities
    0x2d, 0x1a, 0xef, 0x09, 0x1b, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // HT operation
    0x3d, 0x16, 0x9d, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Overlapping BSS scan parameters
    0x4a, 0x0e, 0x14, 0x00, 0x0a, 0x00, 0x2c, 0x01, 0xc8, 0x00, 0x14, 0x00, 0x05, 0x00, 0x19, 0x00,
    // Extended capabilities
    0x7f, 0x08, 0x01, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x40,
    // VHT capabilities
    0xbf, 0x0c, 0xb2, 0x01, 0x80, 0x33, 0xea, 0xff, 0x00, 0x00, 0xea, 0xff, 0x00, 0x00,
    // VHT operation
    0xc0, 0x05, 0x01, 0x9b, 0x00, 0xfc, 0xff,
    // VHT Tx power envelope
    0xc3, 0x04, 0x02, 0xc4, 0xc4, 0xc4,
    // Vendor IE - WMM parameters
    0xdd, 0x18, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00, 0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4,
    0x00, 0x00, 0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00,
    // Vendor IE - Atheros advanced capability
    0xdd, 0x09, 0x00, 0x03, 0x7f, 0x01, 0x01, 0x00, 0x00, 0xff, 0x7f,
    // RSN
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
    // Vendor IE - WPS
    0xdd, 0x1d, 0x00, 0x50, 0xf2, 0x04, 0x10, 0x4a, 0x00, 0x01, 0x10, 0x10, 0x44, 0x00, 0x01, 0x02,
    0x10, 0x3c, 0x00, 0x01, 0x03, 0x10, 0x49, 0x00, 0x06, 0x00, 0x37, 0x2a, 0x00, 0x01, 0x20};

const common::MacAddr kClientMacAddress({0xde, 0xad, 0xbe, 0xef, 0x00, 0x01});
// Sample IPv4 + TCP body
const std::vector<uint8_t> kSampleEthBody = {
    0x00, 0xB0, 0x00, 0x00, 0xE3, 0xDC, 0x78, 0x00, 0x00, 0x40, 0x06, 0xEF, 0x37, 0xC0, 0xA8, 0x01,
    0x03, 0xAC, 0xFD, 0x3F, 0xBC, 0xF2, 0x9C, 0x14, 0x6C, 0x66, 0x6C, 0x0D, 0x31, 0xAF, 0xEC, 0x4E,
    0xD5, 0x80, 0x18, 0x80, 0x00, 0xBB, 0xB4, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0A, 0x82, 0xD7, 0xEC,
    0x54, 0x48, 0x03, 0x6B, 0x32, 0x17, 0x03, 0x03, 0x00, 0xAA, 0x12, 0x2E, 0xDE, 0x85, 0xF7, 0xC4,
    0x6B, 0xEE, 0x10, 0x58, 0xE8, 0xF1, 0x66, 0x16, 0x48, 0xA8, 0x15, 0xA0, 0x1D, 0x5A, 0x5E, 0x20,
    0x13, 0x71, 0xB9, 0x2A, 0x9B, 0x58, 0xE3, 0x66, 0x82, 0xD2, 0xD7, 0x14, 0xF7, 0x29, 0x06, 0x2E,
    0x78, 0x41, 0xB8, 0x21, 0xB2, 0x0B, 0x56, 0x2F, 0xA8, 0xD8, 0xF1, 0x62, 0x2A, 0x60, 0x82, 0xDF,
    0x14, 0x3F, 0x02, 0x3F, 0xD5, 0xD8, 0x55, 0xE2, 0x76, 0xF9, 0x70, 0x8F, 0x5A, 0x4E, 0x53, 0xE0,
    0x15, 0xEE, 0x89, 0x29, 0xDF, 0xB1, 0x1D, 0xCD, 0x47, 0x60, 0x10, 0x1C, 0xC0, 0xB2, 0x64, 0x97,
    0x5E, 0x76, 0x65, 0xCA, 0x2F, 0x3D, 0xE3, 0xCD, 0x75, 0xDB, 0x05, 0x47, 0xC5, 0xF8, 0x08, 0x2F,
    0x0C, 0x7A, 0xC5, 0xF3, 0x6E, 0x17, 0xE7, 0x49, 0x19, 0x96, 0x2F, 0x33, 0x6E, 0x5C, 0x33, 0x0E,
    0x03, 0xA7, 0x5C, 0x5B, 0xB4, 0xDA, 0x67, 0x47, 0xDD, 0xCD, 0xBE, 0xFE, 0xBE, 0x8F, 0xF6, 0xB0,
    0xFE, 0xA2, 0xCB, 0xDB, 0x27, 0x12, 0x4E, 0xD1, 0xD5, 0x1D, 0x5C, 0x19, 0xC8, 0xFC, 0x4F, 0x61,
    0x60, 0x59, 0xA8, 0xEC, 0xC9, 0x9F, 0x63, 0xAE, 0xDF, 0xE2, 0x02, 0xB0, 0x3F, 0x0A, 0x20, 0xA2,
    0xAA, 0x94, 0xCE, 0x74};

// Sample EAPOL-Key packet
const std::vector<uint8_t> kSampleEapol = {
    0x02, 0x03, 0x00, 0x75, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x32, 0x06, 0x7d, 0xbd, 0xe4, 0x95, 0x5f, 0x08, 0x20, 0x3e, 0x60, 0xaf, 0xc5, 0x1f, 0xcf,
    0x25, 0xbf, 0xec, 0xbc, 0x0a, 0x76, 0xbe, 0x08, 0xbf, 0xfc, 0x6b, 0xbd, 0xf7, 0x77, 0xdb, 0x73,
    0xbd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x16, 0xdd, 0x14, 0x00, 0x0f, 0xac, 0x04, 0xf8, 0xac, 0xf0, 0xb5, 0xc5, 0xa3, 0xd1,
    0x2e, 0x83, 0xb6, 0xb5, 0x60, 0x5b, 0x8d, 0x75, 0x68};
class DataFrameTest : public SimTest {
 public:
  // How long an individual test will run for. We need an end time because tests run until no more
  // events remain and so we need to stop aps from beaconing to drain the event queue.
  static constexpr zx::duration kTestDuration = zx::sec(100);

  void Init();
  void Finish();

  // Run through the join => auth => assoc flow
  void StartConnect();

  // Send a eapol request
  void TxEapolRequest(common::MacAddr dstAddr, common::MacAddr srcAddr,
                      const std::vector<uint8_t>& eapol);

  // Send a data frame to the ap
  void ClientTx(common::MacAddr dstAddr, common::MacAddr srcAddr, std::vector<uint8_t>& ethFrame);

 protected:
  struct AssocContext {
    // Information about the BSS we are attempting to associate with. Used to generate the
    // appropriate MLME calls (Join => Auth => Assoc).
    wlan_channel_t channel = kDefaultChannel;
    common::MacAddr bssid = kApBssid;
    cssid_t ssid = kApSsid;
    std::vector<uint8_t> ies = std::vector<uint8_t>(kIes, kIes + sizeof(kIes));

    // There should be one result for each association response received
    std::list<status_code_t> expected_results;

    // Track number of association responses
    size_t connect_resp_count = 0;

    // Track number of deauth indications.
    size_t deauth_ind_count = 0;
  };

  // Context for managing eapol callbacks
  struct EapolContext {
    std::list<std::vector<uint8_t>> sent_data;
    std::list<std::vector<uint8_t>> received_data;
    std::list<wlan_eapol_result_t> tx_eapol_conf_codes;
  };

  // data frames sent by our driver detected by the environment
  std::list<simulation::SimQosDataFrame> env_data_frame_capture_;

  // filter for data frame caputre
  common::MacAddr recv_addr_capture_filter;

  // number of non-eapol data frames received
  size_t non_eapol_data_count;

  // number of eapol frames received
  size_t eapol_ind_count;

  // This is the interface we will use for our single client interface
  SimInterface client_ifc_;

  // The MAC address of our client interface
  common::MacAddr ifc_mac_;

  AssocContext assoc_context_;

  // Keep track of the APs that are in operation so we can easily disable beaconing on all of them
  // at the end of each test.
  std::list<simulation::FakeAp*> aps_;

  EapolContext eapol_context_;

  bool assoc_check_for_eapol_rx_ = false;

  bool testing_rx_freeze_deauth_ = false;

 private:
  // StationIfc overrides
  void Rx(std::shared_ptr<const simulation::SimFrame> frame,
          std::shared_ptr<const simulation::WlanRxInfo> info) override;

  // SME callbacks
  static wlan_fullmac_impl_ifc_protocol_ops_t sme_ops_;
  wlan_fullmac_impl_ifc_protocol sme_protocol_ = {.ops = &sme_ops_, .ctx = this};

  // Fullmac event handlers
  void OnDeauthInd(const wlan_fullmac_deauth_indication_t* ind);
  void OnConnectConf(const wlan_fullmac_connect_confirm_t* resp);
  void OnDisassocInd(const wlan_fullmac_disassoc_indication_t* ind);
  void OnEapolConf(const wlan_fullmac_eapol_confirm_t* resp);
  void OnSignalReport(const wlan_fullmac_signal_report_indication* ind);
  void OnEapolInd(const wlan_fullmac_eapol_indication_t* ind);
};

// Since we're acting as wlan_fullmac, we need handlers for any protocol calls we may receive
wlan_fullmac_impl_ifc_protocol_ops_t DataFrameTest::sme_ops_ = {
    .on_scan_result =
        [](void* cookie, const wlan_fullmac_scan_result_t* result) {
          // Ignore
        },
    .on_scan_end =
        [](void* cookie, const wlan_fullmac_scan_end_t* end) {
          // Ignore
        },
    .connect_conf =
        [](void* cookie, const wlan_fullmac_connect_confirm_t* resp) {
          static_cast<DataFrameTest*>(cookie)->OnConnectConf(resp);
        },
    .deauth_ind =
        [](void* cookie, const wlan_fullmac_deauth_indication_t* ind) {
          static_cast<DataFrameTest*>(cookie)->OnDeauthInd(ind);
        },
    .disassoc_conf =
        [](void* cookie, const wlan_fullmac_disassoc_confirm_t* resp) {
          // Ignore
        },
    .disassoc_ind =
        [](void* cookie, const wlan_fullmac_disassoc_indication_t* ind) {
          static_cast<DataFrameTest*>(cookie)->OnDisassocInd(ind);
        },
    .eapol_conf =
        [](void* cookie, const wlan_fullmac_eapol_confirm_t* resp) {
          static_cast<DataFrameTest*>(cookie)->OnEapolConf(resp);
        },
    .signal_report =
        [](void* cookie, const wlan_fullmac_signal_report_indication* ind) {
          static_cast<DataFrameTest*>(cookie)->OnSignalReport(ind);
        },
    .eapol_ind =
        [](void* cookie, const wlan_fullmac_eapol_indication_t* ind) {
          static_cast<DataFrameTest*>(cookie)->OnEapolInd(ind);
        },
};

// Create our device instance and hook up the callbacks
void DataFrameTest::Init() {
  // Basic initialization
  ASSERT_EQ(SimTest::Init(), ZX_OK);
  assoc_context_.connect_resp_count = 0;
  non_eapol_data_count = 0;
  eapol_ind_count = 0;

  // Bring up the interface
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, &sme_protocol_), ZX_OK);

  // Figure out the interface's mac address
  client_ifc_.GetMacAddr(&ifc_mac_);

  // Schedule a time to terminate execution. Simulation runs until no more events are scheduled,
  // and since we have a beaconing fake AP, that means forever if we don't stop it.
  env_->ScheduleNotification(std::bind(&DataFrameTest::Finish, this), kTestDuration);
}

void DataFrameTest::Finish() {
  for (auto ap : aps_) {
    ap->DisableBeacon();
  }
  aps_.clear();
}

void DataFrameTest::OnDeauthInd(const wlan_fullmac_deauth_indication_t* ind) {
  if (!testing_rx_freeze_deauth_) {
    // This function is only used for rx freeze deauth testing now.
    return;
  }
  assoc_context_.deauth_ind_count++;
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  // Do a re-association right after deauth.
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(200));
}

void DataFrameTest::OnConnectConf(const wlan_fullmac_connect_confirm_t* resp) {
  assoc_context_.connect_resp_count++;
  EXPECT_EQ(resp->result_code, assoc_context_.expected_results.front());
  assoc_context_.expected_results.pop_front();
}

void DataFrameTest::OnEapolConf(const wlan_fullmac_eapol_confirm_t* resp) {
  eapol_context_.tx_eapol_conf_codes.push_back(resp->result_code);
}

void DataFrameTest::OnEapolInd(const wlan_fullmac_eapol_indication_t* ind) {
  std::vector<uint8_t> resp;
  resp.resize(ind->data_count);
  std::memcpy(resp.data(), ind->data_list, ind->data_count);

  eapol_context_.received_data.push_back(std::move(resp));

  if (assoc_check_for_eapol_rx_) {
    ASSERT_EQ(assoc_context_.connect_resp_count, 1U);
  }
  eapol_ind_count++;
}

void DataFrameTest::OnSignalReport(const wlan_fullmac_signal_report_indication* ind) {
  if (!testing_rx_freeze_deauth_) {
    // This function is only used for rx freeze deauth testing now.
    return;
  }
  // Transmit a frame to AP right after each signal report to increase tx count and hold rx count.
  constexpr uint16_t kFrameId = 123;
  auto transmit = [&](void) {
    device_->DataPath().TxEthernet(kFrameId, kClientMacAddress, ifc_mac_, ETH_P_IP, kSampleEthBody);
  };
  env_->ScheduleNotification(transmit, zx::msec(200));
}

void DataFrameTest::OnDisassocInd(const wlan_fullmac_disassoc_indication_t* ind) {}

void DataFrameTest::StartConnect() {
  // Send connect request
  wlan_fullmac_connect_req connect_req = {};
  std::memcpy(connect_req.selected_bss.bssid, assoc_context_.bssid.byte, ETH_ALEN);
  connect_req.selected_bss.ies_list = assoc_context_.ies.data();
  connect_req.selected_bss.ies_count = assoc_context_.ies.size();
  connect_req.selected_bss.channel = assoc_context_.channel;
  connect_req.auth_type = WLAN_AUTH_TYPE_OPEN_SYSTEM;
  connect_req.connect_failure_timeout = 1000;  // ~1s (although value is ignored for now)
  client_ifc_.if_impl_ops_->connect_req(client_ifc_.if_impl_ctx_, &connect_req);
}

void DataFrameTest::TxEapolRequest(common::MacAddr dstAddr, common::MacAddr srcAddr,
                                   const std::vector<uint8_t>& eapol) {
  wlan_fullmac_eapol_req eapol_req = {};
  memcpy(eapol_req.dst_addr, dstAddr.byte, ETH_ALEN);
  memcpy(eapol_req.src_addr, srcAddr.byte, ETH_ALEN);
  eapol_req.data_list = eapol.data();
  eapol_req.data_count = eapol.size();
  client_ifc_.if_impl_ops_->eapol_req(client_ifc_.if_impl_ctx_, &eapol_req);
}

void DataFrameTest::ClientTx(common::MacAddr dstAddr, common::MacAddr srcAddr,
                             std::vector<uint8_t>& ethFrame) {
  simulation::SimQosDataFrame dataFrame(true, false, kApBssid, srcAddr, dstAddr, 0, ethFrame);
  env_->Tx(dataFrame, kDefaultTxInfo, this);
}

void DataFrameTest::Rx(std::shared_ptr<const simulation::SimFrame> frame,
                       std::shared_ptr<const simulation::WlanRxInfo> info) {
  switch (frame->FrameType()) {
    case simulation::SimFrame::FRAME_TYPE_DATA: {
      auto data_frame = std::static_pointer_cast<const simulation::SimDataFrame>(frame);
      if (data_frame->DataFrameType() == simulation::SimDataFrame::FRAME_TYPE_QOS_DATA) {
        auto qos_data_frame =
            std::static_pointer_cast<const simulation::SimQosDataFrame>(data_frame);
        if (data_frame->addr1_ == recv_addr_capture_filter) {
          env_data_frame_capture_.emplace_back(qos_data_frame->toDS_, qos_data_frame->fromDS_,
                                               qos_data_frame->addr1_, qos_data_frame->addr2_,
                                               qos_data_frame->addr3_, qos_data_frame->qosControl_,
                                               qos_data_frame->payload_);
        }
      }
      break;
    }
    default:
      break;
  }
}

// Verify that we can tx frames into the simulated environment through the driver
TEST_F(DataFrameTest, TxDataFrame) {
  // Create our device instance
  Init();

  // Start up our fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  ap.EnableBeacon(zx::msec(100));
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(10));

  constexpr uint16_t kFrameId = 123;
  auto transmit = [&](void) {
    device_->DataPath().TxEthernet(kFrameId, kClientMacAddress, ifc_mac_, ETH_P_IP, kSampleEthBody);
  };

  env_->ScheduleNotification(transmit, zx::sec(1));

  recv_addr_capture_filter = ap.GetBssid();

  env_->Run(kSimulatedClockDuration);

  // Verify frame was sent successfully
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);

  auto& tx_results = device_->DataPath().TxResults();
  ASSERT_EQ(tx_results.size(), 1);
  EXPECT_EQ(tx_results[0].id, kFrameId);
  EXPECT_EQ(tx_results[0].status, ZX_OK);

  EXPECT_EQ(env_data_frame_capture_.size(), 1U);
  EXPECT_EQ(env_data_frame_capture_.front().toDS_, true);
  EXPECT_EQ(env_data_frame_capture_.front().fromDS_, false);
  EXPECT_EQ(env_data_frame_capture_.front().addr2_, ifc_mac_);
  EXPECT_EQ(env_data_frame_capture_.front().addr3_, kClientMacAddress);
  EXPECT_EQ(env_data_frame_capture_.front().payload_, kSampleEthBody);
  EXPECT_TRUE(env_data_frame_capture_.front().qosControl_.has_value());
  EXPECT_EQ(env_data_frame_capture_.front().qosControl_.value(), 6);
}

// Verify that malformed ethernet header frames are detected by the driver
TEST_F(DataFrameTest, TxMalformedDataFrame) {
  // Create our device instance
  Init();

  // Start up our fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  ap.EnableBeacon(zx::msec(100));
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(10));

  // Simulate sending a illegal ethernet frame from us to the AP
  std::vector<uint8_t> illegal = {0x20, 0x43};
  constexpr uint16_t kFrameId = 123;
  auto transmit = [&]() { device_->DataPath().TxRaw(kFrameId, illegal); };
  env_->ScheduleNotification(transmit, zx::sec(1));

  env_->Run(kSimulatedClockDuration);

  // Verify frame was rejected
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);

  auto& tx_results = device_->DataPath().TxResults();
  ASSERT_EQ(tx_results.size(), 1);
  EXPECT_EQ(tx_results[0].id, kFrameId);
  EXPECT_EQ(tx_results[0].status, ZX_ERR_INVALID_ARGS);
}

TEST_F(DataFrameTest, TxEapolFrame) {
  // Create our device instance
  Init();

  // Start up our fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  ap.EnableBeacon(zx::msec(100));
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(10));

  // Simulate sending a EAPOL packet from us to the AP
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::TxEapolRequest, this, kClientMacAddress, ifc_mac_, kSampleEapol),
      zx::sec(1));
  recv_addr_capture_filter = ap.GetBssid();

  env_->Run(kSimulatedClockDuration);

  // Verify response
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  EXPECT_EQ(eapol_context_.tx_eapol_conf_codes.front(), WLAN_EAPOL_RESULT_SUCCESS);

  auto& tx_results = device_->DataPath().TxResults();
  ASSERT_EQ(tx_results.size(), 0);

  ASSERT_EQ(env_data_frame_capture_.size(), 1U);
  EXPECT_EQ(env_data_frame_capture_.front().toDS_, true);
  EXPECT_EQ(env_data_frame_capture_.front().fromDS_, false);
  EXPECT_EQ(env_data_frame_capture_.front().addr2_, ifc_mac_);
  EXPECT_EQ(env_data_frame_capture_.front().addr3_, kClientMacAddress);
  EXPECT_EQ(env_data_frame_capture_.front().payload_, kSampleEapol);
}

// Test driver can receive data frames
TEST_F(DataFrameTest, RxDataFrame) {
  // Create our device instance
  Init();

  zx::duration delay = zx::msec(1);
  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), delay);

  // Want to send packet from test to driver
  std::vector<uint8_t> expected =
      sim_utils::CreateEthernetFrame(ifc_mac_, kClientMacAddress, ETH_P_IP, kSampleEthBody);

  // Ensure the data packet is sent after the client has associated
  delay += kSsidEventDelay + zx::msec(100);
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::ClientTx, this, ifc_mac_, kClientMacAddress, expected), delay);

  // Run
  env_->Run(kSimulatedClockDuration);

  // Confirm that the driver received that packet
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  EXPECT_EQ(eapol_ind_count, 0U);
  EXPECT_EQ(eapol_context_.received_data.size(), 0);

  ASSERT_EQ(device_->DataPath().RxData().size(), 1);
  auto& actual = device_->DataPath().RxData().front();

  ASSERT_EQ(actual.size(), expected.size());
  ASSERT_EQ(actual, expected);
}

// Test driver can receive data frames
TEST_F(DataFrameTest, RxMalformedDataFrame) {
  // Create our device instance
  Init();

  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(30));

  // ethernet frame too small to hold ethernet header
  std::vector<uint8_t> ethFrame = {0x00, 0x45};

  // Want to send packet from test to driver
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::ClientTx, this, ifc_mac_, kClientMacAddress, ethFrame),
      zx::sec(10));

  // Run
  env_->Run(kSimulatedClockDuration);

  // Confirm that the driver received that packet
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  EXPECT_EQ(non_eapol_data_count, 0U);
  ASSERT_EQ(device_->DataPath().RxData().size(), 0U);
}

TEST_F(DataFrameTest, RxEapolFrame) {
  // Create our device instance
  Init();

  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), zx::msec(30));

  // Want to send packet from test to driver
  std::vector<uint8_t> eth =
      sim_utils::CreateEthernetFrame(ifc_mac_, kClientMacAddress, ETH_P_PAE, kSampleEapol);
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::ClientTx, this, ifc_mac_, kClientMacAddress, eth), zx::sec(5));

  // Run
  env_->Run(kSimulatedClockDuration);

  // Confirm that the driver received that packet
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  EXPECT_EQ(eapol_ind_count, 1U);

  EXPECT_EQ(eapol_context_.received_data.size(), 1);

  // The driver strips the ethernet header from the sent frame
  EXPECT_EQ(eapol_context_.received_data.front().size(), kSampleEapol.size());
  EXPECT_EQ(eapol_context_.received_data.front(), kSampleEapol);
  ASSERT_EQ(device_->DataPath().RxData().size(), 0U);
}

TEST_F(DataFrameTest, RxEapolFrameAfterAssoc) {
  // Create our device instance
  Init();

  zx::duration delay = zx::msec(1);

  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), delay);

  // Want to send packet from test to driver
  std::vector<uint8_t> eth =
      sim_utils::CreateEthernetFrame(ifc_mac_, kClientMacAddress, ETH_P_PAE, kSampleEapol);

  // Send the packet before the SSID event is sent from SIM FW
  delay = delay + kSsidEventDelay / 2;
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::ClientTx, this, ifc_mac_, kClientMacAddress, eth), delay);
  assoc_check_for_eapol_rx_ = true;
  // Run
  env_->Run(kSimulatedClockDuration);

  // Confirm that the driver received that packet
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  EXPECT_EQ(eapol_ind_count, 1U);
  ASSERT_EQ(device_->DataPath().RxData().size(), 0U);
}

// Send a ucast packet to client before association is complete. Resulting E_DEAUTH from SIM FW
// should be ignored by the driver and association should complete.
TEST_F(DataFrameTest, RxUcastBeforeAssoc) {
  // Create our device instance
  Init();

  zx::duration delay = zx::msec(1);

  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  // Assoc driver with fake AP
  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), delay);

  std::vector<uint8_t> expected =
      sim_utils::CreateEthernetFrame(ifc_mac_, kClientMacAddress, ETH_P_PAE, kSampleEthBody);

  // Send the packet before the Assoc event is sent by SIM FW.
  delay = delay + kAssocEventDelay / 2;
  env_->ScheduleNotification(
      std::bind(&DataFrameTest::ClientTx, this, ifc_mac_, kClientMacAddress, expected), delay);
  // Run
  env_->Run(kSimulatedClockDuration);

  // Confirm that the driver did not receive the packet
  EXPECT_EQ(device_->DataPath().RxData().size(), 0U);
  EXPECT_EQ(assoc_context_.connect_resp_count, 1U);
  ASSERT_EQ(device_->DataPath().RxData().size(), 0U);
}

TEST_F(DataFrameTest, DeauthWhenRxFreeze) {
  testing_rx_freeze_deauth_ = true;

  constexpr zx::duration kFirstAssocDelay = zx::msec(1);
  constexpr zx::duration kRxFreezeTestDuration = zx::hour(1);

  // Create our device instance
  Init();

  // Start a fake AP
  simulation::FakeAp ap(env_.get(), kApBssid, kApSsid, kDefaultChannel);
  aps_.push_back(&ap);

  assoc_context_.expected_results.push_front(STATUS_CODE_SUCCESS);
  env_->ScheduleNotification(std::bind(&DataFrameTest::StartConnect, this), kFirstAssocDelay);

  env_->Run(kRxFreezeTestDuration);

  // One deauth should be triggered and a deauth_ind was sent to SME, and there should be only two
  // deauths triggered in the one hour test.
  EXPECT_EQ(assoc_context_.deauth_ind_count, (size_t)BRCMF_RX_FREEZE_MAX_DEAUTHS_PER_HOUR);
  // The device got reconnected after deauth.
  EXPECT_EQ(ap.GetNumAssociatedClient(), 1U);

  // Run the test for another one hour,  verify that additional deauths can be triggered.
  env_->Run(kRxFreezeTestDuration);

  EXPECT_EQ(assoc_context_.deauth_ind_count, (size_t)(2 * BRCMF_RX_FREEZE_MAX_DEAUTHS_PER_HOUR));
  // The device got reconnected after deauth.
  EXPECT_EQ(ap.GetNumAssociatedClient(), 1U);
}

}  // namespace wlan::brcmfmac
