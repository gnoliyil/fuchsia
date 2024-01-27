/******************************************************************************
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2015 - 2017 Intel Deutschland GmbH
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
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/dbg.h"

#include <zircon/syscalls.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/debug.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/debugfs.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/runtime.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-csr.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-drv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-io.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-prph.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-trans.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/debug.h"


// TODO(fxbug.dev/119415): review these once we refactor (or reorder) the functions.
static zx_status_t iwl_fw_dbg_restart_recording(struct iwl_trans *trans,
					struct iwl_fw_dbg_params *params);
void iwl_fw_dbg_stop_restart_recording(struct iwl_fw_runtime *fwrt,
               struct iwl_fw_dbg_params *params,
               bool stop);

/**
 * struct iwl_fw_dump_ptrs - set of pointers needed for the fw-error-dump
 *
 * @fwrt_ptr: pointer to the buffer coming from fwrt
 * @trans_ptr: pointer to struct %iwl_trans_dump_data which contains the
 *  transport's data.
 * @trans_len: length of the valid data in trans_ptr
 * @fwrt_len: length of the valid data in fwrt_ptr
 */
struct iwl_fw_dump_ptrs {
  struct iwl_trans_dump_data* trans_ptr;
  void* fwrt_ptr;
  uint32_t fwrt_len;
};

#define RADIO_REG_MAX_READ 0x2ad
static void iwl_read_radio_regs(struct iwl_fw_runtime* fwrt,
                                struct iwl_fw_error_dump_data** dump_data) {
  uint8_t* pos = (void*)(*dump_data)->data;
  unsigned long flags;
  int i;

  IWL_DEBUG_INFO(fwrt, "WRT radio registers dump\n");

  if (!iwl_trans_grab_nic_access(fwrt->trans, &flags)) {
    return;
  }

  (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_RADIO_REG);
  (*dump_data)->len = cpu_to_le32(RADIO_REG_MAX_READ);

  for (i = 0; i < RADIO_REG_MAX_READ; i++) {
    uint32_t rd_cmd = RADIO_RSP_RD_CMD;

    rd_cmd |= i << RADIO_RSP_ADDR_POS;
    iwl_write_prph_no_grab(fwrt->trans, RSP_RADIO_CMD, rd_cmd);
    *pos = (uint8_t)iwl_read_prph_no_grab(fwrt->trans, RSP_RADIO_RDDAT);

    pos++;
  }

  *dump_data = iwl_fw_error_next_data(*dump_data);

  iwl_trans_release_nic_access(fwrt->trans, &flags);
}

static void iwl_fwrt_dump_rxf(struct iwl_fw_runtime* fwrt,
                              struct iwl_fw_error_dump_data** dump_data, int size, uint32_t offset,
                              int fifo_num) {
  struct iwl_fw_error_dump_fifo* fifo_hdr;
  uint32_t* fifo_data;
  uint32_t fifo_len;
  uint32_t i;

  fifo_hdr = (void*)(*dump_data)->data;
  fifo_data = (void*)fifo_hdr->data;
  fifo_len = size;

  /* No need to try to read the data if the length is 0 */
  if (fifo_len == 0) {
    return;
  }

  /* Add a TLV for the RXF */
  (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_RXF);
  (*dump_data)->len = cpu_to_le32(fifo_len + sizeof(*fifo_hdr));

  fifo_hdr->fifo_num = cpu_to_le32(fifo_num);
  fifo_hdr->available_bytes =
      cpu_to_le32(iwl_trans_read_prph(fwrt->trans, RXF_RD_D_SPACE + offset));
  fifo_hdr->wr_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, RXF_RD_WR_PTR + offset));
  fifo_hdr->rd_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, RXF_RD_RD_PTR + offset));
  fifo_hdr->fence_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, RXF_RD_FENCE_PTR + offset));
  fifo_hdr->fence_mode = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, RXF_SET_FENCE_MODE + offset));

  /* Lock fence */
  iwl_trans_write_prph(fwrt->trans, RXF_SET_FENCE_MODE + offset, 0x1);
  /* Set fence pointer to the same place like WR pointer */
  iwl_trans_write_prph(fwrt->trans, RXF_LD_WR2FENCE + offset, 0x1);
  /* Set fence offset */
  iwl_trans_write_prph(fwrt->trans, RXF_LD_FENCE_OFFSET_ADDR + offset, 0x0);

  /* Read FIFO */
  fifo_len /= sizeof(uint32_t); /* Size in DWORDS */
  for (i = 0; i < fifo_len; i++) {
    fifo_data[i] = iwl_trans_read_prph(fwrt->trans, RXF_FIFO_RD_FENCE_INC + offset);
  }
  *dump_data = iwl_fw_error_next_data(*dump_data);
}

static void iwl_fwrt_dump_txf(struct iwl_fw_runtime* fwrt,
                              struct iwl_fw_error_dump_data** dump_data, int size, uint32_t offset,
                              int fifo_num) {
  struct iwl_fw_error_dump_fifo* fifo_hdr;
  uint32_t* fifo_data;
  uint32_t fifo_len;
  size_t i;

  fifo_hdr = (void*)(*dump_data)->data;
  fifo_data = (void*)fifo_hdr->data;
  fifo_len = size;

  /* No need to try to read the data if the length is 0 */
  if (fifo_len == 0) {
    return;
  }

  /* Add a TLV for the FIFO */
  (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_TXF);
  (*dump_data)->len = cpu_to_le32(fifo_len + sizeof(*fifo_hdr));

  fifo_hdr->fifo_num = cpu_to_le32(fifo_num);
  fifo_hdr->available_bytes =
      cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_FIFO_ITEM_CNT + offset));
  fifo_hdr->wr_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_WR_PTR + offset));
  fifo_hdr->rd_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_RD_PTR + offset));
  fifo_hdr->fence_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_FENCE_PTR + offset));
  fifo_hdr->fence_mode = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_LOCK_FENCE + offset));

  /* Set the TXF_READ_MODIFY_ADDR to TXF_WR_PTR */
  iwl_trans_write_prph(fwrt->trans, TXF_READ_MODIFY_ADDR + offset, TXF_WR_PTR + offset);

  /* Dummy-read to advance the read pointer to the head */
  iwl_trans_read_prph(fwrt->trans, TXF_READ_MODIFY_DATA + offset);

  /* Read FIFO */
  fifo_len /= sizeof(uint32_t); /* Size in DWORDS */
  for (i = 0; i < fifo_len; i++) {
    fifo_data[i] = iwl_trans_read_prph(fwrt->trans, TXF_READ_MODIFY_DATA + offset);
  }
  *dump_data = iwl_fw_error_next_data(*dump_data);
}

static void iwl_fw_dump_rxf(struct iwl_fw_runtime* fwrt,
                            struct iwl_fw_error_dump_data** dump_data) {
  struct iwl_fwrt_shared_mem_cfg* cfg = &fwrt->smem_cfg;
  unsigned long flags;

  IWL_DEBUG_INFO(fwrt, "WRT RX FIFO dump\n");

  if (!iwl_trans_grab_nic_access(fwrt->trans, &flags)) {
    return;
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_RXF)) {
    /* Pull RXF1 */
    iwl_fwrt_dump_rxf(fwrt, dump_data, cfg->lmac[0].rxfifo1_size, 0, 0);
    /* Pull RXF2 */
    iwl_fwrt_dump_rxf(fwrt, dump_data, cfg->rxfifo2_size, RXF_DIFF_FROM_PREV, 1);
    /* Pull LMAC2 RXF1 */
    if (fwrt->smem_cfg.num_lmacs > 1) {
      iwl_fwrt_dump_rxf(fwrt, dump_data, cfg->lmac[1].rxfifo1_size, LMAC2_PRPH_OFFSET, 2);
    }
  }

  iwl_trans_release_nic_access(fwrt->trans, &flags);
}

