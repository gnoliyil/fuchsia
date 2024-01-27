/******************************************************************************
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

#include <stdbool.h>
#include <threads.h>
#include <zircon/compiler.h>
#include <zircon/syscalls.h>

#if 0  // NEEDS_PORTING
#include "fw-api.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/acpi.h"
#endif  // NEEDS_PORTING
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/nan.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/scan.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/dbg.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/img.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/notif-wait.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-csr.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-debug.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-drv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-eeprom-parse.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-io.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-modparams.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-op-mode.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-phy-db.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-prph.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-trans.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/mvm.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/rs.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/time-event.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/banjo/ieee80211.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/rcu.h"

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dnt-cfg.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dnt-dispatch.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-tm-gnl.h"
#endif

#define KBUILD_MODNAME "iwlwifi"
#define DRV_DESCRIPTION "The new Intel(R) wireless AGN driver for Linux"

static const struct iwl_op_mode_ops iwl_mvm_ops;
static const struct iwl_op_mode_ops iwl_mvm_ops_mq;

struct iwl_mvm_mod_params iwlmvm_mod_params = {
    .power_scheme = IWL_POWER_SCHEME_BPS, .tfd_q_hang_detect = true
    /* rest of fields are 0 by default */
};

#if 0   // NEEDS_PORTING
module_param_named(init_dbg, iwlmvm_mod_params.init_dbg, bool, 0444);
MODULE_PARM_DESC(init_dbg, "set to true to debug an ASSERT in INIT fw (default: false");
module_param_named(power_scheme, iwlmvm_mod_params.power_scheme, int, 0444);
MODULE_PARM_DESC(power_scheme,
                 "power management scheme: 1-active, 2-balanced, 3-low power, default: 2");
module_param_named(tfd_q_hang_detect, iwlmvm_mod_params.tfd_q_hang_detect, bool, 0444);
MODULE_PARM_DESC(tfd_q_hang_detect, "TFD queues hang detection (default: true");
#endif  // NEEDS_PORTING

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
static void iwl_mvm_rx_fw_logs(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb) {
  iwl_dnt_dispatch_collect_ucode_message(mvm->trans, rxb);
}
#endif

/*
 * module init and exit functions
 */
zx_status_t iwl_mvm_init(void) {
  zx_status_t ret;

#if 0   // NEEDS_PORTING
    ret = iwl_mvm_rate_control_register();
    if (ret) {
        zxlogf(ERROR, "Unable to register rate control algorithm: %d", ret);
        return ret;
    }
#endif  // NEEDS_PORTING

  ret = iwl_opmode_register("iwlmvm", &iwl_mvm_ops);
  if (ret) {
    zxlogf(ERROR, "Unable to register MVM op_mode: %d", ret);
  }

  return ret;
}

void iwl_mvm_exit(void) {
  iwl_opmode_deregister("iwlmvm");
#if 0   // NEEDS_PORTING
    iwl_mvm_rate_control_unregister();
#endif  // NEEDS_PORTING
}

static void iwl_mvm_nic_config(struct iwl_op_mode* op_mode) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  uint8_t radio_cfg_type, radio_cfg_step, radio_cfg_dash;
  uint32_t reg_val = 0;
  uint32_t phy_config = iwl_mvm_get_phy_config(mvm);

  radio_cfg_type = (phy_config & FW_PHY_CFG_RADIO_TYPE) >> FW_PHY_CFG_RADIO_TYPE_POS;
  radio_cfg_step = (phy_config & FW_PHY_CFG_RADIO_STEP) >> FW_PHY_CFG_RADIO_STEP_POS;
  radio_cfg_dash = (phy_config & FW_PHY_CFG_RADIO_DASH) >> FW_PHY_CFG_RADIO_DASH_POS;

  /* SKU control */
	reg_val = CSR_HW_REV_STEP_DASH(mvm->trans->hw_rev);

  /* radio configuration */
  reg_val |= radio_cfg_type << CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
  reg_val |= radio_cfg_step << CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
  reg_val |= radio_cfg_dash << CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;

  WARN_ON((radio_cfg_type << CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE) &
          ~CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE);

  /*
   * TODO: Bits 7-8 of CSR in 8000 HW family and higher set the ADC
   * sampling, and shouldn't be set to any non-zero value.
   * The same is supposed to be true of the other HW, but unsetting
   * them (such as the 7260) causes automatic tests to fail on seemingly
   * unrelated errors. Need to further investigate this, but for now
   * we'll separate cases.
   */
  if (mvm->trans->trans_cfg->device_family < IWL_DEVICE_FAMILY_8000) {
    reg_val |= CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI;
  }

  if (iwl_fw_dbg_is_d3_debug_enabled(&mvm->fwrt)) {
    reg_val |= CSR_HW_IF_CONFIG_REG_D3_DEBUG;
  }

	iwl_trans_set_bits_mask(mvm->trans, CSR_HW_IF_CONFIG_REG,
				CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH |
				CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE |
				CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP |
				CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
				CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
				CSR_HW_IF_CONFIG_REG_BIT_MAC_SI   |
				CSR_HW_IF_CONFIG_REG_D3_DEBUG,
      reg_val);

  IWL_DEBUG_INFO(mvm, "Radio type=0x%x-0x%x-0x%x\n", radio_cfg_type, radio_cfg_step,
                 radio_cfg_dash);

#if 0   // NEEDS_PORTING
    /*
     * W/A : NIC is stuck in a reset state after Early PCIe power off
     * (PCIe power is lost before PERST# is asserted), causing ME FW
     * to lose ownership and not being able to obtain it back.
     */
    if (!mvm->trans->cfg->apmg_not_supported)
        iwl_set_bits_mask_prph(mvm->trans, APMG_PS_CTRL_REG, APMG_PS_CTRL_EARLY_PWR_OFF_RESET_DIS,
                               ~APMG_PS_CTRL_EARLY_PWR_OFF_RESET_DIS);
#endif  // NEEDS_PORTING
}

#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
/* A stub notification handler to receive Debug Host Notification (DHN).
 * The notification handler is empty because the report is processed by
 * trace-cmd and not by the driver
 */
static void iwl_mvm_rx_dhn(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb) {
  struct iwl_rx_packet* pkt = rxb_addr(rxb);
  struct iwl_dhn_hdr* notif = (void*)pkt->data;

  uint32_t length = le32_to_cpu(notif->length);
  uint32_t index_and_mask = le32_to_cpu(notif->index_and_mask);

  IWL_DEBUG_INFO(mvm,
                 "Received Debug Host Notifcation:\n"
                 "length: %u\n"
                 "index and mask: 0x%x\n",
                 length, index_and_mask);
}
#endif /* CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED */

/**
 * enum iwl_rx_handler_context context for Rx handler
 * @RX_HANDLER_SYNC : this means that it will be called in the Rx path
 *  which can't acquire mvm->mutex.
 * @RX_HANDLER_ASYNC_LOCKED : If the handler needs to hold mvm->mutex
 *  (and only in this case!), it should be set as ASYNC. In that case,
 *  it will be called from a worker with mvm->mutex held.
 * @RX_HANDLER_ASYNC_UNLOCKED : in case the handler needs to lock the
 *  mutex itself, it will be called from a worker without mvm->mutex held.
 */
enum iwl_rx_handler_context {
  RX_HANDLER_SYNC,
  RX_HANDLER_ASYNC_LOCKED,
  RX_HANDLER_ASYNC_UNLOCKED,
};

/**
 * struct iwl_rx_handlers handler for FW notification
 * @cmd_id: command id
 * @context: see &iwl_rx_handler_context
 * @fn: the function is called when notification is received
 */
struct iwl_rx_handlers {
  uint16_t cmd_id;
  enum iwl_rx_handler_context context;
  void (*fn)(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb);
};

#define RX_HANDLER(_cmd_id, _fn, _context) \
  { .cmd_id = _cmd_id, .fn = _fn, .context = _context }
#define RX_HANDLER_GRP(_grp, _cmd, _fn, _context) \
  { .cmd_id = WIDE_ID(_grp, _cmd), .fn = _fn, .context = _context }

/*
 * Handlers for fw notifications
 * Convention: RX_HANDLER(CMD_NAME, iwl_mvm_rx_CMD_NAME
 * This list should be in order of frequency for performance purposes.
 *
 * The handler can be one from three contexts, see &iwl_rx_handler_context
 */
