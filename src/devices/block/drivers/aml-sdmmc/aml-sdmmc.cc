// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-sdmmc.h"

#include <fuchsia/hardware/clock/cpp/banjo.h>
#include <fuchsia/hardware/gpio/c/banjo.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <fuchsia/hardware/sdmmc/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/io-buffer.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/phys-iter.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fit/defer.h>
#include <lib/fzl/pinned-vmo.h>
#include <lib/mmio/mmio.h>
#include <lib/sdmmc/hw.h>
#include <lib/sync/completion.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <algorithm>
#include <string>

#include <bits/limits.h>
#include <fbl/algorithm.h>
#include <soc/aml-common/aml-power-domain.h>
#include <soc/aml-common/aml-sdmmc.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d2/s905d2-hw.h>

#include "aml-sdmmc-regs.h"
#include "src/devices/block/drivers/aml-sdmmc/aml-sdmmc-bind.h"

// Limit maximum number of descriptors to 512 for now
#define AML_DMA_DESC_MAX_COUNT 512
#define AML_SDMMC_TRACE(fmt, ...) zxlogf(DEBUG, "%s: " fmt, __func__, ##__VA_ARGS__)
#define AML_SDMMC_INFO(fmt, ...) zxlogf(INFO, "%s: " fmt, __func__, ##__VA_ARGS__)
#define AML_SDMMC_ERROR(fmt, ...) zxlogf(ERROR, "%s: " fmt, __func__, ##__VA_ARGS__)
#define AML_SDMMC_WARNING(fmt, ...) zxlogf(WARNING, "%s: " fmt, __func__, ##__VA_ARGS__)

namespace {

uint32_t log2_ceil(uint32_t blk_sz) {
  if (blk_sz == 1) {
    return 0;
  }
  return 32 - (__builtin_clz(blk_sz - 1));
}

zx_paddr_t PageMask() {
  static uintptr_t page_size = zx_system_get_page_size();
  return page_size - 1;
}

}  // namespace

