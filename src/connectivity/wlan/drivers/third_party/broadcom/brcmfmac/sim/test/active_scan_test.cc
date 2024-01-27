// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <zircon/errors.h>

#include <functional>
#include <list>
#include <memory>
#include <optional>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/cfg80211.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/macaddr.h"

namespace wlan::brcmfmac {
namespace {

constexpr zx::duration kScanStartTime = zx::sec(1);
constexpr zx::duration kSimulatedClockDuration = zx::sec(10);

}  // namespace

struct ApInfo {
  explicit ApInfo(simulation::Environment* env, const common::MacAddr& bssid, const cssid_t& ssid,
                  const wlan_channel_t& channel)
      : ap_(env, bssid, ssid, channel) {}

  simulation::FakeAp ap_;
  bool probe_resp_seen_ = false;
};

struct ClientIfc : public SimInterface {
  void OnScanResult(const wlan_fullmac_scan_result_t* result) override;
  void OnScanEnd(const wlan_fullmac_scan_end_t* end) override;

  // Txn ID for the current scan
  uint64_t scan_txn_id_ = 0;

  // Result of the previous scan
  wlan_scan_result_t scan_result_code_ = ZX_OK;

  std::list<wlan_fullmac_scan_result> scan_results_;
  // BSS's IEs are raw pointers. Store the IEs here so we don't have dangling pointers
  std::vector<std::vector<uint8_t>> seen_ies_;
};

class ActiveScanTest : public SimTest {
 public:
  static constexpr zx::duration kBeaconInterval = zx::msec(100);
  static constexpr uint32_t kDwellTimeMs = 120;

  ActiveScanTest() = default;
  void Init();
  void StartFakeAp(const common::MacAddr& bssid, const cssid_t& ssid, const wlan_channel_t& channel,
                   zx::duration beacon_interval = kBeaconInterval);

  void StartScan(const wlan_fullmac_scan_req_t* req);
  // TODO(fxbug.dev/https://fxbug.dev/83861): Align the way active_scan_test and passive_scan_test
  // verify scan results.
  void VerifyScanResults();
  void EndSimulation();

  bool all_aps_seen_ = false;

  void GetFirmwarePfnMac();

  uint32_t GetNumProbeReqsSeen() { return num_probe_reqs_seen; }

 protected:
  // This is the interface we will use for our single client interface
  ClientIfc client_ifc_;

  // The default active scan request
  const uint8_t default_channels_list_[5] = {1, 2, 3, 4, 5};
  wlan_fullmac_scan_req_t default_scan_req_ = {
      .scan_type = WLAN_SCAN_TYPE_ACTIVE,
      .channels_list = default_channels_list_,
      .channels_count = 5,
      .ssids_list = nullptr,
      .ssids_count = 0,
      .min_channel_time = kDwellTimeMs,
      .max_channel_time = kDwellTimeMs,
  };

 private:
  // StationIfc methods
  void Rx(std::shared_ptr<const simulation::SimFrame> frame,
          std::shared_ptr<const simulation::WlanRxInfo> info) override;

  // All simulated APs
  std::list<std::unique_ptr<ApInfo>> aps_;