static const struct iwl_rx_handlers iwl_mvm_rx_handlers[] = {
    RX_HANDLER(TX_CMD, iwl_mvm_rx_tx_cmd, RX_HANDLER_SYNC),
    RX_HANDLER(BA_NOTIF, iwl_mvm_rx_ba_notif, RX_HANDLER_SYNC),

#if 0   // NEEDS_PORTING
    RX_HANDLER_GRP(DATA_PATH_GROUP, TLC_MNG_UPDATE_NOTIF, iwl_mvm_tlc_update_notif,
                   RX_HANDLER_SYNC),

    RX_HANDLER(BT_PROFILE_NOTIFICATION, iwl_mvm_rx_bt_coex_notif, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER(BEACON_NOTIFICATION, iwl_mvm_rx_beacon_notif, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER(STATISTICS_NOTIFICATION, iwl_mvm_rx_statistics, RX_HANDLER_ASYNC_LOCKED),

    RX_HANDLER(BA_WINDOW_STATUS_NOTIFICATION_ID, iwl_mvm_window_status_notif, RX_HANDLER_SYNC),
#endif  // NEEDS_PORTING

    RX_HANDLER(TIME_EVENT_NOTIFICATION, iwl_mvm_rx_time_event_notif, RX_HANDLER_SYNC),

    RX_HANDLER(MCC_CHUB_UPDATE_CMD, iwl_mvm_rx_chub_update_mcc, RX_HANDLER_ASYNC_LOCKED),

#if 0   // NEEDS_PORTING
    RX_HANDLER(EOSP_NOTIFICATION, iwl_mvm_rx_eosp_notif, RX_HANDLER_SYNC),

    RX_HANDLER(SCAN_ITERATION_COMPLETE, iwl_mvm_rx_lmac_scan_iter_complete_notif, RX_HANDLER_SYNC),
#endif  // NEEDS_PORTING
    RX_HANDLER(SCAN_OFFLOAD_COMPLETE, iwl_mvm_rx_lmac_scan_complete_notif, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER(SCAN_COMPLETE_UMAC, iwl_mvm_rx_umac_scan_complete_notif, RX_HANDLER_ASYNC_LOCKED),
#if 0  // NEEDS_PORTING
    RX_HANDLER(MATCH_FOUND_NOTIFICATION, iwl_mvm_rx_scan_match_found, RX_HANDLER_SYNC),
    RX_HANDLER(SCAN_ITERATION_COMPLETE_UMAC, iwl_mvm_rx_umac_scan_iter_complete_notif,
               RX_HANDLER_SYNC),

    RX_HANDLER(CARD_STATE_NOTIFICATION, iwl_mvm_rx_card_state_notif, RX_HANDLER_SYNC),

    RX_HANDLER(MISSED_BEACONS_NOTIFICATION, iwl_mvm_rx_missed_beacons_notif, RX_HANDLER_SYNC),

    RX_HANDLER(REPLY_ERROR, iwl_mvm_rx_fw_error, RX_HANDLER_SYNC),
    RX_HANDLER(PSM_UAPSD_AP_MISBEHAVING_NOTIFICATION, iwl_mvm_power_uapsd_misbehaving_ap_notif,
               RX_HANDLER_SYNC),
    RX_HANDLER(DTS_MEASUREMENT_NOTIFICATION, iwl_mvm_temp_notif, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER_GRP(PHY_OPS_GROUP, DTS_MEASUREMENT_NOTIF_WIDE, iwl_mvm_temp_notif,
                   RX_HANDLER_ASYNC_UNLOCKED),
    RX_HANDLER_GRP(PHY_OPS_GROUP, CT_KILL_NOTIFICATION, iwl_mvm_ct_kill_notif, RX_HANDLER_SYNC),

    RX_HANDLER(TDLS_CHANNEL_SWITCH_NOTIFICATION, iwl_mvm_rx_tdls_notif, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER(MFUART_LOAD_NOTIFICATION, iwl_mvm_rx_mfuart_notif, RX_HANDLER_SYNC),
    RX_HANDLER(TOF_NOTIFICATION, iwl_mvm_tof_resp_handler, RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER_GRP(DEBUG_GROUP, MFU_ASSERT_DUMP_NTF, iwl_mvm_mfu_assert_dump_notif,
                   RX_HANDLER_SYNC),
    RX_HANDLER_GRP(PROT_OFFLOAD_GROUP, STORED_BEACON_NTF, iwl_mvm_rx_stored_beacon_notif,
                   RX_HANDLER_SYNC),
    RX_HANDLER_GRP(DATA_PATH_GROUP, MU_GROUP_MGMT_NOTIF, iwl_mvm_mu_mimo_grp_notif,
                   RX_HANDLER_SYNC),
    RX_HANDLER_GRP(DATA_PATH_GROUP, STA_PM_NOTIF, iwl_mvm_sta_pm_notif, RX_HANDLER_SYNC),
#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
    RX_HANDLER_GRP(NAN_GROUP, NAN_DISCOVERY_TERMINATE_NOTIF, iwl_mvm_nan_de_term_notif,
                   RX_HANDLER_SYNC),
    RX_HANDLER_GRP(NAN_GROUP, NAN_DISCOVERY_EVENT_NOTIF, iwl_mvm_nan_match, RX_HANDLER_SYNC),
    RX_HANDLER_GRP(MAC_CONF_GROUP, PROBE_RESPONSE_DATA_NOTIF, iwl_mvm_probe_resp_data_notif,
                   RX_HANDLER_ASYNC_LOCKED),
    RX_HANDLER_GRP(MAC_CONF_GROUP, CHANNEL_SWITCH_NOA_NOTIF, iwl_mvm_channel_switch_noa_notif,
                   RX_HANDLER_SYNC),

#endif

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
    RX_HANDLER(DEBUG_LOG_MSG, iwl_mvm_rx_fw_logs, RX_HANDLER_SYNC),
#endif
#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
    RX_HANDLER_GRP(DEBUG_GROUP, DEBUG_HOST_NTF, iwl_mvm_rx_dhn, RX_HANDLER_SYNC),
#endif
#endif  // NEEDS_PORTING
};
#undef RX_HANDLER
#undef RX_HANDLER_GRP

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_legacy_names[] = {
    HCMD_NAME(MVM_ALIVE),
    HCMD_NAME(REPLY_ERROR),
    HCMD_NAME(ECHO_CMD),
    HCMD_NAME(INIT_COMPLETE_NOTIF),
    HCMD_NAME(PHY_CONTEXT_CMD),
    HCMD_NAME(DBG_CFG),
    HCMD_NAME(SCAN_CFG_CMD),
    HCMD_NAME(SCAN_REQ_UMAC),
    HCMD_NAME(SCAN_ABORT_UMAC),
    HCMD_NAME(SCAN_COMPLETE_UMAC),
    HCMD_NAME(TOF_CMD),
    HCMD_NAME(TOF_NOTIFICATION),
    HCMD_NAME(BA_WINDOW_STATUS_NOTIFICATION_ID),
    HCMD_NAME(ADD_STA_KEY),
    HCMD_NAME(ADD_STA),
    HCMD_NAME(REMOVE_STA),
    HCMD_NAME(FW_GET_ITEM_CMD),
    HCMD_NAME(TX_CMD),
    HCMD_NAME(SCD_QUEUE_CFG),
    HCMD_NAME(TXPATH_FLUSH),
    HCMD_NAME(MGMT_MCAST_KEY),
    HCMD_NAME(WEP_KEY),
    HCMD_NAME(SHARED_MEM_CFG),
    HCMD_NAME(TDLS_CHANNEL_SWITCH_CMD),
    HCMD_NAME(MAC_CONTEXT_CMD),
    HCMD_NAME(TIME_EVENT_CMD),
    HCMD_NAME(TIME_EVENT_NOTIFICATION),
    HCMD_NAME(BINDING_CONTEXT_CMD),
    HCMD_NAME(TIME_QUOTA_CMD),
    HCMD_NAME(NON_QOS_TX_COUNTER_CMD),
    HCMD_NAME(LEDS_CMD),
#ifdef CPTCFG_IWLWIFI_LTE_COEX
    HCMD_NAME(LTE_COEX_CONFIG_CMD),
    HCMD_NAME(LTE_COEX_WIFI_REPORTED_CHANNEL_CMD),
    HCMD_NAME(LTE_COEX_STATIC_PARAMS_CMD),
    HCMD_NAME(LTE_COEX_SPS_CMD),
#endif
    HCMD_NAME(LQ_CMD),
    HCMD_NAME(FW_PAGING_BLOCK_CMD),
    HCMD_NAME(SCAN_OFFLOAD_REQUEST_CMD),
    HCMD_NAME(SCAN_OFFLOAD_ABORT_CMD),
    HCMD_NAME(HOT_SPOT_CMD),
    HCMD_NAME(SCAN_OFFLOAD_PROFILES_QUERY_CMD),
#ifdef CPTCFG_IWLWIFI_LTE_COEX
    HCMD_NAME(LTE_COEX_FINE_TUNING_PARAMS_CMD),
#endif
    HCMD_NAME(BT_COEX_UPDATE_REDUCED_TXP),
    HCMD_NAME(BT_COEX_CI),
    HCMD_NAME(PHY_CONFIGURATION_CMD),
    HCMD_NAME(CALIB_RES_NOTIF_PHY_DB),
    HCMD_NAME(PHY_DB_CMD),
    HCMD_NAME(SCAN_OFFLOAD_COMPLETE),
    HCMD_NAME(SCAN_OFFLOAD_UPDATE_PROFILES_CMD),
    HCMD_NAME(CONFIG_2G_COEX_CMD),
    HCMD_NAME(POWER_TABLE_CMD),
    HCMD_NAME(PSM_UAPSD_AP_MISBEHAVING_NOTIFICATION),
    HCMD_NAME(REPLY_THERMAL_MNG_BACKOFF),
    HCMD_NAME(DC2DC_CONFIG_CMD),
    HCMD_NAME(NVM_ACCESS_CMD),
    HCMD_NAME(BEACON_NOTIFICATION),
    HCMD_NAME(BEACON_TEMPLATE_CMD),
    HCMD_NAME(TX_ANT_CONFIGURATION_CMD),
    HCMD_NAME(BT_CONFIG),
    HCMD_NAME(STATISTICS_CMD),
    HCMD_NAME(STATISTICS_NOTIFICATION),
    HCMD_NAME(EOSP_NOTIFICATION),
    HCMD_NAME(REDUCE_TX_POWER_CMD),
    HCMD_NAME(CARD_STATE_NOTIFICATION),
    HCMD_NAME(MISSED_BEACONS_NOTIFICATION),
    HCMD_NAME(TDLS_CONFIG_CMD),
    HCMD_NAME(MAC_PM_POWER_TABLE),
    HCMD_NAME(TDLS_CHANNEL_SWITCH_NOTIFICATION),
    HCMD_NAME(MFUART_LOAD_NOTIFICATION),
    HCMD_NAME(RSS_CONFIG_CMD),
    HCMD_NAME(SCAN_ITERATION_COMPLETE_UMAC),
    HCMD_NAME(REPLY_RX_PHY_CMD),
    HCMD_NAME(REPLY_RX_MPDU_CMD),
    HCMD_NAME(FRAME_RELEASE),
    HCMD_NAME(BA_NOTIF),
    HCMD_NAME(MCC_UPDATE_CMD),
    HCMD_NAME(MCC_CHUB_UPDATE_CMD),
    HCMD_NAME(MARKER_CMD),
    HCMD_NAME(BT_PROFILE_NOTIFICATION),
    HCMD_NAME(BCAST_FILTER_CMD),
    HCMD_NAME(MCAST_FILTER_CMD),
    HCMD_NAME(REPLY_SF_CFG_CMD),
    HCMD_NAME(REPLY_BEACON_FILTERING_CMD),
    HCMD_NAME(D3_CONFIG_CMD),
    HCMD_NAME(PROT_OFFLOAD_CONFIG_CMD),
    HCMD_NAME(OFFLOADS_QUERY_CMD),
    HCMD_NAME(REMOTE_WAKE_CONFIG_CMD),
    HCMD_NAME(MATCH_FOUND_NOTIFICATION),
    HCMD_NAME(DTS_MEASUREMENT_NOTIFICATION),
    HCMD_NAME(WOWLAN_PATTERNS),
    HCMD_NAME(WOWLAN_CONFIGURATION),
    HCMD_NAME(WOWLAN_TSC_RSC_PARAM),
    HCMD_NAME(WOWLAN_TKIP_PARAM),
    HCMD_NAME(WOWLAN_KEK_KCK_MATERIAL),
    HCMD_NAME(WOWLAN_GET_STATUSES),
    HCMD_NAME(SCAN_ITERATION_COMPLETE),
    HCMD_NAME(D0I3_END_CMD),
    HCMD_NAME(LTR_CONFIG),
    HCMD_NAME(DEBUG_LOG_MSG),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_system_names[] = {
    HCMD_NAME(SHARED_MEM_CFG_CMD),
    HCMD_NAME(SOC_CONFIGURATION_CMD),
    HCMD_NAME(INIT_EXTENDED_CFG_CMD),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_mac_conf_names[] = {
    HCMD_NAME(CHANNEL_SWITCH_NOA_NOTIF),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_phy_names[] = {
    HCMD_NAME(CMD_DTS_MEASUREMENT_TRIGGER_WIDE),
    HCMD_NAME(CTDP_CONFIG_CMD),
    HCMD_NAME(TEMP_REPORTING_THRESHOLDS_CMD),
    HCMD_NAME(GEO_TX_POWER_LIMIT),
    HCMD_NAME(CT_KILL_NOTIFICATION),
    HCMD_NAME(DTS_MEASUREMENT_NOTIF_WIDE),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_data_path_names[] = {
    HCMD_NAME(DQA_ENABLE_CMD),
    HCMD_NAME(UPDATE_MU_GROUPS_CMD),
    HCMD_NAME(TRIGGER_RX_QUEUES_NOTIF_CMD),
    HCMD_NAME(STA_HE_CTXT_CMD),
#ifdef CPTCFG_IWLMVM_AX_SOFTAP_TESTMODE
    HCMD_NAME(AX_SOFTAP_TESTMODE_DL_BASIC),
    HCMD_NAME(AX_SOFTAP_TESTMODE_DL_MU_BAR),
    HCMD_NAME(AX_SOFTAP_TESTMODE_UL),
    HCMD_NAME(AX_SOFTAP_CLIENT_TESTMODE),
#endif
    HCMD_NAME(RFH_QUEUE_CONFIG_CMD),
    HCMD_NAME(STA_PM_NOTIF),
    HCMD_NAME(MU_GROUP_MGMT_NOTIF),
    HCMD_NAME(RX_QUEUES_NOTIFICATION),
};

#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_debug_names[] = {
#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
    HCMD_NAME(DEBUG_HOST_NTF),
#endif
    HCMD_NAME(MFU_ASSERT_DUMP_NTF),
};
#endif

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_nan_names[] = {
    HCMD_NAME(NAN_CONFIG_CMD),
    HCMD_NAME(NAN_DISCOVERY_FUNC_CMD),
    HCMD_NAME(NAN_FAW_CONFIG_CMD),
    HCMD_NAME(NAN_DISCOVERY_EVENT_NOTIF),
    HCMD_NAME(NAN_DISCOVERY_TERMINATE_NOTIF),
    HCMD_NAME(NAN_FAW_START_NOTIF),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_prot_offload_names[] = {
    HCMD_NAME(STORED_BEACON_NTF),
};

/* Please keep this array *SORTED* by hex value.
 * Access is done through binary search
 */
static const struct iwl_hcmd_names iwl_mvm_regulatory_and_nvm_names[] = {
    HCMD_NAME(NVM_ACCESS_COMPLETE),
    HCMD_NAME(NVM_GET_INFO),
};

static const struct iwl_hcmd_arr iwl_mvm_groups[] = {
    [LEGACY_GROUP] = HCMD_ARR(iwl_mvm_legacy_names),
    [LONG_GROUP] = HCMD_ARR(iwl_mvm_legacy_names),
    [SYSTEM_GROUP] = HCMD_ARR(iwl_mvm_system_names),
    [MAC_CONF_GROUP] = HCMD_ARR(iwl_mvm_mac_conf_names),
    [PHY_OPS_GROUP] = HCMD_ARR(iwl_mvm_phy_names),
    [DATA_PATH_GROUP] = HCMD_ARR(iwl_mvm_data_path_names),
    [NAN_GROUP] = HCMD_ARR(iwl_mvm_nan_names),
    [PROT_OFFLOAD_GROUP] = HCMD_ARR(iwl_mvm_prot_offload_names),
    [REGULATORY_AND_NVM_GROUP] = HCMD_ARR(iwl_mvm_regulatory_and_nvm_names),
#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
    [DEBUG_GROUP] = HCMD_ARR(iwl_mvm_debug_names),
#endif
};

/* this forward declaration can avoid to export the function */
static void iwl_mvm_async_handlers_wk(void* data);
#ifdef CONFIG_PM
static void iwl_mvm_d0i3_exit_work(struct work_struct* wk);
#endif

static uint32_t iwl_mvm_min_backoff(struct iwl_mvm* mvm) {
  const struct iwl_pwr_tx_backoff* backoff = mvm->cfg->pwr_tx_backoffs;
  uint64_t dflt_pwr_limit;

  if (!backoff) {
    return 0;
  }

  dflt_pwr_limit = iwl_acpi_get_pwr_limit(mvm->dev);

  while (backoff->pwr) {
    if (dflt_pwr_limit >= backoff->pwr) {
      return backoff->backoff;
    }

    backoff++;
  }

  return 0;
}

#if 0   // NEEDS_PORTING
static void iwl_mvm_tx_unblock_dwork(struct work_struct* work) {
    struct iwl_mvm* mvm = container_of(work, struct iwl_mvm, cs_tx_unblock_dwork.work);
    struct ieee80211_vif* tx_blocked_vif;
    struct iwl_mvm_vif* mvmvif;

    mtx_lock(&mvm->mutex);

    tx_blocked_vif =
        rcu_dereference_protected(mvm->csa_tx_blocked_vif, lockdep_is_held(&mvm->mutex));

    if (!tx_blocked_vif) { goto unlock; }

    mvmvif = iwl_mvm_vif_from_mac80211(tx_blocked_vif);
    iwl_mvm_modify_all_sta_disable_tx(mvm, mvmvif, false);
	RCU_INIT_POINTER(mvm->csa_tx_blocked_vif, NULL);
unlock:
    mtx_unlock(&mvm->mutex);
}
#endif  // NEEDS_PORTING

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
static void iwl_mvm_init_modparams(struct iwl_mvm* mvm) {
#define IWL_DBG_CFG(t, n)                 /* nothing */
#define IWL_DBG_CFG_STR(n)                /* nothing */
#define IWL_DBG_CFG_NODEF(t, n)           /* nothing */
#define IWL_DBG_CFG_BIN(n)                /* nothing */
#define IWL_DBG_CFG_BINA(n, max)          /* nothing */
#define IWL_DBG_CFG_RANGE(t, n, min, max) /* nothing */
#define IWL_MOD_PARAM(t, n)               /* nothing */
#define IWL_MVM_MOD_PARAM(t, n)                \
  if (mvm->trans->dbg_cfg.__mvm_mod_param_##n) \
    iwlmvm_mod_params.n = mvm->trans->dbg_cfg.mvm_##n;
#define DBG_CFG_REINCLUDE
#include "iwl-dbg-cfg.h"
#undef IWL_DBG_CFG
#undef IWL_DBG_CFG_STR
#undef IWL_DBG_CFG_NODEF
#undef IWL_DBG_CFG_BIN
#undef IWL_DBG_CFG_BINA
#undef IWL_DBG_CFG_RANGE
#undef IWL_MOD_PARAM
#undef IWL_MVM_MOD_PARAM
}
#endif

static int iwl_mvm_fwrt_dump_start(void* ctx) __TA_NO_THREAD_SAFETY_ANALYSIS {
  struct iwl_mvm* mvm = ctx;
  int ret;

  ret = iwl_mvm_ref_sync(mvm, IWL_MVM_REF_FW_DBG_COLLECT);
  if (ret) {
    return ret;
  }

  mtx_lock(&mvm->mutex);

  return 0;
}

static void iwl_mvm_fwrt_dump_end(void* ctx) __TA_NO_THREAD_SAFETY_ANALYSIS {
  struct iwl_mvm* mvm = ctx;

  mtx_unlock(&mvm->mutex);

  iwl_mvm_unref(mvm, IWL_MVM_REF_FW_DBG_COLLECT);
}

static bool iwl_mvm_fwrt_fw_running(void* ctx) { return iwl_mvm_firmware_running(ctx); }

static zx_status_t iwl_mvm_fwrt_send_hcmd(void* ctx, struct iwl_host_cmd* host_cmd) {
  return ZX_ERR_NOT_SUPPORTED;
#if 0   // NEEDS_PORTING
    struct iwl_mvm* mvm = (struct iwl_mvm*)ctx;
    int ret;

    mtx_lock(&mvm->mutex);
    ret = iwl_mvm_send_cmd(mvm, host_cmd);
    mtx_unlock(&mvm->mutex);

    return ret;
#endif  // NEEDS_PORTING
}

static const struct iwl_fw_runtime_ops iwl_mvm_fwrt_ops = {
    .dump_start = iwl_mvm_fwrt_dump_start,
    .dump_end = iwl_mvm_fwrt_dump_end,
    .fw_running = iwl_mvm_fwrt_fw_running,
    .send_hcmd = iwl_mvm_fwrt_send_hcmd,
};

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
static int iwl_mvm_tm_send_hcmd(void* op_mode, struct iwl_host_cmd* host_cmd) {
  struct iwl_mvm* mvm = (struct iwl_mvm*)op_mode;

  if (WARN_ON_ONCE(!op_mode)) {
    return -EINVAL;
  }

  return iwl_mvm_send_cmd(mvm, host_cmd);
}

static int iwl_mvm_tm_cmd_exec_start(struct iwl_testmode* testmode) {
  struct iwl_mvm* mvm = testmode->op_mode;

  return iwl_mvm_ref_sync(mvm, IWL_MVM_REF_TM_CMD);
}

static void iwl_mvm_tm_cmd_exec_end(struct iwl_testmode* testmode) {
  struct iwl_mvm* mvm = testmode->op_mode;

  iwl_mvm_unref(mvm, IWL_MVM_REF_TM_CMD);
}
#endif

static struct iwl_op_mode* iwl_op_mode_mvm_start(struct iwl_trans* trans, const struct iwl_cfg* cfg,
                                                 const struct iwl_fw* fw,
                                                 struct dentry* dbgfs_dir) {
  struct iwl_op_mode* op_mode;
  struct iwl_mvm* mvm;
  struct iwl_trans_config trans_cfg = {};
  static const uint8_t no_reclaim_cmds[] = {
      TX_CMD,
  };
  int err, scan_size;
  uint32_t min_backoff;

  /*
   * We use IWL_MVM_STATION_COUNT to check the validity of the station
   * index all over the driver - check that its value corresponds to the
   * array size.
   */
  BUILD_BUG_ON(ARRAY_SIZE(mvm->fw_id_to_mac_id) != IWL_MVM_STATION_COUNT);

  // Allocate op_mode and mvm. In the Linux driver these are private members of a ieee80211_hw
  // struct |hw|, which also contains iwl_mvm_hw_ops. The equivalent functionality of ieee80211_hw
  // has not yet been ported.
  op_mode = calloc(1, sizeof(struct iwl_op_mode));
  if (!op_mode) {
    IWL_ERR(trans, "Failed to allocate op mode\n");
  }
  mvm = calloc(1, sizeof(struct iwl_mvm));
  if (!mvm) {
    IWL_ERR(trans, "Failed to allocate mvm\n");
  }

  op_mode->op_mode_specific = mvm;

#if 0   // NEEDS_PORTING
    if (cfg->max_rx_agg_size) {
        hw->max_rx_aggregation_subframes = cfg->max_rx_agg_size;
    } else {
        hw->max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF;
    }

    if (cfg->max_tx_agg_size) {
        hw->max_tx_aggregation_subframes = cfg->max_tx_agg_size;
    } else {
        hw->max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF;
    }
#endif  // NEEDS_PORTING

  mvm->dev = trans->dev;
  mvm->trans = trans;
  mvm->cfg = cfg;
  mvm->fw = fw;

  iwl_fw_runtime_init(&mvm->fwrt, trans, fw, &iwl_mvm_fwrt_ops, mvm, dbgfs_dir);

  mvm->init_status = 0;

  if (iwl_mvm_has_new_rx_api(mvm)) {
    op_mode->ops = &iwl_mvm_ops_mq;
    trans->rx_mpdu_cmd_hdr_size = (trans->trans_cfg->device_family >= IWL_DEVICE_FAMILY_AX210)
                                      ? sizeof(struct iwl_rx_mpdu_desc)
                                      : IWL_RX_DESC_SIZE_V1;
  } else {
    op_mode->ops = &iwl_mvm_ops;
    trans->rx_mpdu_cmd_hdr_size = sizeof(struct iwl_rx_mpdu_res_start);

    if (WARN_ON(trans->num_rx_queues > 1)) {
      goto out_free;
    }
  }

  mvm->fw_restart = iwlwifi_mod_params.fw_restart ? -1 : 0;

  mvm->aux_queue = IWL_MVM_DQA_AUX_QUEUE;
  mvm->snif_queue = IWL_MVM_DQA_INJECT_MONITOR_QUEUE;
  mvm->probe_queue = IWL_MVM_DQA_AP_PROBE_RESP_QUEUE;
  mvm->p2p_dev_queue = IWL_MVM_DQA_P2P_DEVICE_QUEUE;

  mvm->sf_state = SF_UNINIT;
  if (iwl_mvm_has_unified_ucode(mvm)) {
    iwl_fw_set_current_image(&mvm->fwrt, IWL_UCODE_REGULAR);
  } else {
    iwl_fw_set_current_image(&mvm->fwrt, IWL_UCODE_INIT);
  }
  mvm->drop_bcn_ap_mode = true;

  mtx_init(&mvm->mutex, mtx_plain);
  mtx_init(&mvm->d0i3_suspend_mutex, mtx_plain);
  mtx_init(&mvm->async_handlers_lock, mtx_plain);
  list_initialize(&mvm->time_event_list);
  list_initialize(&mvm->aux_roc_te_list);
  list_initialize(&mvm->async_handlers_list);
  mtx_init(&mvm->time_event_lock, mtx_plain);

  iwl_task_create(mvm->dev, iwl_mvm_async_handlers_wk, mvm, &mvm->async_handlers_wk);
#if 0  // NEEDS_PORTING
    INIT_WORK(&mvm->roc_done_wk, iwl_mvm_roc_done_wk);
#ifdef CONFIG_PM
    INIT_WORK(&mvm->d0i3_exit_work, iwl_mvm_d0i3_exit_work);
#endif
#endif  // NEEDS_PORTING

  iwl_task_create(mvm->dev, iwl_mvm_scan_timeout_wk, mvm, &mvm->scan_timeout_task);

#if 0   // NEEDS_PORTING
    INIT_DELAYED_WORK(&mvm->tdls_cs.dwork, iwl_mvm_tdls_ch_switch_work);
    INIT_WORK(&mvm->add_stream_wk, iwl_mvm_add_new_dqa_stream_wk);
#endif  // NEEDS_PORTING

  list_initialize(&mvm->add_stream_txqs);

  mtx_init(&mvm->d0i3_tx_lock, mtx_plain);
  mtx_init(&mvm->refs_lock, mtx_plain);
#if 0   // NEEDS_PORTING
    skb_queue_head_init(&mvm->d0i3_tx);
    init_waitqueue_head(&mvm->d0i3_exit_waitq);
    init_waitqueue_head(&mvm->rx_sync_waitq);
#endif  // NEEDS_PORTING

  atomic_set(&mvm->queue_sync_counter, 0);

#if 0   // NEEDS_PORTING
    SET_IEEE80211_DEV(mvm->hw, mvm->trans->dev);
#endif  // NEEDS_PORTING

  mtx_init(&mvm->tcm.lock, mtx_plain);
#if 0   // NEEDS_PORTING
    INIT_DELAYED_WORK(&mvm->tcm.work, iwl_mvm_tcm_work);
    mvm->tcm.ts = jiffies;
    mvm->tcm.ll_ts = jiffies;
    mvm->tcm.uapsd_nonagg_ts = jiffies;

    INIT_DELAYED_WORK(&mvm->cs_tx_unblock_dwork, iwl_mvm_tx_unblock_dwork);
#endif  // NEEDS_PORTING

  /*
   * Populate the state variables that the transport layer needs
   * to know about.
   */
  trans_cfg.op_mode = op_mode;
  trans_cfg.no_reclaim_cmds = no_reclaim_cmds;
  trans_cfg.n_no_reclaim_cmds = ARRAY_SIZE(no_reclaim_cmds);

  switch (iwlwifi_mod_params.amsdu_size) {
    case IWL_AMSDU_DEF:
      trans_cfg.rx_buf_size = IWL_AMSDU_4K;
      break;
    case IWL_AMSDU_4K:
      trans_cfg.rx_buf_size = IWL_AMSDU_4K;
      break;
    case IWL_AMSDU_8K:
      trans_cfg.rx_buf_size = IWL_AMSDU_8K;
      break;
    case IWL_AMSDU_12K:
      trans_cfg.rx_buf_size = IWL_AMSDU_12K;
      break;
    default:
      zxlogf(ERROR, "%s: Unsupported amsdu_size: %d", KBUILD_MODNAME,
             iwlwifi_mod_params.amsdu_size);
		trans_cfg.rx_buf_size = IWL_AMSDU_4K;
  }

  trans->wide_cmd_header = true;
  trans_cfg.bc_table_dword = mvm->trans->trans_cfg->device_family < IWL_DEVICE_FAMILY_AX210;

  trans_cfg.command_groups = iwl_mvm_groups;
  trans_cfg.command_groups_size = ARRAY_SIZE(iwl_mvm_groups);

  trans_cfg.cmd_queue = IWL_MVM_DQA_CMD_QUEUE;
  trans_cfg.cmd_fifo = IWL_MVM_TX_FIFO_CMD;
  trans_cfg.scd_set_active = true;

#if 0   // NEEDS_PORTING
    trans_cfg.cb_data_offs = offsetof(struct ieee80211_tx_info, driver_data[2]);
#endif  // NEEDS_PORTING

  trans_cfg.sw_csum_tx = IWL_MVM_SW_TX_CSUM_OFFLOAD;

#if 0   // NEEDS_PORTING
    /* Set a short watchdog for the command queue */
    trans_cfg.cmd_q_wdg_timeout = iwl_mvm_get_wd_timeout(mvm, NULL, false, true);
#endif  // NEEDS_PORTING

#if 0   // NEEDS_PORTING
    snprintf(mvm->hw->wiphy->fw_version, sizeof(mvm->hw->wiphy->fw_version), "%s", fw->fw_version);
#endif  // NEEDS_PORTING

  /* Configure transport layer */
  iwl_trans_configure(mvm->trans, &trans_cfg);

  trans->rx_mpdu_cmd = REPLY_RX_MPDU_CMD;
  trans->dbg_dest_tlv = mvm->fw->dbg.dest_tlv;
  trans->dbg_n_dest_reg = mvm->fw->dbg.n_dest_reg;
  memcpy(trans->dbg_conf_tlv, mvm->fw->dbg.conf_tlv, sizeof(trans->dbg_conf_tlv));
  trans->dbg_trigger_tlv = mvm->fw->dbg.trigger_tlv;

  trans->iml = mvm->fw->iml;
  trans->iml_len = mvm->fw->iml_len;

  /* set up notification wait support */
  iwl_notification_wait_init(&mvm->notif_wait);

  /* Init phy db */
  mvm->phy_db = iwl_phy_db_init(trans);
  if (!mvm->phy_db) {
    IWL_ERR(mvm, "Cannot init phy_db\n");
    goto out_free;
  }

  IWL_INFO(mvm, "Detected %s, REV=0x%X\n", mvm->cfg->name, mvm->trans->hw_rev);

  if (iwlwifi_mod_params.nvm_file) {
    mvm->nvm_file_name = iwlwifi_mod_params.nvm_file;
  }
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
  else if (trans->dbg_cfg.nvm_file) {
    mvm->nvm_file_name = trans->dbg_cfg.nvm_file;
  }
#endif
  else {
    IWL_DEBUG_EEPROM(mvm->trans->dev, "working without external nvm file%s\n", "");
  }

  err = iwl_trans_start_hw(mvm->trans);
  if (err) {
    goto out_free;
  }

  mtx_lock(&mvm->mutex);
  iwl_mvm_ref(mvm, IWL_MVM_REF_INIT_UCODE);
  err = iwl_run_init_mvm_ucode(mvm, true);

  if (!iwlmvm_mod_params.init_dbg || !err) {
    iwl_mvm_stop_device(mvm);
  }
  iwl_mvm_unref(mvm, IWL_MVM_REF_INIT_UCODE);
  mtx_unlock(&mvm->mutex);
  if (err < 0) {
    IWL_ERR(mvm, "Failed to run INIT ucode: %d\n", err);
    goto out_free;
  }

  scan_size = iwl_mvm_scan_size(mvm);

  mvm->scan_cmd = malloc(scan_size);
  if (!mvm->scan_cmd) {
    goto out_free;
  }

  /* Set EBS as successful as long as not stated otherwise by the FW. */
  mvm->last_ebs_successful = true;

  err = iwl_mvm_mac_setup_register(mvm);
  if (err) {
    goto out_free;
  }
  mvm->hw_registered = true;

  min_backoff = iwl_mvm_min_backoff(mvm);
  iwl_mvm_thermal_initialize(mvm, min_backoff);

  err = iwl_mvm_dbgfs_register(mvm, dbgfs_dir);
  if (err) {
    goto out_unregister;
  }

  if (!iwl_mvm_has_new_rx_stats_api(mvm)) {
    memset(&mvm->rx_stats_v3, 0, sizeof(struct mvm_statistics_rx_v3));
  } else {
    memset(&mvm->rx_stats, 0, sizeof(struct mvm_statistics_rx));
  }

  /* The transport always starts with a taken reference, we can
   * release it now if d0i3 is supported */
  if (iwl_mvm_is_d0i3_supported(mvm)) {
    iwl_trans_unref(mvm->trans);
  }

  iwl_mvm_tof_init(mvm);

  iwl_mvm_toggle_tx_ant(mvm, &mvm->mgmt_last_antenna_idx);
  return op_mode;

out_unregister:
  if (iwlmvm_mod_params.init_dbg) {
    return op_mode;
  }

#if 0   // NEEDS_PORTING
    ieee80211_unregister_hw(mvm->hw);
#endif  // NEEDS_PORTING
  mvm->hw_registered = false;
  iwl_mvm_leds_exit(mvm);
  iwl_mvm_thermal_exit(mvm);
out_free:
  iwl_fw_flush_dump(&mvm->fwrt);
  iwl_fw_runtime_free(&mvm->fwrt);

  if (iwlmvm_mod_params.init_dbg) {
    return op_mode;
  }

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
  iwl_dnt_free(trans);
#endif
  iwl_phy_db_free(mvm->phy_db);
  kfree(mvm->scan_cmd);
  iwl_trans_op_mode_leave(trans);

  free(op_mode);
  free(mvm);
  return NULL;
}

static void iwl_op_mode_mvm_stop(struct iwl_op_mode* op_mode) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  int i;

  /* If d0i3 is supported, we have released the reference that
   * the transport started with, so we should take it back now
   * that we are leaving.
   */
  if (iwl_mvm_is_d0i3_supported(mvm)) {
    iwl_trans_ref(mvm->trans);
  }

  iwl_mvm_leds_exit(mvm);

  iwl_mvm_thermal_exit(mvm);

  if (mvm->init_status & IWL_MVM_INIT_STATUS_REG_HW_INIT_COMPLETE) {
#if 0   // NEEDS_PORTING
	/*
	 * If we couldn't get ownership on the device and we couldn't
	 * get the NVM from CSME, we haven't registered to mac80211.
	 * In that case, we didn't fail op_mode_start, because we are
	 * waiting for CSME to allow us to get the NVM to register to
	 * mac80211. If that didn't happen, we haven't registered to
	 * mac80211, hence the if below.
	 */
	if (mvm->hw_registered)
    ieee80211_unregister_hw(mvm->hw);
#endif  // NEEDS_PORTING
    mvm->init_status &= ~IWL_MVM_INIT_STATUS_REG_HW_INIT_COMPLETE;
  }

  free(mvm->scan_cmd);
  mvm->scan_cmd = NULL;
  free(mvm->mcast_filter_cmd);
  mvm->mcast_filter_cmd = NULL;

#if defined(CONFIG_PM_SLEEP) && defined(CPTCFG_IWLWIFI_DEBUGFS)
  kfree(mvm->d3_resume_sram);
#endif
  iwl_trans_op_mode_leave(mvm->trans);

  iwl_phy_db_free(mvm->phy_db);
  mvm->phy_db = NULL;

  kfree(mvm->nvm_data);
  for (i = 0; i < NVM_MAX_NUM_SECTIONS; i++) {
    kfree((void*)mvm->nvm_sections[i].data);
  }

  iwl_task_release_sync(mvm->scan_timeout_task);
  iwl_task_release_sync(mvm->async_handlers_wk);

#if 0   // NEEDS_PORTING
    cancel_delayed_work_sync(&mvm->tcm.work);
#endif  // NEEDS_PORTING

  iwl_mvm_tof_clean(mvm);

  iwl_fw_runtime_free(&mvm->fwrt);

  mtx_destroy(&mvm->notif_wait.notif_wait_lock);
  mtx_destroy(&mvm->mutex);
  mtx_destroy(&mvm->d0i3_suspend_mutex);
  mtx_destroy(&mvm->async_handlers_lock);
  mtx_destroy(&mvm->time_event_lock);
  mtx_destroy(&mvm->d0i3_tx_lock);
  mtx_destroy(&mvm->refs_lock);
  mtx_destroy(&mvm->tcm.lock);

  free(op_mode);
  free(mvm);
}

struct iwl_async_handler_entry {
  list_node_t list;
  struct iwl_rx_cmd_buffer rxb;
  enum iwl_rx_handler_context context;
  void (*fn)(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb);
};

void iwl_mvm_async_handlers_purge(struct iwl_mvm* mvm) {
  struct iwl_async_handler_entry *entry, *tmp;

  mtx_lock(&mvm->async_handlers_lock);
  list_for_every_entry_safe (&mvm->async_handlers_list, entry, tmp, struct iwl_async_handler_entry,
                             list) {
    iwl_free_rxb(&entry->rxb);
    list_delete(&entry->list);
    kfree(entry);
  }
  mtx_unlock(&mvm->async_handlers_lock);
}

static void iwl_mvm_async_handlers_wk(void* data) {
  struct iwl_mvm* mvm = (struct iwl_mvm*)data;
  struct iwl_async_handler_entry *entry, *tmp;
  list_node_t local_list;

  /* TODO(fxbug.dev/95160): Ensure that we are not in stop flow (check iwl_mvm_mac_stop) */

  /*
   * Sync with Rx path with a lock. Remove all the entries from this list,
   * add them to a local one (lock free), and then handle them.
   */
  mtx_lock(&mvm->async_handlers_lock);
  list_move(&mvm->async_handlers_list, &local_list);
  mtx_unlock(&mvm->async_handlers_lock);

  list_for_every_entry_safe (&local_list, entry, tmp, struct iwl_async_handler_entry, list) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wthread-safety-analysis"
    if (entry->context == RX_HANDLER_ASYNC_LOCKED) {
      mtx_lock(&mvm->mutex);
    }
    entry->fn(mvm, &entry->rxb);
    iwl_free_rxb(&entry->rxb);
    list_delete(&entry->list);
    if (entry->context == RX_HANDLER_ASYNC_LOCKED) {
      mtx_unlock(&mvm->mutex);
    }
    free(entry);
#pragma GCC diagnostic pop
  }
}

static inline void iwl_mvm_rx_check_trigger(struct iwl_mvm* mvm, struct iwl_rx_packet* pkt) {
#if 0   // NEEDS_PORTING
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct iwl_fw_dbg_trigger_cmd* cmds_trig;
    int i;

    trig = iwl_fw_dbg_trigger_on(&mvm->fwrt, NULL, FW_DBG_TRIGGER_FW_NOTIF);
    if (!trig) { return; }

    cmds_trig = (void*)trig->data;

    for (i = 0; i < ARRAY_SIZE(cmds_trig->cmds); i++) {
        /* don't collect on CMD 0 */
        if (!cmds_trig->cmds[i].cmd_id) { break; }

        if (cmds_trig->cmds[i].cmd_id != pkt->hdr.cmd ||
            cmds_trig->cmds[i].group_id != pkt->hdr.group_id) {
            continue;
        }

        iwl_fw_dbg_collect_trig(&mvm->fwrt, trig, "CMD 0x%02x.%02x received", pkt->hdr.group_id,
                                pkt->hdr.cmd);
        break;
    }
#endif  // NEEDS_PORTING
}

static void iwl_mvm_rx_common(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb,
                              struct iwl_rx_packet* pkt) {
  size_t i;

  iwl_mvm_rx_check_trigger(mvm, pkt);

  /*
   * Do the notification wait before RX handlers so
   * even if the RX handler consumes the RXB we have
   * access to it in the notification wait entry.
   */
  iwl_notification_wait_notify(&mvm->notif_wait, pkt);

  for (i = 0; i < ARRAY_SIZE(iwl_mvm_rx_handlers); i++) {
    const struct iwl_rx_handlers* rx_h = &iwl_mvm_rx_handlers[i];
    struct iwl_async_handler_entry* entry;

    if (rx_h->cmd_id != WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd)) {
      continue;
    }

    if (rx_h->context == RX_HANDLER_SYNC) {
      rx_h->fn(mvm, rxb);
      return;
    }

    entry = calloc(1, sizeof(*entry));
    /* we can't do much... */
    if (!entry) {
      return;
    }

    entry->rxb._iobuf = rxb_steal_iobuf(rxb);
    entry->rxb._offset = rxb->_offset;
    entry->fn = rx_h->fn;
    entry->context = rx_h->context;
    mtx_lock(&mvm->async_handlers_lock);
    list_add_tail(&mvm->async_handlers_list, &entry->list);
    mtx_unlock(&mvm->async_handlers_lock);
    iwl_task_post(mvm->async_handlers_wk, ZX_MSEC(0));
    break;
  }
}

static void iwl_mvm_rx(struct iwl_op_mode* op_mode, struct napi_struct* napi,
                       struct iwl_rx_cmd_buffer* rxb) {
  struct iwl_rx_packet* pkt = rxb_addr(rxb);
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  uint16_t cmd = WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
  /*
   * RX data may be forwarded to userspace in case the user
   * requested to monitor the rx w/o affecting the regular flow.
   * In this case the iwl_test object will handle forwarding the rx
   * data to user space.
   */
  iwl_tm_gnl_send_rx(mvm->trans, rxb);
#endif

  if (likely(cmd == WIDE_ID(LEGACY_GROUP, REPLY_RX_MPDU_CMD))) {
    iwl_mvm_rx_rx_mpdu(mvm, napi, rxb);
  } else if (cmd == WIDE_ID(LEGACY_GROUP, REPLY_RX_PHY_CMD)) {
    iwl_mvm_rx_rx_phy_cmd(mvm, rxb);
  } else {
    iwl_mvm_rx_common(mvm, rxb, pkt);
  }
}

static void iwl_mvm_rx_mq(struct iwl_op_mode* op_mode, struct napi_struct* napi,
                          struct iwl_rx_cmd_buffer* rxb) {
  struct iwl_rx_packet* pkt = rxb_addr(rxb);
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  uint16_t cmd = WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
  /*
   * RX data may be forwarded to userspace in case the user
   * requested to monitor the rx w/o affecting the regular flow.
   * In this case the iwl_test object will handle forwarding the rx
   * data to user space.
   */
  iwl_tm_gnl_send_rx(mvm->trans, rxb);
#endif

  if (likely(cmd == WIDE_ID(LEGACY_GROUP, REPLY_RX_MPDU_CMD))) {
    iwl_mvm_rx_mpdu_mq(mvm, napi, rxb, 0);
  } else if (unlikely(cmd == WIDE_ID(DATA_PATH_GROUP, RX_QUEUES_NOTIFICATION))) {
    iwl_mvm_rx_queue_notif(mvm, rxb, 0);
  } else if (cmd == WIDE_ID(LEGACY_GROUP, FRAME_RELEASE)) {
    iwl_mvm_rx_frame_release(mvm, napi, rxb, 0);
  } else if (cmd == WIDE_ID(DATA_PATH_GROUP, RX_NO_DATA_NOTIF)) {
    iwl_mvm_rx_monitor_ndp(mvm, napi, rxb, 0);
  } else {
    iwl_mvm_rx_common(mvm, rxb, pkt);
  }
}

static void iwl_mvm_async_cb(struct iwl_op_mode* op_mode, const struct iwl_device_cmd* cmd) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);

  /*
   * For now, we only set the CMD_WANT_ASYNC_CALLBACK for ADD_STA
   * commands that need to block the Tx queues.
   */
  iwl_trans_block_txq_ptrs(mvm->trans, false);
}

static void iwl_mvm_queue_state_change(struct iwl_op_mode* op_mode, int hw_queue, bool start) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  uint8_t sta_id = mvm->queue_info[hw_queue].ra_sta_id;
  struct iwl_mvm_sta* mvmsta;
  struct iwl_mvm_txq* mvmtxq;
  int i;
  unsigned long tid_bitmap;

  if (WARN_ON_ONCE(sta_id >= ARRAY_SIZE(mvm->fw_id_to_mac_id))) {
    return;
  }

  iwl_rcu_read_lock(mvm->dev);

  mvmsta = iwl_rcu_load(mvm->fw_id_to_mac_id[sta_id]);
  if (IS_ERR_OR_NULL(mvmsta)) {
    goto out;
  }

  if (iwl_mvm_has_new_tx_api(mvm)) {
    int tid = mvm->tvqm_info[hw_queue].txq_tid;

    tid_bitmap = BIT(tid);
  } else {
    tid_bitmap = mvm->queue_info[hw_queue].tid_bitmap;
  }

  for (i = 0; i < (IWL_MAX_TID_COUNT + 1); i++) {
    if (!(tid_bitmap & (1 << i))) {
      continue;
    }

    int tid = i;

    if (tid == IWL_MAX_TID_COUNT) {
      tid = fuchsia_wlan_ieee80211_TIDS_MAX;
    }

    mvmtxq = mvmsta->txq[tid];
    mvmtxq->stopped = !start;

#if 0   // NEEDS_PORTING
        if (start && mvmsta->sta_state != IEEE80211_STA_NOTEXIST)) { iwl_mvm_mac_itxq_xmit(mvm->hw, txq); }
#endif  // NEEDS_PORTING
  }

out:
  iwl_rcu_read_unlock(mvm->dev);
}

static void iwl_mvm_stop_sw_queue(struct iwl_op_mode* op_mode, int hw_queue) {
  iwl_mvm_queue_state_change(op_mode, hw_queue, false);
}

static void iwl_mvm_wake_sw_queue(struct iwl_op_mode* op_mode, int hw_queue) {
  iwl_mvm_queue_state_change(op_mode, hw_queue, true);
}

static void iwl_mvm_set_rfkill_state(struct iwl_mvm* mvm) {
#if 0   // NEEDS_PORTING
    bool state = iwl_mvm_is_radio_killed(mvm);

    if (state) { wake_up(&mvm->rx_sync_waitq); }

    wiphy_rfkill_set_hw_state(mvm->hw->wiphy, state);
#endif  // NEEDS_PORTING
}

void iwl_mvm_set_hw_ctkill_state(struct iwl_mvm* mvm, bool state) {
  if (state) {
    set_bit(IWL_MVM_STATUS_HW_CTKILL, &mvm->status);
  } else {
    clear_bit(IWL_MVM_STATUS_HW_CTKILL, &mvm->status);
  }

  iwl_mvm_set_rfkill_state(mvm);
}

static bool iwl_mvm_set_hw_rfkill_state(struct iwl_op_mode* op_mode, bool state) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  bool calibrating = READ_ONCE(mvm->calibrating);

  if (state) {
    set_bit(IWL_MVM_STATUS_HW_RFKILL, &mvm->status);
  } else {
    clear_bit(IWL_MVM_STATUS_HW_RFKILL, &mvm->status);
  }

  iwl_mvm_set_rfkill_state(mvm);

#if 0   // NEEDS_PORTING
	 /* iwl_run_init_mvm_ucode is waiting for results, abort it. */
	if (rfkill_safe_init_done)
		iwl_abort_notification_waits(&mvm->notif_wait);

	/*
	 * Don't ask the transport to stop the firmware. We'll do it
	 * after cfg80211 takes us down.
	 */
	if (unified)
		return false;
#endif  // NEEDS_PORTING

  /*
   * Stop the device if we run OPERATIONAL firmware or if we are in the
   * middle of the calibrations.
   */
  return state && (mvm->fwrt.cur_fw_img != IWL_UCODE_INIT || calibrating);
}

static void iwl_mvm_free_skb(struct iwl_op_mode* op_mode, struct sk_buff* skb) {
#if 0   // NEEDS_PORTING
    struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
    struct ieee80211_tx_info* info;

    info = IEEE80211_SKB_CB(skb);
    iwl_trans_free_tx_cmd(mvm->trans, info->driver_data[1]);
    ieee80211_free_txskb(mvm->hw, skb);
#endif  // NEEDS_PORTING
}

struct iwl_mvm_reprobe {
  struct device* dev;
  struct work_struct work;
};

#if 0   // NEEDS_PORTING
static void iwl_mvm_reprobe_wk(struct work_struct* wk) {
    struct iwl_mvm_reprobe* reprobe;

    reprobe = container_of(wk, struct iwl_mvm_reprobe, work);
	if (device_reprobe(reprobe->dev))
		dev_err(reprobe->dev, "reprobe failed!\n");
	put_device(reprobe->dev);
    kfree(reprobe);
    module_put(THIS_MODULE);
}
#endif  // NEEDS_PORTING

void iwl_mvm_nic_restart(struct iwl_mvm* mvm, bool fw_error) {
  IWL_WARN(mvm, "%s() needs porting\n", __func__);
#if 0   // NEEDS_PORTING
	iwl_abort_notification_waits(&mvm->notif_wait);
	iwl_dbg_tlv_del_timers(mvm->trans);

	/*
	 * This is a bit racy, but worst case we tell mac80211 about
	 * a stopped/aborted scan when that was already done which
	 * is not a problem. It is necessary to abort any os scan
	 * here because mac80211 requires having the scan cleared
	 * before restarting.
	 * We'll reset the scan_status to NONE in restart cleanup in
	 * the next start() call from mac80211. If restart isn't called
	 * (no fw restart) scan status will stay busy.
	 */
	iwl_mvm_report_scan_aborted(mvm);

	/*
	 * If we're restarting already, don't cycle restarts.
	 * If INIT fw asserted, it will likely fail again.
	 * If WoWLAN fw asserted, don't restart either, mac80211
	 * can't recover this since we're already half suspended.
	 */
	if (!mvm->fw_restart && fw_error) {
		iwl_fw_error_collect(&mvm->fwrt, false);
	} else if (test_bit(IWL_MVM_STATUS_STARTING,
			    &mvm->status)) {
		IWL_ERR(mvm, "Starting mac, retry will be triggered anyway\n");
	} else if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
		struct iwl_mvm_reprobe *reprobe;

		IWL_ERR(mvm,
			"Firmware error during reconfiguration - reprobe!\n");

		/*
		 * get a module reference to avoid doing this while unloading
		 * anyway and to avoid scheduling a work with code that's
		 * being removed.
		 */
		if (!try_module_get(THIS_MODULE)) {
			IWL_ERR(mvm, "Module is being unloaded - abort\n");
			return;
		}

		reprobe = kzalloc(sizeof(*reprobe), GFP_ATOMIC);
		if (!reprobe) {
			module_put(THIS_MODULE);
			return;
		}
		reprobe->dev = get_device(mvm->trans->dev);
		INIT_WORK(&reprobe->work, iwl_mvm_reprobe_wk);
		schedule_work(&reprobe->work);
	} else if (test_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED,
			    &mvm->status)) {
		IWL_ERR(mvm, "HW restart already requested, but not started\n");
	} else if (mvm->fwrt.cur_fw_img == IWL_UCODE_REGULAR &&
		   mvm->hw_registered &&
		   !test_bit(STATUS_TRANS_DEAD, &mvm->trans->status)) {
		/* This should be first thing before trying to collect any
		 * data to avoid endless loops if any HW error happens while
		 * collecting debug data.
		 */
		set_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &mvm->status);

		if (mvm->fw->ucode_capa.error_log_size) {
			u32 src_size = mvm->fw->ucode_capa.error_log_size;
			u32 src_addr = mvm->fw->ucode_capa.error_log_addr;
			u8 *recover_buf = kzalloc(src_size, GFP_ATOMIC);

			if (recover_buf) {
				mvm->error_recovery_buf = recover_buf;
				iwl_trans_read_mem_bytes(mvm->trans,
							 src_addr,
							 recover_buf,
							 src_size);
			}
		}

		iwl_fw_error_collect(&mvm->fwrt, false);

		if (fw_error && mvm->fw_restart > 0) {
			mvm->fw_restart--;
			ieee80211_restart_hw(mvm->hw);
		} else if (mvm->fwrt.trans->dbg.restart_required) {
			IWL_DEBUG_INFO(mvm, "FW restart requested after debug collection\n");
			mvm->fwrt.trans->dbg.restart_required = FALSE;
			ieee80211_restart_hw(mvm->hw);
		} else if (mvm->trans->trans_cfg->device_family <= IWL_DEVICE_FAMILY_8000) {
			ieee80211_restart_hw(mvm->hw);
		}
	}
#endif  // NEEDS_PORTING
}

