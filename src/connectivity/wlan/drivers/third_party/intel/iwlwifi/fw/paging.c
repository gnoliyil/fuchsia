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
#include <zircon/status.h>
#include <zircon/types.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/commands.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/runtime.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-drv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/memory.h"

void iwl_free_fw_paging(struct iwl_fw_runtime* fwrt) {
  int i;

  for (i = 0; i < NUM_OF_FW_PAGING_BLOCKS; i++) {
    struct iwl_fw_paging* paging = &fwrt->fw_paging_db[i];

    if (paging->io_buf != NULL) {
      iwl_iobuf_release(paging->io_buf);
      paging->io_buf = NULL;
    }
  }
}

static zx_status_t iwl_alloc_fw_paging_mem(struct iwl_fw_runtime* fwrt,
                                           const struct fw_img* image) {
  size_t size = 0;
  zx_status_t ret;

  if (fwrt->fw_paging_db[0].io_buf) {
    return ZX_OK;
  }

  /* ensure BLOCK_2_EXP_SIZE is power of 2 of PAGING_BLOCK_SIZE */
  BUILD_BUG_ON(BIT(BLOCK_2_EXP_SIZE) != PAGING_BLOCK_SIZE);

  int num_of_pages = image->paging_mem_size / FW_PAGING_SIZE;
  fwrt->num_of_paging_blk = DIV_ROUND_UP(num_of_pages, NUM_OF_PAGE_PER_GROUP);
  fwrt->num_of_pages_in_last_blk =
      num_of_pages - NUM_OF_PAGE_PER_GROUP * (fwrt->num_of_paging_blk - 1);

  IWL_DEBUG_FW(fwrt,
               "Paging: allocating mem for %zu paging blocks, each block holds 8 pages, last "
               "block holds %zu pages\n",
               fwrt->num_of_paging_blk, fwrt->num_of_pages_in_last_blk);

  /*
   * Allocate CSS and paging blocks in dram.
   */
  for (size_t blk_idx = 0; blk_idx < fwrt->num_of_paging_blk + 1; blk_idx++) {
    /* For CSS allocate 4KB, for others PAGING_BLOCK_SIZE (32K) */
    size = blk_idx ? PAGING_BLOCK_SIZE : FW_PAGING_SIZE;

    ret =
        iwl_iobuf_allocate_contiguous(fwrt->trans->dev, size, &fwrt->fw_paging_db[blk_idx].io_buf);
    if (ret != ZX_OK) {
      IWL_ERR(fwrt, "Cannot initialize the IO buffer of firmware page: %s\n",
              zx_status_get_string(ret));
      return ret;
    }
  }

  return ZX_OK;
}

//
// Copy the firmware paging blocks (aka image->sec[sec_idx]) into the host DRAM (aka
// fwrt->fw_paging_db[].io_buf).
//
static zx_status_t iwl_fill_paging_mem(struct iwl_fw_runtime* fwrt, const struct fw_img* image) {
  size_t idx;
  int sec_idx;
  zx_status_t ret;

  /*
   * find where is the paging image start point:
   * if CPU2 exist and it's in paging format, then the image looks like:
   * CPU1 sections (2 or more)
   * CPU1_CPU2_SEPARATOR_SECTION delimiter - separate between CPU1 to CPU2
   * CPU2 sections (not paged)
   * PAGING_SEPARATOR_SECTION delimiter - separate between CPU2 non paged to CPU2 paging sec
   * CPU2 paging CSS
   * CPU2 paging image (including instruction and data)
   */
  for (sec_idx = 0; sec_idx < image->num_sec; sec_idx++) {
    if (image->sec[sec_idx].offset == PAGING_SEPARATOR_SECTION) {
      sec_idx++;
      break;
    }
  }

  /*
   * If paging is enabled there should be at least 2 more sections left
   * (one for CSS and one for Paging data)
   */
  if (sec_idx >= image->num_sec - 1) {
    IWL_ERR(fwrt, "Paging: Missing CSS and/or paging sections\n");
    ret = ZX_ERR_INVALID_ARGS;
    goto err;
  }

  /* copy the CSS block to the dram */
  IWL_DEBUG_FW(fwrt, "Paging: load paging CSS to FW, sec = %d\n", sec_idx);

  struct iwl_iobuf* io_buf = fwrt->fw_paging_db[0].io_buf;
  size_t size = iwl_iobuf_size(io_buf);
  if (image->sec[sec_idx].len > size) {
    IWL_ERR(fwrt, "CSS block is larger than paging size: %d > %zu\n", image->sec[sec_idx].len,
            size);
    ret = ZX_ERR_INVALID_ARGS;
    goto err;
  }

  memcpy(iwl_iobuf_virtual(io_buf), image->sec[sec_idx].data, image->sec[sec_idx].len);
  ret = iwl_iobuf_cache_flush(io_buf, 0, size);
  if (ret != ZX_OK) {
    IWL_ERR(fwrt, "Cannot flush the cache of firmware page 0: %s\n", zx_status_get_string(ret));
    return ret;
  }

  IWL_DEBUG_FW(fwrt, "Paging: copied %zu CSS bytes to first block\n", size);

  sec_idx++;

  /*
   * Copy the paging blocks to the dram.  The loop index starts
   * from 1 since the CSS block (index 0) was already copied to
   * dram.  We use num_of_paging_blk + 1 to account for that.
   */
  uint32_t offset = 0;  // offset in source: image->sec[sec_idx].data
  for (idx = 1; idx < fwrt->num_of_paging_blk + 1; idx++) {
    struct iwl_fw_paging* block = &fwrt->fw_paging_db[idx];
    struct iwl_iobuf* io_buf = block->io_buf;
    size_t remaining = image->sec[sec_idx].len - offset;
    size_t len = iwl_iobuf_size(io_buf);

    /*
     * For the last block, we copy all that is remaining,
     * for all other blocks, we copy fw_paging_size at a
     * time. */
    if (idx == fwrt->num_of_paging_blk) {
      len = remaining;
      if (remaining != fwrt->num_of_pages_in_last_blk * FW_PAGING_SIZE) {
        IWL_ERR(fwrt, "Paging: last block contains more data than expected %zu\n", remaining);
        ret = ZX_ERR_INVALID_ARGS;
        goto err;
      }
    } else if (len > remaining) {
      IWL_ERR(fwrt, "Paging: not enough data in other in block %zu (%zu)\n", idx, remaining);
      ret = ZX_ERR_INVALID_ARGS;
      goto err;
    }

    memcpy(iwl_iobuf_virtual(io_buf), image->sec[sec_idx].data + offset, len);
    ret = iwl_iobuf_cache_flush(io_buf, 0, len);
    if (ret != ZX_OK) {
      IWL_ERR(fwrt, "Cannot flush the cache of firmware page %zu: %s\n", idx,
              zx_status_get_string(ret));
      return ret;
    }

    IWL_DEBUG_FW(fwrt, "Paging: copied %zu paging bytes to block %zu\n", len, idx);

    offset += len;
  }

  return ZX_OK;

err:
  iwl_free_fw_paging(fwrt);
  return ret;
}

