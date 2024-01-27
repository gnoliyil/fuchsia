// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/wlan/associnfo/c/banjo.h>
#include <fuchsia/hardware/wlanphyimpl/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>
#include <zircon/errors.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/cfg80211.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/defs.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/macaddr.h"

namespace wlan::brcmfmac {

// Some default AP and association request values
constexpr uint16_t kDefaultCh = 149;
constexpr wlan_channel_t kDefaultChannel = {
    .primary = kDefaultCh, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
// Chanspec value corresponding to kDefaultChannel with current d11 encoder.
constexpr uint16_t kDefaultChanspec = 53397;
constexpr uint16_t kTestChanspec = 0xd0a5;
constexpr uint16_t kTest1Chanspec = 0xd095;
constexpr simulation::WlanTxInfo kDefaultTxInfo = {.channel = kDefaultChannel};
constexpr cssid_t kDefaultSsid = {.len = 15, .data = "Fuchsia Fake AP"};
const common::MacAddr kDefaultBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
const common::MacAddr kFakeMac({0xde, 0xad, 0xbe, 0xef, 0x00, 0x02});
const char kFakeClientName[] = "fake-client-iface";
const char kFakeApName[] = "fake-ap-iface";
// Iovar get/set command pairs used in metadata. The set command is in the metadata and the
// corresponding get command is used to verify if the set was issued.
struct iovar_get_set_cmd_t {
  uint32_t get_cmd;
  uint32_t set_cmd;
};
const iovar_get_set_cmd_t kIovarCmds[2] = {
    {BRCMF_C_GET_PM, BRCMF_C_SET_PM},
    {BRCMF_C_GET_FAKEFRAG, BRCMF_C_SET_FAKEFRAG},
};

class DynamicIfTest : public SimTest {
 public:
  // How long an individual test will run for. We need an end time because tests run until no more
  // events remain and if ap's are beaconing the test will run indefinitely.
  static constexpr zx::duration kTestDuration = zx::sec(100);
  DynamicIfTest() : ap_(env_.get(), kDefaultBssid, kDefaultSsid, kDefaultChannel) {}
  void Init();

  // How many devices have been registered by the fake devhost
  uint32_t DeviceCountByProtocolId(uint32_t proto_id);

  // Force fail an attempt to stop the softAP
  void InjectStopAPError();

  // Force firmware to ignore the start softAP request.
  void InjectStartAPIgnore();

  // Cancel the ignore-start-softAP-request error in firmware.
  void DelInjectedStartAPIgnore();

  // Verify SoftAP channel followed client channel
  void ChannelCheck();

  // Generate an association request to send to the soft AP
  void TxAuthAndAssocReq();
  void VerifyAssocWithSoftAP();

  // Verify the start ap timeout timer is triggered.
  void VerifyStartApTimer();

  // Interfaces to set and get chanspec iovar in sim-fw
  void SetChanspec(bool is_ap_iface, uint16_t* chanspec, zx_status_t expect_result);
  uint16_t GetChanspec(bool is_ap_iface, zx_status_t expect_result);

  // Run a dual mode (apsta) test, verifying AP stop behavior
  void TestApStop(bool use_cdown);

 protected:
  simulation::FakeAp ap_;
  SimInterface client_ifc_;
  SimInterface softap_ifc_;
  void CheckAddIfaceWritesWdev(wlan_mac_role_t role, const char iface_name[], SimInterface& ifc);
};

void DynamicIfTest::Init() {
  ASSERT_EQ(SimTest::Init(), ZX_OK);
  ap_.EnableBeacon(zx::msec(100));
}

uint32_t DynamicIfTest::DeviceCountByProtocolId(uint32_t proto_id) {
  return dev_mgr_->DeviceCountByProtocolId(proto_id);
}

void DynamicIfTest::InjectStopAPError() {
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.AddErrInjIovar("bss", ZX_ERR_IO, BCME_OK, softap_ifc_.iface_id_);
}

void DynamicIfTest::InjectStartAPIgnore() {
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.AddErrInjCmd(BRCMF_C_SET_SSID, ZX_OK, BCME_OK, softap_ifc_.iface_id_);
}

void DynamicIfTest::DelInjectedStartAPIgnore() {
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.DelErrInjCmd(BRCMF_C_SET_SSID);
}

void DynamicIfTest::ChannelCheck() {
  uint16_t softap_chanspec = GetChanspec(true, ZX_OK);
  uint16_t client_chanspec = GetChanspec(false, ZX_OK);
  EXPECT_EQ(softap_chanspec, client_chanspec);
  brcmf_simdev* sim = device_->GetSim();
  wlan_channel_t channel;
  sim->sim_fw->convert_chanspec_to_channel(softap_chanspec, &channel);
  EXPECT_GE(softap_ifc_.stats_.csa_indications.size(), 1U);
  EXPECT_EQ(channel.primary, softap_ifc_.stats_.csa_indications.front().new_channel);
}

void DynamicIfTest::TxAuthAndAssocReq() {
  // Get the mac address of the SoftAP
  common::MacAddr soft_ap_mac;
  softap_ifc_.GetMacAddr(&soft_ap_mac);
  cssid_t ssid = {.len = 6, .data = "Sim_AP"};
  // Pass the auth stop for softAP iface before assoc.
  simulation::SimAuthFrame auth_req_frame(kFakeMac, soft_ap_mac, 1, simulation::AUTH_TYPE_OPEN,
                                          ::fuchsia::wlan::ieee80211::StatusCode::SUCCESS);
  env_->Tx(auth_req_frame, kDefaultTxInfo, this);
  simulation::SimAssocReqFrame assoc_req_frame(kFakeMac, soft_ap_mac, ssid);
  env_->Tx(assoc_req_frame, kDefaultTxInfo, this);
}

void DynamicIfTest::VerifyAssocWithSoftAP() {
  // Verify the event indications were received and
  // the number of clients
  ASSERT_EQ(softap_ifc_.stats_.assoc_indications.size(), 1U);
  ASSERT_EQ(softap_ifc_.stats_.auth_indications.size(), 1U);
  brcmf_simdev* sim = device_->GetSim();
  uint16_t num_clients = sim->sim_fw->GetNumClients(softap_ifc_.iface_id_);
  ASSERT_EQ(num_clients, 1U);
}

void DynamicIfTest::VerifyStartApTimer() {
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.size(), 2U);
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.front().result_code,
            WLAN_START_RESULT_BSS_ALREADY_STARTED_OR_JOINED);
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.back().result_code,
            WLAN_START_RESULT_NOT_SUPPORTED);
}