static void iwl_mvm_nic_error(struct iwl_op_mode *op_mode, bool sync)
{
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);

  if (!test_bit(STATUS_TRANS_DEAD, &mvm->trans->status)) {
    iwl_mvm_dump_nic_error_log(mvm);
  }

#if 0  // NEEDS_PORTING
  if (sync) {
    iwl_fw_error_collect(&mvm->fwrt, true);
    /*
     * Currently, the only case for sync=true is during
     * shutdown, so just stop in this case. If/when that
     * changes, we need to be a bit smarter here.
     */
    return;
  }

  /*
   * If the firmware crashes while we're already considering it
   * to be dead then don't ask for a restart, that cannot do
   * anything useful anyway.
   */
  if (!test_bit(IWL_MVM_STATUS_FIRMWARE_RUNNING, &mvm->status))
    return;
#endif  // NEEDS_PORTING

  zx_time_t dump_time_usec = (zx_clock_get_monotonic() + 500) / 1000;
  char dump_name[64];
  snprintf(dump_name, sizeof(dump_name), "nic_error_dump-%05u.%06u",
           (unsigned int)(dump_time_usec / 1000000), (unsigned int)(dump_time_usec % 1000000));
  iwl_fw_dbg_collect(&mvm->fwrt, FW_DBG_TRIGGER_USER, dump_name, strlen(dump_name));

  iwl_mvm_nic_restart(mvm, true);
}