static zx_status_t iwl_save_fw_paging(struct iwl_fw_runtime* fwrt, const struct fw_img* fw) {
  zx_status_t ret = iwl_alloc_fw_paging_mem(fwrt, fw);
  if (ret != ZX_OK) {
    return ret;
  }

  return iwl_fill_paging_mem(fwrt, fw);
}

/* send paging cmd to FW in case CPU2 has paging image */
static zx_status_t iwl_send_paging_cmd(struct iwl_fw_runtime* fwrt, const struct fw_img* fw) {
  struct iwl_fw_paging_cmd paging_cmd = {
      .flags = cpu_to_le32(
          (uint32_t)(PAGING_CMD_IS_SECURED | PAGING_CMD_IS_ENABLED |
                     (fwrt->num_of_pages_in_last_blk << PAGING_CMD_NUM_OF_PAGES_IN_LAST_GRP_POS))),
      .block_size = cpu_to_le32(BLOCK_2_EXP_SIZE),
      .block_num = cpu_to_le32((uint32_t)fwrt->num_of_paging_blk),
  };
  struct iwl_host_cmd hcmd = {
      .id = iwl_cmd_id(FW_PAGING_BLOCK_CMD, IWL_ALWAYS_LONG_GROUP, 0),
      .len =
          {
              sizeof(paging_cmd),
          },
      .data =
          {
              &paging_cmd,
          },
  };
  size_t blk_idx;

  /* loop for for all paging blocks + CSS block */
  for (blk_idx = 0; blk_idx < fwrt->num_of_paging_blk + 1; blk_idx++) {
    dma_addr_t addr = iwl_iobuf_physical(fwrt->fw_paging_db[blk_idx].io_buf);
    __le32 phy_addr;

    addr = addr >> PAGE_2_EXP_SIZE;
    phy_addr = cpu_to_le32((uint32_t)addr);
    paging_cmd.device_phy_addr[blk_idx] = phy_addr;
  }

  return iwl_trans_send_cmd(fwrt->trans, &hcmd);
}

zx_status_t iwl_init_paging(struct iwl_fw_runtime* fwrt, enum iwl_ucode_type type) {
  const struct fw_img* fw = &fwrt->fw->img[type];

  if (fwrt->trans->trans_cfg->gen2) {
    return ZX_OK;
  }

  /*
   * Configure and operate fw paging mechanism.
   * The driver configures the paging flow only once.
   * The CPU2 paging image is included in the IWL_UCODE_INIT image.
   */
  if (!fw->paging_mem_size) {
    return ZX_OK;
  }

  zx_status_t ret = iwl_save_fw_paging(fwrt, fw);
  if (ret != ZX_OK) {
    IWL_ERR(fwrt, "failed to save the FW paging image\n");
    return ret;
  }

  ret = iwl_send_paging_cmd(fwrt, fw);
  if (ret != ZX_OK) {
    IWL_ERR(fwrt, "failed to send the paging cmd\n");
    iwl_free_fw_paging(fwrt);
    return ret;
  }

  return ZX_OK;
}