void DynamicIfTest::SetChanspec(bool is_ap_iface, uint16_t* chanspec, zx_status_t expect_result) {
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp =
      brcmf_get_ifp(sim->drvr, is_ap_iface ? softap_ifc_.iface_id_ : client_ifc_.iface_id_);
  zx_status_t status = brcmf_fil_iovar_int_set(ifp, "chanspec", *chanspec, nullptr);
  EXPECT_EQ(status, expect_result);
}

uint16_t DynamicIfTest::GetChanspec(bool is_ap_iface, zx_status_t expect_result) {
  brcmf_simdev* sim = device_->GetSim();
  uint32_t chanspec;
  struct brcmf_if* ifp =
      brcmf_get_ifp(sim->drvr, is_ap_iface ? softap_ifc_.iface_id_ : client_ifc_.iface_id_);
  zx_status_t status = brcmf_fil_iovar_int_get(ifp, "chanspec", &chanspec, nullptr);
  EXPECT_EQ(status, expect_result);
  return chanspec;
}

// This function is used to override the default. The iovar list is made up
// of global iovars (not meant for a specific IF) and is arbitrary and is meant
// for test purposes only.
static zx_status_t modified_get_metadata(brcmf_bus* bus, void* data, size_t exp_size,
                                         size_t* actual) {
  wifi_config_t wifi_config = {
      .oob_irq_mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
      .iovar_table =
          {
              {IOVAR_STR_TYPE, {"ampdu_ba_wsize"}, 32},
              {IOVAR_CMD_TYPE, {.iovar_cmd = kIovarCmds[0].set_cmd}, 0},
              {IOVAR_CMD_TYPE, {.iovar_cmd = kIovarCmds[1].set_cmd}, 1},
              {IOVAR_STR_TYPE, {"mpc"}, 0},
              {IOVAR_LIST_END_TYPE, {{0}}, 0},
          },
      .cc_table =
          {
              {"US", 842},
              {"WW", 999},
              {"", 0},
          },
  };
  memcpy(data, &wifi_config, sizeof(wifi_config));
  *actual = sizeof(wifi_config);
  return ZX_OK;
}

// Modified Sim bus txctl to check if C_DOWN & C_UP were set from the driver during
// client disconnect.
static brcmf_bus_ops original_bus_ops;
static uint32_t c_down_cnt = 0;
static uint32_t c_up_cnt = 0;
static zx_status_t modified_bus_txctl(brcmf_bus* bus, unsigned char* msg, unsigned int len) {
  brcmf_proto_bcdc_dcmd* dcmd;
  constexpr size_t hdr_size = sizeof(struct brcmf_proto_bcdc_dcmd);

  if (len < hdr_size) {
    BRCMF_DBG(SIM, "Message length (%u) smaller than BCDC header size (%zd)", len, hdr_size);
    return ZX_ERR_INVALID_ARGS;
  }
  dcmd = reinterpret_cast<brcmf_proto_bcdc_dcmd*>(msg);
  size_t data_len = len - hdr_size;

  if (dcmd->len > data_len) {
    BRCMF_DBG(SIM, "BCDC total message length (%zd) exceeds buffer size (%u)", dcmd->len + hdr_size,
              len);
    // The real firmware allows the true buffer size (dcmd->len) to exceed the length of the txctl
    // itself (len - hdr_size). For an iovar get, we know this is allowed, so the sim firmware
    // should let such a call through.
    if (dcmd->cmd != BRCMF_C_GET_VAR) {
      return ZX_ERR_INVALID_ARGS;
    }
  }

  switch (dcmd->cmd) {
    case BRCMF_C_UP:
      c_up_cnt++;
      break;
    case BRCMF_C_DOWN:
      c_down_cnt++;
      break;
    default:
      break;
  }
  return original_bus_ops.txctl(bus, msg, len);
}