namespace sdmmc {

AmlSdmmc::AmlSdmmc(zx_device_t* parent, zx::bti bti, fdf::MmioBuffer mmio,
                   aml_sdmmc_config_t config, zx::interrupt irq,
                   const ddk::GpioProtocolClient& gpio)
    : AmlSdmmcType(parent),
      mmio_(std::move(mmio)),
      bti_(std::move(bti)),
      reset_gpio_(gpio),
      irq_(std::move(irq)),
      board_config_(config),
      dead_(false),
      pending_txn_(false) {
  for (auto& store : registered_vmos_) {
    store.emplace(vmo_store::Options{});
  }
}

zx_status_t AmlSdmmc::WaitForInterruptImpl() {
  zx::time timestamp;
  return irq_.wait(&timestamp);
}

void AmlSdmmc::ClearStatus() {
  AmlSdmmcStatus::Get()
      .ReadFrom(&mmio_)
      .set_reg_value(AmlSdmmcStatus::kClearStatus)
      .WriteTo(&mmio_);
}

void AmlSdmmc::Inspect::Init(const pdev_device_info_t& device_info) {
  std::string root_name = "aml-sdmmc-port";
  if (device_info.did == PDEV_DID_AMLOGIC_SDMMC_A) {
    root_name += 'A';
  } else if (device_info.did == PDEV_DID_AMLOGIC_SDMMC_B) {
    root_name += 'B';
  } else if (device_info.did == PDEV_DID_AMLOGIC_SDMMC_C) {
    root_name += 'C';
  } else {
    root_name += "-unknown";
  }

  root = inspector.GetRoot().CreateChild(root_name);

  bus_clock_frequency = root.CreateUint(
      "bus_clock_frequency", AmlSdmmcClock::kCtsOscinClkFreq / AmlSdmmcClock::kDefaultClkDiv);
  adj_delay = root.CreateUint("adj_delay", 0);
  delay_lines = root.CreateUint("delay_lines", 0);
  max_delay = root.CreateUint("max_delay", 0);
  longest_window_start = root.CreateUint("longest_window_start", 0);
  longest_window_size = root.CreateUint("longest_window_size", 0);
  longest_window_adj_delay = root.CreateUint("longest_window_adj_delay", 0);
  distance_to_failing_point = root.CreateUint("distance_to_failing_point", 0);
  tuning_method = root.CreateString("tuning_method", "none");
}

zx::result<std::array<uint32_t, AmlSdmmc::kResponseCount>> AmlSdmmc::WaitForInterrupt(
    const sdmmc_req_t& req) {
  zx_status_t status = WaitForInterruptImpl();

  if (status != ZX_OK) {
    AML_SDMMC_ERROR("WaitForInterruptImpl got %d", status);
    return zx::error(status);
  }

  const auto status_irq = AmlSdmmcStatus::Get().ReadFrom(&mmio_);

  auto complete = fit::defer([&]() { ClearStatus(); });

  auto on_bus_error =
      fit::defer([&]() { AmlSdmmcStart::Get().ReadFrom(&mmio_).set_desc_busy(0).WriteTo(&mmio_); });

  if (status_irq.rxd_err()) {
    if (req.suppress_error_messages) {
      AML_SDMMC_TRACE("RX Data CRC Error cmd%d, arg=0x%08x, status=0x%08x", req.cmd_idx, req.arg,
                      status_irq.reg_value());
    } else {
      AML_SDMMC_WARNING("RX Data CRC Error cmd%d, arg=0x%08x, status=0x%08x, consecutive=%lu",
                        req.cmd_idx, req.arg, status_irq.reg_value(), ++consecutive_data_errors_);
    }
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }
  if (status_irq.txd_err()) {
    AML_SDMMC_WARNING("TX Data CRC Error, cmd%d, arg=0x%08x, status=0x%08x, consecutive=%lu",
                      req.cmd_idx, req.arg, status_irq.reg_value(), ++consecutive_data_errors_);
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }
  if (status_irq.desc_err()) {
    AML_SDMMC_ERROR("Controller does not own the descriptor, cmd%d, arg=0x%08x, status=0x%08x",
                    req.cmd_idx, req.arg, status_irq.reg_value());
    return zx::error(ZX_ERR_IO_INVALID);
  }
  if (status_irq.resp_err()) {
    if (req.suppress_error_messages) {
      AML_SDMMC_TRACE("Response CRC Error, cmd%d, arg=0x%08x, status=0x%08x", req.cmd_idx, req.arg,
                      status_irq.reg_value());
    } else {
      AML_SDMMC_WARNING("Response CRC Error, cmd%d, arg=0x%08x, status=0x%08x, consecutive=%lu",
                        req.cmd_idx, req.arg, status_irq.reg_value(), ++consecutive_cmd_errors_);
    }
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }
  if (status_irq.resp_timeout()) {
    // A timeout is acceptable for SD_SEND_IF_COND but not for MMC_SEND_EXT_CSD.
    const bool is_sd_cmd8 =
        req.cmd_idx == SD_SEND_IF_COND && req.cmd_flags == SD_SEND_IF_COND_FLAGS;
    static_assert(SD_SEND_IF_COND == MMC_SEND_EXT_CSD &&
                  (SD_SEND_IF_COND_FLAGS) != (MMC_SEND_EXT_CSD_FLAGS));
    // When mmc dev_ice is being probed with SDIO command this is an expected failure.
    if (req.suppress_error_messages || is_sd_cmd8) {
      AML_SDMMC_TRACE("Response timeout, cmd%d, arg=0x%08x, status=0x%08x", req.cmd_idx, req.arg,
                      status_irq.reg_value());
    } else {
      AML_SDMMC_ERROR("Response timeout, cmd%d, arg=0x%08x, status=0x%08x, consecutive=%lu",
                      req.cmd_idx, req.arg, status_irq.reg_value(), ++consecutive_cmd_errors_);
    }
    return zx::error(ZX_ERR_TIMED_OUT);
  }
  if (status_irq.desc_timeout()) {
    AML_SDMMC_ERROR("Descriptor timeout, cmd%d, arg=0x%08x, status=0x%08x, consecutive=%lu",
                    req.cmd_idx, req.arg, status_irq.reg_value(), ++consecutive_data_errors_);
    return zx::error(ZX_ERR_TIMED_OUT);
  }

  if (!(status_irq.end_of_chain())) {
    AML_SDMMC_ERROR("END OF CHAIN bit is not set, cmd%d, arg=0x%08x, status=0x%08x", req.cmd_idx,
                    req.arg, status_irq.reg_value());
    return zx::error(ZX_ERR_IO_INVALID);
  }

  // At this point we have succeeded and don't need to perform our on-error call
  on_bus_error.cancel();

  consecutive_cmd_errors_ = 0;
  if (req.cmd_flags & SDMMC_RESP_DATA_PRESENT) {
    consecutive_data_errors_ = 0;
  }

  std::array<uint32_t, AmlSdmmc::kResponseCount> response = {};
  if (req.cmd_flags & SDMMC_RESP_LEN_136) {
    response[0] = AmlSdmmcCmdResp::Get().ReadFrom(&mmio_).reg_value();
    response[1] = AmlSdmmcCmdResp1::Get().ReadFrom(&mmio_).reg_value();
    response[2] = AmlSdmmcCmdResp2::Get().ReadFrom(&mmio_).reg_value();
    response[3] = AmlSdmmcCmdResp3::Get().ReadFrom(&mmio_).reg_value();
  } else {
    response[0] = AmlSdmmcCmdResp::Get().ReadFrom(&mmio_).reg_value();
  }

  return zx::ok(response);
}

zx_status_t AmlSdmmc::SdmmcHostInfo(sdmmc_host_info_t* info) {
  dev_info_.prefs = board_config_.prefs;
  memcpy(info, &dev_info_, sizeof(dev_info_));
  return ZX_OK;
}

zx_status_t AmlSdmmc::SdmmcSetBusWidth(sdmmc_bus_width_t bus_width) {
  uint32_t bus_width_val;
  switch (bus_width) {
    case SDMMC_BUS_WIDTH_EIGHT:
      bus_width_val = AmlSdmmcCfg::kBusWidth8Bit;
      break;
    case SDMMC_BUS_WIDTH_FOUR:
      bus_width_val = AmlSdmmcCfg::kBusWidth4Bit;
      break;
    case SDMMC_BUS_WIDTH_ONE:
      bus_width_val = AmlSdmmcCfg::kBusWidth1Bit;
      break;
    default:
      return ZX_ERR_OUT_OF_RANGE;
  }

  AmlSdmmcCfg::Get().ReadFrom(&mmio_).set_bus_width(bus_width_val).WriteTo(&mmio_);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
  return ZX_OK;
}

zx_status_t AmlSdmmc::SdmmcRegisterInBandInterrupt(
    const in_band_interrupt_protocol_t* interrupt_cb) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlSdmmc::SdmmcSetBusFreq(uint32_t freq) {
  uint32_t clk = 0, clk_src = 0, clk_div = 0;
  if (freq == 0) {
    AmlSdmmcClock::Get().ReadFrom(&mmio_).set_cfg_div(0).WriteTo(&mmio_);
    inspect_.bus_clock_frequency.Set(0);
    return ZX_OK;
  }

  if (freq > max_freq_) {
    freq = max_freq_;
  } else if (freq < min_freq_) {
    freq = min_freq_;
  }
  if (freq < AmlSdmmcClock::kFClkDiv2MinFreq) {
    clk_src = AmlSdmmcClock::kCtsOscinClkSrc;
    clk = AmlSdmmcClock::kCtsOscinClkFreq;
  } else {
    clk_src = AmlSdmmcClock::kFClkDiv2Src;
    clk = AmlSdmmcClock::kFClkDiv2Freq;
  }
  // Round the divider up so the frequency is rounded down.
  clk_div = (clk + freq - 1) / freq;
  AmlSdmmcClock::Get().ReadFrom(&mmio_).set_cfg_div(clk_div).set_cfg_src(clk_src).WriteTo(&mmio_);
  inspect_.bus_clock_frequency.Set(clk / clk_div);
  return ZX_OK;
}

void AmlSdmmc::ConfigureDefaultRegs() {
  if (board_config_.version_3) {
    uint32_t clk_val = AmlSdmmcClockV3::Get()
                           .FromValue(0)
                           .set_cfg_div(AmlSdmmcClock::kDefaultClkDiv)
                           .set_cfg_src(AmlSdmmcClock::kDefaultClkSrc)
                           .set_cfg_co_phase(AmlSdmmcClock::kDefaultClkCorePhase)
                           .set_cfg_tx_phase(AmlSdmmcClock::kDefaultClkTxPhase)
                           .set_cfg_rx_phase(AmlSdmmcClock::kDefaultClkRxPhase)
                           .set_cfg_always_on(1)
                           .reg_value();
    AmlSdmmcClockV3::Get().ReadFrom(&mmio_).set_reg_value(clk_val).WriteTo(&mmio_);
  } else {
    uint32_t clk_val = AmlSdmmcClockV2::Get()
                           .FromValue(0)
                           .set_cfg_div(AmlSdmmcClock::kDefaultClkDiv)
                           .set_cfg_src(AmlSdmmcClock::kDefaultClkSrc)
                           .set_cfg_co_phase(AmlSdmmcClock::kDefaultClkCorePhase)
                           .set_cfg_tx_phase(AmlSdmmcClock::kDefaultClkTxPhase)
                           .set_cfg_rx_phase(AmlSdmmcClock::kDefaultClkRxPhase)
                           .set_cfg_always_on(1)
                           .reg_value();
    AmlSdmmcClockV2::Get().ReadFrom(&mmio_).set_reg_value(clk_val).WriteTo(&mmio_);
  }

  uint32_t config_val = AmlSdmmcCfg::Get()
                            .FromValue(0)
                            .set_blk_len(AmlSdmmcCfg::kDefaultBlkLen)
                            .set_resp_timeout(AmlSdmmcCfg::kDefaultRespTimeout)
                            .set_rc_cc(AmlSdmmcCfg::kDefaultRcCc)
                            .set_bus_width(AmlSdmmcCfg::kBusWidth1Bit)
                            .reg_value();
  AmlSdmmcCfg::Get().ReadFrom(&mmio_).set_reg_value(config_val).WriteTo(&mmio_);
  AmlSdmmcStatus::Get()
      .ReadFrom(&mmio_)
      .set_reg_value(AmlSdmmcStatus::kClearStatus)
      .WriteTo(&mmio_);
  AmlSdmmcIrqEn::Get().ReadFrom(&mmio_).set_reg_value(AmlSdmmcStatus::kClearStatus).WriteTo(&mmio_);

  // Zero out any delay line or sampling settings that may have come from the bootloader.
  if (board_config_.version_3) {
    AmlSdmmcAdjust::Get().FromValue(0).WriteTo(&mmio_);
    AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&mmio_);
    AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&mmio_);
  } else {
    AmlSdmmcAdjustV2::Get().FromValue(0).WriteTo(&mmio_);
    AmlSdmmcDelayV2::Get().FromValue(0).WriteTo(&mmio_);
  }
}