static void iwl_mvm_cmd_queue_full(struct iwl_op_mode* op_mode) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);

  WARN_ON(1);
  iwl_mvm_nic_restart(mvm, true);
}

#ifdef CONFIG_PM
struct iwl_d0i3_iter_data {
  struct iwl_mvm* mvm;
  struct ieee80211_vif* connected_vif;
  uint8_t ap_sta_id;
  uint8_t vif_count;
  uint8_t offloading_tid;
  bool disable_offloading;
};

static bool iwl_mvm_disallow_offloading(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                        struct iwl_d0i3_iter_data* iter_data) {
  struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
  struct iwl_mvm_sta* mvmsta;
  uint32_t available_tids = 0;
  uint8_t tid;

  if (WARN_ON(vif->type != NL80211_IFTYPE_STATION || mvmvif->ap_sta_id == IWL_MVM_INVALID_STA)) {
    return false;
  }

  mvmsta = iwl_mvm_sta_from_staid_rcu(mvm, mvmvif->ap_sta_id);
  if (!mvmsta) {
    return false;
  }

  spin_lock_bh(&mvmsta->lock);
  for (tid = 0; tid < IWL_MAX_TID_COUNT; tid++) {
    struct iwl_mvm_tid_data* tid_data = &mvmsta->tid_data[tid];

    /*
     * in case of pending tx packets, don't use this tid
     * for offloading in order to prevent reuse of the same
     * qos seq counters.
     */
    if (iwl_mvm_tid_queued(mvm, tid_data)) {
      continue;
    }

    if (tid_data->state != IWL_AGG_OFF) {
      continue;
    }

    available_tids |= BIT(tid);
  }
  spin_unlock_bh(&mvmsta->lock);

  /*
   * disallow protocol offloading if we have no available tid
   * (with no pending frames and no active aggregation,
   * as we don't handle "holes" properly - the scheduler needs the
   * frame's seq number and TFD index to match)
   */
  if (!available_tids) {
    return true;
  }

  /* for simplicity, just use the first available tid */
  iter_data->offloading_tid = ffs(available_tids) - 1;
  return false;
}