static zx_status_t validate_not_invoked_on_del(struct brcmf_if* ifp,
                                               const struct brcmf_event_msg* e, void* data) {
  struct brcmf_cfg80211_info* cfg = ifp->drvr->config;
  struct brcmf_if_event* ifevent = (struct brcmf_if_event*)data;

  // Ensure this is a DEL event and that vif event handling is not armed.
  EXPECT_EQ(BRCMF_E_IF_DEL, ifevent->action);
  EXPECT_EQ(false, brcmf_cfg80211_vif_event_armed(cfg));

  // If the above are true, then we do not expect the event handler to be invoked.
  EXPECT_TRUE(false);
  return ZX_OK;
}

TEST_F(DynamicIfTest, CreateDestroy) {
  Init();

  uint32_t buf_key_b4_m4;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  // Iovar buf_key_b4_m4 is set to 1 during init. Check if it is set correctly.
  EXPECT_EQ(ZX_OK, brcmf_fil_iovar_data_get(ifp, "buf_key_b4_m4", &buf_key_b4_m4,
                                            sizeof(buf_key_b4_m4), nullptr));
  EXPECT_EQ(buf_key_b4_m4, 1U);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, kFakeMac), ZX_OK);

  // Verify whether the provided MAC addr is used when creating the client iface.
  common::MacAddr client_mac;
  client_ifc_.GetMacAddr(&client_mac);
  EXPECT_EQ(client_mac, kFakeMac);

  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);

  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_, std::nullopt, kDefaultBssid), ZX_OK);

  // Verify whether the default bssid is correctly set to sim-fw when creating softAP iface.
  common::MacAddr soft_ap_mac;
  softap_ifc_.GetMacAddr(&soft_ap_mac);
  EXPECT_EQ(soft_ap_mac, kDefaultBssid);

  EXPECT_EQ(DeleteInterface(&softap_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// fxbug.dev/78738 resulted because of a race created by the invokation of the event handler prior
// to handling of the armed check based interface removal. In this test, we use a fake event handler
// to validate that the handler is invoked after all BRCMF_E_IF_DEL related handling is complete.
TEST_F(DynamicIfTest, EventHandlingOnSoftAPDel) {
  Init();
  brcmf_simdev* sim = device_->GetSim();
  wireless_dev* wdev = nullptr;

  // We manually start the AP interface to avoid the additional logic within
  // StartInterface(). This allows us to keep the interface removal simple.
  wlan_mac_role_t ap_role = WLAN_MAC_ROLE_AP;
  EXPECT_EQ(ZX_OK, softap_ifc_.Init(env_, ap_role));

  wlan_phy_impl_create_iface_req_t req = {
      .role = ap_role,
      .mlme_channel = softap_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, kFakeApName, nullptr, &req, &wdev));

  // Override callback function to validate that it does not get invoked for del event.
  brcmf_fweh_unregister(sim->drvr, BRCMF_E_IF);
  EXPECT_EQ(ZX_OK, brcmf_fweh_register(sim->drvr, BRCMF_E_IF, validate_not_invoked_on_del));

  struct net_device* ndev = wdev->netdev;
  struct brcmf_if* ifp = ndev_to_if(ndev);
  EXPECT_EQ(wdev->iftype, WLAN_MAC_ROLE_AP);
  EXPECT_EQ(ZX_OK, brcmf_fil_bsscfg_data_set(ifp, "interface_remove", nullptr, 0));
}

// Verify if all iovars in metadata were set.
static void verify_metadata_iovars(brcmf_simdev* sim, brcmf_if* ifp) {
  // Get the meta data to check if all iovars were set
  wifi_config_t config;
  size_t actual;

  ASSERT_EQ(brcmf_bus_get_wifi_metadata(sim->drvr->bus_if, &config, sizeof(wifi_config_t), &actual),
            ZX_OK);
  // Check to see if all IOVARs in metadata are being set during init.
  // Go through the iovar table and check if all iovars were set correctly.
  for (int i = 0; i < MAX_IOVAR_ENTRIES; ++i) {
    zx_status_t err;
    uint32_t cur_val;
    switch (config.iovar_table[i].iovar_type) {
      case IOVAR_STR_TYPE: {
        // Get the current value
        err = brcmf_fil_iovar_int_get(ifp, config.iovar_table[i].iovar_str, &cur_val, nullptr);
        ASSERT_EQ(err, ZX_OK);
        BRCMF_DBG(SIM, "iovar %s get: %d new: %d", config.iovar_table[i].iovar_str, cur_val,
                  config.iovar_table[i].val);
        ASSERT_EQ(config.iovar_table[i].val, cur_val);
        break;
      }
      case IOVAR_CMD_TYPE: {
        // Get the corresponding IOVAR Get command for the Set command in the table.
        uint32_t get_cmd = 0;
        bool cmd_found = false;
        for (size_t j = 0; j < sizeof(kIovarCmds) / sizeof(iovar_get_set_cmd_t); ++j) {
          if (kIovarCmds[j].set_cmd == config.iovar_table[i].iovar_cmd) {
            get_cmd = kIovarCmds[j].get_cmd;
            cmd_found = true;
            break;
          }
        }
        ASSERT_EQ(cmd_found, true);
        // Get the current value
        err = brcmf_fil_cmd_data_get(ifp, get_cmd, &cur_val, sizeof(cur_val), nullptr);
        ASSERT_EQ(err, ZX_OK);
        ASSERT_EQ(config.iovar_table[i].val, cur_val);
        break;
      }
      case IOVAR_LIST_END_TYPE: {
        // End of list, done checking iovars
        return;
      }
      default: {
        // Should never get here.
        ZX_ASSERT(0);
      }
    }
  }
}

// Check to see if all IOVARs in metadata are being set during init.
TEST_F(DynamicIfTest, CheckClientInitParams) {
  // Call Preinit to create the sim device (initialization is done in Init()).
  PreInit();
  brcmf_simdev* sim = device_->GetSim();
  // Replace get_wifi_metadata with the local function
  brcmf_bus_ops modified_bus_ops = *(sim->drvr->bus_if->ops);
  modified_bus_ops.get_wifi_metadata = &modified_get_metadata;
  const brcmf_bus_ops* original_bus_ops = sim->drvr->bus_if->ops;
  sim->drvr->bus_if->ops = &modified_bus_ops;

  // Complete initialization.
  Init();
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, kFakeMac), ZX_OK);

  // Get the client ifp to check iovars
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);
  ASSERT_NE(ifp, nullptr);

  // Verify if all iovars in metadata were set to FW.
  verify_metadata_iovars(sim, ifp);

  // Set sim->drvr->bus_if->ops back to the original set of brcmf_bus_ops
  sim->drvr->bus_if->ops = original_bus_ops;
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test case verifies that starting an AP iface using the same MAC address as the existing
// client iface will return an error.
TEST_F(DynamicIfTest, CreateApWithSameMacAsClient) {
  Init();
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  // Create AP iface with the same mac addr.
  common::MacAddr client_mac;
  client_ifc_.GetMacAddr(&client_mac);
  EXPECT_EQ(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_, std::nullopt, client_mac),
            ZX_ERR_ALREADY_EXISTS);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// Ensure AP uses auto-gen MAC address when MAC address is not specified in the StartInterface
