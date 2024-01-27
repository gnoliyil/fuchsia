// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/nand/drivers/aml-rawnand/aml-rawnand.h"

#include <assert.h>

#include <algorithm>
#include <memory>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

#include "src/devices/nand/drivers/aml-rawnand/aml-rawnand-bind.h"

namespace amlrawnand {

static constexpr uint32_t NAND_BUSWIDTH_16 = 0x00000002;

struct NandSetup {
  union {
    uint32_t d32;
    struct {
      unsigned cmd : 22;
      unsigned large_page : 1;
      unsigned no_rb : 1;
      unsigned a2 : 1;
      unsigned reserved25 : 1;
      unsigned page_list : 1;
      unsigned sync_mode : 2;
      unsigned size : 2;
      unsigned active : 1;
    } b;
  } cfg;
  uint16_t id;
  uint16_t max;
};

struct NandCmd {
  uint8_t type;
  uint8_t val;
};

struct ExtInfo {
  uint32_t read_info;
  uint32_t new_type;
  uint32_t page_per_blk;
  uint32_t xlc;
  uint32_t ce_mask;
  uint32_t boot_num;
  uint32_t each_boot_pages;
  uint32_t bbt_occupy_pages;
  uint32_t bbt_start_block;
};

struct NandPage0 {
  NandSetup nand_setup;
  unsigned char page_list[16];
  NandCmd retry_usr[32];
  ExtInfo ext_info;
};

// Controller ECC, OOB, RAND parameters.
struct AmlControllerParams {
  int ecc_strength;  // # of ECC bits per ECC page.
  int user_mode;     // OOB bytes every ECC page or per block ?
  int rand_mode;     // Randomize ?
  int bch_mode;
};

AmlControllerParams AmlParams = {
    8,  // Overwritten using BCH setting from page0.
    2,
    // The 2 following values are overwritten by page0 contents.
    1,                 // rand-mode is 1 for page0.
    AML_ECC_BCH60_1K,  // This is the BCH setting for page0.
};

void AmlRawNand::NandctrlSetCfg(uint32_t val) { mmio_nandreg_.Write32(val, P_NAND_CFG); }

void AmlRawNand::NandctrlSetTimingAsync(int bus_tim, int bus_cyc) {
  static constexpr uint32_t lenmask = (static_cast<uint32_t>(1) << 12) - 1;
  uint32_t value = mmio_nandreg_.Read32(P_NAND_CFG);

  value &= ~lenmask;
  value |= (((bus_cyc & 31) | ((bus_tim & 31) << 5) | (0 << 10)) & lenmask);
  mmio_nandreg_.Write32(value, P_NAND_CFG);
}

void AmlRawNand::NandctrlSendCmd(uint32_t cmd) { mmio_nandreg_.Write32(cmd, P_NAND_CMD); }

static const char* AmlEccString(uint32_t ecc_mode) {
  const char* s;

  switch (ecc_mode) {
    case AML_ECC_BCH8:
      s = "AML_ECC_BCH8";
      break;
    case AML_ECC_BCH8_1K:
      s = "AML_ECC_BCH8_1K";
      break;
    case AML_ECC_BCH24_1K:
      s = "AML_ECC_BCH24_1K";
      break;
    case AML_ECC_BCH30_1K:
      s = "AML_ECC_BCH30_1K";
      break;
    case AML_ECC_BCH40_1K:
      s = "AML_ECC_BCH40_1K";
      break;
    case AML_ECC_BCH50_1K:
      s = "AML_ECC_BCH50_1K";
      break;
    case AML_ECC_BCH60_1K:
      s = "AML_ECC_BCH60_1K";
      break;
    default:
      s = "BAD ECC Algorithm";
      break;
  }
  return s;
}

static uint32_t AmlGetEccPageSize(uint32_t ecc_mode) {
  uint32_t ecc_page;

  switch (ecc_mode) {
    case AML_ECC_BCH8:
      ecc_page = 512;
      break;
    case AML_ECC_BCH8_1K:
    case AML_ECC_BCH24_1K:
    case AML_ECC_BCH30_1K:
    case AML_ECC_BCH40_1K:
    case AML_ECC_BCH50_1K:
    case AML_ECC_BCH60_1K:
      ecc_page = 1024;
      break;
    default:
      ecc_page = 0;
      break;
  }
  return ecc_page;
}

static int AmlGetEccStrength(uint32_t ecc_mode) {
  int ecc_strength;

  switch (ecc_mode) {
    case AML_ECC_BCH8:
    case AML_ECC_BCH8_1K:
      ecc_strength = 8;
      break;
    case AML_ECC_BCH24_1K:
      ecc_strength = 24;
      break;
    case AML_ECC_BCH30_1K:
      ecc_strength = 30;
      break;
    case AML_ECC_BCH40_1K:
      ecc_strength = 40;
      break;
    case AML_ECC_BCH50_1K:
      ecc_strength = 50;
      break;
    case AML_ECC_BCH60_1K:
      ecc_strength = 60;
      break;
    default:
      ecc_strength = -1;
      break;
  }
  return ecc_strength;
}

void AmlRawNand::AmlCmdIdle(uint32_t nand_bus_cycles) {
  uint32_t cmd = chip_select_ | AML_CMD_IDLE | (nand_bus_cycles & 0x3ff);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdIdle(zx::duration duration) {
  // Convert the given duration to a number of NAND bus cycles, rounded up.
  const int64_t cycles =
      ((duration + nand_cycle_time_).to_nsecs() - 1) / nand_cycle_time_.to_nsecs();
  AmlCmdIdle(static_cast<uint32_t>(std::clamp<int64_t>(cycles, 0, UINT32_MAX)));
}

zx_status_t AmlRawNand::AmlWaitCmdQueueEmpty(zx::duration timeout, zx::duration first_interval,
                                             zx::duration polling_interval) {
  zx_status_t ret = ZX_OK;
  zx::duration total_time;

  // Wait until cmd fifo is empty.
  bool first = true;
  while (true) {
    uint32_t cmd_size = mmio_nandreg_.Read32(P_NAND_CMD);
    uint32_t numcmds = (cmd_size >> 22) & 0x1f;
    if (numcmds == 0)
      break;
    zx::duration sleep_interval = first ? first_interval : polling_interval;
    first = false;
    zx::nanosleep(zx::deadline_after(sleep_interval));
    total_time += sleep_interval;
    if (total_time > timeout) {
      ret = ZX_ERR_TIMED_OUT;
      break;
    }
  }
  if (ret == ZX_ERR_TIMED_OUT)
    zxlogf(ERROR, "wait for empty cmd FIFO time out");
  return ret;
}

zx_status_t AmlRawNand::AmlWaitCmdFinish(zx::duration timeout, zx::duration first_interval,
                                         zx::duration polling_interval) {
  // TODO(fxb/94715): We don't need to wait for the queue to be empty here, just for enough space
  // available for the two idle commands below.
  // The queue could be full when this is called, so wait for it to be empty before writing the idle
  // commands.
  zx_status_t status = AmlWaitCmdQueueEmpty(timeout, first_interval, polling_interval);
  if (status != ZX_OK) {
    return status;
  }

  // The controller stores the commands from the queue, a next command, and the current command
  // running on the bus. By enqueuing two idle commands then waiting for the queue to be empty, we
  // can be sure that the bus is idle and going to stay idle after this method returns.
  AmlCmdIdle(0);
  AmlCmdIdle(0);

  return AmlWaitCmdQueueEmpty(timeout, first_interval, polling_interval);
}

void AmlRawNand::AmlCmdSeed(uint32_t seed) {
  uint32_t cmd = AML_CMD_SEED | (0xc2 + (seed & 0x7fff));
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdN2M(uint32_t ecc_pages, uint32_t ecc_pagesize) {
  uint32_t cmd = CMDRWGEN(AML_CMD_N2M, controller_params_.rand_mode, controller_params_.bch_mode, 0,
                          ecc_pagesize, ecc_pages);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdM2N(uint32_t ecc_pages, uint32_t ecc_pagesize) {
  uint32_t cmd = CMDRWGEN(AML_CMD_M2N, controller_params_.rand_mode, controller_params_.bch_mode, 0,
                          ecc_pagesize, ecc_pages);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

namespace {

// Each copy of BL2 is prefixed by a single page of metadata telling us what
// ECC settings to use for NAND. But since we're reading these settings from
// NAND itself, the initial metadata read has to use fixed settings.
//
// These settings are exactly what the bootloader uses.
constexpr int kPage0RandMode = 1;
constexpr int kPage0BchMode = AML_ECC_BCH60_1K;
constexpr int kPage0ShortpageMode = 1;
constexpr int kPage0EccPageSize = 384;
// Even though all the metadata currently fits in a single 384-byte ECC page,
// read and write 8 pages for consistency with the bootloader code and to ensure
// compatibility with future devices that may put additional info here.
constexpr int kPage0NumEccPages = 8;

}  // namespace

void AmlRawNand::AmlCmdM2NPage0() {
  // When shortpage is turned on, page size is given in bytes/8.
  static_assert(kPage0ShortpageMode == 1, "Fix pagesize calculation");
  uint32_t cmd = CMDRWGEN(AML_CMD_M2N, kPage0RandMode, kPage0BchMode, kPage0ShortpageMode,
                          kPage0EccPageSize / 8, kPage0NumEccPages);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdN2MPage0() {
  // When shortpage is turned on, page size is given in bytes/8.
  static_assert(kPage0ShortpageMode == 1, "Fix pagesize calculation");
  uint32_t cmd = CMDRWGEN(AML_CMD_N2M, kPage0RandMode, kPage0BchMode, kPage0ShortpageMode,
                          kPage0EccPageSize / 8, kPage0NumEccPages);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void* AmlRawNand::AmlInfoPtr(int i) {
  auto p = reinterpret_cast<struct AmlInfoFormat*>(buffers_->info_buf);
  return &p[i];
}

// In the case where user_mode == 2, info_buf contains one nfc_info_format
// struct per ECC page on completion of a read. This 8 byte structure has
// the 2 OOB bytes and ECC/error status.
zx_status_t AmlRawNand::AmlGetOOBByte(uint8_t* oob_buf, size_t* oob_actual) {
  struct AmlInfoFormat* info;
  int count = 0;
  uint32_t ecc_pagesize, ecc_pages;

  ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
  ecc_pages = writesize_ / ecc_pagesize;
  // user_mode is 2 in our case - 2 bytes of OOB for every ECC page.
  if (controller_params_.user_mode != 2)
    return ZX_ERR_NOT_SUPPORTED;
  for (uint32_t i = 0; i < ecc_pages; i++) {
    info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
    oob_buf[count++] = static_cast<uint8_t>(info->info_bytes & 0xff);
    oob_buf[count++] = static_cast<uint8_t>((info->info_bytes >> 8) & 0xff);
  }

  if (oob_actual) {
    *oob_actual = count;
  }

  return ZX_OK;
}

zx_status_t AmlRawNand::AmlSetOOBByte(const uint8_t* oob_buf, size_t oob_size, uint32_t ecc_pages) {
  struct AmlInfoFormat* info;
  size_t count = 0;

  // user_mode is 2 in our case - 2 bytes of OOB for every ECC page.
  if (controller_params_.user_mode != 2)
    return ZX_ERR_NOT_SUPPORTED;
  for (uint32_t i = 0; i < ecc_pages; i++) {
    info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));

    // If the caller didn't provide enough OOB bytes to fill all the pages,
    // pad with zeros.
    uint8_t low_byte = (count < oob_size) ? oob_buf[count] : 0x00;
    count++;
    uint8_t high_byte = (count < oob_size) ? oob_buf[count] : 0x00;
    count++;
    info->info_bytes = static_cast<uint16_t>(low_byte | (high_byte << 8));
  }
  return ZX_OK;
}

zx_status_t AmlRawNand::AmlGetECCCorrections(int ecc_pages, uint32_t nand_page,
                                             uint32_t* ecc_corrected, bool* erased) {
  struct AmlInfoFormat* info;
  int bitflips = 0;
  int erased_ecc_pages = 0;
  uint8_t zero_bits;

  for (int i = 0; i < ecc_pages; i++) {
    info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
    if (info->ecc.eccerr_cnt == AML_ECC_UNCORRECTABLE_CNT) {
      if (!controller_params_.rand_mode) {
        zxlogf(WARNING, "%s: ECC failure (non-randomized)@%u", __func__, nand_page);
        stats.failed++;
        return ZX_ERR_IO_DATA_INTEGRITY;
      }
      // Why are we checking for zero_bits here ?
      // To deal with blank NAND pages. A blank page is entirely 0xff.
      // When read with scrambler, the page will be ECC uncorrectable,
      // In theory, if there is a single zero-bit in the page, then that
      // page is not a blank page. But in practice, even fresh NAND chips
      // report a few errors on the read of a page (including blank pages)
      // so we make allowance for a few bitflips. The threshold against
      // which we test the zero-bits is one under which we can correct
      // the bitflips when the page is written to. One option is to set
      // this threshold to be exactly the ECC strength (this is aggressive).
      // TODO(srmohan): What should the correct threshold be ? We could
      // conservatively set this to a small value, or we could have this
      // depend on the quality of the NAND, the wear of the NAND etc.
      zero_bits = info->zero_bits & AML_ECC_UNCORRECTABLE_CNT;
      if (zero_bits >= controller_params_.ecc_strength) {
        zxlogf(WARNING, "%s: ECC failure (randomized)@%u zero_bits=%u", __func__, nand_page,
               zero_bits);
        stats.failed++;
        return ZX_ERR_IO_DATA_INTEGRITY;
      }
      zxlogf(INFO, "%s: Blank Page@%u", __func__, nand_page);
      bitflips = std::max(static_cast<uint8_t>(bitflips), static_cast<uint8_t>(zero_bits));
      ++erased_ecc_pages;
      continue;
    }
    stats.ecc_corrected += info->ecc.eccerr_cnt;
    bitflips = std::max(static_cast<uint8_t>(bitflips), static_cast<uint8_t>(info->ecc.eccerr_cnt));
  }
  if (ecc_corrected) {
    *ecc_corrected = bitflips;
  }
  *erased = false;
  if (erased_ecc_pages == ecc_pages) {
    *erased = true;
  } else if (erased_ecc_pages != 0) {
    zxlogf(WARNING, "%s: Partially erased nand page @%u", __func__, nand_page);
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  return ZX_OK;
}

// The ECC page descriptors are processed in the order that they are stored in memory. Therefore,
// DMA is done when the final page has been marked complete.
zx_status_t AmlRawNand::AmlCheckECCPages(int ecc_pages, int ecc_pagesize) {
  ZX_ASSERT(ecc_pages > 0);

  const volatile uint8_t* ecc_status =
      &reinterpret_cast<AmlInfoFormat*>(AmlInfoPtr(ecc_pages - 1))->ecc.raw_value;

  AmlInfoFormat::ecc_sta ecc{.raw_value = *ecc_status};
  for (; ecc.completed == 0; ecc.raw_value = *ecc_status) {
    // Sleep for the approximate time it takes for a single ECC page to be transferred on the bus.
    zx::nanosleep(zx::deadline_after(nand_cycle_time_ * ecc_pagesize));
  }
  return ZX_OK;
}

void AmlRawNand::AmlQueueRB() {
  uint32_t cmd;

  // Must wait t_WB after READSTART before issuing another command.
  AmlCmdIdle(tWB);

  // The RB IO command tells the controller to start periodically reading status bytes from the
  // chip. A status command must be issued before RB IO for the chip to reply with the status
  // register value in response to these reads.
  cmd = chip_select_ | AML_CMD_CLE | (NAND_CMD_STATUS & 0xff);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);

  // Must wait t_WHR after STATUS before reading from the chip.
  AmlCmdIdle(tWHR);

  cmd = AML_CMD_RB | AML_CMD_IO6 | (0x18 & 0x1f);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdCtrl(int32_t cmd, uint32_t ctrl) {
  if (cmd == NAND_CMD_NONE) {
    if (ctrl > 0) {
      AmlCmdIdle(zx::nsec(ctrl));
    }
    AmlWaitCmdFinish(zx::msec(CMD_FINISH_TIMEOUT_MS), zx::usec(10), zx::usec(10));
    return;
  }

  if (ctrl & NAND_CLE)
    cmd = chip_select_ | AML_CMD_CLE | (cmd & 0xff);
  else
    cmd = chip_select_ | AML_CMD_ALE | (cmd & 0xff);
  mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

uint8_t AmlRawNand::AmlReadByte() {
  uint32_t cmd = chip_select_ | AML_CMD_DRD | 0;
  NandctrlSendCmd(cmd);

  AmlCmdIdle(NAND_TWB_TIME_CYCLE);

  AmlWaitCmdFinish(zx::msec(CMD_FINISH_TIMEOUT_MS), zx::usec(10), zx::usec(10));
  return mmio_nandreg_.Read<uint8_t>(P_NAND_BUF);
}

void AmlRawNand::AmlSetClockRate(uint32_t clk_freq) {
  uint32_t always_on = 0x1 << 24;
  uint32_t clk;

  // For Amlogic type AXG.
  always_on = 0x1 << 28;
  switch (clk_freq) {
    case 24:
      clk = 0x80000201;
      break;
    case 112:
      clk = 0x80000249;
      break;
    case 200:
      clk = 0x80000245;
      break;
    case 250:
      clk = 0x80000244;
      break;
    default:
      clk = 0x80000245;
      break;
  }
  clk |= always_on;
  mmio_clockreg_.Write32(clk, 0);
}

void AmlRawNand::AmlClockInit() {
  uint32_t sys_clk_rate_mhz, bus_cycle, bus_timing;

  sys_clk_rate_mhz = 200;
  AmlSetClockRate(sys_clk_rate_mhz);
  bus_cycle = 6;
  bus_timing = bus_cycle + 1;
  NandctrlSetCfg(0);
  NandctrlSetTimingAsync(bus_timing, (bus_cycle - 1));
  NandctrlSendCmd(1 << 31);

  nand_cycle_time_ = zx::nsec((bus_cycle * kMicrosecondsToNanoseconds) / sys_clk_rate_mhz);
}

void AmlRawNand::AmlAdjustTimings(uint32_t tRC_min, uint32_t tREA_max, uint32_t RHOH_min) {
  uint32_t sys_clk_rate_mhz, bus_cycle, bus_timing;
  // NAND timing defaults.
  static constexpr uint32_t TreaMaxDefault = 20;
  static constexpr uint32_t RhohMinDefault = 15;

  if (!tREA_max)
    tREA_max = TreaMaxDefault;
  if (!RHOH_min)
    RHOH_min = RhohMinDefault;
  if (tREA_max > 30)
    sys_clk_rate_mhz = 112;
  else if (tREA_max > 16)
    sys_clk_rate_mhz = 200;
  else
    sys_clk_rate_mhz = 250;
  AmlSetClockRate(sys_clk_rate_mhz);
  bus_cycle = 6;
  bus_timing = bus_cycle + 1;
  NandctrlSetCfg(0);
  NandctrlSetTimingAsync(bus_timing, (bus_cycle - 1));
  NandctrlSendCmd(1 << 31);

  // Check the math for two common clock rates.
  static_assert(((6 * kMicrosecondsToNanoseconds) / 200) == 30);
  static_assert(((6 * kMicrosecondsToNanoseconds) / 250) == 24);
  nand_cycle_time_ = zx::nsec((bus_cycle * kMicrosecondsToNanoseconds) / sys_clk_rate_mhz);
}

namespace {

bool IsPage0NandPage(uint32_t nand_page) {
  // Backup copies of page0 are located every 128 pages,
  // with the last one at 896.
  static constexpr uint32_t AmlPage0Step = 128;
  static constexpr uint32_t AmlPage0MaxAddr = 896;

  return ((nand_page <= AmlPage0MaxAddr) && ((nand_page % AmlPage0Step) == 0));
}

// The ROM bootloader looks in the OOB bytes for magic values so we need
// to write them to all BL2 pages.
//
// Most NAND pages contain 8 bytes OOB userdata (4 ECC pages per NAND page x 2
// userdata bytes per ECC page). Page0 metadata however uses shortpage mode with
// 8 ECC pages per NAND page, so we need up to 16 OOB userdata bytes.
constexpr uint8_t kRomMagicOobBuffer[] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA,
                                          0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA};
constexpr size_t kRomMagicOobSize = sizeof(kRomMagicOobBuffer);

// Returns true if the given page number requires writing magic OOB values.
constexpr bool PageRequiresMagicOob(uint32_t nand_page) {
  // BL2 lives in 0x0-0x3FFFFF, which is pages 0-1023.
  return nand_page <= 1023;
}

}  // namespace

zx_status_t AmlRawNand::RawNandReadPageHwecc(uint32_t nand_page, uint8_t* data, size_t data_size,
                                             size_t* data_actual, uint8_t* oob, size_t oob_size,
                                             size_t* oob_actual, uint32_t* ecc_correct) {
  zx_status_t status;
  uint32_t ecc_pagesize;
  uint32_t ecc_pages;
  bool erased = false;
  bool page0 = IsPage0NandPage(nand_page);

  if (!page0) {
    ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
    ecc_pages = writesize_ / ecc_pagesize;
  } else {
    ecc_pagesize = kPage0EccPageSize;
    ecc_pages = kPage0NumEccPages;
  }

  fbl::AutoLock lock(&mutex_);
  if (shutdown_) {
    return ZX_ERR_CANCELED;
  }

  if ((status = AmlRawNandAllocBufs()) != ZX_OK)
    return status;

  // Zero out the ECC page descriptors in case the completed bit is still set from a previous
  // operation.
  memset(info_buffer().virt(), 0, ecc_pages * sizeof(AmlInfoFormat));

  SelectChip();

  // A large number of commands are being enqueued after this point, so make sure the queue is
  // empty beforehand.
  AmlWaitCmdQueueEmpty(zx::msec(CMD_FINISH_TIMEOUT_MS), polling_timings_.cmd_flush.min,
                       polling_timings_.cmd_flush.interval);

  // Send the page address into the controller.
  onfi_->OnfiCommand(NAND_CMD_READ0, 0x00, nand_page, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));

  AmlQueueRB();

  // As per section 5.14 of the ONFI specification, another READ0 command must be issued after
  // reading the status register. tRR delay is probably not required due to the extra read
  // command.
  onfi_->OnfiCommand(NAND_CMD_READ0, -1, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));

  mmio_nandreg_.Write32(GENCMDDADDRL(AML_CMD_ADL, buffers_->data_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDDADDRH(AML_CMD_ADH, buffers_->data_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDIADDRL(AML_CMD_AIL, buffers_->info_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDIADDRH(AML_CMD_AIH, buffers_->info_buf_paddr), P_NAND_CMD);

  if ((page0 && kPage0RandMode) || controller_params_.rand_mode) {
    // Only need to set the seed if randomizing is enabled.
    AmlCmdSeed(nand_page);
  }

  if (!page0) {
    AmlCmdN2M(ecc_pages, ecc_pagesize);
  } else {
    AmlCmdN2MPage0();
  }

  // Waiting for the command queue to be empty here does not work. The controller seems to continue
  // processing ECC after the N2M command is off the queue, so poll the completed bit here to find
  // out when DMA is really done.
  status = AmlCheckECCPages(ecc_pages, ecc_pagesize);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: AmlCheckECCPages failed %d", __func__, status);
    return status;
  }

  status = AmlGetECCCorrections(ecc_pages, nand_page, ecc_correct, &erased);
  if (status != ZX_OK) {
    zxlogf(WARNING, "%s: Uncorrectable ECC error on read", __func__);
    *ecc_correct = controller_params_.ecc_strength + 1;
  }

  // Finally copy out the data and oob as needed.
  if (oob != nullptr) {
    size_t num_bytes;
    // Need to try to fetch it first, just to get the oob_actual size
    if (AmlGetOOBByte(reinterpret_cast<uint8_t*>(oob), &num_bytes) != ZX_OK) {
      num_bytes = 0;
    }
    if (erased) {
      memset(oob, 0xff, num_bytes);
    }
    if (oob_actual != nullptr) {
      *oob_actual = num_bytes;
    }
  }
  if (data != nullptr) {
    // Page0 is always 384 bytes.
    size_t num_bytes = (page0 ? 384 : writesize_);
    // Clean up any possible bit flips on supposed erased pages.
    if (erased) {
      memset(data, 0xff, num_bytes);
    } else {
      memcpy(data, buffers_->data_buf, num_bytes);
    }
    if (data_actual) {
      *data_actual = num_bytes;
    }
  }

  return status;
}

// TODO : Right now, the driver uses a buffer for DMA, which
// is not needed. We should initiate DMA to/from pages passed in.
zx_status_t AmlRawNand::RawNandWritePageHwecc(const uint8_t* data, size_t data_size,
                                              const uint8_t* oob, size_t oob_size,
                                              uint32_t nand_page) {
  zx_status_t status;
  uint32_t ecc_pagesize = 0;  // Initialize to silence compiler.
  uint32_t ecc_pages;
  bool page0 = IsPage0NandPage(nand_page);

  if (!page0) {
    ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
    ecc_pages = writesize_ / ecc_pagesize;
  } else {
    ecc_pages = kPage0NumEccPages;
  }

  fbl::AutoLock lock(&mutex_);
  if (shutdown_) {
    return ZX_ERR_CANCELED;
  }

  if (zx_status_t status = AmlRawNandAllocBufs(); status != ZX_OK)
    return status;

  if (data != nullptr) {
    memcpy(buffers_->data_buf, data, writesize_);
  }

  if (PageRequiresMagicOob(nand_page)) {
    // Writing the wrong OOB bytes will brick the device, raise an error if
    // the caller tried to provide their own here.
    if (oob != nullptr) {
      zxlogf(ERROR, "%s: Cannot write provided OOB, page %u requires specific OOB bytes", __func__,
             nand_page);
      return ZX_ERR_INVALID_ARGS;
    }

    oob = kRomMagicOobBuffer;
    oob_size = kRomMagicOobSize;
  }

  if (oob != nullptr) {
    AmlSetOOBByte(reinterpret_cast<const uint8_t*>(oob), oob_size, ecc_pages);
  }

  onfi_->OnfiCommand(NAND_CMD_SEQIN, 0x00, nand_page, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  mmio_nandreg_.Write32(GENCMDDADDRL(AML_CMD_ADL, buffers_->data_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDDADDRH(AML_CMD_ADH, buffers_->data_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDIADDRL(AML_CMD_AIL, buffers_->info_buf_paddr), P_NAND_CMD);
  mmio_nandreg_.Write32(GENCMDIADDRH(AML_CMD_AIH, buffers_->info_buf_paddr), P_NAND_CMD);

  if ((page0 && kPage0RandMode) || controller_params_.rand_mode) {
    // Only need to set the seed if randomizing is enabled.
    AmlCmdSeed(nand_page);
  }

  if (!page0) {
    AmlCmdM2N(ecc_pages, ecc_pagesize);
  } else {
    AmlCmdM2NPage0();
  }

  status = AmlWaitCmdFinish(zx::msec(CMD_FINISH_TIMEOUT_MS), polling_timings_.cmd_flush.min,
                            polling_timings_.cmd_flush.interval);
  if (status != ZX_OK) {
    return status;
  }
  onfi_->OnfiCommand(NAND_CMD_PAGEPROG, -1, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  return onfi_->OnfiWait(zx::msec(AML_WRITE_PAGE_TIMEOUT_MS), polling_timings_.write.interval);
}

zx_status_t AmlRawNand::RawNandEraseBlock(uint32_t nand_page) {
  // nandblock has to be erasesize_ aligned.
  if (nand_page % erasesize_pages_) {
    zxlogf(ERROR, "%s: NAND block %u must be a erasesize_pages (%u) multiple", __func__, nand_page,
           erasesize_pages_);
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&mutex_);
  if (shutdown_) {
    return ZX_ERR_CANCELED;
  }

  onfi_->OnfiCommand(NAND_CMD_ERASE1, -1, nand_page, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  onfi_->OnfiCommand(NAND_CMD_ERASE2, -1, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  return onfi_->OnfiWait(zx::msec(AML_ERASE_BLOCK_TIMEOUT_MS), polling_timings_.erase.interval);
}

zx_status_t AmlRawNand::AmlGetFlashType() {
  uint8_t nand_maf_id, nand_dev_id;
  uint8_t id_data[8];
  struct nand_chip_table* nand_chip;

  onfi_->OnfiCommand(NAND_CMD_RESET, -1, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  onfi_->OnfiCommand(NAND_CMD_READID, 0x00, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  // Read manufacturer and device IDs.
  nand_maf_id = AmlReadByte();
  nand_dev_id = AmlReadByte();
  // Read again.
  onfi_->OnfiCommand(NAND_CMD_READID, 0x00, -1, static_cast<uint32_t>(chipsize_), chip_delay_,
                     (controller_params_.options & NAND_BUSWIDTH_16));
  // Read entire ID string.
  for (uint32_t i = 0; i < sizeof(id_data); i++)
    id_data[i] = AmlReadByte();
  if (id_data[0] != nand_maf_id || id_data[1] != nand_dev_id) {
    zxlogf(ERROR, "second ID read did not match %02x,%02x against %02x,%02x", nand_maf_id,
           nand_dev_id, id_data[0], id_data[1]);
  }

  zxlogf(INFO, "%s: manufacturer_id = %x, device_ide = %x, extended_id = %x", __func__, nand_maf_id,
         nand_dev_id, id_data[3]);
  nand_chip = onfi_->FindNandChipTable(nand_maf_id, nand_dev_id);
  if (nand_chip == nullptr) {
    zxlogf(ERROR,
           "%s: Cound not find matching NAND chip. NAND chip unsupported."
           " This is FATAL\n",
           __func__);
    return ZX_ERR_UNAVAILABLE;
  }
  if (nand_chip->extended_id_nand) {
    // Initialize pagesize, eraseblk size, oobsize_ and
    // buswidth from extended parameters queried just now.
    uint8_t extid = id_data[3];

    writesize_ = 1024 << (extid & 0x03);
    extid = static_cast<uint8_t>(extid >> 2);
    // Calc oobsize_.
    oobsize_ = (8 << (extid & 0x01)) * (writesize_ >> 9);
    extid = static_cast<uint8_t>(extid >> 2);
    // Calc blocksize. Blocksize is multiples of 64KiB.
    erasesize_ = (64 * 1024) << (extid & 0x03);
    extid = static_cast<uint8_t>(extid >> 2);
    // Get buswidth information.
    bus_width_ = (extid & 0x01) ? NAND_BUSWIDTH_16 : 0;
  } else {
    // Initialize pagesize, eraseblk size, oobsize_ and
    // buswidth from values in table.
    writesize_ = nand_chip->page_size;
    oobsize_ = nand_chip->oobsize;
    erasesize_ = nand_chip->erase_block_size;
    bus_width_ = nand_chip->bus_width;
  }
  erasesize_pages_ = erasesize_ / writesize_;
  chipsize_ = nand_chip->chipsize;
  page_shift_ = ffs(writesize_) - 1;
  polling_timings_ = nand_chip->polling_timings;

  // We found a matching device in our database, use it to
  // initialize. Adjust timings and set various parameters.
  AmlAdjustTimings(nand_chip->timings.tRC_min, nand_chip->timings.tREA_max,
                   nand_chip->timings.RHOH_min);
  // chip_delay is used OnfiCommand(), after sending down some commands
  // to the NAND chip.
  chip_delay_ = nand_chip->chip_delay_us;
  nand_timings_ = nand_chip->timings;
  zxlogf(INFO,
         "NAND %s %s: chip size = %lu(GB), page size = %u, oob size = %u\n"
         "eraseblock size = %u, chip delay (us) = %u\n",
         nand_chip->manufacturer_name, nand_chip->device_name, chipsize_, writesize_, oobsize_,
         erasesize_, chip_delay_);
  return ZX_OK;
}

zx_status_t AmlRawNand::RawNandGetNandInfo(nand_info_t* nand_info) {
  uint64_t capacity;
  zx_status_t status = ZX_OK;

  nand_info->page_size = writesize_;
  nand_info->pages_per_block = erasesize_pages_;
  capacity = chipsize_ * (1024 * 1024);
  capacity /= erasesize_;
  nand_info->num_blocks = static_cast<uint32_t>(capacity);
  nand_info->ecc_bits = controller_params_.ecc_strength;

  nand_info->nand_class = NAND_CLASS_PARTMAP;
  memset(&nand_info->partition_guid, 0, sizeof(nand_info->partition_guid));

  if (controller_params_.user_mode == 2)
    nand_info->oob_size = (writesize_ / AmlGetEccPageSize(controller_params_.bch_mode)) * 2;
  else
    status = ZX_ERR_NOT_SUPPORTED;
  return status;
}

void AmlRawNand::AmlSetEncryption() { mmio_nandreg_.SetBits32((1 << 17), P_NAND_CFG); }

zx_status_t AmlRawNand::AmlReadPage0(uint8_t* data, size_t data_size, uint8_t* oob, size_t oob_size,
                                     uint32_t nand_page, uint32_t* ecc_correct, int retries) {
  zx_status_t status;

  retries++;
  do {
    status = RawNandReadPageHwecc(nand_page, data, data_size, nullptr, oob, oob_size, nullptr,
                                  ecc_correct);
  } while (status != ZX_OK && --retries > 0);
  if (status != ZX_OK)
    zxlogf(ERROR, "%s: Read error", __func__);
  return status;
}

zx_status_t AmlRawNand::AmlNandInitFromPage0() {
  zx_status_t status;
  NandPage0* page0;
  uint32_t ecc_correct;

  std::unique_ptr<uint8_t[]> buffer(new uint8_t[writesize_]);
  uint8_t* data = buffer.get();
  // There are 8 copies of page0 spaced apart by 128 pages
  // starting at Page 0. Read the first we can.
  for (uint32_t i = 0; i < 8; i++) {
    status = AmlReadPage0(data, writesize_, nullptr, 0, i * 128, &ecc_correct, 3);
    if (status == ZX_OK)
      break;
  }
  if (status != ZX_OK) {
    // Could not read any of the page0 copies. This is a fatal error.
    zxlogf(ERROR, "%s: Page0 Read (all copies) failed", __func__);
    return status;
  }

  page0 = reinterpret_cast<NandPage0*>(data);
  controller_params_.rand_mode = (page0->nand_setup.cfg.d32 >> 19) & 0x1;
  controller_params_.bch_mode = (page0->nand_setup.cfg.d32 >> 14) & 0x7;

  controller_params_.ecc_strength = AmlGetEccStrength(controller_params_.bch_mode);
  if (controller_params_.ecc_strength < 0) {
    zxlogf(INFO, "%s: BAD ECC strength computed from BCH Mode", __func__);
    return ZX_ERR_BAD_STATE;
  }

  zxlogf(INFO, "%s: NAND BCH Mode is %s", __func__, AmlEccString(controller_params_.bch_mode));
  return ZX_OK;
}

zx_status_t AmlRawNand::AmlRawNandAllocBufs() {
  if (buffers_)
    return ZX_OK;
  Buffers& buffers = buffers_.emplace();

  // The iobuffers MUST be uncachable. Making these cachable, with
  // cache flush/invalidate at the right places in the code does not
  // work. We see data corruptions caused by speculative cache prefetching
  // done by ARM. Note also that these corruptions are not easily reproducible.
  zx_status_t status = buffers.data_buffer.Init(
      bti_.get(), writesize_, IO_BUFFER_UNCACHED | IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "io_buffer_init(data_buffer) failed");
    buffers_.reset();
    return status;
  }
  ZX_DEBUG_ASSERT(writesize_ > 0);
  status = buffers.info_buffer.Init(bti_.get(), writesize_,
                                    IO_BUFFER_UNCACHED | IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "io_buffer_init(info_buffer) failed");
    buffers_.reset();
    return status;
  }
  buffers.data_buf = buffers.data_buffer.virt();
  buffers.info_buf = buffers.info_buffer.virt();
  buffers.data_buf_paddr = buffers.data_buffer.phys();
  buffers.info_buf_paddr = buffers.info_buffer.phys();
  return ZX_OK;
}

zx_status_t AmlRawNand::AmlNandInit() {
  zx_status_t status;

  // Do nand scan to get manufacturer and other info.
  status = AmlGetFlashType();
  if (status != ZX_OK)
    return status;
  controller_params_.ecc_strength = AmlParams.ecc_strength;
  controller_params_.user_mode = AmlParams.user_mode;
  controller_params_.rand_mode = AmlParams.rand_mode;
  static constexpr auto NAND_USE_BOUNCE_BUFFER = 0x1;
  controller_params_.options = NAND_USE_BOUNCE_BUFFER;
  controller_params_.bch_mode = AmlParams.bch_mode;

  // Note on OOB byte settings.
  // The default config for OOB is 2 bytes per OOB page. This is the
  // settings we use. So nothing to be done for OOB. If we ever need
  // to switch to 16 bytes of OOB per NAND page, we need to set the
  // right bits in the CFG register.
  {
    fbl::AutoLock lock(&mutex_);
    status = AmlRawNandAllocBufs();
    if (status != ZX_OK)
      return status;
  }

  // Read one of the copies of page0, and use that to initialize
  // ECC algorithm and rand-mode.
  status = AmlNandInitFromPage0();

  static constexpr uint32_t chipsel[2] = {NAND_CE0, NAND_CE1};
  // Force chip_select to 0.
  chip_select_ = chipsel[0];

  return status;
}

void AmlRawNand::DdkRelease() {
  // This should result in the dtors of all members to be
  // called (so the MmioBuffers, bti, irq handle should get
  // cleaned up).
  delete this;
}

void AmlRawNand::CleanUpIrq() { irq_.destroy(); }

void AmlRawNand::DdkUnbind(ddk::UnbindTxn txn) {
  CleanUpIrq();
  txn.Reply();
}

void AmlRawNand::DdkSuspend(ddk::SuspendTxn txn) {
  const uint8_t suspend_reason = txn.suspend_reason() & DEVICE_MASK_SUSPEND_REASON;
  if (suspend_reason != DEVICE_SUSPEND_REASON_SUSPEND_RAM) {
    fbl::AutoLock lock(&mutex_);
    shutdown_ = true;
    buffers_.reset();
  }
  txn.Reply(ZX_OK, txn.requested_state());
}

zx_status_t AmlRawNand::Init() {
  onfi_->Init([this](int32_t cmd, uint32_t ctrl) -> void { AmlCmdCtrl(cmd, ctrl); },
              [this]() -> uint8_t { return AmlReadByte(); });

  AmlClockInit();
  zx_status_t status = AmlNandInit();
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml_raw_nand: AmlNandInit() failed - This is FATAL");
    CleanUpIrq();
  }
  return status;
}

zx_status_t AmlRawNand::Bind() {
  zx_status_t status = DdkAdd("aml-raw_nand");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAdd failed", __FILE__);
    CleanUpIrq();
  }
  return status;
}

zx_status_t AmlRawNand::Create(void* ctx, zx_device_t* parent) {
  zx_status_t status;

  ddk::PDevFidl pdev(parent);
  if (!pdev.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_PDEV not available", __FILE__);
    return ZX_ERR_NO_RESOURCES;
  }

  zx::bti bti;
  if ((status = pdev.GetBti(0, &bti)) != ZX_OK) {
    zxlogf(ERROR, "%s: pdev_get_bti failed", __FILE__);
    return status;
  }

  static constexpr uint32_t NandRegWindow = 0;
  static constexpr uint32_t ClockRegWindow = 1;
  std::optional<fdf::MmioBuffer> mmio_nandreg;
  status = pdev.MapMmio(NandRegWindow, &mmio_nandreg);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: pdev.MapMmio nandreg failed", __FILE__);
    return status;
  }

  std::optional<fdf::MmioBuffer> mmio_clockreg;
  status = pdev.MapMmio(ClockRegWindow, &mmio_clockreg);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: pdev.MapMmio clockreg failed", __FILE__);
    return status;
  }

  zx::interrupt irq;
  if ((status = pdev.GetInterrupt(0, &irq)) != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to map interrupt", __FILE__);
    return status;
  }
  fbl::AllocChecker ac;
  std::unique_ptr<AmlRawNand> device(
      new (&ac) AmlRawNand(parent, *std::move(mmio_nandreg), *std::move(mmio_clockreg),
                           std::move(bti), std::move(irq), std::make_unique<Onfi>()));

  if (!ac.check()) {
    zxlogf(ERROR, "%s: AmlRawNand alloc failed", __FILE__);
    return ZX_ERR_NO_MEMORY;
  }

  if ((status = device->Init()) != ZX_OK) {
    return status;
  }

  if ((status = device->Bind()) != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = device.release();
  return ZX_OK;
}

static constexpr zx_driver_ops_t amlrawnand_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlRawNand::Create;
  return ops;
}();

}  // namespace amlrawnand

ZIRCON_DRIVER(aml_rawnand, amlrawnand::amlrawnand_driver_ops, "zircon", "0.1");