static void iwl_fw_dump_txf(struct iwl_fw_runtime* fwrt,
                            struct iwl_fw_error_dump_data** dump_data) {
  struct iwl_fw_error_dump_fifo* fifo_hdr;
  struct iwl_fwrt_shared_mem_cfg* cfg = &fwrt->smem_cfg;
  uint32_t* fifo_data;
  uint32_t fifo_len;
  unsigned long flags;
  uint32_t i, j;

  IWL_DEBUG_INFO(fwrt, "WRT TX FIFO dump\n");

  if (!iwl_trans_grab_nic_access(fwrt->trans, &flags)) {
    return;
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_TXF)) {
    /* Pull TXF data from LMAC1 */
    for (i = 0; i < fwrt->smem_cfg.num_txfifo_entries; i++) {
      /* Mark the number of TXF we're pulling now */
      iwl_trans_write_prph(fwrt->trans, TXF_LARC_NUM, i);
      iwl_fwrt_dump_txf(fwrt, dump_data, cfg->lmac[0].txfifo_size[i], 0, i);
    }

    /* Pull TXF data from LMAC2 */
    if (fwrt->smem_cfg.num_lmacs > 1) {
      for (i = 0; i < fwrt->smem_cfg.num_txfifo_entries; i++) {
        /* Mark the number of TXF we're pulling now */
        iwl_trans_write_prph(fwrt->trans, TXF_LARC_NUM + LMAC2_PRPH_OFFSET, i);
        iwl_fwrt_dump_txf(fwrt, dump_data, cfg->lmac[1].txfifo_size[i], LMAC2_PRPH_OFFSET,
                          i + cfg->num_txfifo_entries);
      }
    }
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_INTERNAL_TXF) &&
      fw_has_capa(&fwrt->fw->ucode_capa, IWL_UCODE_TLV_CAPA_EXTEND_SHARED_MEM_CFG)) {
    /* Pull UMAC internal TXF data from all TXFs */
    for (i = 0; i < ARRAY_SIZE(fwrt->smem_cfg.internal_txfifo_size); i++) {
      fifo_hdr = (void*)(*dump_data)->data;
      fifo_data = (void*)fifo_hdr->data;
      fifo_len = fwrt->smem_cfg.internal_txfifo_size[i];

      /* No need to try to read the data if the length is 0 */
      if (fifo_len == 0) {
        continue;
      }

      /* Add a TLV for the internal FIFOs */
      (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_INTERNAL_TXF);
      (*dump_data)->len = cpu_to_le32(fifo_len + sizeof(*fifo_hdr));

      fifo_hdr->fifo_num = cpu_to_le32(i);

      /* Mark the number of TXF we're pulling now */
      iwl_trans_write_prph(fwrt->trans, TXF_CPU2_NUM, i + fwrt->smem_cfg.num_txfifo_entries);

      fifo_hdr->available_bytes =
          cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_CPU2_FIFO_ITEM_CNT));
      fifo_hdr->wr_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_CPU2_WR_PTR));
      fifo_hdr->rd_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_CPU2_RD_PTR));
      fifo_hdr->fence_ptr = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_CPU2_FENCE_PTR));
      fifo_hdr->fence_mode = cpu_to_le32(iwl_trans_read_prph(fwrt->trans, TXF_CPU2_LOCK_FENCE));

      /* Set TXF_CPU2_READ_MODIFY_ADDR to TXF_CPU2_WR_PTR */
      iwl_trans_write_prph(fwrt->trans, TXF_CPU2_READ_MODIFY_ADDR, TXF_CPU2_WR_PTR);

      /* Dummy-read to advance the read pointer to head */
      iwl_trans_read_prph(fwrt->trans, TXF_CPU2_READ_MODIFY_DATA);

      /* Read FIFO */
      fifo_len /= sizeof(uint32_t); /* Size in DWORDS */
      for (j = 0; j < fifo_len; j++) {
        fifo_data[j] = iwl_trans_read_prph(fwrt->trans, TXF_CPU2_READ_MODIFY_DATA);
      }
      *dump_data = iwl_fw_error_next_data(*dump_data);
    }
  }

  iwl_trans_release_nic_access(fwrt->trans, &flags);
}

#define IWL8260_ICCM_OFFSET 0x44000 /* Only for B-step */
#define IWL8260_ICCM_LEN 0xC000     /* Only for B-step */

struct iwl_prph_range {
  uint32_t start, end;
};

static const struct iwl_prph_range iwl_prph_dump_addr_comm[] = {
    {.start = 0x00a00000, .end = 0x00a00000}, {.start = 0x00a0000c, .end = 0x00a00024},
    {.start = 0x00a0002c, .end = 0x00a0003c}, {.start = 0x00a00410, .end = 0x00a00418},
    {.start = 0x00a00420, .end = 0x00a00420}, {.start = 0x00a00428, .end = 0x00a00428},
    {.start = 0x00a00430, .end = 0x00a0043c}, {.start = 0x00a00444, .end = 0x00a00444},
    {.start = 0x00a004c0, .end = 0x00a004cc}, {.start = 0x00a004d8, .end = 0x00a004d8},
    {.start = 0x00a004e0, .end = 0x00a004f0}, {.start = 0x00a00840, .end = 0x00a00840},
    {.start = 0x00a00850, .end = 0x00a00858}, {.start = 0x00a01004, .end = 0x00a01008},
    {.start = 0x00a01010, .end = 0x00a01010}, {.start = 0x00a01018, .end = 0x00a01018},
    {.start = 0x00a01024, .end = 0x00a01024}, {.start = 0x00a0102c, .end = 0x00a01034},
    {.start = 0x00a0103c, .end = 0x00a01040}, {.start = 0x00a01048, .end = 0x00a01094},
    {.start = 0x00a01c00, .end = 0x00a01c20}, {.start = 0x00a01c58, .end = 0x00a01c58},
    {.start = 0x00a01c7c, .end = 0x00a01c7c}, {.start = 0x00a01c28, .end = 0x00a01c54},
    {.start = 0x00a01c5c, .end = 0x00a01c5c}, {.start = 0x00a01c60, .end = 0x00a01cdc},
    {.start = 0x00a01ce0, .end = 0x00a01d0c}, {.start = 0x00a01d18, .end = 0x00a01d20},
    {.start = 0x00a01d2c, .end = 0x00a01d30}, {.start = 0x00a01d40, .end = 0x00a01d5c},
    {.start = 0x00a01d80, .end = 0x00a01d80}, {.start = 0x00a01d98, .end = 0x00a01d9c},
    {.start = 0x00a01da8, .end = 0x00a01da8}, {.start = 0x00a01db8, .end = 0x00a01df4},
    {.start = 0x00a01dc0, .end = 0x00a01dfc}, {.start = 0x00a01e00, .end = 0x00a01e2c},
    {.start = 0x00a01e40, .end = 0x00a01e60}, {.start = 0x00a01e68, .end = 0x00a01e6c},
    {.start = 0x00a01e74, .end = 0x00a01e74}, {.start = 0x00a01e84, .end = 0x00a01e90},
    {.start = 0x00a01e9c, .end = 0x00a01ec4}, {.start = 0x00a01ed0, .end = 0x00a01ee0},
    {.start = 0x00a01f00, .end = 0x00a01f1c}, {.start = 0x00a01f44, .end = 0x00a01ffc},
    {.start = 0x00a02000, .end = 0x00a02048}, {.start = 0x00a02068, .end = 0x00a020f0},
    {.start = 0x00a02100, .end = 0x00a02118}, {.start = 0x00a02140, .end = 0x00a0214c},
    {.start = 0x00a02168, .end = 0x00a0218c}, {.start = 0x00a021c0, .end = 0x00a021c0},
    {.start = 0x00a02400, .end = 0x00a02410}, {.start = 0x00a02418, .end = 0x00a02420},
    {.start = 0x00a02428, .end = 0x00a0242c}, {.start = 0x00a02434, .end = 0x00a02434},
    {.start = 0x00a02440, .end = 0x00a02460}, {.start = 0x00a02468, .end = 0x00a024b0},
    {.start = 0x00a024c8, .end = 0x00a024cc}, {.start = 0x00a02500, .end = 0x00a02504},
    {.start = 0x00a0250c, .end = 0x00a02510}, {.start = 0x00a02540, .end = 0x00a02554},
    {.start = 0x00a02580, .end = 0x00a025f4}, {.start = 0x00a02600, .end = 0x00a0260c},
    {.start = 0x00a02648, .end = 0x00a02650}, {.start = 0x00a02680, .end = 0x00a02680},
    {.start = 0x00a026c0, .end = 0x00a026d0}, {.start = 0x00a02700, .end = 0x00a0270c},
    {.start = 0x00a02804, .end = 0x00a02804}, {.start = 0x00a02818, .end = 0x00a0281c},
    {.start = 0x00a02c00, .end = 0x00a02db4}, {.start = 0x00a02df4, .end = 0x00a02fb0},
    {.start = 0x00a03000, .end = 0x00a03014}, {.start = 0x00a0301c, .end = 0x00a0302c},
    {.start = 0x00a03034, .end = 0x00a03038}, {.start = 0x00a03040, .end = 0x00a03048},
    {.start = 0x00a03060, .end = 0x00a03068}, {.start = 0x00a03070, .end = 0x00a03074},
    {.start = 0x00a0307c, .end = 0x00a0307c}, {.start = 0x00a03080, .end = 0x00a03084},
    {.start = 0x00a0308c, .end = 0x00a03090}, {.start = 0x00a03098, .end = 0x00a03098},
    {.start = 0x00a030a0, .end = 0x00a030a0}, {.start = 0x00a030a8, .end = 0x00a030b4},
    {.start = 0x00a030bc, .end = 0x00a030bc}, {.start = 0x00a030c0, .end = 0x00a0312c},
    {.start = 0x00a03c00, .end = 0x00a03c5c}, {.start = 0x00a04400, .end = 0x00a04454},
    {.start = 0x00a04460, .end = 0x00a04474}, {.start = 0x00a044c0, .end = 0x00a044ec},
    {.start = 0x00a04500, .end = 0x00a04504}, {.start = 0x00a04510, .end = 0x00a04538},
    {.start = 0x00a04540, .end = 0x00a04548}, {.start = 0x00a04560, .end = 0x00a0457c},
    {.start = 0x00a04590, .end = 0x00a04598}, {.start = 0x00a045c0, .end = 0x00a045f4},
};