// request.
TEST_F(DynamicIfTest, CreateApWithNoMACAddress) {
  Init();
  brcmf_simdev* sim = device_->GetSim();

  // Get the expected auto-gen MAC addr that AP will use when no MAC addr is passed.
  // Note, since the default MAC addr of client iface is same as the AP iface, we use that to figure
  // out the auto-gen MAC addr.
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  wlan::common::MacAddr expected_mac_addr;
  EXPECT_EQ(brcmf_gen_ap_macaddr(ifp, expected_mac_addr), ZX_OK);

  // Ensure passing nullopt for mac_addr results in use of auto generated MAC address.
  EXPECT_EQ(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_, std::nullopt, std::nullopt), ZX_OK);
  common::MacAddr softap_mac;
  softap_ifc_.GetMacAddr(&softap_mac);
  EXPECT_EQ(softap_mac, expected_mac_addr);

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&softap_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies that if we want to create an client iface with the same MAC address as the
// pre-set one, no error will be returned.
TEST_F(DynamicIfTest, CreateClientWithPreAllocMac) {
  Init();
  common::MacAddr pre_set_mac;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  zx_status_t status =
      brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", pre_set_mac.byte, ETH_ALEN, nullptr);
  EXPECT_EQ(status, ZX_OK);

  EXPECT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, pre_set_mac), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies that we still successfully create an iface with a random