static void iwl_mvm_enter_d0i3_iterator(void* _data, uint8_t* mac, struct ieee80211_vif* vif) {
  struct iwl_d0i3_iter_data* data = _data;
  struct iwl_mvm* mvm = data->mvm;
  struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
  uint32_t flags = CMD_ASYNC | CMD_HIGH_PRIO | CMD_SEND_IN_IDLE;

  IWL_DEBUG_RPM(mvm, "entering D0i3 - vif %pM\n", vif->addr);
  if (vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.assoc) {
    return;
  }

  /*
   * in case of pending tx packets or active aggregations,
   * avoid offloading features in order to prevent reuse of
   * the same qos seq counters.
   */
  if (iwl_mvm_disallow_offloading(mvm, vif, data)) {
    data->disable_offloading = true;
  }

  iwl_mvm_update_d0i3_power_mode(mvm, vif, true, flags);
  iwl_mvm_send_proto_offload(mvm, vif, data->disable_offloading, false, flags);

  /*
   * on init/association, mvm already configures POWER_TABLE_CMD
   * and REPLY_MCAST_FILTER_CMD, so currently don't
   * reconfigure them (we might want to use different
   * params later on, though).
   */
  data->ap_sta_id = mvmvif->ap_sta_id;
  data->vif_count++;

  /*
   * no new commands can be sent at this stage, so it's safe
   * to save the vif pointer during d0i3 entrance.
   */
  data->connected_vif = vif;
}

