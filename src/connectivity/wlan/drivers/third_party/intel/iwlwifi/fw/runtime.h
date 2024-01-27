/******************************************************************************
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
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
 *
 *****************************************************************************/
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_RUNTIME_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_RUNTIME_H_

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/dbg-tlv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/debug.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/paging.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/img.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-config.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-eeprom-parse.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-trans.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/task.h"

struct iwl_fw_runtime_ops {
  int (*dump_start)(void* ctx);
  void (*dump_end)(void* ctx);
  bool (*fw_running)(void* ctx);
  int (*send_hcmd)(void* ctx, struct iwl_host_cmd* host_cmd);
};

#define MAX_NUM_LMAC 2
struct iwl_fwrt_shared_mem_cfg {
  uint32_t num_lmacs;
  uint32_t num_txfifo_entries;
  struct {
    uint32_t txfifo_size[TX_FIFO_MAX_NUM];
    uint32_t rxfifo1_size;
  } lmac[MAX_NUM_LMAC];
  uint32_t rxfifo2_size;
  uint32_t internal_txfifo_addr;
  uint32_t internal_txfifo_size[TX_FIFO_INTERNAL_MAX_NUM];
};

enum iwl_fw_runtime_status {
  IWL_FWRT_STATUS_DUMPING = 0,
  IWL_FWRT_STATUS_WAIT_ALIVE,
};

/**
 * struct iwl_fw_runtime - runtime data for firmware
 * @fw: firmware image
 * @cfg: NIC configuration
 * @dev: device pointer
 * @ops: user ops
 * @ops_ctx: user ops context
 * @status: status flags
 * @fw_paging_db: paging database
 * @num_of_paging_blk: number of paging blocks
 * @num_of_pages_in_last_blk: number of pages in the last block
 * @smem_cfg: saved firmware SMEM configuration
 * @cur_fw_img: current firmware image, must be maintained by
 *  the driver by calling &iwl_fw_set_current_image()
 * @dump: debug dump data
 */
struct iwl_fw_runtime {
  struct iwl_trans* trans;
  const struct iwl_fw* fw;
  struct device* dev;

  const struct iwl_fw_runtime_ops* ops;
  void* ops_ctx;

  unsigned long status;

  /* Paging */
  //
  // Note that the first block is special (the CSS block). Please check the comment in paging.c.
  //
  struct iwl_fw_paging fw_paging_db[NUM_OF_FW_PAGING_BLOCKS];
  //
  // Derived from fw->img[cur_fw_img].paging_mem_size in iwl_alloc_fw_paging_mem().
  size_t num_of_paging_blk;
  //
  // Also see iwl_alloc_fw_paging_mem() how this is determined.
  size_t num_of_pages_in_last_blk;

  enum iwl_ucode_type cur_fw_img;

  /* memory configuration */
  struct iwl_fwrt_shared_mem_cfg smem_cfg;

  /* debug */
  struct {
    const struct iwl_fw_dump_desc* desc;
    bool monitor_only;
    struct iwl_task* wk;

    uint8_t conf;

    /* ts of the beginning of a non-collect fw dbg data period */
    unsigned long non_collect_ts_start[IWL_FW_TRIGGER_ID_NUM - 1];
    uint32_t* d3_debug_data;
    struct iwl_fw_ini_active_regs active_regs[IWL_FW_INI_MAX_REGION_ID];
    struct iwl_fw_ini_active_triggers active_trigs[IWL_FW_TRIGGER_ID_NUM];
    uint32_t lmac_err_id[MAX_NUM_LMAC];
    uint32_t umac_err_id;
  } dump;
#ifdef CPTCFG_IWLWIFI_DEBUGFS
  struct {
    struct delayed_work wk;
    uint32_t delay;
    uint64_t seq;
  } timestamp;
  bool tpc_enabled;
#endif /* CPTCFG_IWLWIFI_DEBUGFS */
};

void iwl_fw_runtime_init(struct iwl_fw_runtime* fwrt, struct iwl_trans* trans,
                         const struct iwl_fw* fw, const struct iwl_fw_runtime_ops* ops,
                         void* ops_ctx, struct dentry* dbgfs_dir);

void iwl_fw_runtime_free(struct iwl_fw_runtime* fwrt);

void iwl_fw_runtime_suspend(struct iwl_fw_runtime* fwrt);

void iwl_fw_runtime_resume(struct iwl_fw_runtime* fwrt);

static inline void iwl_fw_set_current_image(struct iwl_fw_runtime* fwrt,
                                            enum iwl_ucode_type cur_fw_img) {
  fwrt->cur_fw_img = cur_fw_img;
}

int iwl_init_paging(struct iwl_fw_runtime* fwrt, enum iwl_ucode_type type);
void iwl_free_fw_paging(struct iwl_fw_runtime* fwrt);

void iwl_get_shared_mem_conf(struct iwl_fw_runtime* fwrt);
zx_status_t iwl_set_soc_latency(struct iwl_fw_runtime *fwrt);
zx_status_t iwl_configure_rxq(struct iwl_fw_runtime *fwrt);

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_RUNTIME_H_