// MAC address even if the bootloader MAC address cannot be retrieved.
TEST_F(DynamicIfTest, CreateClientWithRandomMac) {
  Init();
  brcmf_simdev* sim = device_->GetSim();

  // Replace get_bootloader_mac_addr with a function that will fail
  brcmf_bus_ops modified_bus_ops = *(sim->drvr->bus_if->ops);
  modified_bus_ops.get_bootloader_macaddr = [](brcmf_bus* bus, uint8_t* mac_addr) {
    return ZX_ERR_NOT_SUPPORTED;
  };
  const brcmf_bus_ops* original_bus_ops = sim->drvr->bus_if->ops;
  sim->drvr->bus_if->ops = &modified_bus_ops;

  // Test that get_bootloader_macaddr was indeed replaced
  uint8_t bootloader_macaddr[ETH_ALEN];
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED,
            brcmf_bus_get_bootloader_macaddr(sim->drvr->bus_if, bootloader_macaddr));

  EXPECT_EQ(ZX_OK, client_ifc_.Init(env_, WLAN_MAC_ROLE_CLIENT));
  wireless_dev* wdev = nullptr;
  wlan_phy_impl_create_iface_req_t req = {
      .role = WLAN_MAC_ROLE_CLIENT,
      .mlme_channel = client_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, kFakeClientName, nullptr, &req, &wdev));
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_del_iface(sim->drvr->config, wdev));

  // Set sim->drvr->bus_if->ops back to the original set of brcmf_bus_ops
  sim->drvr->bus_if->ops = original_bus_ops;
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies brcmf_cfg80211_add_iface() returns ZX_ERR_INVALID_ARGS if the wdev_out
// argument is nullptr.
TEST_F(DynamicIfTest, CreateIfaceMustProvideWdevOut) {
  Init();
  brcmf_simdev* sim = device_->GetSim();

  wlan_mac_role_t client_role = WLAN_MAC_ROLE_CLIENT;
  EXPECT_EQ(ZX_OK, client_ifc_.Init(env_, client_role));
  wlan_phy_impl_create_iface_req_t req = {
      .role = client_role,
      .mlme_channel = client_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            brcmf_cfg80211_add_iface(sim->drvr, kFakeClientName, nullptr, &req, nullptr));

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

void DynamicIfTest::CheckAddIfaceWritesWdev(wlan_mac_role_t role, const char iface_name[],
                                            SimInterface& ifc) {
  brcmf_simdev* sim = device_->GetSim();
  wireless_dev* wdev = nullptr;

  EXPECT_EQ(ZX_OK, ifc.Init(env_, role));
  wlan_phy_impl_create_iface_req_t req = {
      .role = role,
      .mlme_channel = ifc.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, iface_name, nullptr, &req, &wdev));
  EXPECT_NE(nullptr, wdev);
  EXPECT_NE(nullptr, wdev->netdev);
  EXPECT_EQ(wdev->iftype, role);

  EXPECT_EQ(ZX_OK, brcmf_cfg80211_del_iface(sim->drvr->config, wdev));

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies brcmf_cfg80211_add_iface() behavior with respect to
// the wdev_out argument and the client role.
TEST_F(DynamicIfTest, CreateClientWritesWdev) {
  Init();
  CheckAddIfaceWritesWdev(WLAN_MAC_ROLE_CLIENT, kFakeClientName, client_ifc_);
}

// This test verifies brcmf_cfg80211_add_iface() behavior with respect to
// the wdev_out argument and the AP role.
TEST_F(DynamicIfTest, CreateApWritesWdev) {
  Init();
  CheckAddIfaceWritesWdev(WLAN_MAC_ROLE_AP, kFakeApName, softap_ifc_);
}

// This test verifies new client interface names are assigned, and that the default for the
// primary network interface is kPrimaryNetworkInterfaceName (defined in core.h)
TEST_F(DynamicIfTest, CreateClientWithCustomName) {
  Init();
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  wireless_dev* wdev = nullptr;

  wlan_mac_role_t client_role = WLAN_MAC_ROLE_CLIENT;
  EXPECT_EQ(ZX_OK, client_ifc_.Init(env_, client_role));

  wlan_phy_impl_create_iface_req_t req = {
      .role = client_role,
      .mlme_channel = client_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(0, strcmp(brcmf_ifname(ifp), kPrimaryNetworkInterfaceName));
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, kFakeClientName, nullptr, &req, &wdev));
  EXPECT_EQ(0, strcmp(wdev->netdev->name, kFakeClientName));
  EXPECT_EQ(0, strcmp(brcmf_ifname(ifp), kFakeClientName));
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_del_iface(sim->drvr->config, wdev));
  EXPECT_EQ(0, strcmp(brcmf_ifname(ifp), kPrimaryNetworkInterfaceName));

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies new ap interface names are assigned.
TEST_F(DynamicIfTest, CreateApWithCustomName) {
  Init();
  brcmf_simdev* sim = device_->GetSim();
  wireless_dev* wdev = nullptr;

  wlan_mac_role_t ap_role = WLAN_MAC_ROLE_AP;
  EXPECT_EQ(ZX_OK, softap_ifc_.Init(env_, ap_role));

  wlan_phy_impl_create_iface_req_t req = {
      .role = ap_role,
      .mlme_channel = softap_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, kFakeApName, nullptr, &req, &wdev));
  EXPECT_EQ(0, strcmp(wdev->netdev->name, kFakeApName));
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_del_iface(sim->drvr->config, wdev));

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies the truncation of long interface names.
TEST_F(DynamicIfTest, CreateClientWithLongName) {
  Init();
  brcmf_simdev* sim = device_->GetSim();
  wireless_dev* wdev = nullptr;

  wlan_mac_role_t client_role = WLAN_MAC_ROLE_CLIENT;
  EXPECT_EQ(ZX_OK, client_ifc_.Init(env_, client_role));

  size_t really_long_name_len = NET_DEVICE_NAME_MAX_LEN + 1;
  ASSERT_GT(really_long_name_len,
            (size_t)NET_DEVICE_NAME_MAX_LEN);  // assert + 1 did not cause an overflow
  char really_long_name[really_long_name_len];
  for (size_t i = 0; i < really_long_name_len - 1; i++) {
    really_long_name[i] = '0' + ((i + 1) % 10);
  }
  really_long_name[really_long_name_len - 1] = '\0';

  char truncated_name[NET_DEVICE_NAME_MAX_LEN];
  strlcpy(truncated_name, really_long_name, sizeof(truncated_name));
  ASSERT_LT(strlen(truncated_name),
            strlen(really_long_name));  // sanity check that truncated_name is actually shorter

  wlan_phy_impl_create_iface_req_t req = {
      .role = client_role,
      .mlme_channel = client_ifc_.ch_mlme_,
      .has_init_sta_addr = false,
  };
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_add_iface(sim->drvr, really_long_name, nullptr, &req, &wdev));
  EXPECT_EQ(0, strcmp(wdev->netdev->name, truncated_name));
  EXPECT_EQ(ZX_OK, brcmf_cfg80211_del_iface(sim->drvr->config, wdev));
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies that creating a client interface with a pre-set MAC address will not cause
// the pre-set MAC to be remembered by the driver.
TEST_F(DynamicIfTest, CreateClientWithCustomMac) {
  Init();
  common::MacAddr retrieved_mac;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  EXPECT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, kFakeMac), ZX_OK);
  EXPECT_EQ(ZX_OK,
            brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", retrieved_mac.byte, ETH_ALEN, nullptr));
  EXPECT_EQ(retrieved_mac, kFakeMac);

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// This test verifies that creating a client interface with a custom MAC address will not cause
// subsequent client ifaces to use the same custom MAC address instead of using the bootloader
// (or random) MAC address.
TEST_F(DynamicIfTest, ClientDefaultMacFallback) {
  Init();
  common::MacAddr pre_set_mac;
  common::MacAddr retrieved_mac;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, 0);
  EXPECT_EQ(ZX_OK,
            brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", pre_set_mac.byte, ETH_ALEN, nullptr));

  // Create a client with a custom MAC address
  EXPECT_EQ(ZX_OK, StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, kFakeMac));
  EXPECT_EQ(ZX_OK,
            brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", retrieved_mac.byte, ETH_ALEN, nullptr));
  EXPECT_EQ(retrieved_mac, kFakeMac);

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);

  // Create a client without a custom MAC address
  EXPECT_EQ(ZX_OK, StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_));
  EXPECT_EQ(ZX_OK,
            brcmf_fil_iovar_data_get(ifp, "cur_etheraddr", retrieved_mac.byte, ETH_ALEN, nullptr));
  EXPECT_EQ(retrieved_mac, pre_set_mac);

  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// Test to check if C_DOWN & C_UP are set in FW during client disconnect.