static const struct iwl_prph_range iwl_prph_dump_addr_9000[] = {
    {.start = 0x00a05c00, .end = 0x00a05c18},
    {.start = 0x00a05400, .end = 0x00a056e8},
    {.start = 0x00a08000, .end = 0x00a098bc},
    {.start = 0x00a02400, .end = 0x00a02758},
};

static const struct iwl_prph_range iwl_prph_dump_addr_22000[] = {
    {.start = 0x00a00000, .end = 0x00a00000}, {.start = 0x00a0000c, .end = 0x00a00024},
    {.start = 0x00a0002c, .end = 0x00a00034}, {.start = 0x00a0003c, .end = 0x00a0003c},
    {.start = 0x00a00410, .end = 0x00a00418}, {.start = 0x00a00420, .end = 0x00a00420},
    {.start = 0x00a00428, .end = 0x00a00428}, {.start = 0x00a00430, .end = 0x00a0043c},
    {.start = 0x00a00444, .end = 0x00a00444}, {.start = 0x00a00840, .end = 0x00a00840},
    {.start = 0x00a00850, .end = 0x00a00858}, {.start = 0x00a01004, .end = 0x00a01008},
    {.start = 0x00a01010, .end = 0x00a01010}, {.start = 0x00a01018, .end = 0x00a01018},
    {.start = 0x00a01024, .end = 0x00a01024}, {.start = 0x00a0102c, .end = 0x00a01034},
    {.start = 0x00a0103c, .end = 0x00a01040}, {.start = 0x00a01048, .end = 0x00a01050},
    {.start = 0x00a01058, .end = 0x00a01058}, {.start = 0x00a01060, .end = 0x00a01070},
    {.start = 0x00a0108c, .end = 0x00a0108c}, {.start = 0x00a01c20, .end = 0x00a01c28},
    {.start = 0x00a01d10, .end = 0x00a01d10}, {.start = 0x00a01e28, .end = 0x00a01e2c},
    {.start = 0x00a01e60, .end = 0x00a01e60}, {.start = 0x00a01e80, .end = 0x00a01e80},
    {.start = 0x00a01ea0, .end = 0x00a01ea0}, {.start = 0x00a02000, .end = 0x00a0201c},
    {.start = 0x00a02024, .end = 0x00a02024}, {.start = 0x00a02040, .end = 0x00a02048},
    {.start = 0x00a020c0, .end = 0x00a020e0}, {.start = 0x00a02400, .end = 0x00a02404},
    {.start = 0x00a0240c, .end = 0x00a02414}, {.start = 0x00a0241c, .end = 0x00a0243c},
    {.start = 0x00a02448, .end = 0x00a024bc}, {.start = 0x00a024c4, .end = 0x00a024cc},
    {.start = 0x00a02508, .end = 0x00a02508}, {.start = 0x00a02510, .end = 0x00a02514},
    {.start = 0x00a0251c, .end = 0x00a0251c}, {.start = 0x00a0252c, .end = 0x00a0255c},
    {.start = 0x00a02564, .end = 0x00a025a0}, {.start = 0x00a025a8, .end = 0x00a025b4},
    {.start = 0x00a025c0, .end = 0x00a025c0}, {.start = 0x00a025e8, .end = 0x00a025f4},
    {.start = 0x00a02c08, .end = 0x00a02c18}, {.start = 0x00a02c2c, .end = 0x00a02c38},
    {.start = 0x00a02c68, .end = 0x00a02c78}, {.start = 0x00a03000, .end = 0x00a03000},
    {.start = 0x00a03010, .end = 0x00a03014}, {.start = 0x00a0301c, .end = 0x00a0302c},
    {.start = 0x00a03034, .end = 0x00a03038}, {.start = 0x00a03040, .end = 0x00a03044},
    {.start = 0x00a03060, .end = 0x00a03068}, {.start = 0x00a03070, .end = 0x00a03070},
    {.start = 0x00a0307c, .end = 0x00a03084}, {.start = 0x00a0308c, .end = 0x00a03090},
    {.start = 0x00a03098, .end = 0x00a03098}, {.start = 0x00a030a0, .end = 0x00a030a0},
    {.start = 0x00a030a8, .end = 0x00a030b4}, {.start = 0x00a030bc, .end = 0x00a030c0},
    {.start = 0x00a030c8, .end = 0x00a030f4}, {.start = 0x00a03100, .end = 0x00a0312c},
    {.start = 0x00a03c00, .end = 0x00a03c5c}, {.start = 0x00a04400, .end = 0x00a04454},
    {.start = 0x00a04460, .end = 0x00a04474}, {.start = 0x00a044c0, .end = 0x00a044ec},
    {.start = 0x00a04500, .end = 0x00a04504}, {.start = 0x00a04510, .end = 0x00a04538},
    {.start = 0x00a04540, .end = 0x00a04548}, {.start = 0x00a04560, .end = 0x00a04560},
    {.start = 0x00a04570, .end = 0x00a0457c}, {.start = 0x00a04590, .end = 0x00a04590},
    {.start = 0x00a04598, .end = 0x00a04598}, {.start = 0x00a045c0, .end = 0x00a045f4},
    {.start = 0x00a0c000, .end = 0x00a0c018}, {.start = 0x00a0c020, .end = 0x00a0c028},
    {.start = 0x00a0c038, .end = 0x00a0c094}, {.start = 0x00a0c0c0, .end = 0x00a0c104},
    {.start = 0x00a0c10c, .end = 0x00a0c118}, {.start = 0x00a0c150, .end = 0x00a0c174},
    {.start = 0x00a0c17c, .end = 0x00a0c188}, {.start = 0x00a0c190, .end = 0x00a0c198},
    {.start = 0x00a0c1a0, .end = 0x00a0c1a8}, {.start = 0x00a0c1b0, .end = 0x00a0c1b8},
};

static const struct iwl_prph_range iwl_prph_dump_addr_ax210[] = {
	{ .start = 0x00d03c00, .end = 0x00d03c64 },
	{ .start = 0x00d05c18, .end = 0x00d05c1c },
	{ .start = 0x00d0c000, .end = 0x00d0c174 },
};

static void iwl_read_prph_block(struct iwl_trans* trans, uint32_t start, uint32_t len_bytes,
                                __le32* data) {
  uint32_t i;

  for (i = 0; i < len_bytes; i += 4) {
    *data++ = cpu_to_le32(iwl_read_prph_no_grab(trans, start + i));
  }
}

static void iwl_dump_prph(struct iwl_fw_runtime* fwrt,
                          const struct iwl_prph_range* iwl_prph_dump_addr, uint32_t range_len,
                          void* ptr) {
  struct iwl_fw_error_dump_prph* prph;
  struct iwl_trans* trans = fwrt->trans;
  struct iwl_fw_error_dump_data** data = (struct iwl_fw_error_dump_data**)ptr;
  unsigned long flags;
  uint32_t i;

  if (!data) {
    return;
  }

  IWL_DEBUG_INFO(trans, "WRT PRPH dump\n");

  if (!iwl_trans_grab_nic_access(trans, &flags)) {
    return;
  }

  for (i = 0; i < range_len; i++) {
    /* The range includes both boundaries */
    int num_bytes_in_chunk = iwl_prph_dump_addr[i].end - iwl_prph_dump_addr[i].start + 4;

    (*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_PRPH);
    (*data)->len = cpu_to_le32(sizeof(*prph) + num_bytes_in_chunk);
    prph = (void*)(*data)->data;
    prph->prph_start = cpu_to_le32(iwl_prph_dump_addr[i].start);

    iwl_read_prph_block(trans, iwl_prph_dump_addr[i].start,
                        /* our range is inclusive, hence + 4 */
                        iwl_prph_dump_addr[i].end - iwl_prph_dump_addr[i].start + 4,
                        (void*)prph->data);

    *data = iwl_fw_error_next_data(*data);
  }

  iwl_trans_release_nic_access(trans, &flags);
}

static void iwl_fw_get_prph_len(struct iwl_fw_runtime* fwrt,
                                const struct iwl_prph_range* iwl_prph_dump_addr, uint32_t range_len,
                                void* ptr) {
  uint32_t* prph_len = (uint32_t*)ptr;
  uint32_t i, num_bytes_in_chunk;

  if (!prph_len) {
    return;
  }

  for (i = 0; i < range_len; i++) {
    /* The range includes both boundaries */
    num_bytes_in_chunk = iwl_prph_dump_addr[i].end - iwl_prph_dump_addr[i].start + 4;

    *prph_len += sizeof(struct iwl_fw_error_dump_data) + sizeof(struct iwl_fw_error_dump_prph) +
                 num_bytes_in_chunk;
  }
}