  // Mac address of sim_fw
  common::MacAddr sim_fw_mac_;
  common::MacAddr last_pfn_mac_ = common::kZeroMac;
  std::optional<common::MacAddr> sim_fw_pfn_mac_;
  uint32_t num_probe_reqs_seen = 0;
};

void ClientIfc::OnScanResult(const wlan_fullmac_scan_result_t* result) {
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(scan_txn_id_, result->txn_id);

  wlan_fullmac_scan_result_t copy = *result;
  // Copy the IES data over since the original location may change data by the time we verify.
  std::vector<uint8_t> ies(copy.bss.ies_list, copy.bss.ies_list + copy.bss.ies_count);
  seen_ies_.push_back(ies);
  copy.bss.ies_list = seen_ies_.at(seen_ies_.size() - 1).data();
  scan_results_.emplace_back(copy);
}

void ClientIfc::OnScanEnd(const wlan_fullmac_scan_end_t* end) { scan_result_code_ = end->code; }

void ActiveScanTest::Init() {
  ASSERT_EQ(SimTest::Init(), ZX_OK);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  // Get the interface MAC address
  client_ifc_.GetMacAddr(&sim_fw_mac_);
}

void ActiveScanTest::StartFakeAp(const common::MacAddr& bssid, const cssid_t& ssid,
                                 const wlan_channel_t& channel, zx::duration beacon_interval) {
  auto ap_info = std::make_unique<ApInfo>(env_.get(), bssid, ssid, channel);
  // Beacon is also enabled here to make sure this is not disturbing the correct result.
  ap_info->ap_.EnableBeacon(beacon_interval);
  aps_.push_back(std::move(ap_info));
}

// Tell the DUT to run a scan
void ActiveScanTest::StartScan(const wlan_fullmac_scan_req_t* req) {
  client_ifc_.if_impl_ops_->start_scan(client_ifc_.if_impl_ctx_, req);
}

// Called when simulation time has run out. Takes down all fake APs and the simulated DUT.
void ActiveScanTest::EndSimulation() {
  for (auto& ap_info : aps_) {
    ap_info->ap_.DisableBeacon();
  }
}

void ActiveScanTest::GetFirmwarePfnMac() {
  brcmf_simdev* sim = device_->GetSim();
  if (!sim_fw_pfn_mac_) {
    struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);
    zx_status_t status =
        brcmf_fil_iovar_data_get(ifp, "pfn_macaddr", sim_fw_pfn_mac_->byte, ETH_ALEN, nullptr);
    EXPECT_EQ(status, ZX_OK);
  }
}

void ActiveScanTest::VerifyScanResults() {
  // TODO(fxbug.dev/83883): Verify the timestamp of each scan result.
  for (auto result : client_ifc_.scan_results_) {
    int matches_seen = 0;

    for (auto& ap_info : aps_) {
      common::MacAddr mac_addr = ap_info->ap_.GetBssid();
      ASSERT_EQ(sizeof(result.bss.bssid), sizeof(mac_addr.byte));
      if (!std::memcmp(result.bss.bssid, mac_addr.byte, sizeof(mac_addr.byte))) {
        ap_info->probe_resp_seen_ = true;
        matches_seen++;

        // Verify SSID
        cssid_t ssid_info = ap_info->ap_.GetSsid();
        auto ssid = brcmf_find_ssid_in_ies(result.bss.ies_list, result.bss.ies_count);
        EXPECT_EQ(ssid.size(), ssid_info.len);
        ASSERT_LE(ssid_info.len, sizeof(ssid_info.data));
        EXPECT_EQ(memcmp(ssid.data(), ssid_info.data, ssid_info.len), 0);

        // Verify channel
        wlan_channel_t channel = ap_info->ap_.GetChannel();
        EXPECT_EQ(result.bss.channel.primary, channel.primary);
        EXPECT_EQ(result.bss.channel.cbw, channel.cbw);
        EXPECT_EQ(result.bss.channel.secondary80, channel.secondary80);

        // Verify has RSSI value
        ASSERT_LT(result.bss.rssi_dbm, 0);
      }
    }

    // There should be exactly one AP per result.
    EXPECT_EQ(matches_seen, 1);
  }

  for (auto& ap_info : aps_) {
    if (ap_info->probe_resp_seen_ == false) {
      // Failure
      return;
    }
  }

  // pfn mac should be different from the one from last scan.
  EXPECT_NE(last_pfn_mac_, *sim_fw_pfn_mac_);
  last_pfn_mac_ = *sim_fw_pfn_mac_;

  // pfn mac will be set back to firmware mac after active scan.
  GetFirmwarePfnMac();
  EXPECT_EQ(sim_fw_mac_, *sim_fw_pfn_mac_);

  sim_fw_pfn_mac_.reset();

  // The probe response from all APs were seen
  all_aps_seen_ = true;
}