TEST_F(DynamicIfTest, CheckIfDownUpCalled) {
  // Call Preinit to create the sim device (initialization is done in Init()).
  PreInit();
  brcmf_simdev* sim = device_->GetSim();
  // Replace txctl with the local function
  brcmf_bus_ops modified_bus_ops = *(sim->drvr->bus_if->ops);
  modified_bus_ops.txctl = &modified_bus_txctl;
  original_bus_ops = *(sim->drvr->bus_if->ops);
  sim->drvr->bus_if->ops = &modified_bus_ops;

  // Complete initialization.
  Init();
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_, std::nullopt, kFakeMac), ZX_OK);
  c_down_cnt = 0;
  c_up_cnt = 0;
  // Associate to FakeAp
  client_ifc_.AssociateWith(ap_, zx::msec(10));

  constexpr reason_code_t deauth_reason = REASON_CODE_LEAVING_NETWORK_DISASSOC;
  // Schedule a deauth from SME
  env_->ScheduleNotification([&] { client_ifc_.DeauthenticateFrom(kDefaultBssid, deauth_reason); },
                             zx::sec(1));
  env_->Run(kTestDuration);
  // Check if C_DOWN & C_UP are set in FW.
  EXPECT_EQ(c_down_cnt, 1u);
  EXPECT_EQ(c_up_cnt, 1u);

  // Set sim->drvr->bus_if->ops back to the original set of brcmf_bus_ops
  sim->drvr->bus_if->ops = &original_bus_ops;
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