void AmlSdmmc::SdmmcHwReset() {
  if (reset_gpio_.is_valid()) {
    reset_gpio_.ConfigOut(0);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    reset_gpio_.ConfigOut(1);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
  }
  ConfigureDefaultRegs();
}

zx_status_t AmlSdmmc::SdmmcSetTiming(sdmmc_timing_t timing) {
  auto config = AmlSdmmcCfg::Get().ReadFrom(&mmio_);
  if (timing == SDMMC_TIMING_HS400 || timing == SDMMC_TIMING_HSDDR ||
      timing == SDMMC_TIMING_DDR50) {
    if (timing == SDMMC_TIMING_HS400) {
      config.set_chk_ds(1);
    } else {
      config.set_chk_ds(0);
    }
    config.set_ddr(1);
    auto clk = AmlSdmmcClock::Get().ReadFrom(&mmio_);
    uint32_t clk_div = clk.cfg_div();
    if (clk_div & 0x01) {
      clk_div++;
    }
    clk_div /= 2;
    clk.set_cfg_div(clk_div).WriteTo(&mmio_);
  } else {
    config.set_ddr(0);
  }

  config.WriteTo(&mmio_);
  return ZX_OK;
}

zx_status_t AmlSdmmc::SdmmcSetSignalVoltage(sdmmc_voltage_t voltage) {
  // Amlogic controller does not allow to modify voltage
  // We do not return an error here since things work fine without switching the voltage.
  return ZX_OK;
}

aml_sdmmc_desc_t* AmlSdmmc::SetupCmdDesc(const sdmmc_req_t& req) {
  aml_sdmmc_desc_t* const desc = reinterpret_cast<aml_sdmmc_desc_t*>(descs_buffer_.virt());
  auto cmd_cfg = AmlSdmmcCmdCfg::Get().FromValue(0);
  if (req.cmd_flags == 0) {
    cmd_cfg.set_no_resp(1);
  } else {
    if (req.cmd_flags & SDMMC_RESP_LEN_136) {
      cmd_cfg.set_resp_128(1);
    }

    if (!(req.cmd_flags & SDMMC_RESP_CRC_CHECK)) {
      cmd_cfg.set_resp_no_crc(1);
    }

    if (req.cmd_flags & SDMMC_RESP_LEN_48B) {
      cmd_cfg.set_r1b(1);
    }

    cmd_cfg.set_resp_num(1);
  }
  cmd_cfg.set_cmd_idx(req.cmd_idx)
      .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
      .set_error(0)
      .set_owner(1)
      .set_end_of_chain(0);

  desc->cmd_info = cmd_cfg.reg_value();
  desc->cmd_arg = req.arg;
  desc->data_addr = 0;
  desc->resp_addr = 0;
  return desc;
}

