/*
 * Copyright (c) 2019 The Fuchsia Authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_FW_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_FW_H_

#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <stdint.h>
#include <sys/types.h>
#include <zircon/types.h>

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-frame.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-sta-ifc.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/bcdc.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/brcmu_d11.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/brcmu_wifi.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/cfg80211.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil_types.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_errinj.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_hw.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_iovar.h"
#include "wlan/drivers/components/frame_storage.h"

namespace wlan::brcmfmac {

const common::MacAddr kZeroMac({0x0, 0x0, 0x0, 0x0, 0x0, 0x0});
// The amount of time we will wait for an association response after an association request
constexpr zx::duration kAssocTimeout = zx::sec(1);
// The amount of time we will wait for an authentication response after an authentication request
constexpr zx::duration kAuthTimeout = zx::sec(1);
// The amount of time we will wait for a reassociation response after a reassociation request
constexpr zx::duration kReassocTimeout = zx::sec(1);
// The amount of time we will wait for a beacon from an associated device before disassociating
// Timing based off broadcom firmware default value
constexpr uint32_t kBeaconTimeoutSeconds = 8;
// Delay between receiving start AP request and sending E_LINK event
constexpr zx::duration kStartAPLinkEventDelay = zx::msec(10);
// Delay before sending ASSOC event after client association
constexpr zx::duration kAssocEventDelay = zx::msec(10);
// Delay before sending REASSOC event after client reassociation
constexpr zx::duration kReassocEventDelay = zx::msec(10);
// Delay before sending ROAM event after client reassociation
constexpr zx::duration kRoamEventDelay = zx::msec(10);
// Delay between events E_LINK and E_SSID.
constexpr zx::duration kSsidEventDelay = zx::msec(100);
// Delay in sending E_LINK event during assoc & disassoc.
constexpr zx::duration kLinkEventDelay = zx::msec(5);
// Delay in sending E_DISASSOC event during disassoc.
constexpr zx::duration kDisassocEventDelay = zx::msec(1);
// Delay between cancelling a scan and receiving an abort event
constexpr zx::duration kAbortScanDelay = zx::msec(10);
// Delay before sending AP_STARTED event after client association
constexpr zx::duration kApStartedEventDelay = zx::msec(1);
// Size allocated to hold association frame IEs in SIM FW
#define ASSOC_IES_MAX_LEN 1000

class SimFirmware {
  class BcdcResponse {
   public:
    void Clear();

    // Copy data into buffer. This function will return ZX_ERR_INTERNAL
    // if len_ exceeds INT_MAX.
    zx_status_t Get(uint8_t* data, size_t len, int* rxlen_out);

    bool IsClear();

    // Copy data from buffer.
    void Set(uint8_t* data, size_t new_len);

   private:
    size_t len_;
    uint8_t msg_[BRCMF_DCMD_MAXLEN];
  };

  struct ScanResult {
    wlan_channel_t channel;
    common::MacAddr bssid;
    wlan::CapabilityInfo bss_capability;
    int8_t rssi_dbm;
    // Note: SSID appears in an IE.
    std::list<std::shared_ptr<wlan::simulation::InformationElement>> ies;
  };

  using ScanResultHandler = std::function<void(const ScanResult&)>;
  using ScanDoneHandler = std::function<void(brcmf_fweh_event_status_t)>;

  struct ScanOpts {
    // Unique scan identifier
    uint16_t sync_id;

    bool is_active;

    // Optional filters
    std::optional<cssid_t> ssid;
    std::optional<common::MacAddr> bssid;

    // Time per channel
    zx::duration dwell_time;

    // When a scan is in progress, the total number of channels being scanned
    std::vector<uint16_t> channels;

    // Function to call when we receive a beacon while scanning
    ScanResultHandler on_result_fn;

    // Function to call when we have finished scanning
    ScanDoneHandler on_done_fn;

    // Maximum number of probe requests sent per channel during active scan.
    uint16_t active_scan_max_attempts;
  };

  struct AssocOpts {
    common::MacAddr bssid;
    cssid_t ssid;
    bss_type_t bss_type;
  };

  // During a reassociation attempt, this is where reassoc params are stored.
  struct ReassocOpts {
    common::MacAddr bssid;
  };

 public:
  struct ScanState {
    // HOME means listening to home channel between scan channels
    enum { STOPPED, SCANNING, HOME } state = STOPPED;
    std::unique_ptr<ScanOpts> opts;
    // Next channel to scan (from channels)
    size_t channel_index;
    // The interface idx on which the scan is being done
    uint16_t ifidx;
    // Number of probe requests sent in the current channel so far during active scan.
    uint16_t active_scan_attempts;
  };

  struct AssocState {
    enum AssocStateName {
      NOT_ASSOCIATED,
      SCANNING,
      AUTHENTICATION_CHALLENGE_FAILURE,
      ASSOCIATING,
      ASSOCIATED,
      REASSOCIATING,
    } state = NOT_ASSOCIATED;

    std::unique_ptr<AssocOpts> opts;
    // Holds data for an in-progress reassociation (if any).
    std::unique_ptr<ReassocOpts> reassoc_opts;
    // Results seen during pre-assoc scan
    std::list<ScanResult> scan_results;
    // Unique id of timer event used to timeout an association request
    uint64_t assoc_timer_id;
    // Unique id of timer event used to timeout a reassociation request
    uint64_t reassoc_timer_id;
    // Association attempt number
    uint8_t num_attempts;
    bool is_beacon_watchdog_active = false;
    uint64_t beacon_watchdog_id_;
  };

  struct AuthState {
    enum {
      NOT_AUTHENTICATED,
      EXPECTING_SECOND,
      EXPECTING_FOURTH,

      // These are states for SAE external supplicant authentication. The real firmware reuses
      // states of normal SHARED_KEY authentication, here we add seperate states for SAE
      // authentication to make the logic more readable.
      EXPECTING_EXTERNAL_COMMIT,
      EXPECTING_AP_COMMIT,
      EXPECTING_EXTERNAL_CONFIRM,
      EXPECTING_AP_CONFIRM,
      EXPECTING_EXTERNAL_HANDSHAKE_RESP,

      AUTHENTICATED,
    } state = NOT_AUTHENTICATED;

    uint64_t auth_timer_id;
    enum simulation::SimSecProtoType sec_type = simulation::SEC_PROTO_TYPE_OPEN;
    common::MacAddr bssid;
  };

  struct ChannelSwitchState {
    enum {
      HOME,
      // SWITCHING means the event for conducting channel switch has already been set.
      SWITCHING
    } state = HOME;

    uint8_t new_channel;
    uint64_t switch_timer_id;
  };

  struct PacketBuf {
    std::unique_ptr<uint8_t[]> data;
    uint32_t len;
    // this is just to remember the coming netbuf's allocated_size, this, if the usage of
    // brcmf_netbuf is removed, this can be removed as well.
    uint32_t allocated_size_of_buf_in;
  };

  SimFirmware() = delete;
  explicit SimFirmware(brcmf_simdev* simdev);
  ~SimFirmware();

  SimErrorInjector err_inj_;
  simulation::StationIfc* GetHardwareIfc();
  void GetChipInfo(uint32_t* chip, uint32_t* chiprev);
  int32_t GetPM();
  // Num of clients currently associated with the SoftAP IF
  uint16_t GetNumClients(uint16_t ifidx);

  void TriggerFirmwareDisassoc(::fuchsia::wlan::ieee80211::ReasonCode reason);

  // Firmware iovar accessors
  zx_status_t IovarsSet(uint16_t ifidx, const char* name, const void* value, size_t value_len,
                        bcme_status_t* fw_err);
  zx_status_t IovarsGet(uint16_t ifidx, const char* name, void* value_out, size_t value_len,
                        bcme_status_t* fw_err);

  // Firmware error injection related methods
  void ErrorInjectSetBit(size_t inject_bit);
  void ErrorInjectClearBit(size_t inject_bit);
  void ErrorInjectAllClear();

  // channel-chanspec helper functions
  void convert_chanspec_to_channel(uint16_t chanspec, wlan_channel_t* ch);
  uint16_t convert_channel_to_chanspec(wlan_channel_t* channel);

  // Bus operations: calls from driver
  zx_status_t BusPreinit();
  void BusStop();
  zx_status_t BusTxCtl(unsigned char* msg, uint len);
  zx_status_t BusTxFrameSingle(const wlan::drivers::components::Frame& frame);
  zx_status_t BusTxFrames(cpp20::span<wlan::drivers::components::Frame> frames);
  zx_status_t BusRxCtl(unsigned char* msg, uint len, int* rxlen_out);
  zx_status_t BusFlushTxQueue(int ifidx);
  zx_status_t BusGetBootloaderMacAddr(uint8_t* mac_addr);
  zx_status_t BusQueueRxSpace(const rx_space_buffer_t* buffer_list, size_t buffer_count,
                              uint8_t* vmo_addrs[]);
  drivers::components::FrameContainer BusAcquireTxSpace(size_t count);
  // This function returns the wsec_key_list for an iface to outside.
  std::vector<brcmf_wsec_key_le> GetKeyList(uint16_t ifidx);

  std::optional<drivers::components::Frame> GetRxFrame();

  zx_status_t SetupIovarTable();
  zx_status_t SetupInternalVmo();

  // Direct handlers for different iovars.
  zx_status_t IovarIfaceVarSet(SimIovarSetReq* req);
  zx_status_t IovarIfaceVarGet(SimIovarGetReq* req);
  zx_status_t IovarSet(SimIovarSetReq* req);
  zx_status_t IovarGet(SimIovarGetReq* req);
  zx_status_t IovarAssocInfoGet(SimIovarGetReq* req);
  zx_status_t IovarAssocMgrCmdSet(SimIovarSetReq* req);
  zx_status_t IovarAssocRespIesGet(SimIovarGetReq* req);
  zx_status_t IovarBssSet(SimIovarSetReq* req);

  zx_status_t IovarCapGet(SimIovarGetReq* req);
  zx_status_t IovarChanspecSet(SimIovarSetReq* req);
  zx_status_t IovarCrashSet(SimIovarSetReq* req);
  zx_status_t IovarCurEtheraddrSet(SimIovarSetReq* req);
  zx_status_t IovarCurEtheraddrGet(SimIovarGetReq* req);
  zx_status_t IovarEscanSet(SimIovarSetReq* req);
  zx_status_t IovarInterfaceRemoveSet(SimIovarSetReq* req);
  zx_status_t IovarJoinSet(SimIovarSetReq* req);
  zx_status_t IovarNmodeGet(SimIovarGetReq* req);
  zx_status_t IovarMpcSet(SimIovarSetReq* req);
  zx_status_t IovarPfnMacaddrSet(SimIovarSetReq* req);
  zx_status_t IovarPfnMacaddrGet(SimIovarGetReq* req);
  zx_status_t IovarRrmGet(SimIovarGetReq* req);
  zx_status_t IovarRxchainGet(SimIovarGetReq* req);
  zx_status_t IovarSnrGet(SimIovarGetReq* req);
  zx_status_t IovarSsidSet(SimIovarSetReq* req);
  zx_status_t IovarStbcTxSet(SimIovarSetReq* req);
  zx_status_t IovarStbcTxGet(SimIovarGetReq* req);
  zx_status_t IovarTxstreamsSet(SimIovarSetReq* req);
  zx_status_t IovarTxstreamsGet(SimIovarGetReq* req);
  zx_status_t IovarVerGet(SimIovarGetReq* req);
  zx_status_t IovarVhtModeGet(SimIovarGetReq* req);
  zx_status_t IovarWmeAcStaGet(SimIovarGetReq* req);
  zx_status_t IovarWmeApsdGet(SimIovarGetReq* req);
  zx_status_t IovarWsecKeySet(SimIovarSetReq* req);
  zx_status_t IovarWsecKeyGet(SimIovarGetReq* req);
  zx_status_t IovarWstatsCountersGet(SimIovarGetReq* req);

 private:
  struct Client {
    // When we receive an authentication request of a client, if it's a reasonable request, we
    // will directly create the client with AUTHENTICATED state, and if it's not a reasonable
    // request, we will not record anything for this client, so the first state of a client is
    // AUTHENTICATED, no INIT or HOME state needed.
    enum State { AUTHENTICATED, ASSOCIATED };

    Client(common::MacAddr mac_addr, State state) : mac_addr(mac_addr), state(state) {}
    common::MacAddr mac_addr;
    State state;
  };
  /* SoftAP specific config
   * infra_mode - If AP is operating in Infra mode
   * beacon_period - Beacon period
   * dtim_period - DTIM period
   * ssid - ssid - non-zero length indicates AP start else AP stop
   * ap_started - indicates if ap has been started or stopped
   * clients - List of associated clients (mac address)
   */
  struct ApConfig {
    uint32_t infra_mode;
    uint32_t beacon_period;
    uint32_t dtim_period;
    brcmf_ssid_le ssid;
    bool ap_started;
    std::list<std::shared_ptr<Client>> clients;
  };

  /* This structure contains the variables related to an iface entry in SIM FW.
   * mac_addr - Mac address of the interface
   * mac_addr_set - Flag indicating if the mac address is set
   * chanspec - The operating channel of this interface
   * bsscfgidx - input from the driver indicating the bss index
   * allocated - maintained by SIM FW to indicate entry is allocated
   * iface_id - the iface id allocated by SIM FW - in this case always the array index of the
   * table cur_key_idx - The index indicates which key we are using for this iface wsec_key_list -
   * Storing keys for this iface ap_mode - is the iface in SoftAP(true) or Client(false) mode
   * ap_config - SoftAP specific config (set when interface is configured as SoftAP)
   */

  typedef struct sim_iface_entry {
    common::MacAddr mac_addr;
    bool mac_addr_set;
    int32_t bsscfgidx;
    bool allocated;
    int8_t iface_id;
    uint32_t is_up;
    bool ap_mode;
    ApConfig ap_config;

    // Iovar related fields
    uint16_t chanspec;
    uint32_t wsec = 0;
    uint32_t cur_key_idx = 0;
    std::vector<brcmf_wsec_key_le> wsec_key_list;
    uint32_t wpa_auth = 0;
    uint32_t tlv = 0;
    uint16_t auth_type = BRCMF_AUTH_MODE_OPEN;
    uint32_t allmulti = 0;
  } sim_iface_entry_t;

  // This value is specific to firmware, and drives some behavior (notably the interpretation
  // of the chanspec encoding).
  static constexpr uint32_t kIoType = BRCMU_D11AC_IOTYPE;

  // Default interface identification string
  static constexpr const char* kDefaultIfcName = "wl\x30";

  // Max number of interfaces supported
  static constexpr uint8_t kMaxIfSupported = 4;

  // BCDC interface
  std::unique_ptr<std::vector<uint8_t>> CreateBcdcBuffer(int16_t ifidx, size_t requested_size,
                                                         size_t* offset_out);
  zx_status_t BcdcVarOp(uint16_t ifidx, brcmf_proto_bcdc_dcmd* msg, uint8_t* data, size_t len,
                        bool is_set);

  // Iovar handlers
  zx_status_t SetMacAddr(uint16_t ifidx, const uint8_t* mac_addr);
  common::MacAddr GetMacAddr(uint16_t ifidx);
  zx_status_t HandleEscanRequest(const brcmf_escan_params_le* value, size_t value_len);
  zx_status_t HandleIfaceTblReq(const bool add_entry, const void* data, uint8_t* iface_id,
                                int32_t bsscfgidx);
  zx_status_t HandleIfaceRequest(const bool add_iface, const void* data, const size_t len,
                                 int32_t bsscfgidx);
  zx_status_t HandleJoinRequest(const void* value, size_t value_len);
  void HandleAssocReq(std::shared_ptr<const simulation::SimAssocReqFrame> frame);
  void HandleDisconnectForClientIF(std::shared_ptr<const simulation::SimManagementFrame> frame,
                                   const common::MacAddr& bssid,
                                   ::fuchsia::wlan::ieee80211::ReasonCode reason);
  void HandleAuthReq(std::shared_ptr<const simulation::SimAuthFrame> frame);
  void HandleAuthResp(std::shared_ptr<const simulation::SimAuthFrame> frame);
  void HandleReassocResp(std::shared_ptr<const simulation::SimReassocReqFrame> frame);

  // Generic scan operations
  zx_status_t ScanStart(std::unique_ptr<ScanOpts> opts);
  void ScanContinue();
  void ScanComplete(brcmf_fweh_event_status_t status);

  // Escan operations
  zx_status_t EscanStart(uint16_t sync_id, const brcmf_scan_params_le* params, size_t params_len);
  void EscanResultSeen(const ScanResult& scan_result);
  void EscanComplete(brcmf_fweh_event_status_t event_status);

  // Association operations
  void AssocInit(std::unique_ptr<AssocOpts> assoc_opts, wlan_channel_t& channel);
  void AssocScanResultSeen(const ScanResult& scan_result);
  void AssocScanDone(brcmf_fweh_event_status_t event_status);
  void AuthStart();  // Scan complete, start authentication process
  void AssocStart();
  void SetAssocState(AssocState::AssocStateName state);
  void AssocClearContext();
  void AuthClearContext();
  void AssocHandleFailure(::fuchsia::wlan::ieee80211::StatusCode status);
  void AuthHandleFailure();
  void DisassocStart(brcmf_scb_val_le* scb_val);
  void DisassocLocalClient(::fuchsia::wlan::ieee80211::ReasonCode reason);
  void SetStateToDisassociated(::fuchsia::wlan::ieee80211::ReasonCode reason,
                               bool locally_initiated);
  void ReassocInit(std::unique_ptr<ReassocOpts> reassoc_opts, wlan_channel_t& channel);
  void ReassocStart();
  void ReassocHandleFailure(::fuchsia::wlan::ieee80211::StatusCode status);
  zx_status_t ReassocToCurrentAp(std::shared_ptr<const simulation::SimReassocRespFrame> frame);
  zx_status_t ReassocToDifferentAp(std::shared_ptr<const simulation::SimReassocRespFrame> frame);

  void RestartBeaconWatchdog();
  void DisableBeaconWatchdog();
  void HandleBeaconTimeout();

  // Handlers for events from hardware
  void Rx(std::shared_ptr<const simulation::SimFrame> frame,
          std::shared_ptr<const simulation::WlanRxInfo> info);

  // Perform in-firmware ARP processing, if enabled. Returns a value indicating if the frame was
  // completely offloaded (and doesn't need to be passed to the driver).
  bool OffloadArpFrame(int16_t ifidx, std::shared_ptr<const simulation::SimDataFrame> data_frame);

  void RxMgmtFrame(std::shared_ptr<const simulation::SimManagementFrame> mgmt_frame,
                   std::shared_ptr<const simulation::WlanRxInfo> info);
  void RxDataFrame(std::shared_ptr<const simulation::SimDataFrame> data_frame,
                   std::shared_ptr<const simulation::WlanRxInfo> info);
  static int8_t RssiDbmFromSignalStrength(double signal_strength);
  void RxBeacon(const wlan_channel_t& channel,
                std::shared_ptr<const simulation::SimBeaconFrame> frame, double signal_strength);
  void RxAssocResp(std::shared_ptr<const simulation::SimAssocRespFrame> frame);
  void RxDisassocReq(std::shared_ptr<const simulation::SimDisassocReqFrame> frame);
  void RxAssocReq(std::shared_ptr<const simulation::SimAssocReqFrame> frame);
  void RxReassocResp(std::shared_ptr<const simulation::SimReassocRespFrame> frame);
  void RxActionFrame(std::shared_ptr<const simulation::SimActionFrame> action_frame);
  void RxWnmActionFrame(std::shared_ptr<const simulation::SimWnmActionFrame> wnm_frame);
  void RxBtmReqFrame(std::shared_ptr<const simulation::SimBtmReqFrame> btm_req_frame);
  void RxProbeResp(const wlan_channel_t& channel,
                   std::shared_ptr<const simulation::SimProbeRespFrame> frame,
                   double signal_strength);
  void RxAuthFrame(std::shared_ptr<const simulation::SimAuthFrame> frame);

  // Handler for channel switch.
  void ConductChannelSwitch(const wlan_channel_t& dst_channel, uint8_t mode);
  void RxDeauthReq(std::shared_ptr<const simulation::SimDeauthFrame> frame);

  void StopSoftAP(uint16_t ifidx);

  // Update the SAE status when firmware receives an auth frame from remote stations.
  zx_status_t RemoteUpdateExternalSaeStatus(uint16_t seq_num,
                                            ::fuchsia::wlan::ieee80211::StatusCode status_code,
                                            const uint8_t* sae_payload, size_t text_len);
  // Update the SAE status when firmware receives an auth frame from the driver.
  zx_status_t LocalUpdateExternalSaeStatus(uint16_t seq_num,
                                           ::fuchsia::wlan::ieee80211::StatusCode status_code,
                                           const uint8_t* sae_payload, size_t text_len);

  // Allocate a buffer for an event (brcmf_event)
  std::shared_ptr<std::vector<uint8_t>> CreateEventBuffer(size_t requested_size,
                                                          brcmf_event_msg_be** msg_be,
                                                          size_t* offset_out);

  // Wrap the buffer in an event and send back to the driver over the bus
  void SendEventToDriver(size_t payload_size, std::shared_ptr<std::vector<uint8_t>> buffer_in,
                         uint32_t event_type, uint32_t status, uint16_t ifidx,
                         char* ifname = nullptr, uint16_t flags = 0, uint32_t reason = 0,
                         std::optional<common::MacAddr> addr = {},
                         std::optional<zx::duration> delay = {});

  // Send received frame over the bus to the driver
  void SendFrameToDriver(uint16_t ifidx, size_t payload_size, const std::vector<uint8_t>& buffer_in,
                         std::shared_ptr<const simulation::WlanRxInfo> info);

  // Get the idx of the SoftAP IF based on Mac address
  int16_t GetIfidxByMac(const common::MacAddr& addr);

  // Get the channel of IF the parameter indicates whether we need to find softAP ifidx or client
  // ifidx.
  wlan_channel_t GetIfChannel(bool is_ap);

  // Get IF idx of matching bsscfgidx.
  int16_t GetIfidxByBsscfgidx(int32_t bsscfgidx);

  zx_status_t SetIFChanspec(uint16_t ifidx, uint16_t chanspec);
  // motivation_auth means whether this function is triggered by receiving disauth frame, the
  // value "true" means it is triggered by a deauth frame, and "false" means ut's triggered by a
  // disassoc frame.
  bool FindAndRemoveClient(const common::MacAddr client_mac, bool motivation_deauth,
                           ::fuchsia::wlan::ieee80211::ReasonCode deauth_reason);
  std::shared_ptr<Client> FindClient(const common::MacAddr client_mac);
  void ScheduleLinkEvent(zx::duration when, uint16_t ifidx);
  void SendAPStartLinkEvent(uint16_t ifidx);
  zx_status_t StopInterface(const int32_t bsscfgidx);

  // Schedule an event to release and re-create itself.
  void ResetSimFirmware();

  // This is the simulator object that represents the interface between the driver and the
  // firmware. We will use it to send back events.
  brcmf_simdev* simdev_;

  // Context for encoding/decoding chanspecs
  brcmu_d11inf d11_inf_;

  // Next message to pass back to a BCDC Rx Ctl request
  BcdcResponse bcdc_response_;

  // Simulated hardware state
  SimHardware hw_;

  // Interface table made up of IF entries. Each entry is analogous to an IF
  // created in the driver (see the comments above for the contents of each
  // entry). Interface specific config/parameters are stored in this table
  sim_iface_entry_t iface_tbl_[kMaxIfSupported] = {};

  // The two ifidxs we support for now, the client ifidx is always 0(according to real firmware),
  // and softap_ifidx is not fixed. here if the value of it is std::optnull, that means the softap
  // iface has not been allocated or has not been started yet.
  const uint16_t kClientIfidx = 0;
  std::optional<uint16_t> softap_ifidx_;

  // States for client iface
  ScanState scan_state_;
  AssocState assoc_state_;
  AuthState auth_state_;
  ChannelSwitchState channel_switch_state_;

  // This vmo and tx frame storage are used only internally
  // Note that this VMO id should not conflict with the ones used by SimDataPath
  static constexpr uint8_t kInternalVmoId = 0;
  zx::vmo vmo_;
  drivers::components::FrameStorage internal_tx_frame_storage_;

  // This stores frames given to SimFw through calls to BusQueueRxSpace
  drivers::components::FrameStorage rx_frame_storage_;

  // Internal firmware state variables
  std::array<uint8_t, ETH_ALEN> mac_addr_;
  bool mac_addr_set_ = false;
  common::MacAddr pfn_mac_addr_;
  bool default_passive_scan_ = true;
  uint32_t default_passive_time_ = -1;  // In ms. -1 indicates value has not been set.
  int32_t power_mode_ = 0;              // Default value of PM in FW is 0 (OFF)
  struct brcmf_fil_country_le country_code_;
  uint32_t assoc_max_retries_ = 0;
  uint32_t mpc_ = 1;  // Read FW appears to be setting this to 1 by default.
  uint32_t beacon_timeout_ = kBeaconTimeoutSeconds;
  std::atomic<unsigned long> error_inject_bits_ = 0;
  uint8_t assoc_resp_ies_[ASSOC_IES_MAX_LEN];
  size_t assoc_resp_ies_len_ = 0;
  uint32_t arpoe_ = 0;
  uint32_t arp_ol_ = 0;
  uint32_t ndoe_ = 0;
  uint32_t mchan_ = 1;  // This feature is enabled by default in firmware.
  uint32_t ampdu_ba_wsize_ = 64;
  uint32_t fakefrag_ = 0;
  int32_t stbc_tx_ = 0;               // 0 = disabled, 1 = enabled, -1 = auto
  uint32_t txstreams_ = 1;            // Number of Tx streams
  uint32_t wnm_ = 1;                  // This feature is enabled by default in firmware.
  uint32_t wnm_bsstrans_resp_ = 0x5;  // Default seen in real firmware.
  uint32_t roam_off_ = 0;             // Roam engine is enabled by default.
  uint32_t buf_key_b4_m4_ = 0;        // Buffer key until EAPOL 4th frame is sent out

  std::unordered_map<std::string, SimIovar> iovar_table_;
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_FW_H_