TEST_F(DynamicIfTest, DualInterfaces) {
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 2u);

  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeleteInterface(&softap_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

// Start both client and SoftAP interfaces simultaneously and check if
// the client can associate to a FakeAP and a fake client can associate to the
// SoftAP.
TEST_F(DynamicIfTest, ConnectBothInterfaces) {
  // Create our device instances
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  // Start our SoftAP
  softap_ifc_.StartSoftAp();

  // Associate to FakeAp
  client_ifc_.AssociateWith(ap_, zx::msec(10));
  // Associate to SoftAP
  env_->ScheduleNotification(std::bind(&DynamicIfTest::TxAuthAndAssocReq, this), zx::msec(100));

  env_->Run(kTestDuration);

  // Check if the client's assoc with FakeAP succeeded
  EXPECT_EQ(client_ifc_.stats_.connect_attempts, 1U);
  EXPECT_EQ(client_ifc_.stats_.connect_successes, 1U);
  // Verify Assoc with SoftAP succeeded
  VerifyAssocWithSoftAP();
  // Disassoc related tests are in connect_test.cc
}

void DynamicIfTest::TestApStop(bool use_cdown) {
  // Create our device instances
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  // Start our SoftAP
  softap_ifc_.StartSoftAp();

  // Optionally force the use of a C_DOWN command, which has the side-effect of bringing down the
  // client interface.
  if (use_cdown) {
    InjectStopAPError();
  }

  // Associate to FakeAp
  client_ifc_.AssociateWith(ap_, zx::msec(10));

  // Associate to SoftAP
  env_->ScheduleNotification(std::bind(&DynamicIfTest::TxAuthAndAssocReq, this), zx::msec(100));

  // Verify Assoc with SoftAP succeeded
  env_->ScheduleNotification(std::bind(&DynamicIfTest::VerifyAssocWithSoftAP, this), zx::msec(150));
  env_->ScheduleNotification(std::bind(&SimInterface::StopSoftAp, &softap_ifc_), zx::msec(160));

  env_->Run(kTestDuration);

  // Check if the client's assoc with FakeAP succeeded
  EXPECT_EQ(client_ifc_.stats_.connect_attempts, 1U);
  EXPECT_EQ(client_ifc_.stats_.connect_successes, 1U);
  // Disassoc and other assoc scenarios are covered in connect_test.cc
}

// Start both client and SoftAP interfaces simultaneously and check if
// the client can associate to a FakeAP and a fake client can associate to the
// SoftAP. Set PM mode and ensure it does not get affected during start/stop/delete
// of the IFs as well as during client assoc.
TEST_F(DynamicIfTest, PM_ModeDoesNotGetAffected) {
  // Create our device instances
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);
  uint32_t pm_mode = PM_FAST;
  zx_status_t err = brcmf_fil_cmd_int_set(ifp, BRCMF_C_SET_PM, pm_mode, nullptr);
  EXPECT_EQ(err, ZX_OK);
  uint32_t cur_pm_mode = PM_OFF;
  err = brcmf_fil_cmd_int_get(ifp, BRCMF_C_GET_PM, &cur_pm_mode, nullptr);
  EXPECT_EQ(err, ZX_OK);
  EXPECT_EQ(cur_pm_mode, pm_mode);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  // Start our SoftAP
  softap_ifc_.StartSoftAp();

  // Associate to FakeAp
  client_ifc_.AssociateWith(ap_, zx::msec(10));
  // Associate to SoftAP
  env_->ScheduleNotification(std::bind(&DynamicIfTest::TxAuthAndAssocReq, this), zx::msec(100));

  env_->Run(kTestDuration);

  // Check if the client's assoc with FakeAP succeeded
  EXPECT_EQ(client_ifc_.stats_.connect_attempts, 1U);
  EXPECT_EQ(client_ifc_.stats_.connect_successes, 1U);
  // Verify Assoc with SoftAP succeeded
  VerifyAssocWithSoftAP();
  EXPECT_EQ(DeleteInterface(&softap_ifc_), ZX_OK);
  cur_pm_mode = PM_OFF;
  err = brcmf_fil_cmd_int_get(ifp, BRCMF_C_GET_PM, &cur_pm_mode, nullptr);
  EXPECT_EQ(err, ZX_OK);
  EXPECT_EQ(cur_pm_mode, pm_mode);
}
// Start both client and SoftAP interfaces simultaneously and check if stopping the AP's beacons
// does not affect the client.
TEST_F(DynamicIfTest, StopAPDoesntAffectClientIF) {
  TestApStop(false);
  // Verify that we didn't shut down our client interface
  EXPECT_EQ(client_ifc_.stats_.deauth_indications.size(), 0U);
  EXPECT_EQ(client_ifc_.stats_.disassoc_indications.size(), 0U);
}

// Start both client and SoftAP interfaces simultaneously and check if stopping the AP with iovar
// bss fails, does not bring down the client when C_DOWN is issued
TEST_F(DynamicIfTest, UsingCdownDoesntAffectClientIF) {
  TestApStop(true);
  // Verify that the client interface did not disconnect.
  EXPECT_EQ(client_ifc_.stats_.disassoc_indications.size(), 0U);
}

TEST_F(DynamicIfTest, SetClientChanspecAfterAPStarted) {
  // Create our device instances
  Init();

  uint16_t chanspec;
  // Create softAP iface and start
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);
  softap_ifc_.StartSoftAp(SimInterface::kDefaultSoftApSsid, kDefaultChannel);

  // The chanspec of softAP iface should be set to default one.
  chanspec = GetChanspec(true, ZX_OK);
  EXPECT_EQ(chanspec, kDefaultChanspec);

  // After creating client iface and setting a different chanspec to it, chanspec of softAP will
  // change as a result of this operation.
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  chanspec = kTestChanspec;
  SetChanspec(false, &chanspec, ZX_OK);

  // Confirm chanspec of AP is same as client
  chanspec = GetChanspec(true, ZX_OK);
  EXPECT_EQ(chanspec, kTestChanspec);
}