static void iwl_fw_prph_handler(struct iwl_fw_runtime* fwrt, void* ptr,
                                void (*handler)(struct iwl_fw_runtime*,
                                                const struct iwl_prph_range*, uint32_t, void*)) {
  uint32_t range_len;

	if (fwrt->trans->trans_cfg->device_family >= IWL_DEVICE_FAMILY_AX210) {
		range_len = ARRAY_SIZE(iwl_prph_dump_addr_ax210);
		handler(fwrt, iwl_prph_dump_addr_ax210, range_len, ptr);
	} else if (fwrt->trans->trans_cfg->device_family >=
		   IWL_DEVICE_FAMILY_22000) {
    range_len = ARRAY_SIZE(iwl_prph_dump_addr_22000);
    handler(fwrt, iwl_prph_dump_addr_22000, range_len, ptr);
  } else {
    range_len = ARRAY_SIZE(iwl_prph_dump_addr_comm);
    handler(fwrt, iwl_prph_dump_addr_comm, range_len, ptr);

    if (fwrt->trans->trans_cfg->mq_rx_supported) {
      range_len = ARRAY_SIZE(iwl_prph_dump_addr_9000);
      handler(fwrt, iwl_prph_dump_addr_9000, range_len, ptr);
    }
  }
}

static void iwl_fw_dump_mem(struct iwl_fw_runtime* fwrt, struct iwl_fw_error_dump_data** dump_data,
                            uint32_t len, uint32_t ofs, uint32_t type) {
  struct iwl_fw_error_dump_mem* dump_mem;

  if (!len) {
    return;
  }

  (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_MEM);
  (*dump_data)->len = cpu_to_le32(len + sizeof(*dump_mem));
  dump_mem = (void*)(*dump_data)->data;
  dump_mem->type = cpu_to_le32(type);
  dump_mem->offset = cpu_to_le32(ofs);
  iwl_trans_read_mem_bytes(fwrt->trans, ofs, dump_mem->data, len);
  *dump_data = iwl_fw_error_next_data(*dump_data);

  IWL_DEBUG_INFO(fwrt, "WRT memory dump. Type=%u\n", dump_mem->type);
}

static void iwl_fw_dump_named_mem(struct iwl_fw_runtime* fwrt,
                                  struct iwl_fw_error_dump_data** dump_data, uint32_t len,
                                  uint32_t ofs, uint8_t* name, uint8_t name_len) {
  struct iwl_fw_error_dump_named_mem* dump_mem;

  if (!len) {
    return;
  }

  (*dump_data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_MEM);
  (*dump_data)->len = cpu_to_le32(len + sizeof(*dump_mem));
  dump_mem = (void*)(*dump_data)->data;
  dump_mem->type = cpu_to_le32(IWL_FW_ERROR_DUMP_MEM_NAMED_MEM);
  dump_mem->offset = cpu_to_le32(ofs);
  dump_mem->name_len = name_len;
  memcpy(dump_mem->name, name, name_len);
  iwl_trans_read_mem_bytes(fwrt->trans, ofs, dump_mem->data, len);
  *dump_data = iwl_fw_error_next_data(*dump_data);

  IWL_DEBUG_INFO(fwrt, "WRT memory dump. Type=%u\n", dump_mem->type);
}

#define ADD_LEN(len, item_len, const_len) \
  do {                                    \
    size_t item = item_len;               \
    len += (!!item) * const_len + item;   \
  } while (0)

static int iwl_fw_rxf_len(struct iwl_fw_runtime* fwrt, struct iwl_fwrt_shared_mem_cfg* mem_cfg) {
  size_t hdr_len = sizeof(struct iwl_fw_error_dump_data) + sizeof(struct iwl_fw_error_dump_fifo);
  uint32_t fifo_len = 0;
  uint32_t i;

  if (!iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_RXF)) {
    return 0;
  }

  /* Count RXF2 size */
  ADD_LEN(fifo_len, mem_cfg->rxfifo2_size, hdr_len);

  /* Count RXF1 sizes */
  if (WARN_ON(mem_cfg->num_lmacs > MAX_NUM_LMAC)) {
    mem_cfg->num_lmacs = MAX_NUM_LMAC;
  }

  for (i = 0; i < mem_cfg->num_lmacs; i++) {
    ADD_LEN(fifo_len, mem_cfg->lmac[i].rxfifo1_size, hdr_len);
  }

  return fifo_len;
}

static int iwl_fw_txf_len(struct iwl_fw_runtime* fwrt, struct iwl_fwrt_shared_mem_cfg* mem_cfg) {
  size_t hdr_len = sizeof(struct iwl_fw_error_dump_data) + sizeof(struct iwl_fw_error_dump_fifo);
  uint32_t fifo_len = 0;
  uint32_t i;

  if (!iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_TXF)) {
    goto dump_internal_txf;
  }

  /* Count TXF sizes */
  if (WARN_ON(mem_cfg->num_lmacs > MAX_NUM_LMAC)) {
    mem_cfg->num_lmacs = MAX_NUM_LMAC;
  }

  for (i = 0; i < mem_cfg->num_lmacs; i++) {
    uint32_t j;

    for (j = 0; j < mem_cfg->num_txfifo_entries; j++) {
      ADD_LEN(fifo_len, mem_cfg->lmac[i].txfifo_size[j], hdr_len);
    }
  }

dump_internal_txf:
  if (!(iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_INTERNAL_TXF) &&
        fw_has_capa(&fwrt->fw->ucode_capa, IWL_UCODE_TLV_CAPA_EXTEND_SHARED_MEM_CFG))) {
    goto out;
  }

  for (size_t i = 0; i < ARRAY_SIZE(mem_cfg->internal_txfifo_size); i++) {
    ADD_LEN(fifo_len, mem_cfg->internal_txfifo_size[i], hdr_len);
  }

out:
  return fifo_len;
}

static void iwl_dump_paging(struct iwl_fw_runtime* fwrt, struct iwl_fw_error_dump_data** data) {
  size_t i;

  IWL_DEBUG_INFO(fwrt, "WRT paging dump\n");
  for (i = 1; i < fwrt->num_of_paging_blk + 1; i++) {
    struct iwl_fw_error_dump_paging* paging;
    struct iwl_iobuf* iobuf = fwrt->fw_paging_db[i].io_buf;

    (*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_PAGING);
    (*data)->len = cpu_to_le32(sizeof(*paging) + PAGING_BLOCK_SIZE);
    paging = (void*)(*data)->data;
    paging->index = cpu_to_le32(i);
    iwl_iobuf_cache_flush(iobuf, 0, PAGING_BLOCK_SIZE);
    memcpy(paging->data, iwl_iobuf_virtual(iobuf), PAGING_BLOCK_SIZE);
    (*data) = iwl_fw_error_next_data(*data);
  }
}

