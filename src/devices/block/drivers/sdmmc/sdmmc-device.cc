// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdmmc-device.h"

#include <endian.h>
#include <fuchsia/hardware/sdio/c/banjo.h>
#include <fuchsia/hardware/sdmmc/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/sdio/hw.h>
#include <lib/zx/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pretty/hexdump.h>

namespace {

constexpr uint32_t kInitializationFrequencyHz = 400'000;

constexpr zx::duration kVoltageStabilizationTime = zx::msec(5);
constexpr zx::duration kDataStabilizationTime = zx::msec(1);

constexpr uint32_t GetBits(uint32_t x, uint32_t mask, uint32_t loc) { return (x & mask) >> loc; }

constexpr void UpdateBits(uint32_t* x, uint32_t mask, uint32_t loc, uint32_t val) {
  *x &= ~mask;
  *x |= ((val << loc) & mask);
}

}  // namespace

namespace sdmmc {

zx_status_t SdmmcDevice::Init() { return host_.HostInfo(&host_info_); }

zx_status_t SdmmcDevice::Request(const sdmmc_req_t& req, uint32_t response[4], uint32_t retries,
                                 zx::duration wait_time) const {
  if (retries == 0) {
    retries = retries_;
  }

  zx_status_t st;
  while (((st = host_.Request(&req, response)) != ZX_OK) && retries > 0) {
    retries--;
    if (wait_time.get() > 0) {
      zx::nanosleep(zx::deadline_after(wait_time));
    }
  }
  return st;
}

zx_status_t SdmmcDevice::RequestWithBlockRead(const sdmmc_req_t& req, uint32_t response[4],
                                              cpp20::span<uint8_t> read_data) const {
  zx::vmo vmo;
  zx_status_t st = zx::vmo::create(read_data.size(), 0, &vmo);
  if (st != ZX_OK) {
    return st;
  }

  const sdmmc_buffer_region_t vmo_region = {
      .buffer = {.vmo = vmo.get()},
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 0,
      .size = read_data.size(),
  };

  sdmmc_req_t request = req;
  request.buffers_list = &vmo_region;
  request.buffers_count = 1;
  if ((st = Request(request, response)) != ZX_OK) {
    return st;
  }

  return vmo.read(read_data.data(), 0, read_data.size());
}

// SD/MMC shared ops

zx_status_t SdmmcDevice::SdmmcGoIdle() {
  sdmmc_req_t req = {};
  req.cmd_idx = SDMMC_GO_IDLE_STATE;
  req.arg = 0;
  req.cmd_flags = SDMMC_GO_IDLE_STATE_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::SdmmcSendStatus(uint32_t* status) {
  sdmmc_req_t req = {};
  req.cmd_idx = SDMMC_SEND_STATUS;
  req.arg = RcaArg();
  req.cmd_flags = SDMMC_SEND_STATUS_FLAGS;
  uint32_t response[4];
  zx_status_t st = Request(req, response);
  if (st == ZX_OK) {
    *status = response[0];
  }
  return st;
}

zx_status_t SdmmcDevice::SdmmcStopTransmission(uint32_t* status) {
  zx_status_t st;
  for (uint32_t i = 0; i < kTryAttempts; i++) {
    sdmmc_req_t req = {};
    req.cmd_idx = SDMMC_STOP_TRANSMISSION;
    req.arg = 0;
    req.cmd_flags = SDMMC_STOP_TRANSMISSION_FLAGS;
    req.suppress_error_messages = i < (kTryAttempts - 1);
    uint32_t response[4];
    if ((st = Request(req, response)) == ZX_OK) {
      if (status) {
        *status = response[0];
      }
      break;
    }
  }
  return st;
}

zx_status_t SdmmcDevice::SdmmcWaitForState(uint32_t state) {
  for (uint32_t i = 0; i < kTryAttempts; i++) {
    sdmmc_req_t req = {};
    req.cmd_idx = SDMMC_SEND_STATUS;
    req.arg = RcaArg();
    req.cmd_flags = SDMMC_SEND_STATUS_FLAGS;
    req.suppress_error_messages = i < (kTryAttempts - 1);
    uint32_t response[4];
    zx_status_t st = Request(req, response);
    if (st == ZX_OK && MMC_STATUS_CURRENT_STATE(response[0]) == state) {
      return ZX_OK;
    }
  }
  return ZX_ERR_TIMED_OUT;
}

zx_status_t SdmmcDevice::SdmmcIoRequestWithRetries(const sdmmc_req_t& request, uint32_t* retries) {
  zx_status_t st;
  for (uint32_t i = 0; i < kTryAttempts; i++) {
    if (i > 0) {
      (*retries)++;
    }

    sdmmc_req_t req = request;
    req.suppress_error_messages = i < (kTryAttempts - 1);

    uint32_t unused_response[4];
    if ((st = Request(req, unused_response)) == ZX_OK) {
      break;
    }

    // Wait for the card to go idle (TRAN state) before retrying. SdmmcStopTransmission waits for
    // the busy signal on dat0, so the card should be back in TRAN immediately after.

    uint32_t status;
    if (SdmmcStopTransmission(&status) == ZX_OK &&
        MMC_STATUS_CURRENT_STATE(status) == MMC_STATUS_CURRENT_STATE_TRAN) {
      continue;
    }

    SdmmcWaitForState(MMC_STATUS_CURRENT_STATE_TRAN);
  }

  return st;
}

// SD ops

zx_status_t SdmmcDevice::SdSendAppCmd() {
  sdmmc_req_t req = {};
  req.cmd_idx = SDMMC_APP_CMD;
  req.arg = RcaArg();
  req.cmd_flags = SDMMC_APP_CMD_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::SdSendOpCond(uint32_t flags, uint32_t* ocr) {
  zx_status_t st = SdSendAppCmd();
  if (st != ZX_OK) {
    return st;
  }

  sdmmc_req_t req = {};
  req.cmd_idx = SD_APP_SEND_OP_COND;
  req.arg = flags;
  req.cmd_flags = SD_APP_SEND_OP_COND_FLAGS;
  uint32_t response[4];
  if ((st = Request(req, response)) != ZX_OK) {
    return st;
  }

  *ocr = response[0];
  return ZX_OK;
}

zx_status_t SdmmcDevice::SdSendIfCond() {
  // TODO what is this parameter?
  uint32_t arg = 0x1aa;
  sdmmc_req_t req = {};
  req.cmd_idx = SD_SEND_IF_COND;
  req.arg = arg;
  req.cmd_flags = SD_SEND_IF_COND_FLAGS;
  req.suppress_error_messages = true;
  uint32_t response[4];
  zx_status_t st = Request(req, response);
  if (st != ZX_OK) {
    zxlogf(DEBUG, "SD_SEND_IF_COND failed, retcode = %d", st);
    return st;
  }
  if ((response[0] & 0xfff) != arg) {
    // The card should have replied with the pattern that we sent.
    zxlogf(DEBUG, "SDMMC_SEND_IF_COND got bad reply = %" PRIu32 "", response[0]);
    return ZX_ERR_BAD_STATE;
  } else {
    return ZX_OK;
  }
}

zx_status_t SdmmcDevice::SdSendRelativeAddr(uint16_t* card_status) {
  sdmmc_req_t req = {};
  req.cmd_idx = SD_SEND_RELATIVE_ADDR;
  req.arg = 0;
  req.cmd_flags = SD_SEND_RELATIVE_ADDR_FLAGS;

  uint32_t response[4];
  zx_status_t st = Request(req, response);
  if (st != ZX_OK) {
    zxlogf(DEBUG, "SD_SEND_RELATIVE_ADDR failed, retcode = %d", st);
    return st;
  }

  rca_ = static_cast<uint16_t>(response[0] >> 16);

  if (card_status != nullptr) {
    *card_status = response[0] & 0xffff;
  }

  return st;
}

zx_status_t SdmmcDevice::SdSelectCard() {
  sdmmc_req_t req = {};
  req.cmd_idx = SD_SELECT_CARD;
  req.arg = RcaArg();
  req.cmd_flags = SD_SELECT_CARD_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::SdSendScr(std::array<uint8_t, 8>& scr) {
  zx_status_t st = SdSendAppCmd();
  if (st != ZX_OK) {
    return st;
  }

  sdmmc_req_t req = {};
  req.cmd_idx = SD_APP_SEND_SCR;
  req.arg = 0;
  req.cmd_flags = SD_APP_SEND_SCR_FLAGS;
  req.blocksize = 8;
  uint32_t unused_response[4];
  return RequestWithBlockRead(req, unused_response, {scr.data(), scr.size()});
}

zx_status_t SdmmcDevice::SdSetBusWidth(sdmmc_bus_width_t width) {
  if (width != SDMMC_BUS_WIDTH_ONE && width != SDMMC_BUS_WIDTH_FOUR) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t st = SdSendAppCmd();
  if (st != ZX_OK) {
    return st;
  }

  sdmmc_req_t req = {};
  req.cmd_idx = SD_APP_SET_BUS_WIDTH;
  req.arg = (width == SDMMC_BUS_WIDTH_FOUR ? 2 : 0);
  req.cmd_flags = SD_APP_SET_BUS_WIDTH_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::SdSwitchUhsVoltage(uint32_t ocr) {
  zx_status_t st = ZX_OK;
  sdmmc_req_t req = {};
  req.cmd_idx = SD_VOLTAGE_SWITCH;
  req.arg = 0;
  req.cmd_flags = SD_VOLTAGE_SWITCH_FLAGS;

  if (signal_voltage_ == SDMMC_VOLTAGE_V180) {
    return ZX_OK;
  }

  uint32_t unused_response[4];
  if ((st = Request(req, unused_response)) != ZX_OK) {
    zxlogf(DEBUG, "SD_VOLTAGE_SWITCH failed, retcode = %d", st);
    return st;
  }

  if ((st = host_.SetBusFreq(0)) != ZX_OK) {
    zxlogf(DEBUG, "SD_VOLTAGE_SWITCH failed, retcode = %d", st);
    return st;
  }

  if ((st = host_.SetSignalVoltage(SDMMC_VOLTAGE_V180)) != ZX_OK) {
    zxlogf(DEBUG, "SD_VOLTAGE_SWITCH failed, retcode = %d", st);
    return st;
  }

  // Wait 5ms for the voltage to stabilize. See section 3.6.1. in the SDHCI specification.
  zx::nanosleep(zx::deadline_after(kVoltageStabilizationTime));

  if ((st = host_.SetBusFreq(kInitializationFrequencyHz)) != ZX_OK) {
    zxlogf(DEBUG, "SD_VOLTAGE_SWITCH failed, retcode = %d", st);
    return st;
  }

  // Wait 1ms for the data lines to stabilize.
  zx::nanosleep(zx::deadline_after(kDataStabilizationTime));

  signal_voltage_ = SDMMC_VOLTAGE_V180;
  return ZX_OK;
}

// SDIO specific ops

zx_status_t SdmmcDevice::SdioSendOpCond(uint32_t ocr, uint32_t* rocr) {
  zx_status_t st = ZX_OK;
  sdmmc_req_t req = {};
  req.cmd_idx = SDIO_SEND_OP_COND;
  req.arg = ocr;
  req.cmd_flags = SDIO_SEND_OP_COND_FLAGS;
  req.suppress_error_messages = true;
  for (size_t i = 0; i < 100; i++) {
    uint32_t response[4];
    if ((st = Request(req, response, 3, zx::msec(10))) != ZX_OK) {
      // fail on request error
      break;
    }
    // No need to wait for busy clear if probing
    if ((ocr == 0) || (response[0] & MMC_OCR_BUSY)) {
      *rocr = response[0];
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::msec(10)));
  }
  return st;
}

zx_status_t SdmmcDevice::SdioIoRwDirect(bool write, uint32_t fn_idx, uint32_t reg_addr,
                                        uint8_t write_byte, uint8_t* read_byte) {
  uint32_t cmd_arg = 0;
  if (write) {
    cmd_arg |= SDIO_IO_RW_DIRECT_RW_FLAG;
    if (read_byte) {
      cmd_arg |= SDIO_IO_RW_DIRECT_RAW_FLAG;
    }
  }
  UpdateBits(&cmd_arg, SDIO_IO_RW_DIRECT_FN_IDX_MASK, SDIO_IO_RW_DIRECT_FN_IDX_LOC, fn_idx);
  UpdateBits(&cmd_arg, SDIO_IO_RW_DIRECT_REG_ADDR_MASK, SDIO_IO_RW_DIRECT_REG_ADDR_LOC, reg_addr);
  UpdateBits(&cmd_arg, SDIO_IO_RW_DIRECT_WRITE_BYTE_MASK, SDIO_IO_RW_DIRECT_WRITE_BYTE_LOC,
             write_byte);
  sdmmc_req_t req = {};
  req.cmd_idx = SDIO_IO_RW_DIRECT;
  req.arg = cmd_arg;
  if (reg_addr == SDIO_CIA_CCCR_ASx_ABORT_SEL_CR_ADDR) {
    req.cmd_flags = SDIO_IO_RW_DIRECT_ABORT_FLAGS;
    req.suppress_error_messages = true;
  } else {
    req.cmd_flags = SDIO_IO_RW_DIRECT_FLAGS;
  }
  uint32_t response[4];
  zx_status_t st = Request(req, response);
  if (st != ZX_OK) {
    // Let the platform driver handle logging of this error.
    zxlogf(DEBUG, "SDIO_IO_RW_DIRECT failed, retcode = %d", st);
    return st;
  }
  if (read_byte) {
    *read_byte = static_cast<uint8_t>(GetBits(response[0], SDIO_IO_RW_DIRECT_RESP_READ_BYTE_MASK,
                                              SDIO_IO_RW_DIRECT_RESP_READ_BYTE_LOC));
  }
  return ZX_OK;
}

zx_status_t SdmmcDevice::SdioIoRwExtended(uint32_t caps, bool write, uint8_t fn_idx,
                                          uint32_t reg_addr, bool incr, uint32_t blk_count,
                                          uint32_t blk_size,
                                          cpp20::span<const sdmmc_buffer_region_t> buffers) {
  uint32_t cmd_arg = 0;
  if (write) {
    cmd_arg |= SDIO_IO_RW_EXTD_RW_FLAG;
  }
  UpdateBits(&cmd_arg, SDIO_IO_RW_EXTD_FN_IDX_MASK, SDIO_IO_RW_EXTD_FN_IDX_LOC, fn_idx);
  UpdateBits(&cmd_arg, SDIO_IO_RW_EXTD_REG_ADDR_MASK, SDIO_IO_RW_EXTD_REG_ADDR_LOC, reg_addr);
  if (incr) {
    cmd_arg |= SDIO_IO_RW_EXTD_OP_CODE_INCR;
  }

  if (blk_count > 1) {
    if (caps & SDIO_CARD_MULTI_BLOCK) {
      cmd_arg |= SDIO_IO_RW_EXTD_BLOCK_MODE;
      UpdateBits(&cmd_arg, SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_MASK, SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_LOC,
                 blk_count);
    } else {
      // Convert the request into byte mode?
      return ZX_ERR_NOT_SUPPORTED;
    }
  } else {
    // SDIO Spec Table 5-3
    uint32_t arg_blk_size = (blk_size == 512) ? 0 : blk_size;
    UpdateBits(&cmd_arg, SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_MASK, SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_LOC,
               arg_blk_size);
  }
  sdmmc_req_t req = {};
  req.cmd_idx = SDIO_IO_RW_DIRECT_EXTENDED;
  req.arg = cmd_arg;
  req.cmd_flags = write ? (SDIO_IO_RW_DIRECT_EXTENDED_FLAGS)
                        : (SDIO_IO_RW_DIRECT_EXTENDED_FLAGS | SDMMC_CMD_READ),
  req.blocksize = blk_size;
  req.client_id = fn_idx;
  req.buffers_list = buffers.data();
  req.buffers_count = buffers.size();

  uint32_t response[4] = {};
  zx_status_t st = host_.Request(&req, response);
  if (st != ZX_OK) {
    zxlogf(ERROR, "SDIO_IO_RW_DIRECT_EXTENDED failed, retcode = %d", st);
    return st;
  }
  return ZX_OK;
}

// MMC ops

zx_status_t SdmmcDevice::MmcSendOpCond(uint32_t ocr, uint32_t* rocr) {
  // Request sector addressing if not probing
  uint32_t arg = (ocr == 0) ? ocr : ((1 << 30) | ocr);
  sdmmc_req_t req = {};
  req.cmd_idx = MMC_SEND_OP_COND;
  req.arg = arg;
  req.cmd_flags = MMC_SEND_OP_COND_FLAGS;
  zx_status_t st;
  for (int i = 100; i; i--) {
    uint32_t response[4];
    if ((st = Request(req, response)) != ZX_OK) {
      // fail on request error
      break;
    }
    // No need to wait for busy clear if probing
    if ((arg == 0) || (response[0] & MMC_OCR_BUSY)) {
      *rocr = response[0];
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::msec(10)));
  }
  return st;
}

zx_status_t SdmmcDevice::MmcAllSendCid(std::array<uint8_t, SDMMC_CID_SIZE>& cid) {
  sdmmc_req_t req = {};
  req.cmd_idx = SDMMC_ALL_SEND_CID;
  req.arg = 0;
  req.cmd_flags = SDMMC_ALL_SEND_CID_FLAGS;
  uint32_t response[4];
  zx_status_t st = Request(req, response);
  auto* const cid_u32 = reinterpret_cast<uint32_t*>(cid.data());
  if (st == ZX_OK) {
    cid_u32[0] = response[0];
    cid_u32[1] = response[1];
    cid_u32[2] = response[2];
    cid_u32[3] = response[3];
  }
  return st;
}

zx_status_t SdmmcDevice::MmcSetRelativeAddr(uint16_t rca) {
  rca_ = rca;
  sdmmc_req_t req = {};
  req.cmd_idx = MMC_SET_RELATIVE_ADDR;
  req.arg = RcaArg();
  req.cmd_flags = MMC_SET_RELATIVE_ADDR_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::MmcSendCsd(std::array<uint8_t, SDMMC_CSD_SIZE>& csd) {
  sdmmc_req_t req = {};
  req.cmd_idx = SDMMC_SEND_CSD;
  req.arg = RcaArg();
  req.cmd_flags = SDMMC_SEND_CSD_FLAGS;
  uint32_t response[4];
  zx_status_t st = Request(req, response);
  auto* const csd_u32 = reinterpret_cast<uint32_t*>(csd.data());
  if (st == ZX_OK) {
    csd_u32[0] = response[0];
    csd_u32[1] = response[1];
    csd_u32[2] = response[2];
    csd_u32[3] = response[3];
  }
  return st;
}

zx_status_t SdmmcDevice::MmcSendExtCsd(std::array<uint8_t, MMC_EXT_CSD_SIZE>& ext_csd) {
  // EXT_CSD is send in a data stage
  sdmmc_req_t req = {};
  req.cmd_idx = MMC_SEND_EXT_CSD;
  req.arg = 0;
  req.blocksize = 512;
  req.cmd_flags = MMC_SEND_EXT_CSD_FLAGS;
  uint32_t unused_response[4];
  zx_status_t st = RequestWithBlockRead(req, unused_response, {ext_csd.data(), ext_csd.size()});
  if (st != ZX_OK) {
    return st;
  }

  if (zxlog_level_enabled(TRACE)) {
    zxlogf(TRACE, "EXT_CSD:");
    hexdump8_ex(ext_csd.data(), ext_csd.size(), 0);
  }

  return ZX_OK;
}

zx_status_t SdmmcDevice::MmcSelectCard() {
  sdmmc_req_t req = {};
  req.cmd_idx = MMC_SELECT_CARD;
  req.arg = RcaArg();
  req.cmd_flags = MMC_SELECT_CARD_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

zx_status_t SdmmcDevice::MmcSwitch(uint8_t index, uint8_t value) {
  // Send the MMC_SWITCH command
  uint32_t arg = (3 << 24) |  // write byte
                 (index << 16) | (value << 8);
  sdmmc_req_t req = {};
  req.cmd_idx = MMC_SWITCH;
  req.arg = arg;
  req.cmd_flags = MMC_SWITCH_FLAGS;
  uint32_t unused_response[4];
  return Request(req, unused_response);
}

}  // namespace sdmmc