zx::result<std::pair<aml_sdmmc_desc_t*, std::vector<fzl::PinnedVmo>>> AmlSdmmc::SetupDataDescs(
    const sdmmc_req_t& req, aml_sdmmc_desc_t* const cur_desc) {
  const uint32_t req_blk_len = log2_ceil(req.blocksize);
  if (req_blk_len > AmlSdmmcCfg::kMaxBlkLen) {
    AML_SDMMC_ERROR("blocksize %u is greater than the max (%u)", 1 << req_blk_len,
                    1 << AmlSdmmcCfg::kMaxBlkLen);
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  AmlSdmmcCfg::Get().ReadFrom(&mmio_).set_blk_len(req_blk_len).WriteTo(&mmio_);

  std::vector<fzl::PinnedVmo> pinned_vmos;
  pinned_vmos.reserve(req.buffers_count);

  aml_sdmmc_desc_t* desc = cur_desc;
  SdmmcVmoStore& vmos = *registered_vmos_[req.client_id];
  for (size_t i = 0; i < req.buffers_count; i++) {
    if (req.buffers_list[i].type == SDMMC_BUFFER_TYPE_VMO_HANDLE) {
      auto status = SetupUnownedVmoDescs(req, req.buffers_list[i], desc);
      if (!status.is_ok()) {
        return zx::error(status.error_value());
      }

      pinned_vmos.push_back(std::move(std::get<1>(status.value())));
      desc = std::get<0>(status.value());
    } else {
      vmo_store::StoredVmo<OwnedVmoInfo>* const stored_vmo =
          vmos.GetVmo(req.buffers_list[i].buffer.vmo_id);
      if (stored_vmo == nullptr) {
        AML_SDMMC_ERROR("no VMO %u for client %u", req.buffers_list[i].buffer.vmo_id,
                        req.client_id);
        return zx::error(ZX_ERR_NOT_FOUND);
      }
      auto status = SetupOwnedVmoDescs(req, req.buffers_list[i], *stored_vmo, desc);
      if (status.is_error()) {
        return zx::error(status.error_value());
      }
      desc = status.value();
    }
  }

  if (desc == cur_desc) {
    AML_SDMMC_ERROR("empty descriptor list!");
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  return zx::ok(std::pair{desc - 1, std::move(pinned_vmos)});
}

zx::result<aml_sdmmc_desc_t*> AmlSdmmc::SetupOwnedVmoDescs(const sdmmc_req_t& req,
                                                           const sdmmc_buffer_region_t& buffer,
                                                           vmo_store::StoredVmo<OwnedVmoInfo>& vmo,
                                                           aml_sdmmc_desc_t* const cur_desc) {
  if (!(req.cmd_flags & SDMMC_CMD_READ) && !(vmo.meta().rights & SDMMC_VMO_RIGHT_READ)) {
    AML_SDMMC_ERROR("Request would read from write-only VMO");
    return zx::error(ZX_ERR_ACCESS_DENIED);
  }
  if ((req.cmd_flags & SDMMC_CMD_READ) && !(vmo.meta().rights & SDMMC_VMO_RIGHT_WRITE)) {
    AML_SDMMC_ERROR("Request would write to read-only VMO");
    return zx::error(ZX_ERR_ACCESS_DENIED);
  }

  if (buffer.offset + buffer.size > vmo.meta().size) {
    AML_SDMMC_ERROR("buffer reads past vmo end: offset %zu, size %zu, vmo size %zu",
                    buffer.offset + vmo.meta().offset, buffer.size, vmo.meta().size);
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }

  fzl::PinnedVmo::Region regions[SDMMC_PAGES_COUNT];
  size_t offset = buffer.offset;
  size_t remaining = buffer.size;
  aml_sdmmc_desc_t* desc = cur_desc;
  while (remaining > 0) {
    size_t region_count = 0;
    zx_status_t status = vmo.GetPinnedRegions(offset + vmo.meta().offset, buffer.size, regions,
                                              std::size(regions), &region_count);
    if (status != ZX_OK && status != ZX_ERR_BUFFER_TOO_SMALL) {
      AML_SDMMC_ERROR("failed to get pinned regions: %d", status);
      return zx::error(status);
    }

    const size_t last_offset = offset;
    for (size_t i = 0; i < region_count; i++) {
      zx::result<aml_sdmmc_desc_t*> next_desc = PopulateDescriptors(req, desc, regions[i]);
      if (next_desc.is_error()) {
        return next_desc;
      }

      desc = next_desc.value();
      offset += regions[i].size;
      remaining -= regions[i].size;
    }

    if (offset == last_offset) {
      AML_SDMMC_ERROR("didn't get any pinned regions");
      return zx::error(ZX_ERR_BAD_STATE);
    }
  }

  return zx::ok(desc);
}

zx::result<std::pair<aml_sdmmc_desc_t*, fzl::PinnedVmo>> AmlSdmmc::SetupUnownedVmoDescs(
    const sdmmc_req_t& req, const sdmmc_buffer_region_t& buffer, aml_sdmmc_desc_t* const cur_desc) {
  const bool is_read = req.cmd_flags & SDMMC_CMD_READ;
  const uint64_t pagecount =
      ((buffer.offset & PageMask()) + buffer.size + PageMask()) / zx_system_get_page_size();

  const zx::unowned_vmo vmo(buffer.buffer.vmo);
  const uint32_t options = is_read ? ZX_BTI_PERM_WRITE : ZX_BTI_PERM_READ;

  fzl::PinnedVmo pinned_vmo;
  zx_status_t status = pinned_vmo.PinRange(
      buffer.offset & ~PageMask(), pagecount * zx_system_get_page_size(), *vmo, bti_, options);
  if (status != ZX_OK) {
    AML_SDMMC_ERROR("bti-pin failed with error %d", status);
    return zx::error(status);
  }

  // We don't own this VMO, and therefore cannot make any assumptions about the state of the
  // cache. The cache must be clean and invalidated for reads so that the final clean + invalidate
  // doesn't overwrite main memory with stale data from the cache, and must be clean for writes so
  // that main memory has the latest data.
  if (req.cmd_flags & SDMMC_CMD_READ) {
    status =
        vmo->op_range(ZX_VMO_OP_CACHE_CLEAN_INVALIDATE, buffer.offset, buffer.size, nullptr, 0);
  } else {
    status = vmo->op_range(ZX_VMO_OP_CACHE_CLEAN, buffer.offset, buffer.size, nullptr, 0);
  }

  if (status != ZX_OK) {
    AML_SDMMC_ERROR("Cache op on unowned VMO failed: %s", zx_status_get_string(status));
    return zx::error(status);
  }

  aml_sdmmc_desc_t* desc = cur_desc;
  for (uint32_t i = 0; i < pinned_vmo.region_count(); i++) {
    fzl::PinnedVmo::Region region = pinned_vmo.region(i);
    if (i == 0) {
      region.phys_addr += buffer.offset & PageMask();
      region.size -= buffer.offset & PageMask();
    }
    if (i == pinned_vmo.region_count() - 1) {
      const size_t end_offset =
          (pagecount * zx_system_get_page_size()) - buffer.size - (buffer.offset & PageMask());
      region.size -= end_offset;
    }

    zx::result<aml_sdmmc_desc_t*> next_desc = PopulateDescriptors(req, desc, region);
    if (next_desc.is_error()) {
      return zx::error(next_desc.error_value());
    }
    desc = next_desc.value();
  }

  return zx::ok(std::pair{desc, std::move(pinned_vmo)});
}

zx::result<aml_sdmmc_desc_t*> AmlSdmmc::PopulateDescriptors(const sdmmc_req_t& req,
                                                            aml_sdmmc_desc_t* const cur_desc,
                                                            fzl::PinnedVmo::Region region) {
  if (region.phys_addr > UINT32_MAX || (region.phys_addr + region.size) > UINT32_MAX) {
    AML_SDMMC_ERROR("DMA goes out of accessible range: 0x%0zx, %zu", region.phys_addr, region.size);
    return zx::error(ZX_ERR_BAD_STATE);
  }

  const bool use_block_mode = (1 << log2_ceil(req.blocksize)) == req.blocksize;
  const aml_sdmmc_desc_t* const descs_end =
      descs() + (descs_buffer_.size() / sizeof(aml_sdmmc_desc_t));

  const size_t max_desc_size =
      use_block_mode ? req.blocksize * AmlSdmmcCmdCfg::kMaxBlockCount : req.blocksize;

  aml_sdmmc_desc_t* desc = cur_desc;
  while (region.size > 0) {
    const size_t desc_size = std::min(region.size, max_desc_size);

    if (desc >= descs_end) {
      AML_SDMMC_ERROR("request with more than %d chunks is unsupported\n", AML_DMA_DESC_MAX_COUNT);
      return zx::error(ZX_ERR_NOT_SUPPORTED);
    }
    if (region.phys_addr % AmlSdmmcCmdCfg::kDataAddrAlignment != 0) {
      // The last two bits must be zero to indicate DDR/big-endian.
      AML_SDMMC_ERROR("DMA start address must be 4-byte aligned");
      return zx::error(ZX_ERR_NOT_SUPPORTED);
    }
    if (desc_size % req.blocksize != 0) {
      AML_SDMMC_ERROR("DMA length %zu is not multiple of block size %u", desc_size, req.blocksize);
      return zx::error(ZX_ERR_NOT_SUPPORTED);
    }

    auto cmd = AmlSdmmcCmdCfg::Get().FromValue(desc->cmd_info);
    if (desc != descs()) {
      cmd = AmlSdmmcCmdCfg::Get().FromValue(0);
      cmd.set_no_resp(1).set_no_cmd(1);
      desc->cmd_arg = 0;
      desc->resp_addr = 0;
    }

    cmd.set_data_io(1);
    if (!(req.cmd_flags & SDMMC_CMD_READ)) {
      cmd.set_data_wr(1);
    }
    cmd.set_owner(1).set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout).set_error(0);

    const size_t blockcount = desc_size / req.blocksize;
    if (use_block_mode) {
      cmd.set_block_mode(1).set_len(static_cast<uint32_t>(blockcount));
    } else if (blockcount == 1) {
      cmd.set_length(req.blocksize);
    } else {
      AML_SDMMC_ERROR("can't send more than one block of size %u", req.blocksize);
      return zx::error(ZX_ERR_NOT_SUPPORTED);
    }

    desc->cmd_info = cmd.reg_value();
    desc->data_addr = static_cast<uint32_t>(region.phys_addr);
    desc++;

    region.phys_addr += desc_size;
    region.size -= desc_size;
  }

  return zx::ok(desc);
}

zx_status_t AmlSdmmc::FinishReq(const sdmmc_req_t& req) {
  if ((req.cmd_flags & SDMMC_RESP_DATA_PRESENT) && (req.cmd_flags & SDMMC_CMD_READ)) {
    const cpp20::span<const sdmmc_buffer_region_t> regions{req.buffers_list, req.buffers_count};
    for (const auto& region : regions) {
      if (region.type != SDMMC_BUFFER_TYPE_VMO_HANDLE) {
        continue;
      }

      // Invalidate the cache so that the next CPU read will pick up data that was written to main
      // memory by the controller.
      zx_status_t status = zx_vmo_op_range(region.buffer.vmo, ZX_VMO_OP_CACHE_CLEAN_INVALIDATE,
                                           region.offset, region.size, nullptr, 0);
      if (status != ZX_OK) {
        AML_SDMMC_ERROR("Failed to clean/invalidate cache: %s", zx_status_get_string(status));
        return status;
      }
    }
  }

  return ZX_OK;
}

void AmlSdmmc::WaitForBus() const {
  while (!AmlSdmmcStatus::Get().ReadFrom(&mmio_).cmd_i()) {
    zx::nanosleep(zx::deadline_after(zx::usec(10)));
  }
}

zx_status_t AmlSdmmc::TuningDoTransfer(zx::unowned_vmo received_block, size_t blk_pattern_size,
                                       uint32_t tuning_cmd_idx) {
  const sdmmc_buffer_region_t buffer = {
      .buffer = {.vmo = received_block->get()},
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 0,
      .size = blk_pattern_size,
  };
  const sdmmc_req_t tuning_req{
      .cmd_idx = tuning_cmd_idx,
      .cmd_flags = MMC_SEND_TUNING_BLOCK_FLAGS,
      .arg = 0,
      .blocksize = static_cast<uint32_t>(blk_pattern_size),
      .suppress_error_messages = true,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };

  uint32_t unused_response[4];
  return AmlSdmmc::SdmmcRequest(&tuning_req, unused_response);
}

bool AmlSdmmc::TuningTestSettings(cpp20::span<const uint8_t> tuning_blk, uint32_t tuning_cmd_idx,
                                  zx::unowned_vmo received_block) {
  zx_status_t status = ZX_OK;
  size_t n;
  for (n = 0; n < AML_SDMMC_TUNING_TEST_ATTEMPTS; n++) {
    status = TuningDoTransfer(received_block->borrow(), tuning_blk.size(), tuning_cmd_idx);
    if (status != ZX_OK) {
      break;
    }

    uint8_t tuning_res[512] = {0};
    if ((status = received_block->read(tuning_res, 0, tuning_blk.size())) != ZX_OK) {
      AML_SDMMC_ERROR("Failed to read VMO: %s", zx_status_get_string(status));
      break;
    }
    if (memcmp(tuning_blk.data(), tuning_res, tuning_blk.size()) != 0) {
      break;
    }
  }
  return (n == AML_SDMMC_TUNING_TEST_ATTEMPTS);
}

AmlSdmmc::TuneWindow AmlSdmmc::ProcessTuningResults(uint32_t param_max,
                                                    fit::function<bool(uint32_t)> check_param) {
  return ProcessTuningResultsInternal(param_max, std::move(check_param), /* wrap = */ false);
}

AmlSdmmc::TuneWindow AmlSdmmc::ProcessTuningResultsWithWrapping(
    uint32_t param_max, fit::function<bool(uint32_t)> check_param) {
  return ProcessTuningResultsInternal(param_max, std::move(check_param), /* wrap = */ true);
}

AmlSdmmc::TuneWindow AmlSdmmc::ProcessTuningResultsInternal(
    uint32_t param_max, fit::function<bool(uint32_t)> check_param, bool wrap) {
  TuneWindow best_window, current_window;
  uint32_t first_size = 0;

  uint64_t tuning_results = 0;
  for (uint32_t param = 0; param <= param_max; param++) {
    if (check_param(param)) {
      tuning_results |= 1ULL << param;

      current_window.size++;
      if (current_window.start == 0) {
        first_size = current_window.size;
      }
    } else {
      if (current_window.size > best_window.size) {
        best_window = current_window;
      }

      current_window = {.start = param + 1, .size = 0};
    }
  }

  // If wrapping is enabled then the first and last windows are combined.
  const uint32_t last_window_size = current_window.size + (wrap ? first_size : 0);

  if (current_window.start == 0) {
    // The best window will not have been set if no values failed. If that happens the current
    // window start will still be set to zero -- check for that case and update the best window.
    best_window = {.start = 0, .size = param_max + 1};
  } else if (last_window_size > best_window.size) {
    // If the final value passed then the last (and current) window was never checked against the
    // best window. Make the last window the best window if it is larger than the previous best.
    best_window = {.start = current_window.start, .size = last_window_size};
  }

  best_window.results.results = tuning_results;
  best_window.results.param_max = param_max;
  return best_window;
}

AmlSdmmc::TuneResults AmlSdmmc::TuneDelayLines(cpp20::span<const uint8_t> tuning_blk,
                                               uint32_t tuning_cmd_idx,
                                               zx::unowned_vmo received_block) {
  TuneResults results = {};
  for (uint32_t i = 0; i <= max_delay(); i++) {
    SetDelayLines(i);
    if (TuningTestSettings(tuning_blk, tuning_cmd_idx, received_block->borrow())) {
      results.results |= 1ULL << i;
    }
  }
  results.param_max = max_delay();
  return results;
}

void AmlSdmmc::SetAdjDelay(uint32_t adj_delay) {
  if (board_config_.version_3) {
    AmlSdmmcAdjust::Get().ReadFrom(&mmio_).set_adj_delay(adj_delay).set_adj_fixed(1).WriteTo(
        &mmio_);
  } else {
    AmlSdmmcAdjustV2::Get().ReadFrom(&mmio_).set_adj_delay(adj_delay).set_adj_fixed(1).WriteTo(
        &mmio_);
  }
}

void AmlSdmmc::SetDelayLines(uint32_t delay) {
  if (board_config_.version_3) {
    AmlSdmmcDelay1::Get()
        .ReadFrom(&mmio_)
        .set_dly_0(delay)
        .set_dly_1(delay)
        .set_dly_2(delay)
        .set_dly_3(delay)
        .set_dly_4(delay)
        .WriteTo(&mmio_);
    AmlSdmmcDelay2::Get()
        .ReadFrom(&mmio_)
        .set_dly_5(delay)
        .set_dly_6(delay)
        .set_dly_7(delay)
        .set_dly_8(delay)
        .set_dly_9(delay)
        .WriteTo(&mmio_);
  } else {
    AmlSdmmcDelayV2::Get()
        .ReadFrom(&mmio_)
        .set_dly_0(delay)
        .set_dly_1(delay)
        .set_dly_2(delay)
        .set_dly_3(delay)
        .set_dly_4(delay)
        .set_dly_5(delay)
        .set_dly_6(delay)
        .set_dly_7(delay)
        .WriteTo(&mmio_);
    AmlSdmmcAdjustV2::Get().ReadFrom(&mmio_).set_dly_8(delay).set_dly_9(delay).WriteTo(&mmio_);
  }
}

uint32_t AmlSdmmc::max_delay() const {
  return board_config_.version_3 ? AmlSdmmcClock::kMaxDelay : AmlSdmmcClock::kMaxDelayV2;
}

inline uint32_t AbsDifference(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

uint32_t AmlSdmmc::DistanceToFailingPoint(TuneSettings point,
                                          cpp20::span<const TuneResults> adj_delay_results) {
  uint64_t results = adj_delay_results[point.adj_delay].results;
  uint32_t min_distance = max_delay();
  for (uint32_t i = 0; i <= max_delay(); i++, results >>= 1) {
    if ((results & 1) == 0) {
      const uint32_t distance = AbsDifference(i, point.delay);
      if (distance < min_distance) {
        min_distance = distance;
      }
    }
  }

  return min_distance;
}

zx_status_t AmlSdmmc::SdmmcPerformTuning(uint32_t tuning_cmd_idx) {
  cpp20::span<const uint8_t> tuning_blk;

  uint32_t bw = AmlSdmmcCfg::Get().ReadFrom(&mmio_).bus_width();
  if (bw == AmlSdmmcCfg::kBusWidth4Bit) {
    tuning_blk = cpp20::span<const uint8_t>(aml_sdmmc_tuning_blk_pattern_4bit,
                                            sizeof(aml_sdmmc_tuning_blk_pattern_4bit));
  } else if (bw == AmlSdmmcCfg::kBusWidth8Bit) {
    tuning_blk = cpp20::span<const uint8_t>(aml_sdmmc_tuning_blk_pattern_8bit,
                                            sizeof(aml_sdmmc_tuning_blk_pattern_8bit));
  } else {
    AML_SDMMC_ERROR("Tuning at wrong buswidth: %d", bw);
    return ZX_ERR_INTERNAL;
  }

  const uint32_t clk_div = AmlSdmmcClock::Get().ReadFrom(&mmio_).cfg_div();

  zx::vmo received_block;
  zx_status_t status = zx::vmo::create(tuning_blk.size(), 0, &received_block);
  if (status != ZX_OK) {
    AML_SDMMC_ERROR("Failed to create VMO: %s", zx_status_get_string(status));
    return status;
  }

  TuneResults adj_delay_results[AmlSdmmcClock::kMaxClkDiv] = {};
  for (uint32_t i = 0; i < clk_div; i++) {
    SetAdjDelay(i);

    char property_name[28];  // strlen("tuning_results_adj_delay_63")
    snprintf(property_name, sizeof(property_name), "tuning_results_adj_delay_%u", i);

    adj_delay_results[i] = TuneDelayLines(tuning_blk, tuning_cmd_idx, received_block.borrow());

    const std::string results = adj_delay_results[i].ToString();

    inspect::Node node = inspect_.root.CreateChild(property_name);
    inspect_.tuning_results.push_back(node.CreateString("tuning_results", results));
    inspect_.tuning_results_nodes.push_back(std::move(node));

    // Add a leading zero so that fx iquery show-file sorts the results properly.
    AML_SDMMC_INFO("Tuning results [%02u]: %s", i, results.c_str());
  }

  zx::result<TuneSettings> tuning_settings;
  if (board_config_.use_new_tuning) {
    tuning_settings = PerformNewTuning({adj_delay_results, adj_delay_results + clk_div});
  } else {
    inspect_.tuning_method.Set("old");
    tuning_settings = PerformOldTuning({adj_delay_results, adj_delay_results + clk_div});
  }

  if (tuning_settings.is_error()) {
    SetAdjDelay(0);
    SetDelayLines(0);
    return tuning_settings.status_value();
  }

  SetAdjDelay(tuning_settings->adj_delay);
  inspect_.adj_delay.Set(tuning_settings->adj_delay);

  SetDelayLines(tuning_settings->delay);
  inspect_.delay_lines.Set(tuning_settings->delay);

  inspect_.distance_to_failing_point.Set(
      DistanceToFailingPoint(*tuning_settings, adj_delay_results));

  AML_SDMMC_INFO("Clock divider %u, adj delay %u, delay %u", clk_div, tuning_settings->adj_delay,
                 tuning_settings->delay);
  return ZX_OK;
}

zx::result<AmlSdmmc::TuneSettings> AmlSdmmc::PerformNewTuning(
    cpp20::span<const TuneResults> adj_delay_results) {
  TuneWindow largest_failing_window = {};
  uint32_t failing_adj_delay = 0;
  for (uint32_t i = 0; i < adj_delay_results.size(); i++) {
    const TuneWindow failing_window = ProcessTuningResults(max_delay(), [&, i](uint32_t delay) {
      // Invert the results so that the largest failing window is returned.
      return (adj_delay_results[i].results & (1ULL << delay)) == 0;
    });

    if (failing_window.size > largest_failing_window.size) {
      largest_failing_window = failing_window;
      failing_adj_delay = i;
    }
  }

  if (largest_failing_window.size == 0) {
    AML_SDMMC_WARNING("No tuning transfers failed, falling back to old tuning");
    inspect_.tuning_method.Set("old; new failed because all transfers succeeded");
    return PerformOldTuning(adj_delay_results);
  }

  const uint32_t clk_div = AmlSdmmcClock::Get().ReadFrom(&mmio_).cfg_div();
  const uint32_t best_adj_delay = (failing_adj_delay + (clk_div / 2)) % clk_div;

  // For even dividers adj_delay will be exactly 180 degrees phase shifted from the chosen point,
  // so set the delay lines to the middle of the largest failing window. For odd dividers just
  // choose the first failing delay value, and set adj_delay to as close as 180 degrees shifted as
  // possible (rounding down).
  const uint32_t best_delay =
      (clk_div % 2 == 0) ? largest_failing_window.middle() : largest_failing_window.start;

  const TuneSettings results{.adj_delay = best_adj_delay, .delay = best_delay};

  // TODO(fxbug.dev/68436): Get rid if the old tuning method once we have confidence in the new one.
  if (!adj_delay_results[best_adj_delay].all_passed()) {
    const zx::result<TuneSettings> old_results = PerformOldTuning(adj_delay_results);
    if (old_results.is_ok()) {
      const uint32_t new_tuning_distance = DistanceToFailingPoint(results, adj_delay_results);
      const uint32_t old_tuning_distance = DistanceToFailingPoint(*old_results, adj_delay_results);

      if (new_tuning_distance < old_tuning_distance) {
        AML_SDMMC_WARNING("New tuning was closer to a failing point, falling back to old tuning");
        inspect_.tuning_method.Set("old; new was closer to a failing point");
        return old_results;
      }
    }
  }

  inspect_.longest_window_start.Set(largest_failing_window.start);
  inspect_.longest_window_size.Set(largest_failing_window.size);
  inspect_.longest_window_adj_delay.Set(failing_adj_delay);
  inspect_.tuning_method.Set("new");

  AML_SDMMC_INFO("Largest failing window: adj_delay %u, delay start %u, size %u, middle %u",
                 failing_adj_delay, largest_failing_window.start, largest_failing_window.size,
                 largest_failing_window.middle());
  return zx::ok(results);
}

zx::result<AmlSdmmc::TuneSettings> AmlSdmmc::PerformOldTuning(
    cpp20::span<const TuneResults> adj_delay_results) {
  // The delay line values don't actually wrap, but treating them like they do is the current tuning
  // behavior. Preserve that behavior so as to not affect any of the devices that haven't been
  // switched over to the new tuning.
  TuneWindow windows_with_wrapping[adj_delay_results.size()];
  for (uint32_t i = 0; i < adj_delay_results.size(); i++) {
    windows_with_wrapping[i] = ProcessTuningResultsWithWrapping(
        max_delay(),
        [&, i](uint32_t delay) { return (adj_delay_results[i].results & (1ULL << delay)) != 0; });
  }

  const uint32_t clk_div = AmlSdmmcClock::Get().ReadFrom(&mmio_).cfg_div();

  // Create a window of working adj_delay values where the delay lines are set to zero, and set
  // adj_delay to the middle of the window. We already swept adj_delay and the delay lines, so just
  // process the existing results to determine the working window.
  const TuneWindow adj_delay_window = ProcessTuningResultsWithWrapping(
      clk_div - 1,
      [&](uint32_t adj_delay) { return windows_with_wrapping[adj_delay].results.results & 1; });

  if (adj_delay_window.size == 0) {
    AML_SDMMC_ERROR("No window found for any phase");
    inspect_.tuning_results.push_back(inspect_.root.CreateString("tuning_results", "failed"));
    return zx::error(ZX_ERR_IO);
  }

  const uint32_t best_adj_delay =
      adj_delay_window.size == clk_div ? 0 : adj_delay_window.middle() % clk_div;

  const TuneWindow& delay_window = windows_with_wrapping[best_adj_delay];
  if (delay_window.size == 0) {
    AML_SDMMC_ERROR("No delay window found");
    inspect_.tuning_results.push_back(inspect_.root.CreateString("tuning_results", "failed"));
    return zx::error(ZX_ERR_IO);
  }

  const uint32_t best_delay = delay_window.middle() % (max_delay() + 1);

  inspect_.longest_window_start.Set(delay_window.start);
  inspect_.longest_window_size.Set(delay_window.size);
  inspect_.longest_window_adj_delay.Set(best_adj_delay);

  return zx::ok(TuneSettings{.adj_delay = best_adj_delay, .delay = best_delay});
}

zx_status_t AmlSdmmc::SdmmcRegisterVmo(uint32_t vmo_id, uint8_t client_id, zx::vmo vmo,
                                       uint64_t offset, uint64_t size, uint32_t vmo_rights) {
  if (client_id >= std::size(registered_vmos_)) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (vmo_rights == 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  vmo_store::StoredVmo<OwnedVmoInfo> stored_vmo(std::move(vmo), OwnedVmoInfo{
                                                                    .offset = offset,
                                                                    .size = size,
                                                                    .rights = vmo_rights,
                                                                });
  const uint32_t read_perm = (vmo_rights & SDMMC_VMO_RIGHT_READ) ? ZX_BTI_PERM_READ : 0;
  const uint32_t write_perm = (vmo_rights & SDMMC_VMO_RIGHT_WRITE) ? ZX_BTI_PERM_WRITE : 0;
  zx_status_t status = stored_vmo.Pin(bti_, read_perm | write_perm, true);
  if (status != ZX_OK) {
    AML_SDMMC_ERROR("Failed to pin VMO %u for client %u: %s", vmo_id, client_id,
                    zx_status_get_string(status));
    return status;
  }

  return registered_vmos_[client_id]->RegisterWithKey(vmo_id, std::move(stored_vmo));
}

zx_status_t AmlSdmmc::SdmmcUnregisterVmo(uint32_t vmo_id, uint8_t client_id, zx::vmo* out_vmo) {
  if (client_id >= std::size(registered_vmos_)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  vmo_store::StoredVmo<OwnedVmoInfo>* const vmo_info = registered_vmos_[client_id]->GetVmo(vmo_id);
  if (!vmo_info) {
    return ZX_ERR_NOT_FOUND;
  }

  zx_status_t status = vmo_info->vmo()->duplicate(ZX_RIGHT_SAME_RIGHTS, out_vmo);
  if (status != ZX_OK) {
    return status;
  }

  return registered_vmos_[client_id]->Unregister(vmo_id).status_value();
}

zx_status_t AmlSdmmc::SdmmcRequest(const sdmmc_req_t* req, uint32_t out_response[4]) {
  if (req->client_id >= std::size(registered_vmos_)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  {
    fbl::AutoLock lock(&mtx_);
    if (dead_) {
      return ZX_ERR_CANCELED;
    }

    pending_txn_ = true;
  }

  // Wait for the bus to become idle before issuing the next request. This could be necessary if the
  // card is driving CMD low after a voltage switch.
  WaitForBus();

  // stop executing
  AmlSdmmcStart::Get().ReadFrom(&mmio_).set_desc_busy(0).WriteTo(&mmio_);

  std::optional<std::vector<fzl::PinnedVmo>> pinned_vmos;

  aml_sdmmc_desc_t* desc = SetupCmdDesc(*req);
  aml_sdmmc_desc_t* last_desc = desc;
  if (req->cmd_flags & SDMMC_RESP_DATA_PRESENT) {
    auto status = SetupDataDescs(*req, desc);
    if (status.is_error()) {
      AML_SDMMC_ERROR("Failed to setup data descriptors");

      fbl::AutoLock lock(&mtx_);
      pending_txn_ = false;
      txn_finished_.Signal();

      return status.error_value();
    }
    last_desc = std::get<0>(status.value());
    pinned_vmos.emplace(std::move(std::get<1>(status.value())));
  }

  auto cmd_info = AmlSdmmcCmdCfg::Get().FromValue(last_desc->cmd_info);
  cmd_info.set_end_of_chain(1);
  last_desc->cmd_info = cmd_info.reg_value();
  AML_SDMMC_TRACE("SUBMIT req:%p cmd_idx: %d cmd_cfg: 0x%x cmd_dat: 0x%x cmd_arg: 0x%x", req,
                  req->cmd_idx, desc->cmd_info, desc->data_addr, desc->cmd_arg);

  zx_paddr_t desc_phys;

  auto start_reg = AmlSdmmcStart::Get().ReadFrom(&mmio_);
  desc_phys = descs_buffer_.phys();
  descs_buffer_.CacheFlush(0, descs_buffer_.size());
  // Read desc from external DDR
  start_reg.set_desc_int(0);

  ClearStatus();

  start_reg.set_desc_busy(1).set_desc_addr((static_cast<uint32_t>(desc_phys)) >> 2).WriteTo(&mmio_);

  zx::result<std::array<uint32_t, AmlSdmmc::kResponseCount>> response = WaitForInterrupt(*req);
  if (response.is_ok()) {
    memcpy(out_response, response.value().data(), sizeof(uint32_t) * AmlSdmmc::kResponseCount);
  }

  if (zx_status_t status = FinishReq(*req); status != ZX_OK) {
    return status;
  }

  fbl::AutoLock lock(&mtx_);
  pending_txn_ = false;
  txn_finished_.Signal();

  return response.status_value();
}

zx_status_t AmlSdmmc::Init(const pdev_device_info_t& device_info) {
  // The core clock must be enabled before attempting to access the start register.
  ConfigureDefaultRegs();

  // Stop processing DMA descriptors before releasing quarantine.
  AmlSdmmcStart::Get().ReadFrom(&mmio_).set_desc_busy(0).WriteTo(&mmio_);
  zx_status_t status = bti_.release_quarantine();
  if (status != ZX_OK) {
    AML_SDMMC_ERROR("Failed to release quarantined pages");
    return status;
  }

  dev_info_.caps = SDMMC_HOST_CAP_BUS_WIDTH_8 | SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR104 |
                   SDMMC_HOST_CAP_SDR50 | SDMMC_HOST_CAP_DDR50;
  if (board_config_.supports_dma) {
    dev_info_.caps |= SDMMC_HOST_CAP_DMA;
    status = descs_buffer_.Init(bti_.get(), AML_DMA_DESC_MAX_COUNT * sizeof(aml_sdmmc_desc_t),
                                IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
      AML_SDMMC_ERROR("Failed to allocate dma descriptors");
      return status;
    }
    dev_info_.max_transfer_size = AML_DMA_DESC_MAX_COUNT * zx_system_get_page_size();
  } else {
    dev_info_.max_transfer_size = AML_SDMMC_MAX_PIO_DATA_SIZE;
  }

  dev_info_.max_transfer_size_non_dma = AML_SDMMC_MAX_PIO_DATA_SIZE;
  max_freq_ = board_config_.max_freq;
  min_freq_ = board_config_.min_freq;

  inspect_.Init(device_info);
  inspect_.max_delay.Set(max_delay() + 1);

  return ZX_OK;
}

zx_status_t AmlSdmmc::Bind() {
  // Note: This name can't be changed without migrating users in other repos.
  zx_status_t status = DdkAdd(ddk::DeviceAddArgs("aml-sd-emmc").set_inspect_vmo(GetInspectVmo()));
  if (status != ZX_OK) {
    irq_.destroy();
    AML_SDMMC_ERROR("DdkAdd failed");
  }
  return status;
}

zx_status_t AmlSdmmc::Create(void* ctx, zx_device_t* parent) {
  auto pdev = ddk::PDevFidl::FromFragment(parent);
  if (!pdev.is_valid()) {
    AML_SDMMC_ERROR("Could not get pdev.");
    return ZX_ERR_NO_RESOURCES;
  }

  zx::bti bti;
  zx_status_t status = ZX_OK;
  if ((status = pdev.GetBti(0, &bti)) != ZX_OK) {
    AML_SDMMC_ERROR("Failed to get BTI: %d", status);
    return status;
  }

  std::optional<fdf::MmioBuffer> mmio;
  status = pdev.MapMmio(0, &mmio);
  if (status != ZX_OK) {
    AML_SDMMC_ERROR("Failed to get mmio: %d", status);
    return status;
  }

  // Populate board specific information
  aml_sdmmc_config_t config;
  size_t actual;
  status = device_get_metadata(parent, DEVICE_METADATA_PRIVATE, &config, sizeof(config), &actual);
  if (status != ZX_OK || actual != sizeof(config)) {
    AML_SDMMC_ERROR("Failed to get metadata: %d", status);
    return status;
  }

  zx::interrupt irq;
  if ((status = pdev.GetInterrupt(0, &irq)) != ZX_OK) {
    AML_SDMMC_ERROR("Failed to get interrupt: %d", status);
    return status;
  }

  pdev_device_info_t dev_info;
  if ((status = pdev.GetDeviceInfo(&dev_info)) != ZX_OK) {
    AML_SDMMC_ERROR("Failed to get device info: %d", status);
    return status;
  }

  // For AV400, the clock source was set to GP0_PLL(1152M) in bootloader,
  // We should change to xtal(24M), then can divide to 400k for initial stage.
  if (dev_info.pid == PDEV_PID_AMLOGIC_A5) {
    ddk::ClockProtocolClient emmc_clk_sel(parent, "clk-emmc-sel");
    if ((status = emmc_clk_sel.SetInput(AmlSdmmcClock::kCtsOscinClkSrc)) != ZX_OK) {
      AML_SDMMC_ERROR("Could not SetInput clock for emmc: %d", status);
      return status;
    }
  } else if (dev_info.pid == PDEV_PID_AMLOGIC_A1) {
    zx::resource smc_handler;
    status = pdev.GetSmc(0, &smc_handler);
    if (status != ZX_OK) {
      AML_SDMMC_ERROR("Failed to get smc handler: %s", zx_status_get_string(status));
      return status;
    }

    // Power domain
    static const zx_smc_parameters_t kSetPdCall =
        aml_pd_smc::CreatePdSmcCall(A1_PDID_SD_EMMC, kPowerOn);
    zx_smc_result_t result;
    status = zx_smc_call(smc_handler.get(), &kSetPdCall, &result);
    if (status != ZX_OK) {
      AML_SDMMC_ERROR("Call zx_smc_call failed: %s", zx_status_get_string(status));
      return status;
    }

    // Clock source
    ddk::ClockProtocolClient sdio_clk_gate(parent, "sdio-clk-gate");
    if ((status = sdio_clk_gate.Enable()) != ZX_OK) {
      AML_SDMMC_ERROR("Could not enable the clk for sdio: %s", zx_status_get_string(status));
      return status;
    }
  }

  // Optional protocol.
  ddk::GpioProtocolClient reset_gpio(parent, "gpio-wifi-power-on");
  if (!reset_gpio.is_valid()) {
    // Alternative name.
    reset_gpio = ddk::GpioProtocolClient(parent, "gpio");
  }

  auto dev = std::make_unique<AmlSdmmc>(parent, std::move(bti), *std::move(mmio), config,
                                        std::move(irq), reset_gpio);

  if ((status = dev->Init(dev_info)) != ZX_OK) {
    return status;
  }

  if ((status = dev->Bind()) != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* placeholder = dev.release();
  return ZX_OK;
}

void AmlSdmmc::ShutDown() {
  // If there's a pending request, wait for it to complete (and any pages to be unpinned) before
  // proceeding with suspend/unbind.
  {
    fbl::AutoLock lock(&mtx_);
    dead_ = true;

    if (pending_txn_) {
      AML_SDMMC_ERROR("A request was pending after suspend/release");
    }

    while (pending_txn_) {
      txn_finished_.Wait(&mtx_);
    }
  }
}

void AmlSdmmc::DdkSuspend(ddk::SuspendTxn txn) {
  ShutDown();

  // DdkRelease() is not always called after this, so manually unpin the DMA buffer.
  descs_buffer_.release();

  txn.Reply(ZX_OK, txn.requested_state());
}

void AmlSdmmc::DdkRelease() {
  ShutDown();
  irq_.destroy();
  delete this;
}

static constexpr zx_driver_ops_t aml_sdmmc_driver_ops = []() {
  zx_driver_ops_t driver_ops = {};
  driver_ops.version = DRIVER_OPS_VERSION;
  driver_ops.bind = AmlSdmmc::Create;
  return driver_ops;
}();

}  // namespace sdmmc

ZIRCON_DRIVER(aml_sdmmc, sdmmc::aml_sdmmc_driver_ops, "zircon", "0.1");