static void iwl_mvm_set_wowlan_data(struct iwl_mvm* mvm, struct iwl_wowlan_config_cmd* cmd,
                                    struct iwl_d0i3_iter_data* iter_data) {
  struct ieee80211_sta* ap_sta;
  struct iwl_mvm_sta* mvm_ap_sta;

  if (iter_data->ap_sta_id == IWL_MVM_INVALID_STA) {
    return;
  }

  iwl_rcu_read_lock(mvm->dev);

  ap_sta = iwl_rcu_load(mvm->fw_id_to_mac_id[iter_data->ap_sta_id]);
  if (IS_ERR_OR_NULL(ap_sta)) {
    goto out;
  }

  mvm_ap_sta = iwl_mvm_sta_from_mac80211(ap_sta);
  cmd->is_11n_connection = ap_sta->ht_cap.ht_supported;
  cmd->offloading_tid = iter_data->offloading_tid;
  cmd->flags =
      ENABLE_L3_FILTERING | ENABLE_NBNS_FILTERING | ENABLE_DHCP_FILTERING | ENABLE_STORE_BEACON;
  /*
   * The d0i3 uCode takes care of the nonqos counters,
   * so configure only the qos seq ones.
   */
  iwl_mvm_set_wowlan_qos_seq(mvm_ap_sta, cmd);
out:
  iwl_rcu_read_unlock(mvm->dev);
}