TEST_F(DynamicIfTest, SetAPChanspecAfterClientCreated) {
  // Create our device instances
  Init();

  // Create client iface and set chanspec
  uint16_t chanspec = kTestChanspec;
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  SetChanspec(false, &chanspec, ZX_OK);

  // Create and start softAP iface to and set another chanspec
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);
  softap_ifc_.StartSoftAp();
  // When we call StartSoftAP, the kDefaultCh will be transformed into chanspec(in this case the
  // value is 53397) and set to softAP iface, but since there is already a client iface activated,
  // that input chanspec will be ignored and set to client's chanspec.
  chanspec = GetChanspec(true, ZX_OK);
  EXPECT_EQ(chanspec, kTestChanspec);

  // Now if we set chanspec again to softAP when it already have a chanspec, this operation is
  // silently rejected
  chanspec = kTest1Chanspec;
  SetChanspec(true, &chanspec, ZX_OK);
}

// Start SoftAP after client assoc. SoftAP's channel should get set to client's channel
TEST_F(DynamicIfTest, CheckSoftAPChannel) {
  // Create our device instances
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  zx::duration delay = zx::msec(10);
  // Associate to FakeAp
  client_ifc_.AssociateWith(ap_, delay);
  // Start our SoftAP
  delay += zx::msec(10);
  env_->ScheduleNotification(std::bind(&SimInterface::StartSoftAp, &softap_ifc_,
                                       SimInterface::kDefaultSoftApSsid, kDefaultChannel, 100, 100),
                             delay);

  // Wait until SIM FW sends AP Start confirmation. This is set as a
  // scheduled event to ensure test runs until AP Start confirmation is
  // received.
  delay += kStartAPLinkEventDelay + kApStartedEventDelay + zx::msec(10);
  env_->ScheduleNotification(std::bind(&DynamicIfTest::ChannelCheck, this), delay);
  env_->Run(kTestDuration);

  EXPECT_EQ(client_ifc_.stats_.connect_successes, 1U);
}

// This intricate test name means that, the timeout timer should fire when SME issued an iface start
// request for softAP iface, but firmware didn't respond anything, at the same time, SME is still
// keep sending the iface start request.
TEST_F(DynamicIfTest, StartApIfaceTimeoutWithReqSpamAndFwIgnore) {
  // Create both ifaces, client iface is not needed in test, but created to keep the consistent
  // context.
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  // Make firmware ignore the start AP req.
  InjectStartAPIgnore();
  env_->ScheduleNotification(std::bind(&SimInterface::StartSoftAp, &softap_ifc_,
                                       SimInterface::kDefaultSoftApSsid, kDefaultChannel, 100, 100),
                             zx::msec(10));
  // Make sure this extra request will not refresh the timer.
  env_->ScheduleNotification(std::bind(&SimInterface::StartSoftAp, &softap_ifc_,
                                       SimInterface::kDefaultSoftApSsid, kDefaultChannel, 100, 100),
                             zx::msec(510));

  env_->ScheduleNotification(std::bind(&DynamicIfTest::VerifyStartApTimer, this), zx::msec(1011));
  // Make firmware back to normal.
  env_->ScheduleNotification(std::bind(&DynamicIfTest::DelInjectedStartAPIgnore, this),
                             zx::msec(1011));
  // Issue start AP request again.
  env_->ScheduleNotification(std::bind(&SimInterface::StartSoftAp, &softap_ifc_,
                                       SimInterface::kDefaultSoftApSsid, kDefaultChannel, 100, 100),
                             zx::msec(1100));

  env_->Run(kTestDuration);

  // Make sure the AP iface finally stated successfully.
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.size(), 3U);
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.back().result_code, WLAN_START_RESULT_SUCCESS);
}

// This test case verifies that a scan request comes while a AP start req is in progress will be
// rejected. Because the AP start request will return a success immediately in SIM, so here we
// inject a ignore error for AP start req to simulate the pending of it.
TEST_F(DynamicIfTest, RejectScanWhenApStartReqIsPending) {
  constexpr uint64_t kScanId = 0x18c5f;
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_);

  InjectStartAPIgnore();
  env_->ScheduleNotification(std::bind(&SimInterface::StartSoftAp, &softap_ifc_,
                                       SimInterface::kDefaultSoftApSsid, kDefaultChannel, 100, 100),
                             zx::msec(30));
  // The timeout of AP start is 1000 msec, so a scan request before zx::msec(1030) will be rejected.
  env_->ScheduleNotification(std::bind(&SimInterface::StartScan, &client_ifc_, kScanId, false,
                                       std::optional<const std::vector<uint8_t>>{}),
                             zx::msec(100));

  env_->Run(kTestDuration);
  // There will be no result received from firmware, because the fake external AP's channel number
  // is 149, The scan has been stopped before reaching that channel.
  EXPECT_EQ(client_ifc_.ScanResultList(kScanId)->size(), 0U);
  ASSERT_NE(client_ifc_.ScanResultCode(kScanId), std::nullopt);
  EXPECT_EQ(client_ifc_.ScanResultCode(kScanId).value(), WLAN_SCAN_RESULT_SHOULD_WAIT);

  // AP start will also fail because the request is ignored in firmware.
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.size(), 1U);
  EXPECT_EQ(softap_ifc_.stats_.start_confirmations.back().result_code,
            WLAN_START_RESULT_NOT_SUPPORTED);
}
}  // namespace wlan::brcmfmac
