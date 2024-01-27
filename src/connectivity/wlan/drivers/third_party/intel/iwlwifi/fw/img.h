/******************************************************************************
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016        Intel Deutschland GmbH
 * Copyright(c) 2018 Intel Corporation
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

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_IMG_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_IMG_H_

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/dbg-tlv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/error-dump.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/file.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/compiler.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/memory.h"

/**
 * enum iwl_ucode_type
 *
 * The type of ucode.
 *
 * @IWL_UCODE_REGULAR: Normal runtime ucode
 * @IWL_UCODE_INIT: Initial ucode
 * @IWL_UCODE_WOWLAN: Wake on Wireless enabled ucode
 * @IWL_UCODE_REGULAR_USNIFFER: Normal runtime ucode when using usniffer image
 */
enum iwl_ucode_type {
  IWL_UCODE_REGULAR,
  IWL_UCODE_INIT,
  IWL_UCODE_WOWLAN,
  IWL_UCODE_REGULAR_USNIFFER,
  IWL_UCODE_TYPE_MAX,
};

/*
 * enumeration of ucode section.
 * This enumeration is used directly for older firmware (before 16.0).
 * For new firmware, there can be up to 4 sections (see below) but the
 * first one packaged into the firmware file is the DATA section and
 * some debugging code accesses that.
 */
enum iwl_ucode_sec {
  IWL_UCODE_SECTION_DATA,
  IWL_UCODE_SECTION_INST,
};

struct iwl_ucode_capabilities {
  uint32_t max_probe_length;
  uint32_t n_scan_channels;
  uint32_t standard_phy_calibration_size;
  uint32_t flags;
#if IS_ENABLED(CPTCFG_IWLFMAC)
  uint32_t fmac_api_version;
  uint32_t fmac_error_log_addr;
  uint32_t fmac_error_log_size;
#endif
  unsigned long _api[BITS_TO_LONGS(NUM_IWL_UCODE_TLV_API)];
  unsigned long _capa[BITS_TO_LONGS(NUM_IWL_UCODE_TLV_CAPA)];

	const struct iwl_fw_cmd_version *cmd_versions;
	u32 n_cmd_versions;
};

static inline bool fw_has_api(const struct iwl_ucode_capabilities* capabilities,
                              iwl_ucode_tlv_api_t api) {
  return test_bit((__force long)api, capabilities->_api);
}

static inline bool fw_has_capa(const struct iwl_ucode_capabilities* capabilities,
                               iwl_ucode_tlv_capa_t capa) {
  return test_bit((__force long)capa, capabilities->_capa);
}

/* one for each uCode image (inst/data, init/runtime/wowlan) */
struct fw_desc {
  const void* data; /* vmalloc'ed data */
  uint32_t len;     /* size in bytes */
  uint32_t offset;  /* offset in the device */
};

struct fw_img {
  struct fw_desc* sec;
  int num_sec;
  bool is_dual_cpus;
  uint32_t paging_mem_size;
};

/*
 * Block paging calculations
 */
#define PAGE_2_EXP_SIZE 12                  /* 4K == 2^12 */
#define FW_PAGING_SIZE BIT(PAGE_2_EXP_SIZE) /* page size is 4KB */
#define PAGE_PER_GROUP_2_EXP_SIZE 3
/* 8 pages per group */
#define NUM_OF_PAGE_PER_GROUP BIT(PAGE_PER_GROUP_2_EXP_SIZE)
/* don't change, support only 32KB size */
#define PAGING_BLOCK_SIZE (NUM_OF_PAGE_PER_GROUP * FW_PAGING_SIZE)
/* 32K == 2^15 */
#define BLOCK_2_EXP_SIZE (PAGE_2_EXP_SIZE + PAGE_PER_GROUP_2_EXP_SIZE)

/*
 * Image paging calculations
 */
#define BLOCK_PER_IMAGE_2_EXP_SIZE 5
/* 2^5 == 32 blocks per image */
#define NUM_OF_BLOCK_PER_IMAGE BIT(BLOCK_PER_IMAGE_2_EXP_SIZE)
/* maximum image size 1024KB */
#define MAX_PAGING_IMAGE_SIZE (NUM_OF_BLOCK_PER_IMAGE * PAGING_BLOCK_SIZE)

/* Virtual address signature */
#define PAGING_ADDR_SIG 0xAA000000

#define PAGING_CMD_IS_SECURED BIT(9)
#define PAGING_CMD_IS_ENABLED BIT(8)
#define PAGING_CMD_NUM_OF_PAGES_IN_LAST_GRP_POS 0
#define PAGING_TLV_SECURE_MASK 1

/**
 * struct iwl_fw_paging
 * @fw_paging_phys: page phy pointer
 * @fw_paging_block: pointer to the allocated block
 * @fw_paging_size: page size
 */
struct iwl_fw_paging {
  struct iwl_iobuf* io_buf;
};

/**
 * struct iwl_fw_cscheme_list - a cipher scheme list
 * @size: a number of entries
 * @cs: cipher scheme entries
 */
struct iwl_fw_cscheme_list {
  uint8_t size;
  struct iwl_fw_cipher_scheme cs[];
} __packed;

/**
 * enum iwl_fw_type - iwlwifi firmware type
 * @IWL_FW_DVM: DVM firmware
 * @IWL_FW_MVM: MVM firmware
 */
enum iwl_fw_type {
  IWL_FW_DVM,
  IWL_FW_MVM,
#if IS_ENABLED(CPTCFG_IWLFMAC)
  IWL_FW_FMAC,
#endif
};