static struct iwl_fw_error_dump_file* _iwl_fw_error_dump(struct iwl_fw_runtime* fwrt,
                                                         struct iwl_fw_dump_ptrs* fw_error_dump) {
  struct iwl_fw_error_dump_file* dump_file;
  struct iwl_fw_error_dump_data* dump_data;
  struct iwl_fw_error_dump_info* dump_info;
  struct iwl_fw_error_dump_smem_cfg* dump_smem_cfg;
  struct iwl_fw_error_dump_trigger_desc* dump_trig;
  uint32_t sram_len, sram_ofs;
  const struct iwl_fw_dbg_mem_seg_tlv* fw_mem = fwrt->fw->dbg.mem_tlv;
  struct iwl_fwrt_shared_mem_cfg* mem_cfg = &fwrt->smem_cfg;
  uint32_t file_len, fifo_len = 0, prph_len = 0, radio_len = 0;
  uint32_t smem_len = fwrt->fw->dbg.n_mem_tlv ? 0 : fwrt->trans->cfg->smem_len;
  uint32_t sram2_len = fwrt->fw->dbg.n_mem_tlv ? 0 : fwrt->trans->cfg->dccm2_len;
  uint32_t i;

  /* SRAM - include stack CCM if driver knows the values for it */
  if (!fwrt->trans->cfg->dccm_offset || !fwrt->trans->cfg->dccm_len) {
    const struct fw_img* img;

    if (fwrt->cur_fw_img >= IWL_UCODE_TYPE_MAX) {
      return NULL;
    }
    img = &fwrt->fw->img[fwrt->cur_fw_img];
    sram_ofs = img->sec[IWL_UCODE_SECTION_DATA].offset;
    sram_len = img->sec[IWL_UCODE_SECTION_DATA].len;
  } else {
    sram_ofs = fwrt->trans->cfg->dccm_offset;
    sram_len = fwrt->trans->cfg->dccm_len;
  }

  /* reading RXF/TXF sizes */
  if (test_bit(STATUS_FW_ERROR, &fwrt->trans->status)) {
    fifo_len = iwl_fw_rxf_len(fwrt, mem_cfg);
    fifo_len += iwl_fw_txf_len(fwrt, mem_cfg);

    /* Make room for PRPH registers */
    if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_PRPH)) {
      iwl_fw_prph_handler(fwrt, &prph_len, iwl_fw_get_prph_len);
    }

		if (fwrt->trans->trans_cfg->device_family ==
		    IWL_DEVICE_FAMILY_7000 &&
		    iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_RADIO_REG)) {
      radio_len = sizeof(*dump_data) + RADIO_REG_MAX_READ;
    }
  }

  file_len = sizeof(*dump_file) + fifo_len + prph_len + radio_len;

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_DEV_FW_INFO)) {
    file_len += sizeof(*dump_data) + sizeof(*dump_info);
  }
  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_MEM_CFG)) {
    file_len += sizeof(*dump_data) + sizeof(*dump_smem_cfg);
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_MEM)) {
    size_t hdr_len = sizeof(*dump_data) + sizeof(struct iwl_fw_error_dump_mem);

    /* Dump SRAM only if no mem_tlvs */
    if (!fwrt->fw->dbg.n_mem_tlv) {
      ADD_LEN(file_len, sram_len, hdr_len);
    }

    /* Make room for all mem types that exist */
    ADD_LEN(file_len, smem_len, hdr_len);
    ADD_LEN(file_len, sram2_len, hdr_len);

    for (i = 0; i < fwrt->fw->dbg.n_mem_tlv; i++) {
      ADD_LEN(file_len, le32_to_cpu(fw_mem[i].len), hdr_len);
    }
  }

  /* Make room for fw's virtual image pages, if it exists */
  if (iwl_fw_dbg_is_paging_enabled(fwrt))
    file_len += fwrt->num_of_paging_blk *
                (sizeof(*dump_data) + sizeof(struct iwl_fw_error_dump_paging) + PAGING_BLOCK_SIZE);

  if (iwl_fw_dbg_is_d3_debug_enabled(fwrt) && fwrt->dump.d3_debug_data) {
    file_len += sizeof(*dump_data) + fwrt->trans->cfg->d3_debug_data_length * 2;
  }

  /* If we only want a monitor dump, reset the file length */
  if (fwrt->dump.monitor_only) {
    file_len =
        sizeof(*dump_file) + sizeof(*dump_data) * 2 + sizeof(*dump_info) + sizeof(*dump_smem_cfg);
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_ERROR_INFO) && fwrt->dump.desc) {
    file_len += sizeof(*dump_data) + sizeof(*dump_trig) + fwrt->dump.desc->len;
  }

  dump_file = malloc(file_len);
  if (!dump_file) {
    return NULL;
  }

  fw_error_dump->fwrt_ptr = dump_file;

  dump_file->barker = cpu_to_le32(IWL_FW_ERROR_DUMP_BARKER);
  dump_data = (void*)dump_file->data;

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_DEV_FW_INFO)) {
    dump_data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_DEV_FW_INFO);
    dump_data->len = cpu_to_le32(sizeof(*dump_info));
    dump_info = (void*)dump_data->data;
		dump_info->hw_type =
			cpu_to_le32(CSR_HW_REV_TYPE(fwrt->trans->hw_rev));
		dump_info->hw_step =
			cpu_to_le32(fwrt->trans->hw_rev_step);
    memcpy(dump_info->fw_human_readable, fwrt->fw->human_readable,
           sizeof(dump_info->fw_human_readable));
    strncpy((char*)dump_info->dev_human_readable, fwrt->trans->cfg->name,
            sizeof(dump_info->dev_human_readable) - 1);
    strncpy((char*)dump_info->bus_human_readable, "<fuchsia>",
            sizeof(dump_info->bus_human_readable) - 1);
    dump_info->num_of_lmacs = (uint8_t)fwrt->smem_cfg.num_lmacs;
    dump_info->lmac_err_id[0] = cpu_to_le32(fwrt->dump.lmac_err_id[0]);
    if (fwrt->smem_cfg.num_lmacs > 1) {
      dump_info->lmac_err_id[1] = cpu_to_le32(fwrt->dump.lmac_err_id[1]);
    }
    dump_info->umac_err_id = cpu_to_le32(fwrt->dump.umac_err_id);

    dump_data = iwl_fw_error_next_data(dump_data);
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_MEM_CFG)) {
    /* Dump shared memory configuration */
    dump_data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_MEM_CFG);
    dump_data->len = cpu_to_le32(sizeof(*dump_smem_cfg));
    dump_smem_cfg = (void*)dump_data->data;
    dump_smem_cfg->num_lmacs = cpu_to_le32(mem_cfg->num_lmacs);
    dump_smem_cfg->num_txfifo_entries = cpu_to_le32(mem_cfg->num_txfifo_entries);
    for (i = 0; i < MAX_NUM_LMAC; i++) {
      int j;
      uint32_t* txf_size = mem_cfg->lmac[i].txfifo_size;

      for (j = 0; j < TX_FIFO_MAX_NUM; j++) {
        dump_smem_cfg->lmac[i].txfifo_size[j] = cpu_to_le32(txf_size[j]);
      }
      dump_smem_cfg->lmac[i].rxfifo1_size = cpu_to_le32(mem_cfg->lmac[i].rxfifo1_size);
    }
    dump_smem_cfg->rxfifo2_size = cpu_to_le32(mem_cfg->rxfifo2_size);
    dump_smem_cfg->internal_txfifo_addr = cpu_to_le32(mem_cfg->internal_txfifo_addr);
    for (i = 0; i < TX_FIFO_INTERNAL_MAX_NUM; i++) {
      dump_smem_cfg->internal_txfifo_size[i] = cpu_to_le32(mem_cfg->internal_txfifo_size[i]);
    }

    dump_data = iwl_fw_error_next_data(dump_data);
  }

  /* We only dump the FIFOs if the FW is in error state */
  if (fifo_len) {
    iwl_fw_dump_rxf(fwrt, &dump_data);
    iwl_fw_dump_txf(fwrt, &dump_data);
    if (radio_len) {
      iwl_read_radio_regs(fwrt, &dump_data);
    }
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_ERROR_INFO) && fwrt->dump.desc) {
    dump_data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_ERROR_INFO);
    dump_data->len = cpu_to_le32(sizeof(*dump_trig) + fwrt->dump.desc->len);
    dump_trig = (void*)dump_data->data;
    memcpy(dump_trig, &fwrt->dump.desc->trig_desc, sizeof(*dump_trig) + fwrt->dump.desc->len);

    dump_data = iwl_fw_error_next_data(dump_data);
  }

  /* In case we only want monitor dump, skip to dump trasport data */
  if (fwrt->dump.monitor_only) {
    goto out;
  }

  if (iwl_fw_dbg_type_on(fwrt, IWL_FW_ERROR_DUMP_MEM)) {
    const struct iwl_fw_dbg_mem_seg_tlv* fw_dbg_mem = fwrt->fw->dbg.mem_tlv;

    if (!fwrt->fw->dbg.n_mem_tlv) {
      iwl_fw_dump_mem(fwrt, &dump_data, sram_len, sram_ofs, IWL_FW_ERROR_DUMP_MEM_SRAM);
    }

    for (i = 0; i < fwrt->fw->dbg.n_mem_tlv; i++) {
      uint32_t len = le32_to_cpu(fw_dbg_mem[i].len);
      uint32_t ofs = le32_to_cpu(fw_dbg_mem[i].ofs);

      iwl_fw_dump_mem(fwrt, &dump_data, len, ofs, le32_to_cpu(fw_dbg_mem[i].data_type));
    }

    iwl_fw_dump_mem(fwrt, &dump_data, smem_len, fwrt->trans->cfg->smem_offset,
                    IWL_FW_ERROR_DUMP_MEM_SMEM);

    iwl_fw_dump_mem(fwrt, &dump_data, sram2_len, fwrt->trans->cfg->dccm2_offset,
                    IWL_FW_ERROR_DUMP_MEM_SRAM);
  }

  if (iwl_fw_dbg_is_d3_debug_enabled(fwrt) && fwrt->dump.d3_debug_data) {
    uint32_t addr = fwrt->trans->cfg->d3_debug_data_base_addr;
    size_t data_size = fwrt->trans->cfg->d3_debug_data_length;

    dump_data->type = cpu_to_le32(IWL_FW_ERROR_DUMP_D3_DEBUG_DATA);
    dump_data->len = cpu_to_le32(data_size * 2);

    memcpy(dump_data->data, fwrt->dump.d3_debug_data, data_size);

    kfree(fwrt->dump.d3_debug_data);
    fwrt->dump.d3_debug_data = NULL;

    iwl_trans_read_mem_bytes(fwrt->trans, addr, dump_data->data + data_size, data_size);

    dump_data = iwl_fw_error_next_data(dump_data);
  }

  /* Dump fw's virtual image */
  if (iwl_fw_dbg_is_paging_enabled(fwrt)) {
    iwl_dump_paging(fwrt, &dump_data);
  }

  if (prph_len) {
    iwl_fw_prph_handler(fwrt, &dump_data, iwl_dump_prph);
  }

out:
  dump_file->file_len = cpu_to_le32(file_len);
  return dump_file;
}

