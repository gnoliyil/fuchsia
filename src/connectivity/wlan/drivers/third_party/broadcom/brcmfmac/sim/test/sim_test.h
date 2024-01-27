// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <zircon/types.h>

#include <map>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_device.h"
#include "zircon/system/ulib/sync/include/lib/sync/cpp/completion.h"

namespace wlan::brcmfmac {

// This class represents an interface created on a simulated device, collecting all of the
// attributes related to that interface.
class SimInterface {
 public:
  // Track state of association
  struct AssocContext {
    enum AssocState {
      kNone,
      kAssociating,
      kAssociated,
    } state = kNone;

    common::MacAddr bssid;
    std::vector<uint8_t> ies;
    wlan_channel_t channel;
  };

  struct SoftApContext {
    cssid_t ssid;
  };

  // Useful statistics about operations performed
  struct Stats {
    size_t connect_attempts = 0;
    size_t connect_successes = 0;
    std::list<wlan_fullmac_connect_confirm_t> connect_results;
    std::list<wlan_fullmac_assoc_ind_t> assoc_indications;
    std::list<wlan_fullmac_auth_ind_t> auth_indications;
    std::list<wlan_fullmac_deauth_confirm_t> deauth_results;
    std::list<wlan_fullmac_deauth_indication_t> deauth_indications;
    std::list<wlan_fullmac_disassoc_indication_t> disassoc_indications;
    std::list<wlan_fullmac_channel_switch_info_t> csa_indications;
    std::list<wlan_fullmac_start_confirm_t> start_confirmations;
    std::list<wlan_fullmac_stop_confirm_t> stop_confirmations;
  };

  // Default scan options
  static const std::vector<uint8_t> kDefaultScanChannels;
  static constexpr uint32_t kDefaultActiveScanDwellTimeMs = 40;
  static constexpr uint32_t kDefaultPassiveScanDwellTimeMs = 120;

  // SoftAP defaults
  static constexpr cssid_t kDefaultSoftApSsid = {.len = 10, .data = "Sim_SoftAP"};
  static constexpr wlan_channel_t kDefaultSoftApChannel = {
      .primary = 11, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
  static constexpr uint32_t kDefaultSoftApBeaconPeriod = 100;
  static constexpr uint32_t kDefaultSoftApDtimPeriod = 100;

  SimInterface() = default;
  SimInterface(const SimInterface&) = delete;

  zx_status_t Init(std::shared_ptr<simulation::Environment> env, wlan_mac_role_t role);

  ~SimInterface() {
    if (if_impl_ops_)
      if_impl_ops_->stop(if_impl_ctx_);
    if (ch_sme_ != ZX_HANDLE_INVALID) {
      zx_handle_close(ch_sme_);
    }
    if (ch_mlme_ != ZX_HANDLE_INVALID) {
      zx_handle_close(ch_mlme_);
    }
  }

  // Default SME Callbacks
  virtual void OnScanResult(const wlan_fullmac_scan_result_t* result);
  virtual void OnScanEnd(const wlan_fullmac_scan_end_t* end);
  virtual void OnAuthInd(const wlan_fullmac_auth_ind_t* resp);
  virtual void OnDeauthConf(const wlan_fullmac_deauth_confirm_t* resp);
  virtual void OnDeauthInd(const wlan_fullmac_deauth_indication_t* ind);
  virtual void OnConnectConf(const wlan_fullmac_connect_confirm_t* resp);
  virtual void OnRoamConf(const wlan_fullmac_roam_confirm_t* resp);
  virtual void OnAssocInd(const wlan_fullmac_assoc_ind_t* ind);
  virtual void OnDisassocConf(const wlan_fullmac_disassoc_confirm_t* resp) {}
  virtual void OnDisassocInd(const wlan_fullmac_disassoc_indication_t* ind);
  virtual void OnStartConf(const wlan_fullmac_start_confirm_t* resp);
  virtual void OnStopConf(const wlan_fullmac_stop_confirm_t* resp);
  virtual void OnEapolConf(const wlan_fullmac_eapol_confirm_t* resp) {}
  virtual void OnChannelSwitch(const wlan_fullmac_channel_switch_info_t* ind);
  virtual void OnSignalReport(const wlan_fullmac_signal_report_indication_t* ind) {}
  virtual void OnEapolInd(const wlan_fullmac_eapol_indication_t* ind) {}
  virtual void OnWmmStatusResp(const zx_status_t status, const wlan_wmm_parameters_t* resp) {}
  virtual void OnRelayCapturedFrame(const wlan_fullmac_captured_frame_result_t* result) {}
  virtual void OnDataRecv(const void* data, size_t data_size, uint32_t flags) {}

  // Query an interface
  void Query(wlan_fullmac_query_info_t* out_info);

  // Query for MAC sublayer feature support on an interface
  void QueryMacSublayerSupport(mac_sublayer_support_t* out_resp);

  // Query for security feature support on an interface
  void QuerySecuritySupport(security_support_t* out_resp);

  // Query for spectrum management support on an interface
  void QuerySpectrumManagementSupport(spectrum_management_support_t* out_resp);

  // Stop an interface
  void StopInterface();

  // Get the Mac address of an interface
  void GetMacAddr(common::MacAddr* out_macaddr);

  // Start an assocation with a fake AP. We can use these for subsequent association events, but
  // not interleaved association events (which I doubt are terribly useful, anyway). Note that for
  // the moment only non-authenticated associations are supported.
  void StartConnect(const common::MacAddr& bssid, const cssid_t& ssid,
                    const wlan_channel_t& channel);
  void AssociateWith(const simulation::FakeAp& ap,
                     std::optional<zx::duration> delay = std::nullopt);

  void DeauthenticateFrom(const common::MacAddr& bssid, reason_code_t reason);

  // Scan operations
  void StartScan(uint64_t txn_id = 0, bool active = false,
                 std::optional<const std::vector<uint8_t>> channels =
                     std::optional<const std::vector<uint8_t>>{});
  std::optional<wlan_scan_result_t> ScanResultCode(uint64_t txn_id);
  const std::list<wlan_fullmac_scan_result_t>* ScanResultList(uint64_t txn_id);

  // SoftAP operation
  void StartSoftAp(const cssid_t& ssid = kDefaultSoftApSsid,
                   const wlan_channel_t& channel = kDefaultSoftApChannel,
                   uint32_t beacon_period = kDefaultSoftApBeaconPeriod,
                   uint32_t dtim_period = kDefaultSoftApDtimPeriod);
  void StopSoftAp();

  zx_status_t SetMulticastPromisc(bool enable);

  std::shared_ptr<simulation::Environment> env_;

  static wlan_fullmac_impl_ifc_protocol_ops_t default_sme_dispatch_tbl_;
  wlan_fullmac_impl_ifc_protocol default_ifc_ = {.ops = &default_sme_dispatch_tbl_, .ctx = this};

  // This provides our DDK (wlanif-impl) API into the interface
  void* if_impl_ctx_ = nullptr;
  const wlan_fullmac_impl_protocol_ops_t* if_impl_ops_ = nullptr;

  // Unique identifier provided by the driver
  uint16_t iface_id_;

  // Handles for SME <=> MLME communication, required but never used for communication (since no
  // SME is present).
  zx_handle_t ch_sme_ = ZX_HANDLE_INVALID;   // SME-owned side
  zx_handle_t ch_mlme_ = ZX_HANDLE_INVALID;  // MLME-owned side

  // Current state of association
  AssocContext assoc_ctx_ = {};

  // Current state of soft AP
  SoftApContext soft_ap_ctx_ = {};

  // Allows us to track individual operations
  Stats stats_ = {};

 private:
  wlan_mac_role_t role_ = 0;

  // Track scan results
  struct ScanStatus {
    // If not present, indicates that the scan has not completed yet
    std::optional<wlan_scan_result_t> result_code = std::nullopt;
    std::list<wlan_fullmac_scan_result_t> result_list;
  };
  // One entry per scan started
  std::map<uint64_t, ScanStatus> scan_results_;
  // BSS's IEs are raw pointers. Store the IEs here so we don't have dangling pointers
  std::vector<std::vector<uint8_t>> scan_results_ies_;
};

// A base class that can be used for creating simulation tests. It provides functionality that
// should be common to most tests (like creating a new device instance and setting up and plugging
// into the environment). It also provides a factory method for creating a new interface on the
// simulated device.
class SimTest : public ::zxtest::Test, public simulation::StationIfc {
 public:
  SimTest();
  ~SimTest();