int iwl_mvm_enter_d0i3(struct iwl_op_mode* op_mode) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  uint32_t flags = CMD_ASYNC | CMD_HIGH_PRIO | CMD_SEND_IN_IDLE;
  int ret;
  struct iwl_d0i3_iter_data d0i3_iter_data = {
      .mvm = mvm,
  };
  struct iwl_wowlan_config_cmd wowlan_config_cmd = {
      .wakeup_filter = cpu_to_le32(IWL_WOWLAN_WAKEUP_RX_FRAME | IWL_WOWLAN_WAKEUP_BEACON_MISS |
                                   IWL_WOWLAN_WAKEUP_LINK_CHANGE),
  };
  struct iwl_d3_manager_config d3_cfg_cmd = {
      .min_sleep_time = cpu_to_le32(1000),
      .wakeup_flags = cpu_to_le32(IWL_WAKEUP_D3_CONFIG_FW_ERROR),
  };

  IWL_DEBUG_RPM(mvm, "MVM entering D0i3\n");

  if (WARN_ON_ONCE(mvm->fwrt.cur_fw_img != IWL_UCODE_REGULAR)) {
    return -EINVAL;
  }

  set_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status);

  /*
   * iwl_mvm_ref_sync takes a reference before checking the flag.
   * so by checking there is no held reference we prevent a state
   * in which iwl_mvm_ref_sync continues successfully while we
   * configure the firmware to enter d0i3
   */
  if (iwl_mvm_ref_taken(mvm)) {
    IWL_DEBUG_RPM(mvm->trans, "abort d0i3 due to taken ref\n");
    clear_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status);
    wake_up(&mvm->d0i3_exit_waitq);
    return 1;
  }

  ieee80211_iterate_active_interfaces_atomic(mvm->hw, IEEE80211_IFACE_ITER_NORMAL,
                                             iwl_mvm_enter_d0i3_iterator, &d0i3_iter_data);
  if (d0i3_iter_data.vif_count == 1) {
    mvm->d0i3_ap_sta_id = d0i3_iter_data.ap_sta_id;
    mvm->d0i3_offloading = !d0i3_iter_data.disable_offloading;
  } else {
    WARN_ON_ONCE(d0i3_iter_data.vif_count > 1);
    mvm->d0i3_ap_sta_id = IWL_MVM_INVALID_STA;
    mvm->d0i3_offloading = false;
  }

  iwl_mvm_pause_tcm(mvm, true);
  /* make sure we have no running tx while configuring the seqno */
  synchronize_net();

  /* Flush the hw queues, in case something got queued during entry */
  /* TODO new tx api */
  if (iwl_mvm_has_new_tx_api(mvm)) {
    WARN_ONCE(1, "d0i3: Need to implement flush TX queue\n");
  } else {
    ret = iwl_mvm_flush_tx_path(mvm, iwl_mvm_flushable_queues(mvm), flags);
    if (ret) {
      return ret;
    }
  }

  /* configure wowlan configuration only if needed */
  if (mvm->d0i3_ap_sta_id != IWL_MVM_INVALID_STA) {
    /* wake on beacons only if beacon storing isn't supported */
    if (!fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_BEACON_STORING)) {
      wowlan_config_cmd.wakeup_filter |= cpu_to_le32(IWL_WOWLAN_WAKEUP_BCN_FILTERING);
    }

    iwl_mvm_wowlan_config_key_params(mvm, d0i3_iter_data.connected_vif, true, flags);

    iwl_mvm_set_wowlan_data(mvm, &wowlan_config_cmd, &d0i3_iter_data);

    ret = iwl_mvm_send_cmd_pdu(mvm, WOWLAN_CONFIGURATION, flags, sizeof(wowlan_config_cmd),
                               &wowlan_config_cmd);
    if (ret) {
      return ret;
    }
  }

  return iwl_mvm_send_cmd_pdu(mvm, D3_CONFIG_CMD, flags | CMD_MAKE_TRANS_IDLE, sizeof(d3_cfg_cmd),
                              &d3_cfg_cmd);
}