void ActiveScanTest::Rx(std::shared_ptr<const simulation::SimFrame> frame,
                        std::shared_ptr<const simulation::WlanRxInfo> info) {
  GetFirmwarePfnMac();

  ASSERT_EQ(frame->FrameType(), simulation::SimFrame::FRAME_TYPE_MGMT);

  auto mgmt_frame = std::static_pointer_cast<const simulation::SimManagementFrame>(frame);

  if (mgmt_frame->MgmtFrameType() == simulation::SimManagementFrame::FRAME_TYPE_PROBE_REQ) {
    // When a probe request is sent out, the src mac address should not be real mac address.
    auto probe_req = std::static_pointer_cast<const simulation::SimProbeReqFrame>(mgmt_frame);
    EXPECT_NE(probe_req->src_addr_, sim_fw_mac_);
    EXPECT_EQ(probe_req->src_addr_, *sim_fw_pfn_mac_);
    num_probe_reqs_seen++;
  }

  if (mgmt_frame->MgmtFrameType() == simulation::SimManagementFrame::FRAME_TYPE_PROBE_RESP) {
    auto probe_resp = std::static_pointer_cast<const simulation::SimProbeRespFrame>(mgmt_frame);
    EXPECT_NE(probe_resp->dst_addr_, sim_fw_mac_);
    EXPECT_EQ(probe_resp->dst_addr_, *sim_fw_pfn_mac_);
  }
}

// AP 1&2 on channel 2.
constexpr wlan_channel_t kDefaultChannel1 = {
    .primary = 2, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
constexpr cssid_t kAp1Ssid = {.len = 16, .data = "Fuchsia Fake AP1"};
const common::MacAddr kAp1Bssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
constexpr cssid_t kAp2Ssid = {.len = 16, .data = "Fuchsia Fake AP2"};
const common::MacAddr kAp2Bssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbd});

// AP 3 on channel 4.
constexpr wlan_channel_t kDefaultChannel2 = {
    .primary = 4, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
constexpr cssid_t kAp3Ssid = {.len = 16, .data = "Fuchsia Fake AP3"};
const common::MacAddr kAp3Bssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbe});

// This test case might fail in a very low possibility because it's random.
TEST_F(ActiveScanTest, RandomMacThreeAps) {
  // Create simulated device
  Init();

  // Start the first AP
  StartFakeAp(kAp1Bssid, kAp1Ssid, kDefaultChannel1);
  StartFakeAp(kAp2Bssid, kAp2Ssid, kDefaultChannel1);
  StartFakeAp(kAp3Bssid, kAp3Ssid, kDefaultChannel2);

  default_scan_req_.txn_id = ++client_ifc_.scan_txn_id_;

  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &default_scan_req_),
                             kScanStartTime);

  // Schedule scan end in environment
  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  VerifyScanResults();
  EXPECT_EQ(all_aps_seen_, true);
  EXPECT_EQ(client_ifc_.scan_result_code_, ZX_OK);
}

TEST_F(ActiveScanTest, ScanTwice) {
  Init();
  default_scan_req_.txn_id = ++client_ifc_.scan_txn_id_;

  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &default_scan_req_),
                             kScanStartTime);

  env_->Run(kSimulatedClockDuration);

  VerifyScanResults();

  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &default_scan_req_),
                             kScanStartTime);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  VerifyScanResults();
  EXPECT_EQ(client_ifc_.scan_result_code_, ZX_OK);
}

// Ensure that the FW sends out the max # probe requests set by the
// driver (as there are no APs in the environment).
TEST_F(ActiveScanTest, CheckNumProbeReqsSent) {
  Init();
  default_scan_req_.txn_id = ++client_ifc_.scan_txn_id_;
  default_scan_req_.channels_count = 1;

  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &default_scan_req_),
                             kScanStartTime);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  EXPECT_EQ(GetNumProbeReqsSeen(), (uint32_t)BRCMF_ACTIVE_SCAN_NUM_PROBES);
  EXPECT_EQ(client_ifc_.scan_result_code_, ZX_OK);
}

