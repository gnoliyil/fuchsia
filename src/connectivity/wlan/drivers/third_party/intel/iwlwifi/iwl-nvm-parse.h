/******************************************************************************
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
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
 *****************************************************************************/
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_NVM_PARSE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_NVM_PARSE_H_

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-eeprom-parse.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/compiler.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/regulatory.h"

/**
 * enum iwl_nvm_sbands_flags - modification flags for the channel profiles
 *
 * @IWL_NVM_SBANDS_FLAGS_LAR: LAR is enabled
 * @IWL_NVM_SBANDS_FLAGS_NO_WIDE_IN_5GHZ: disallow 40, 80 and 160MHz on 5GHz
 */
enum iwl_nvm_sbands_flags {
  IWL_NVM_SBANDS_FLAGS_LAR = BIT(0),
  IWL_NVM_SBANDS_FLAGS_NO_WIDE_IN_5GHZ = BIT(1),
};

/**
 * iwl_parse_nvm_data - parse NVM data and return values
 *
 * This function parses all NVM values we need and then
 * returns a (newly allocated) struct containing all the
 * relevant values for driver use. The struct must be freed
 * later with iwl_free_nvm_data().
 */
struct iwl_nvm_data* iwl_parse_nvm_data(struct iwl_trans* trans, const struct iwl_cfg* cfg,
                                        const struct iwl_fw *fw,
                                        const __be16* nvm_hw, const __le16* nvm_sw,
                                        const __le16* nvm_calib, const __le16* regulatory,
                                        const __le16* mac_override, const __le16* phy_sku,
                                        uint8_t tx_chains, uint8_t rx_chains,
                                        bool lar_fw_supported);

/**
 * iwl_parse_mcc_info - parse MCC (mobile country code) info coming from FW
 *
 * This function parses the regulatory channel data received as a MCC_UPDATE_CMD command.
 * The parsing result is saved in the 'mvm'.
 */
void iwl_parse_nvm_mcc_info(struct mcc_info* mcc_info, const struct iwl_cfg* cfg, int num_of_ch,
                            const __le32* channels, uint16_t fw_mcc, uint16_t geo_info);

/**
 * struct iwl_nvm_section - describes an NVM section in memory.
 *
 * This struct holds an NVM section read from the NIC using NVM_ACCESS_CMD,
 * and saved for later use by the driver. Not all NVM sections are saved
 * this way, only the needed ones.
 */
struct iwl_nvm_section {
  uint16_t length;
  const uint8_t* data;
};

/**
 * iwl_read_external_nvm - Reads external NVM from a file into nvm_sections
 */
int iwl_read_external_nvm(struct iwl_trans* trans, const char* nvm_file_name,
                          struct iwl_nvm_section* nvm_sections);
void iwl_nvm_fixups(uint32_t hw_id, unsigned int section, uint8_t* data, unsigned int len);

/**
 * iwl_get_nvm - retrieve NVM data from firmware
 *
 * Allocates a new iwl_nvm_data structure, fills it with
 * NVM data, and returns it to caller.
 */
zx_status_t iwl_get_nvm(struct iwl_trans* trans, const struct iwl_fw* fw,
                        struct iwl_nvm_data** ret_nvm);

//
// cfg_rates_to_80211 - convert iwl_cfg80211_rates to 802.11 rate.
//
// iwl_cfg80211_rates are defined in units of 100 kbit/s.
// 802.11 rates are defined in IEEE Std 802.11-2016, 9.4.2.3, which are in units of 500 kbit/s.
//
static inline unsigned cfg_rates_to_80211(uint16_t cfg) { return cfg / 5; }

// extern
extern uint16_t iwl_cfg80211_rates[];

//
// Get the index of the 'rate' in the iwl_cfg80211_rates[].
//
size_t iwl_get_rate_index(uint16_t rate);

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_NVM_PARSE_H_