static void iwl_mvm_exit_d0i3_iterator(void* _data, uint8_t* mac, struct ieee80211_vif* vif) {
  struct iwl_mvm* mvm = _data;
  uint32_t flags = CMD_ASYNC | CMD_HIGH_PRIO;

  IWL_DEBUG_RPM(mvm, "exiting D0i3 - vif %pM\n", vif->addr);
  if (vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.assoc) {
    return;
  }

  iwl_mvm_update_d0i3_power_mode(mvm, vif, false, flags);
}

struct iwl_mvm_d0i3_exit_work_iter_data {
  struct iwl_mvm* mvm;
  struct iwl_wowlan_status* status;
  uint32_t wakeup_reasons;
};

static void iwl_mvm_d0i3_exit_work_iter(void* _data, uint8_t* mac, struct ieee80211_vif* vif) {
  struct iwl_mvm_d0i3_exit_work_iter_data* data = _data;
  struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
  uint32_t reasons = data->wakeup_reasons;

  /* consider only the relevant station interface */
  if (vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.assoc ||
      data->mvm->d0i3_ap_sta_id != mvmvif->ap_sta_id) {
    return;
  }

  if (reasons & IWL_WOWLAN_WAKEUP_BY_DISCONNECTION_ON_DEAUTH) {
    iwl_mvm_connection_loss(data->mvm, vif, "D0i3");
  } else if (reasons & IWL_WOWLAN_WAKEUP_BY_DISCONNECTION_ON_MISSED_BEACON) {
    ieee80211_beacon_loss(vif);
  } else {
    iwl_mvm_d0i3_update_keys(data->mvm, vif, data->status);
  }
}

void iwl_mvm_d0i3_enable_tx(struct iwl_mvm* mvm, __le16* qos_seq) {
  struct ieee80211_sta* sta = NULL;
  struct iwl_mvm_sta* mvm_ap_sta;
  int i;
  bool wake_queues = false;

  iwl_assert_lock_held(&mvm->mutex);

  spin_lock_bh(&mvm->d0i3_tx_lock);

  if (mvm->d0i3_ap_sta_id == IWL_MVM_INVALID_STA) {
    goto out;
  }

  IWL_DEBUG_RPM(mvm, "re-enqueue packets\n");

  /* get the sta in order to update seq numbers and re-enqueue skbs */
  sta = rcu_dereference_protected(mvm->fw_id_to_mac_id[mvm->d0i3_ap_sta_id],
                                  lockdep_is_held(&mvm->mutex));

  if (IS_ERR_OR_NULL(sta)) {
    sta = NULL;
    goto out;
  }

  if (mvm->d0i3_offloading && qos_seq) {
    /* update qos seq numbers if offloading was enabled */
    mvm_ap_sta = iwl_mvm_sta_from_mac80211(sta);
    for (i = 0; i < IWL_MAX_TID_COUNT; i++) {
      uint16_t seq = le16_to_cpu(qos_seq[i]);
      /* firmware stores last-used one, we store next one */
      seq += 0x10;
      mvm_ap_sta->tid_data[i].seq_number = seq;
    }
  }
out:
  /* re-enqueue (or drop) all packets */
  while (!skb_queue_empty(&mvm->d0i3_tx)) {
    struct sk_buff* skb = __skb_dequeue(&mvm->d0i3_tx);

    if (!sta || iwl_mvm_tx_skb(mvm, skb, sta)) {
      ieee80211_free_txskb(mvm->hw, skb);
    }

    /* if the skb_queue is not empty, we need to wake queues */
    wake_queues = true;
  }
  clear_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status);
  wake_up(&mvm->d0i3_exit_waitq);
  mvm->d0i3_ap_sta_id = IWL_MVM_INVALID_STA;
  if (wake_queues) {
    ieee80211_wake_queues(mvm->hw);
  }

  spin_unlock_bh(&mvm->d0i3_tx_lock);
}

static void iwl_mvm_d0i3_exit_work(struct work_struct* wk) {
  struct iwl_mvm* mvm = container_of(wk, struct iwl_mvm, d0i3_exit_work);
  struct iwl_mvm_d0i3_exit_work_iter_data iter_data = {
      .mvm = mvm,
  };

  struct iwl_wowlan_status* status;
  uint32_t wakeup_reasons = 0;
  __le16* qos_seq = NULL;

  mtx_lock(&mvm->mutex);

  status = iwl_mvm_send_wowlan_get_status(mvm);
  if (IS_ERR_OR_NULL(status)) {
    /* set to NULL so we don't need to check before kfree'ing */
    status = NULL;
    goto out;
  }

  wakeup_reasons = le32_to_cpu(status->wakeup_reasons);
  qos_seq = status->qos_seq_ctr;

  IWL_DEBUG_RPM(mvm, "wakeup reasons: 0x%x\n", wakeup_reasons);

  iter_data.wakeup_reasons = wakeup_reasons;
  iter_data.status = status;
  ieee80211_iterate_active_interfaces(mvm->hw, IEEE80211_IFACE_ITER_NORMAL,
                                      iwl_mvm_d0i3_exit_work_iter, &iter_data);
out:
  iwl_mvm_d0i3_enable_tx(mvm, qos_seq);

  IWL_DEBUG_INFO(mvm, "d0i3 exit completed (wakeup reasons: 0x%x)\n", wakeup_reasons);

  /* qos_seq might point inside resp_pkt, so free it only now */
  kfree(status);

  /* the FW might have updated the regdomain */
  iwl_mvm_update_changed_regdom(mvm);

  iwl_mvm_resume_tcm(mvm);
  iwl_mvm_unref(mvm, IWL_MVM_REF_EXIT_WORK);
  mtx_unlock(&mvm->mutex);
}

int _iwl_mvm_exit_d0i3(struct iwl_mvm* mvm) {
  uint32_t flags = CMD_ASYNC | CMD_HIGH_PRIO | CMD_SEND_IN_IDLE | CMD_WAKE_UP_TRANS;
  int ret;

  IWL_DEBUG_RPM(mvm, "MVM exiting D0i3\n");

  if (WARN_ON_ONCE(mvm->fwrt.cur_fw_img != IWL_UCODE_REGULAR)) {
    return -EINVAL;
  }

  mtx_lock(&mvm->d0i3_suspend_mutex);
  if (test_bit(D0I3_DEFER_WAKEUP, &mvm->d0i3_suspend_flags)) {
    IWL_DEBUG_RPM(mvm, "Deferring d0i3 exit until resume\n");
    __set_bit(D0I3_PENDING_WAKEUP, &mvm->d0i3_suspend_flags);
    mtx_unlock(&mvm->d0i3_suspend_mutex);
    return 0;
  }
  mtx_unlock(&mvm->d0i3_suspend_mutex);

  ret = iwl_mvm_send_cmd_pdu(mvm, D0I3_END_CMD, flags, 0, NULL);
  if (ret) {
    goto out;
  }

  ieee80211_iterate_active_interfaces_atomic(mvm->hw, IEEE80211_IFACE_ITER_NORMAL,
                                             iwl_mvm_exit_d0i3_iterator, mvm);
out:
  schedule_work(&mvm->d0i3_exit_work);
  return ret;
}

int iwl_mvm_exit_d0i3(struct iwl_op_mode* op_mode) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);

  iwl_mvm_ref(mvm, IWL_MVM_REF_EXIT_WORK);
  return _iwl_mvm_exit_d0i3(mvm);
}

#define IWL_MVM_D0I3_OPS .enter_d0i3 = iwl_mvm_enter_d0i3, .exit_d0i3 = iwl_mvm_exit_d0i3,
#else /* CONFIG_PM */
#define IWL_MVM_D0I3_OPS
#endif /* CONFIG_PM */

#define IWL_MVM_COMMON_OPS                                                            \
  /* these could be differentiated */                                                 \
  .async_cb = iwl_mvm_async_cb, .queue_full = iwl_mvm_stop_sw_queue,                  \
  .queue_not_full = iwl_mvm_wake_sw_queue, .hw_rf_kill = iwl_mvm_set_hw_rfkill_state, \
  .free_skb = iwl_mvm_free_skb, .nic_error = iwl_mvm_nic_error,                       \
  .cmd_queue_full = iwl_mvm_cmd_queue_full, .nic_config = iwl_mvm_nic_config,         \
  IWL_MVM_D0I3_OPS /* as we only register one, these MUST be common! */               \
      .start = iwl_op_mode_mvm_start,                                                 \
  .stop = iwl_op_mode_mvm_stop
#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
#define IWL_MVM_COMMON_TEST_OPS                    \
  .test_ops = {                                    \
      .send_hcmd = iwl_mvm_tm_send_hcmd,           \
      .cmd_exec_start = iwl_mvm_tm_cmd_exec_start, \
      .cmd_exec_end = iwl_mvm_tm_cmd_exec_end,     \
  },
#else
#define IWL_MVM_COMMON_TEST_OPS
#endif

static const struct iwl_op_mode_ops iwl_mvm_ops = {
    IWL_MVM_COMMON_OPS,
    IWL_MVM_COMMON_TEST_OPS.rx = iwl_mvm_rx,
};

static void iwl_mvm_rx_mq_rss(struct iwl_op_mode* op_mode, struct napi_struct* napi,
                              struct iwl_rx_cmd_buffer* rxb, unsigned int queue) {
  struct iwl_mvm* mvm = IWL_OP_MODE_GET_MVM(op_mode);
  struct iwl_rx_packet* pkt = rxb_addr(rxb);
  uint16_t cmd = WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);

  if (unlikely(cmd == WIDE_ID(LEGACY_GROUP, FRAME_RELEASE))) {
    iwl_mvm_rx_frame_release(mvm, napi, rxb, queue);
  } else if (unlikely(cmd == WIDE_ID(DATA_PATH_GROUP, RX_QUEUES_NOTIFICATION))) {
    iwl_mvm_rx_queue_notif(mvm, rxb, queue);
  } else if (likely(cmd == WIDE_ID(LEGACY_GROUP, REPLY_RX_MPDU_CMD))) {
    iwl_mvm_rx_mpdu_mq(mvm, napi, rxb, queue);
  }
}

static const struct iwl_op_mode_ops iwl_mvm_ops_mq = {
    IWL_MVM_COMMON_OPS,
    IWL_MVM_COMMON_TEST_OPS.rx = iwl_mvm_rx_mq,
    .rx_rss = iwl_mvm_rx_mq_rss,
};