static void iwl_dump_prph_ini(struct iwl_trans* trans, struct iwl_fw_error_dump_data** data,
                              struct iwl_fw_ini_region_cfg* reg) {
  struct iwl_fw_error_dump_prph* prph;
  unsigned long flags;
  uint32_t i, size = le32_to_cpu(reg->num_regions);

  IWL_DEBUG_INFO(trans, "WRT PRPH dump\n");

  if (!iwl_trans_grab_nic_access(trans, &flags)) {
    return;
  }

  for (i = 0; i < size; i++) {
    (*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_PRPH);
    (*data)->len = cpu_to_le32(le32_to_cpu(reg->size) + sizeof(*prph));
    prph = (void*)(*data)->data;
    prph->prph_start = reg->start_addr[i];
    prph->data[0] = cpu_to_le32(iwl_read_prph_no_grab(trans, le32_to_cpu(prph->prph_start)));
    *data = iwl_fw_error_next_data(*data);
  }
  iwl_trans_release_nic_access(trans, &flags);
}

static void iwl_dump_csr_ini(struct iwl_trans* trans, struct iwl_fw_error_dump_data** data,
                             struct iwl_fw_ini_region_cfg* reg) {
  int i, num = le32_to_cpu(reg->num_regions);
  uint32_t size = le32_to_cpu(reg->size);

  IWL_DEBUG_INFO(trans, "WRT CSR dump\n");

  for (i = 0; i < num; i++) {
    uint32_t add = le32_to_cpu(reg->start_addr[i]);
    __le32* val;
    uint32_t j;

    (*data)->type = cpu_to_le32(IWL_FW_ERROR_DUMP_CSR);
    (*data)->len = cpu_to_le32(size);
    val = (void*)(*data)->data;

    for (j = 0; j < size; j += 4) {
      *val++ = cpu_to_le32(iwl_trans_read32(trans, j + add));
    }

    *data = iwl_fw_error_next_data(*data);
  }
}

static int iwl_fw_ini_get_trigger_len(struct iwl_fw_runtime* fwrt,
                                      struct iwl_fw_ini_trigger* trigger) {
  int i, num, size = 0, hdr_len = sizeof(struct iwl_fw_error_dump_data);

  if (!trigger || !trigger->num_regions) {
    return 0;
  }

  num = le32_to_cpu(trigger->num_regions);
  for (i = 0; i < num; i++) {
    uint32_t reg_id = le32_to_cpu(trigger->data[i]);
    struct iwl_fw_ini_region_cfg* reg;
    enum iwl_fw_ini_region_type type;
    uint32_t num_entries;

    if (WARN_ON(reg_id >= ARRAY_SIZE(fwrt->dump.active_regs))) {
      continue;
    }

    reg = fwrt->dump.active_regs[reg_id].reg;
    if (WARN(!reg, "Unassigned region %d\n", reg_id)) {
      continue;
    }

    type = le32_to_cpu(reg->region_type);
    num_entries = le32_to_cpu(reg->num_regions);

    switch (type) {
      case IWL_FW_INI_REGION_DEVICE_MEMORY:
        size += hdr_len + sizeof(struct iwl_fw_error_dump_named_mem) + le32_to_cpu(reg->size);
        break;
      case IWL_FW_INI_REGION_PERIPHERY_MAC:
      case IWL_FW_INI_REGION_PERIPHERY_PHY:
      case IWL_FW_INI_REGION_PERIPHERY_AUX:
        size += num_entries * (hdr_len + sizeof(struct iwl_fw_error_dump_prph) + sizeof(uint32_t));
        break;
      case IWL_FW_INI_REGION_TXF:
        size += iwl_fw_txf_len(fwrt, &fwrt->smem_cfg);
        break;
      case IWL_FW_INI_REGION_RXF:
        size += iwl_fw_rxf_len(fwrt, &fwrt->smem_cfg);
        break;
      case IWL_FW_INI_REGION_PAGING:
        if (!iwl_fw_dbg_is_paging_enabled(fwrt)) {
          break;
        }
        size += fwrt->num_of_paging_blk *
                (hdr_len + sizeof(struct iwl_fw_error_dump_paging) + PAGING_BLOCK_SIZE);
        break;
      case IWL_FW_INI_REGION_CSR:
        size += num_entries * (hdr_len + le32_to_cpu(reg->size));
        break;
      case IWL_FW_INI_REGION_DRAM_BUFFER:
      /* Transport takes care of DRAM dumping */
      case IWL_FW_INI_REGION_INTERNAL_BUFFER:
      case IWL_FW_INI_REGION_DRAM_IMR:
      /* Undefined yet */
      default:
        break;
    }
  }
  return size;
}

static void iwl_fw_ini_dump_trigger(struct iwl_fw_runtime* fwrt, struct iwl_fw_ini_trigger* trigger,
                                    struct iwl_fw_error_dump_data** data, uint32_t* dump_mask) {
  int i, num = le32_to_cpu(trigger->num_regions);

  for (i = 0; i < num; i++) {
    uint32_t reg_id = le32_to_cpu(trigger->data[i]);
    enum iwl_fw_ini_region_type type;
    struct iwl_fw_ini_region_cfg* reg;

    if (reg_id >= ARRAY_SIZE(fwrt->dump.active_regs)) {
      continue;
    }

    reg = fwrt->dump.active_regs[reg_id].reg;
    /* Don't warn, get_trigger_len already warned */
    if (!reg) {
      continue;
    }

    type = le32_to_cpu(reg->region_type);
    switch (type) {
      case IWL_FW_INI_REGION_DEVICE_MEMORY:
        if (WARN_ON(le32_to_cpu(reg->num_regions) > 1)) {
          continue;
        }
        iwl_fw_dump_named_mem(fwrt, data, le32_to_cpu(reg->size), le32_to_cpu(reg->start_addr[0]),
                              reg->name, (uint8_t)le32_to_cpu(reg->name_len));
        break;
      case IWL_FW_INI_REGION_PERIPHERY_MAC:
      case IWL_FW_INI_REGION_PERIPHERY_PHY:
      case IWL_FW_INI_REGION_PERIPHERY_AUX:
        iwl_dump_prph_ini(fwrt->trans, data, reg);
        break;
      case IWL_FW_INI_REGION_DRAM_BUFFER:
        *dump_mask |= IWL_FW_ERROR_DUMP_FW_MONITOR;
        break;
      case IWL_FW_INI_REGION_PAGING:
        if (iwl_fw_dbg_is_paging_enabled(fwrt)) {
          iwl_dump_paging(fwrt, data);
        } else {
          *dump_mask |= IWL_FW_ERROR_DUMP_PAGING;
        }
        break;
      case IWL_FW_INI_REGION_TXF:
        iwl_fw_dump_txf(fwrt, data);
        break;
      case IWL_FW_INI_REGION_RXF:
        iwl_fw_dump_rxf(fwrt, data);
        break;
      case IWL_FW_INI_REGION_CSR:
        iwl_dump_csr_ini(fwrt->trans, data, reg);
        break;
      case IWL_FW_INI_REGION_DRAM_IMR:
      case IWL_FW_INI_REGION_INTERNAL_BUFFER:
      /* This is undefined yet */
      default:
        break;
    }
  }
}

static struct iwl_fw_error_dump_file* _iwl_fw_error_ini_dump(struct iwl_fw_runtime* fwrt,
                                                             struct iwl_fw_dump_ptrs* fw_error_dump,
                                                             uint32_t* dump_mask) {
  uint32_t size, id = le32_to_cpu(fwrt->dump.desc->trig_desc.type);
  struct iwl_fw_error_dump_data* dump_data;
  struct iwl_fw_error_dump_file* dump_file;
  struct iwl_fw_ini_trigger *trigger, *ext;

  if (id == FW_DBG_TRIGGER_FW_ASSERT) {
    id = IWL_FW_TRIGGER_ID_FW_ASSERT;
  } else if (id == FW_DBG_TRIGGER_USER) {
    id = IWL_FW_TRIGGER_ID_USER_TRIGGER;
  } else if (id < FW_DBG_TRIGGER_MAX) {
    return NULL;
  }

  if (WARN_ON(id >= ARRAY_SIZE(fwrt->dump.active_trigs))) {
    return NULL;
  }

  trigger = fwrt->dump.active_trigs[id].conf;
  ext = fwrt->dump.active_trigs[id].conf_ext;

  size = sizeof(*dump_file);
  size += iwl_fw_ini_get_trigger_len(fwrt, trigger);
  size += iwl_fw_ini_get_trigger_len(fwrt, ext);

  if (!size) {
    return NULL;
  }

  dump_file = malloc(size);
  if (!dump_file) {
    return NULL;
  }

  fw_error_dump->fwrt_ptr = dump_file;

  dump_file->barker = cpu_to_le32(IWL_FW_ERROR_DUMP_BARKER);
  dump_data = (void*)dump_file->data;
  dump_file->file_len = cpu_to_le32(size);

  *dump_mask = 0;
  if (trigger) {
    iwl_fw_ini_dump_trigger(fwrt, trigger, &dump_data, dump_mask);
  }
  if (ext) {
    iwl_fw_ini_dump_trigger(fwrt, ext, &dump_data, dump_mask);
  }

  return dump_file;
}