  // In some cases (like error injection that affects the initialization) we want to work with
  // an uninitialized device. This method will allocate, but not initialize the device. To complete
  // initialization, the Init() function can be called after PreInit().
  zx_status_t PreInit();

  // Allocate device (if it hasn't already been allocated) and initialize it. This function doesn't
  // require PreInit() to be called first.
  zx_status_t Init();

  std::shared_ptr<simulation::Environment> env_;

 protected:
  // Create a new interface on the simulated device, providing the specified role and function
  // callbacks
  zx_status_t StartInterface(
      wlan_mac_role_t role, SimInterface* sim_ifc,
      std::optional<const wlan_fullmac_impl_ifc_protocol*> sme_protocol = std::nullopt,
      std::optional<common::MacAddr> mac_addr = std::nullopt);

  // Stop and delete a SimInterface
  zx_status_t DeleteInterface(SimInterface* ifc);

  // To notify simulator that an interface was destroyed.
  // e.g. when going through crash recovery.
  zx_status_t InterfaceDestroyed(SimInterface* sim_ifc);

  // Fake device manager
  std::unique_ptr<simulation::FakeDevMgr> dev_mgr_;

  // brcmfmac's concept of a device
  brcmfmac::SimDevice* device_ = nullptr;

  // Keep track of the ifaces we created during test by iface id.
  std::map<uint16_t, SimInterface*> ifaces_;

  fdf::WireSharedClient<fuchsia_wlan_phyimpl::WlanPhyImpl> client_;
  fidl::WireSyncClient<fuchsia_factory_wlan::Iovar> factory_device_;
  fdf::Dispatcher client_dispatcher_;
  fdf::Dispatcher driver_dispatcher_;
  fdf::Arena test_arena_;
  libsync::Completion completion_;
  libsync::Completion client_completion_;

 private:
  // StationIfc methods - by default, do nothing. These can/will be overridden by superclasses.
  void Rx(std::shared_ptr<const simulation::SimFrame> frame,
          std::shared_ptr<const simulation::WlanRxInfo> info) override {}
  async::Loop loop_;
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_