/**
 * struct iwl_fw_dbg - debug data
 *
 * @dest_tlv: points to debug destination TLV (typically SRAM or DRAM)
 * @n_dest_reg: num of reg_ops in dest_tlv
 * @conf_tlv: array of pointers to configuration HCMDs
 * @trigger_tlv: array of pointers to triggers TLVs
 * @trigger_tlv_len: lengths of the @dbg_trigger_tlv entries
 * @mem_tlv: Runtime addresses to dump
 * @n_mem_tlv: number of runtime addresses
 * @dump_mask: bitmask of dump regions
 */
struct iwl_fw_dbg {
  struct iwl_fw_dbg_dest_tlv_v1* dest_tlv;
  uint8_t n_dest_reg;
  struct iwl_fw_dbg_conf_tlv* conf_tlv[FW_DBG_CONF_MAX];
  struct iwl_fw_dbg_trigger_tlv* trigger_tlv[FW_DBG_TRIGGER_MAX];
  size_t trigger_tlv_len[FW_DBG_TRIGGER_MAX];
  struct iwl_fw_dbg_mem_seg_tlv* mem_tlv;
  size_t n_mem_tlv;
  uint32_t dump_mask;
};

/**
 * struct iwl_fw_ini_active_triggers
 * @active: is this trigger active
 * @apply_point: last apply point that updated this trigger
 * @conf: active trigger
 * @conf_ext: second trigger, contains extra regions to dump
 */
struct iwl_fw_ini_active_triggers {
  bool active;
  enum iwl_fw_ini_apply_point apply_point;
  struct iwl_fw_ini_trigger* conf;
  struct iwl_fw_ini_trigger* conf_ext;
};

/**
 * struct iwl_fw_ini_active_regs
 * @reg: active region from TLV
 * @apply_point: apply point where it became active
 */
struct iwl_fw_ini_active_regs {
  struct iwl_fw_ini_region_cfg* reg;
  enum iwl_fw_ini_apply_point apply_point;
};

/**
 * struct iwl_fw - variables associated with the firmware
 *
 * @ucode_ver: ucode version from the ucode file
 * @fw_version: firmware version string
 * @img: ucode image like ucode_rt, ucode_init, ucode_wowlan.
 * @iml_len: length of the image loader image
 * @iml: image loader fw image
 * @ucode_capa: capabilities parsed from the ucode file.
 * @enhance_sensitivity_table: device can do enhanced sensitivity.
 * @init_evtlog_ptr: event log offset for init ucode.
 * @init_evtlog_size: event log size for init ucode.
 * @init_errlog_ptr: error log offfset for init ucode.
 * @inst_evtlog_ptr: event log offset for runtime ucode.
 * @inst_evtlog_size: event log size for runtime ucode.
 * @inst_errlog_ptr: error log offfset for runtime ucode.
 * @type: firmware type (&enum iwl_fw_type)
 * @cipher_scheme: optional external cipher scheme.
 * @human_readable: human readable version
 *  we get the ALIVE from the uCode
 */
struct iwl_fw {
  uint32_t ucode_ver;

  char fw_version[ETHTOOL_FWVERS_LEN];

  /* ucode images */
  struct fw_img img[IWL_UCODE_TYPE_MAX];
  size_t iml_len;
  uint8_t* iml;

  struct iwl_ucode_capabilities ucode_capa;
  bool enhance_sensitivity_table;

  uint32_t init_evtlog_ptr, init_evtlog_size, init_errlog_ptr;
  uint32_t inst_evtlog_ptr, inst_evtlog_size, inst_errlog_ptr;

  struct iwl_tlv_calib_ctrl default_calib[IWL_UCODE_TYPE_MAX];
  uint32_t phy_config;
  uint8_t valid_tx_ant;
  uint8_t valid_rx_ant;

  enum iwl_fw_type type;

  struct iwl_fw_cipher_scheme cs[IWL_UCODE_MAX_CS];
  uint8_t human_readable[FW_VER_HUMAN_READABLE_SZ];

  struct iwl_fw_dbg dbg;
};

static inline const char* get_fw_dbg_mode_string(int mode) {
  switch (mode) {
    case SMEM_MODE:
      return "SMEM";
    case EXTERNAL_MODE:
      return "EXTERNAL_DRAM";
    case MARBH_MODE:
      return "MARBH";
    case MIPI_MODE:
      return "MIPI";
    default:
      return "UNKNOWN";
  }
}

static inline bool iwl_fw_dbg_conf_usniffer(const struct iwl_fw* fw, uint8_t id) {
  const struct iwl_fw_dbg_conf_tlv* conf_tlv = fw->dbg.conf_tlv[id];

  if (!conf_tlv) {
    return false;
  }

  return conf_tlv->usniffer;
}

static inline const struct fw_img* iwl_get_ucode_image(const struct iwl_fw* fw,
                                                       enum iwl_ucode_type ucode_type) {
  if (ucode_type >= IWL_UCODE_TYPE_MAX) {
    return NULL;
  }

  return &fw->img[ucode_type];
}

u8 iwl_fw_lookup_cmd_ver(const struct iwl_fw *fw, u32 cmd_id, u8 def);

u8 iwl_fw_lookup_notif_ver(const struct iwl_fw *fw, u8 grp, u8 cmd, u8 def);
const char *iwl_fw_lookup_assert_desc(u32 num);

#define FW_SYSASSERT_CPU_MASK		0xf0000000
#define FW_SYSASSERT_PNVM_MISSING	0x0010070d

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_IMG_H_