void iwl_fw_error_dump(struct iwl_fw_runtime* fwrt) {
  struct iwl_fw_dump_ptrs* fw_error_dump;
  struct iwl_fw_error_dump_file* dump_file;
  uint32_t file_len;
  uint32_t dump_mask = fwrt->fw->dbg.dump_mask;

  IWL_DEBUG_INFO(fwrt, "WRT dump start\n");

  /* there's no point in fw dump if the bus is dead */
  if (test_bit(STATUS_TRANS_DEAD, &fwrt->trans->status)) {
    IWL_ERR(fwrt, "Skip fw error dump since bus is dead\n");
    goto out;
  }

  fw_error_dump = malloc(sizeof(*fw_error_dump));
  if (!fw_error_dump) {
    goto out;
  }

  if (fwrt->trans->ini_valid) {
    dump_file = _iwl_fw_error_ini_dump(fwrt, fw_error_dump, &dump_mask);
  } else {
    dump_file = _iwl_fw_error_dump(fwrt, fw_error_dump);
  }

  if (!dump_file) {
    kfree(fw_error_dump);
    goto out;
  }

  if (!fwrt->trans->ini_valid && fwrt->dump.monitor_only) {
    dump_mask &= IWL_FW_ERROR_DUMP_FW_MONITOR;
  }

  fw_error_dump->trans_ptr = iwl_trans_dump_data(fwrt->trans, dump_mask);
  file_len = le32_to_cpu(dump_file->file_len);
  fw_error_dump->fwrt_len = file_len;
  if (fw_error_dump->trans_ptr) {
    file_len += fw_error_dump->trans_ptr->len;
    dump_file->file_len = cpu_to_le32(file_len);
  }

  // Collect all the relevant segments of the core dump.
  const char* core_dump_ptrs[2] = {};
  size_t core_dump_sizes[2] = {};
  size_t core_dump_count = 0;
  core_dump_ptrs[core_dump_count] = fw_error_dump->fwrt_ptr;
  core_dump_sizes[core_dump_count] = fw_error_dump->fwrt_len;
  ++core_dump_count;
  if (fw_error_dump->trans_ptr) {
    core_dump_ptrs[core_dump_count] = (char*)fw_error_dump->trans_ptr->data;
    core_dump_sizes[core_dump_count] = fw_error_dump->trans_ptr->len;
    ++core_dump_count;
  }

  // Dump the core dump segments and free their storage.
  char core_dump_name[64];
  snprintf(core_dump_name, sizeof(core_dump_name), "%.*s", (int)fwrt->dump.desc->len,
           fwrt->dump.desc->trig_desc.data);
  iwl_debug_core_dump(fwrt->trans->dev, core_dump_name, core_dump_ptrs, core_dump_sizes,
                      core_dump_count);
  vfree(fw_error_dump->fwrt_ptr);
  vfree(fw_error_dump->trans_ptr);
  kfree(fw_error_dump);

out:
  iwl_fw_free_dump_desc(fwrt);
  clear_bit(IWL_FWRT_STATUS_DUMPING, &fwrt->status);
  IWL_DEBUG_INFO(fwrt, "WRT dump done\n");
}
IWL_EXPORT_SYMBOL(iwl_fw_error_dump)

const struct iwl_fw_dump_desc iwl_dump_desc_assert = {
    .trig_desc =
        {
            .type = cpu_to_le32(FW_DBG_TRIGGER_FW_ASSERT),
        },
};
IWL_EXPORT_SYMBOL(iwl_dump_desc_assert)

zx_status_t iwl_fw_dbg_collect_desc(struct iwl_fw_runtime* fwrt,
                                    const struct iwl_fw_dump_desc* desc, bool monitor_only,
                                    zx_duration_t delay) {
  /*
   * If the loading of the FW completed successfully, the next step is to
   * get the SMEM config data. Thus, if fwrt->smem_cfg.num_lmacs is non
   * zero, the FW was already loaded successully. If the state is "NO_FW"
   * in such a case - exit, since FW may be dead. Otherwise, we
   * can try to collect the data, since FW might just not be fully
   * loaded (no "ALIVE" yet), and the debug data is accessible.
   *
   * Corner case: got the FW alive but crashed before getting the SMEM
   *  config. In such a case, due to HW access problems, we might
   *  collect garbage.
   */
  if (fwrt->trans->state == IWL_TRANS_NO_FW && fwrt->smem_cfg.num_lmacs) {
    return ZX_ERR_IO;
  }

  if (test_and_set_bit(IWL_FWRT_STATUS_DUMPING, &fwrt->status) ||
      test_bit(IWL_FWRT_STATUS_WAIT_ALIVE, &fwrt->status)) {
    return ZX_ERR_SHOULD_WAIT;
  }

  if (WARN_ON(fwrt->dump.desc)) {
    iwl_fw_free_dump_desc(fwrt);
  }

  IWL_WARN(fwrt, "Collecting data: trigger %d fired.\n", le32_to_cpu(desc->trig_desc.type));

  fwrt->dump.desc = desc;
  fwrt->dump.monitor_only = monitor_only;

  iwl_task_post(fwrt->dump.wk, delay);

  return ZX_OK;
}
IWL_EXPORT_SYMBOL(iwl_fw_dbg_collect_desc)

zx_status_t _iwl_fw_dbg_collect(struct iwl_fw_runtime* fwrt, enum iwl_fw_dbg_trigger trig,
                                const char* str, size_t len,
                                struct iwl_fw_dbg_trigger_tlv* trigger) {
  struct iwl_fw_dump_desc* desc;
  zx_duration_t delay = 0;
  bool monitor_only = false;

  if (trigger) {
    uint16_t occurrences = le16_to_cpu(trigger->occurrences) - 1;

    if (!le16_to_cpu(trigger->occurrences)) {
      return ZX_OK;
    }

    if (trigger->flags & IWL_FW_DBG_FORCE_RESTART) {
      IWL_WARN(fwrt, "Force restart: trigger %d fired.\n", trig);
      iwl_force_nmi(fwrt->trans);
      return ZX_OK;
    }

    trigger->occurrences = cpu_to_le16(occurrences);
    delay = ZX_MSEC(le16_to_cpu(trigger->trig_dis_ms));
    monitor_only = trigger->mode & IWL_FW_DBG_TRIGGER_MONITOR_ONLY;
  }

  desc = malloc(sizeof(*desc) + len);
  if (!desc) {
    return ZX_ERR_NO_MEMORY;
  }

  desc->len = len;
  desc->trig_desc.type = cpu_to_le32(trig);
  memcpy(desc->trig_desc.data, str, len);

  return iwl_fw_dbg_collect_desc(fwrt, desc, monitor_only, delay);
}
IWL_EXPORT_SYMBOL(_iwl_fw_dbg_collect)

zx_status_t iwl_fw_dbg_collect(struct iwl_fw_runtime* fwrt, uint32_t id, const char* str,
                               size_t len) {
  struct iwl_fw_dump_desc* desc;
  uint32_t occur = 0;
  zx_duration_t delay = 0;

  if (!fwrt->trans->ini_valid) {
    return _iwl_fw_dbg_collect(fwrt, id, str, len, NULL);
  }

  if (id == FW_DBG_TRIGGER_USER) {
    id = IWL_FW_TRIGGER_ID_USER_TRIGGER;
  }

  if (WARN_ON(!fwrt->dump.active_trigs[id].active)) {
    return ZX_ERR_INVALID_ARGS;
  }

  delay = ZX_USEC(le32_to_cpu(fwrt->dump.active_trigs[id].conf->ignore_consec));
  occur = le32_to_cpu(fwrt->dump.active_trigs[id].conf->occurrences);
  if (!occur) {
    return ZX_OK;
  }

  if (le32_to_cpu(fwrt->dump.active_trigs[id].conf->force_restart)) {
    IWL_WARN(fwrt, "Force restart: trigger %d fired.\n", id);
    iwl_force_nmi(fwrt->trans);
    return ZX_OK;
  }

  desc = malloc(sizeof(*desc) + len);
  if (!desc) {
    return ZX_ERR_NO_MEMORY;
  }

  occur--;
  fwrt->dump.active_trigs[id].conf->occurrences = cpu_to_le32(occur);

  desc->len = len;
  desc->trig_desc.type = cpu_to_le32(id);
  memcpy(desc->trig_desc.data, str, len);

  return iwl_fw_dbg_collect_desc(fwrt, desc, true, delay);
}
IWL_EXPORT_SYMBOL(iwl_fw_dbg_collect)

zx_status_t iwl_fw_dbg_collect_trig(struct iwl_fw_runtime* fwrt,
                                    struct iwl_fw_dbg_trigger_tlv* trigger, const char* fmt, ...) {
  size_t len = 0;
  char buf[64];

  if (fwrt->trans->ini_valid) {
    return ZX_OK;
  }

  if (fmt) {
    va_list ap;

    buf[sizeof(buf) - 1] = '\0';

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* check for truncation */
    if (WARN_ON_ONCE(buf[sizeof(buf) - 1])) {
      buf[sizeof(buf) - 1] = '\0';
    }

    len = strlen(buf) + 1;
  }

  return _iwl_fw_dbg_collect(fwrt, le32_to_cpu(trigger->id), buf, len, trigger);
}
IWL_EXPORT_SYMBOL(iwl_fw_dbg_collect_trig)