// This test is to verify brcmfmac driver will return an error when an empty list
// of channels is indicated in active scan test request.
TEST_F(ActiveScanTest, EmptyChannelList) {
  constexpr zx::duration kScanStartTime = zx::sec(1);

  Init();

  StartFakeAp(kAp1Bssid, kAp1Ssid, kDefaultChannel1);

  // Case contains empty channels_list.
  wlan_fullmac_scan_req_t req_empty_channels_list = {
      .txn_id = ++client_ifc_.scan_txn_id_,
      .scan_type = WLAN_SCAN_TYPE_ACTIVE,
      .channels_list = nullptr,
      .channels_count = 0,
      .ssids_list = nullptr,
      .ssids_count = 0,
      .min_channel_time = kDwellTimeMs,
      .max_channel_time = kDwellTimeMs,
  };

  // Two active scans are scheduled,
  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &req_empty_channels_list),
                             kScanStartTime);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  VerifyScanResults();
  EXPECT_EQ(client_ifc_.scan_result_code_, WLAN_SCAN_RESULT_INVALID_ARGS);
}

// This test is to verify brcmfmac driver will return an error when an invalid ssid list
// is indicated in active scan test request.
TEST_F(ActiveScanTest, SsidTooLong) {
  constexpr zx::duration kScanStartTime = zx::sec(1);

  Init();

  StartFakeAp(kAp1Bssid, kAp1Ssid, kDefaultChannel1);

  cssid_t invalid_scan_ssid = {
      .len = 33,
      .data = "1234567890",
  };

  cssid_t valid_scan_ssid = {
      .len = 16,
      .data = "Fuchsia Fake AP1",
  };

  // Case contains over-size ssid in ssids_list in request.
  const uint8_t channels_list[] = {1, 2, 3, 4, 5};
  const cssid_t ssids_list[] = {valid_scan_ssid, invalid_scan_ssid};
  wlan_fullmac_scan_req_t req_break_ssids_list = {
      .txn_id = ++client_ifc_.scan_txn_id_,
      .scan_type = WLAN_SCAN_TYPE_ACTIVE,
      .channels_list = channels_list,
      .channels_count = 5,
      .ssids_list = ssids_list,
      .ssids_count = 2,
      .min_channel_time = kDwellTimeMs,
      .max_channel_time = kDwellTimeMs,
  };

  // Two active scans are scheduled,
  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &req_break_ssids_list),
                             kScanStartTime);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  VerifyScanResults();
  EXPECT_EQ(client_ifc_.scan_result_code_, WLAN_SCAN_RESULT_INVALID_ARGS);
}

// This test case verifies that the driver returns SHOULD_WAIT as the scan result code when firmware
// is busy.
TEST_F(ActiveScanTest, ScanWhenFirmwareBusy) {
  Init();

  // Start the first AP
  StartFakeAp(kAp1Bssid, kAp1Ssid, kDefaultChannel1);

  // Set up our injector
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.AddErrInjIovar("escan", ZX_OK, BCME_BUSY);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::StartScan, this, &default_scan_req_),
                             kScanStartTime);

  env_->ScheduleNotification(std::bind(&ActiveScanTest::EndSimulation, this),
                             kSimulatedClockDuration);
  env_->Run(kSimulatedClockDuration);

  // Verify that there is no scan result and the scan result code is WLAN_SCAN_RESULT_SHOULD_WAIT.
  EXPECT_EQ(client_ifc_.scan_results_.size(), 0U);
  EXPECT_EQ(client_ifc_.scan_result_code_, WLAN_SCAN_RESULT_SHOULD_WAIT);
}

}  // namespace wlan::brcmfmac