#if 0  // NEEDS_PORTING

int iwl_fw_start_dbg_conf(struct iwl_fw_runtime *fwrt, u8 conf_id)
{
	u8 *ptr;
	int ret;
	int i;

	if (WARN_ONCE(conf_id >= ARRAY_SIZE(fwrt->fw->dbg.conf_tlv),
		      "Invalid configuration %d\n", conf_id))
		return -EINVAL;

	/* EARLY START - firmware's configuration is hard coded */
	if ((!fwrt->fw->dbg.conf_tlv[conf_id] ||
	     !fwrt->fw->dbg.conf_tlv[conf_id]->num_of_hcmds) &&
	    conf_id == FW_DBG_START_FROM_ALIVE)
		return 0;

	if (!fwrt->fw->dbg.conf_tlv[conf_id])
		return -EINVAL;

	if (fwrt->dump.conf != FW_DBG_INVALID)
		IWL_INFO(fwrt, "FW already configured (%d) - re-configuring\n",
			 fwrt->dump.conf);

	/* Send all HCMDs for configuring the FW debug */
	ptr = (void *)&fwrt->fw->dbg.conf_tlv[conf_id]->hcmd;
	for (i = 0; i < fwrt->fw->dbg.conf_tlv[conf_id]->num_of_hcmds; i++) {
		struct iwl_fw_dbg_conf_hcmd *cmd = (void *)ptr;
		struct iwl_host_cmd hcmd = {
			.id = cmd->id,
			.len = { le16_to_cpu(cmd->len), },
			.data = { cmd->data, },
		};

		ret = iwl_trans_send_cmd(fwrt->trans, &hcmd);
		if (ret)
			return ret;

		ptr += sizeof(*cmd);
		ptr += le16_to_cpu(cmd->len);
	}

	fwrt->dump.conf = conf_id;

	return 0;
}
IWL_EXPORT_SYMBOL(iwl_fw_start_dbg_conf);

#endif  // NEEDS_PORTING

#ifdef CPTCFG_IWLWIFI_DEBUGFS
static inline void iwl_fw_set_dbg_rec_on(struct iwl_fw_runtime* fwrt) {
  if (fwrt->fw->dbg.dest_tlv && fwrt->cur_fw_img == IWL_UCODE_REGULAR) {
    fwrt->trans->dbg_rec_on = true;
  }
}
#endif

/* this function assumes dump_start was called beforehand and dump_end will be
 * called afterwards
 */
void iwl_fw_dbg_collect_sync(struct iwl_fw_runtime* fwrt) {
  struct iwl_fw_dbg_params params = {0};

  if (!test_bit(IWL_FWRT_STATUS_DUMPING, &fwrt->status)) {
    return;
  }

  if (fwrt->ops && fwrt->ops->fw_running && !fwrt->ops->fw_running(fwrt->ops_ctx)) {
    IWL_ERR(fwrt, "Firmware not running - cannot dump error\n");
    iwl_fw_free_dump_desc(fwrt);
    clear_bit(IWL_FWRT_STATUS_DUMPING, &fwrt->status);
    return;
  }

  iwl_fw_dbg_stop_restart_recording(fwrt, &params, true);

  iwl_fw_error_dump(fwrt);

  /* start recording again if the firmware is not crashed */
  if (!test_bit(STATUS_FW_ERROR, &fwrt->trans->status) && fwrt->fw->dbg.dest_tlv) {
    /* wait before we collect the data till the DBGC stop */
    zx_nanosleep(zx_deadline_after(ZX_USEC(500)));
    iwl_fw_dbg_restart_recording(fwrt->trans, &params);
  }
}
IWL_EXPORT_SYMBOL(iwl_fw_dbg_collect_sync)

void iwl_fw_error_dump_wk(void* data) {
  struct iwl_fw_runtime* fwrt = (struct iwl_fw_runtime*)data;

  if (fwrt->ops && fwrt->ops->dump_start && fwrt->ops->dump_start(fwrt->ops_ctx)) {
    return;
  }

  iwl_fw_dbg_collect_sync(fwrt);

  if (fwrt->ops && fwrt->ops->dump_end) {
    fwrt->ops->dump_end(fwrt->ops_ctx);
  }
}

#if 0  // NEEDS_PORTING

void iwl_fw_dbg_read_d3_debug_data(struct iwl_fw_runtime *fwrt)
{
	const struct iwl_cfg *cfg = fwrt->trans->cfg;

	if (!iwl_fw_dbg_is_d3_debug_enabled(fwrt))
		return;

	if (!fwrt->dump.d3_debug_data) {
		fwrt->dump.d3_debug_data = kmalloc(cfg->d3_debug_data_length,
						   GFP_KERNEL);
		if (!fwrt->dump.d3_debug_data) {
			IWL_ERR(fwrt,
				"failed to allocate memory for D3 debug data\n");
			return;
		}
	}

	/* if the buffer holds previous debug data it is overwritten */
	iwl_trans_read_mem_bytes(fwrt->trans, cfg->d3_debug_data_base_addr,
				 fwrt->dump.d3_debug_data,
				 cfg->d3_debug_data_length);

	if (fwrt->sanitize_ops && fwrt->sanitize_ops->frob_mem)
		fwrt->sanitize_ops->frob_mem(fwrt->sanitize_ctx,
					     cfg->d3_debug_data_base_addr,
					     fwrt->dump.d3_debug_data,
					     cfg->d3_debug_data_length);
}
IWL_EXPORT_SYMBOL(iwl_fw_dbg_read_d3_debug_data);

#endif  // NEEDS_PORTING

static int iwl_fw_dbg_suspend_resume_hcmd(struct iwl_trans *trans, bool suspend)
{
	struct iwl_dbg_suspend_resume_cmd cmd = {
		.operation = suspend ?
			cpu_to_le32(DBGC_SUSPEND_CMD) :
			cpu_to_le32(DBGC_RESUME_CMD),
	};
	struct iwl_host_cmd hcmd = {
		.id = WIDE_ID(DEBUG_GROUP, DBGC_SUSPEND_RESUME),
		.data[0] = &cmd,
		.len[0] = sizeof(cmd),
	};

	return iwl_trans_send_cmd(trans, &hcmd);
}

static void iwl_fw_dbg_stop_recording(struct iwl_trans *trans,
				      struct iwl_fw_dbg_params *params)
{
	if (trans->trans_cfg->device_family == IWL_DEVICE_FAMILY_7000) {
		iwl_set_bits_prph(trans, MON_BUFF_SAMPLE_CTL, 0x100);
		return;
	}

	if (params) {
		params->in_sample = iwl_read_umac_prph(trans, DBGC_IN_SAMPLE);
		params->out_ctrl = iwl_read_umac_prph(trans, DBGC_OUT_CTRL);
	}

	iwl_write_umac_prph(trans, DBGC_IN_SAMPLE, 0);
	/* wait for the DBGC to finish writing the internal buffer to DRAM to
	 * avoid halting the HW while writing
	 */
	zx_nanosleep(zx_deadline_after(ZX_USEC(1000)));
	iwl_write_umac_prph(trans, DBGC_OUT_CTRL, 0);
}

static zx_status_t iwl_fw_dbg_restart_recording(struct iwl_trans *trans,
					struct iwl_fw_dbg_params *params)
{
	if (!params)
		return ZX_ERR_IO;

	if (trans->trans_cfg->device_family == IWL_DEVICE_FAMILY_7000) {
		iwl_clear_bits_prph(trans, MON_BUFF_SAMPLE_CTL, 0x100);
		iwl_clear_bits_prph(trans, MON_BUFF_SAMPLE_CTL, 0x1);
		iwl_set_bits_prph(trans, MON_BUFF_SAMPLE_CTL, 0x1);
	} else {
		iwl_write_umac_prph(trans, DBGC_IN_SAMPLE, params->in_sample);
		iwl_write_umac_prph(trans, DBGC_OUT_CTRL, params->out_ctrl);
	}

	return ZX_OK;
}

void iwl_fw_dbg_stop_restart_recording(struct iwl_fw_runtime *fwrt,
				       struct iwl_fw_dbg_params *params,
				       bool stop)
{
	int ret __maybe_unused = 0;

	if (test_bit(STATUS_FW_ERROR, &fwrt->trans->status))
		return;

	if (fw_has_capa(&fwrt->fw->ucode_capa,
			IWL_UCODE_TLV_CAPA_DBG_SUSPEND_RESUME_CMD_SUPP))
		ret = iwl_fw_dbg_suspend_resume_hcmd(fwrt->trans, stop);
	else if (stop)
		iwl_fw_dbg_stop_recording(fwrt->trans, params);
	else
		ret = iwl_fw_dbg_restart_recording(fwrt->trans, params);
#ifdef CONFIG_IWLWIFI_DEBUGFS
	if (!ret) {
		if (stop)
			fwrt->trans->dbg.rec_on = false;
		else
			iwl_fw_set_dbg_rec_on(fwrt);
	}
#endif
}
