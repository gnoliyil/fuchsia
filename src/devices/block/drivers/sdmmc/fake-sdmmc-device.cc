// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fake-sdmmc-device.h"

#include <lib/fzl/vmo-mapper.h>
#include <lib/sdio/hw.h>
#include <lib/sdmmc/hw.h>

#include <zxtest/zxtest.h>

namespace sdmmc {

zx_status_t FakeSdmmcDevice::SdmmcHostInfo(sdmmc_host_info_t* out_info) {
  memcpy(out_info, &host_info_, sizeof(host_info_));
  return ZX_OK;
}

zx_status_t FakeSdmmcDevice::SdmmcRequestInternal(const sdmmc_req_t& req, uint32_t out_response[4],
                                                  cpp20::span<uint8_t> out_data) {
  command_counts_[req.cmd_idx]++;

  if (!out_data.empty() && out_data.size() % req.blocksize) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Zero out the buffer for read requests.
  if (!out_data.empty() && (req.cmd_flags & SDMMC_RESP_DATA_PRESENT) &&
      (req.cmd_flags & SDMMC_CMD_READ)) {
    memset(out_data.data(), 0, out_data.size());
  }

  out_response[0] = 0;
  out_response[1] = 0;
  out_response[2] = 0;
  out_response[3] = 0;

  switch (req.cmd_idx) {
    case SDMMC_READ_BLOCK:
    case SDMMC_READ_MULTIPLE_BLOCK: {
      if ((req.arg & kBadRegionMask) == kBadRegionStart) {
        return ZX_ERR_IO;
      }

      memcpy(out_data.data(), Read(req.arg * kBlockSize, out_data.size()).data(), out_data.size());
      break;
    }
    case SDMMC_WRITE_BLOCK:
    case SDMMC_WRITE_MULTIPLE_BLOCK: {
      if ((req.arg & kBadRegionMask) == kBadRegionStart) {
        return ZX_ERR_IO;
      }

      Write(req.arg * kBlockSize, out_data);
      break;
    }
    case MMC_ERASE_GROUP_START:
      if ((req.arg & kBadRegionMask) == kBadRegionStart) {
        erase_group_start_.reset();
        erase_group_end_.reset();
        return ZX_ERR_IO;
      }

      if (erase_group_end_) {
        out_response[0] = MMC_STATUS_ERASE_SEQ_ERR;
        erase_group_start_.reset();
        erase_group_end_.reset();
      } else {
        erase_group_start_ = req.arg;
      }
      break;
    case MMC_ERASE_GROUP_END:
      if ((req.arg & kBadRegionMask) == kBadRegionStart) {
        erase_group_start_.reset();
        erase_group_end_.reset();
        return ZX_ERR_IO;
      }

      if (!erase_group_start_) {
        out_response[0] = MMC_STATUS_ERASE_SEQ_ERR;
        erase_group_start_.reset();
        erase_group_end_.reset();
      } else if (req.arg < erase_group_start_) {
        out_response[0] = MMC_STATUS_ERASE_PARAM;
        erase_group_start_.reset();
        erase_group_end_.reset();
      } else {
        erase_group_end_ = req.arg;
      }
      break;
    case SDMMC_ERASE:
      if (!erase_group_start_ || !erase_group_end_) {
        out_response[0] = MMC_STATUS_ERASE_SEQ_ERR;
      } else if (req.arg != MMC_ERASE_DISCARD_ARG || *erase_group_start_ > *erase_group_end_) {
        out_response[0] = MMC_STATUS_ERASE_PARAM;
      } else {
        Erase(*erase_group_start_ * kBlockSize,
              (*erase_group_end_ - *erase_group_start_ + 1) * kBlockSize);
      }

      erase_group_start_.reset();
      erase_group_end_.reset();
      break;
    case SDIO_IO_RW_DIRECT: {
      const uint32_t address =
          (req.arg & SDIO_IO_RW_DIRECT_REG_ADDR_MASK) >> SDIO_IO_RW_DIRECT_REG_ADDR_LOC;
      const uint8_t function =
          (req.arg & SDIO_IO_RW_DIRECT_FN_IDX_MASK) >> SDIO_IO_RW_DIRECT_FN_IDX_LOC;
      if (req.arg & SDIO_IO_RW_DIRECT_RW_FLAG) {
        Write(address,
              std::vector{static_cast<uint8_t>(req.arg & SDIO_IO_RW_DIRECT_WRITE_BYTE_MASK)},
              function);
      } else {
        out_response[0] = Read(address, 1, function)[0];
      }
      break;
    }
    case SDIO_IO_RW_DIRECT_EXTENDED: {
      const uint32_t address =
          (req.arg & SDIO_IO_RW_EXTD_REG_ADDR_MASK) >> SDIO_IO_RW_EXTD_REG_ADDR_LOC;
      const uint8_t function =
          (req.arg & SDIO_IO_RW_EXTD_FN_IDX_MASK) >> SDIO_IO_RW_EXTD_FN_IDX_LOC;
      const uint32_t block_mode = req.arg & SDIO_IO_RW_EXTD_BLOCK_MODE;
      const uint32_t blocks = req.arg & SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_MASK;
      const std::vector<uint8_t> block_size_reg = Read(0x10 | (function << 8), 2, 0);
      const uint32_t block_size = block_size_reg[0] | (block_size_reg[1] << 8);
      const uint32_t transfer_size =
          block_mode ? (block_size * blocks) : (blocks == 0 ? 512 : blocks);
      if (req.arg & SDIO_IO_RW_DIRECT_RW_FLAG) {
        Write(address, {out_data.data(), transfer_size}, function);
      } else {
        memcpy(out_data.data(), Read(address, transfer_size, function).data(), transfer_size);
      }
      break;
    }
    default:
      break;
  }

  zx_status_t status = ZX_OK;

  if (command_callbacks_.find(req.cmd_idx) != command_callbacks_.end()) {
    const auto& callback = command_callbacks_[req.cmd_idx];
    if (std::holds_alternative<CommandCallbackRequestOnly>(callback)) {
      std::get<CommandCallbackRequestOnly>(callback)(req);
    } else if (std::holds_alternative<CommandCallbackResponseOnly>(callback)) {
      std::get<CommandCallbackResponseOnly>(callback)(out_response);
    } else if (std::holds_alternative<CommandCallbackRequestAndResponse>(callback)) {
      std::get<CommandCallbackRequestAndResponse>(callback)(req, out_response);
    } else if (std::holds_alternative<CommandCallbackWithData>(callback)) {
      std::get<CommandCallbackWithData>(callback)(out_data);
    } else if (std::holds_alternative<CommandCallbackWithStatus>(callback)) {
      status = std::get<CommandCallbackWithStatus>(callback)(req);
    }
  }

  requests_.push_back(req);
  return status;
}

zx_status_t FakeSdmmcDevice::SdmmcRegisterInBandInterrupt(
    const in_band_interrupt_protocol_t* interrupt_cb) {
  interrupt_cb_ = *interrupt_cb;
  return ZX_OK;
}

zx_status_t FakeSdmmcDevice::SdmmcRegisterVmo(uint32_t vmo_id, uint8_t client_id, zx::vmo vmo,
                                              uint64_t offset, uint64_t size, uint32_t vmo_rights) {
  if (client_id >= std::size(registered_vmos_)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  return registered_vmos_[client_id]->RegisterWithKey(vmo_id, std::move(vmo),
                                                      OwnedVmoInfo{.offset = offset, .size = size});
}

zx_status_t FakeSdmmcDevice::SdmmcUnregisterVmo(uint32_t vmo_id, uint8_t client_id,
                                                zx::vmo* out_vmo) {
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

zx_status_t FakeSdmmcDevice::SdmmcRequest(const sdmmc_req_t* req, uint32_t out_response[4]) {
  if (req->client_id >= std::size(registered_vmos_)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  cpp20::span buffers{req->buffers_list, req->buffers_count};
  SdmmcVmoStore& owned_vmos = *registered_vmos_[req->client_id];

  fzl::VmoMapper linear_vmo;
  cpp20::span<uint8_t> linear_buffer{};
  if (req->cmd_flags & SDMMC_RESP_DATA_PRESENT) {
    size_t total_size = 0;
    for (const sdmmc_buffer_region_t& buffer : buffers) {
      total_size += buffer.size;
    }

    if (total_size % req->blocksize != 0) {
      return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status = linear_vmo.CreateAndMap(total_size, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (status != ZX_OK) {
      return status;
    }

    if (!(req->cmd_flags & SDMMC_CMD_READ)) {
      uint8_t* const linear_buffer = static_cast<uint8_t*>(linear_vmo.start());
      if ((status = CopySdmmcRegions(buffers, owned_vmos, linear_buffer, false)) != ZX_OK) {
        return status;
      }
    }

    linear_buffer = {static_cast<uint8_t*>(linear_vmo.start()), linear_vmo.size()};
  }

  zx_status_t status = SdmmcRequestInternal(*req, out_response, linear_buffer);
  if (status != ZX_OK) {
    return status;
  }

  if ((req->cmd_flags & SDMMC_RESP_DATA_PRESENT) && (req->cmd_flags & SDMMC_CMD_READ)) {
    uint8_t* const linear_buffer = static_cast<uint8_t*>(linear_vmo.start());
    return CopySdmmcRegions(buffers, owned_vmos, linear_buffer, true);
  }

  return ZX_OK;
}

std::vector<uint8_t> FakeSdmmcDevice::Read(size_t address, size_t size, uint8_t func) {
  std::map<size_t, std::unique_ptr<uint8_t[]>>& sectors = sectors_[func];

  std::vector<uint8_t> ret;
  size_t start = address;
  for (; start < address + size; start = (start & kBlockMask) + kBlockSize) {
    if (sectors.find(start & kBlockMask) == sectors.end()) {
      sectors[start & kBlockMask].reset(new uint8_t[kBlockSize]);
      memset(sectors[start & kBlockMask].get(), 0xff, kBlockSize);
    }

    const size_t read_offset = start - (start & kBlockMask);
    const size_t read_size = std::min(kBlockSize - read_offset, size - start + address);
    const uint8_t* const read_ptr = sectors[start & kBlockMask].get() + read_offset;
    ret.insert(ret.end(), read_ptr, read_ptr + read_size);
  }

  return ret;
}

void FakeSdmmcDevice::Write(size_t address, cpp20::span<const uint8_t> data, uint8_t func) {
  std::map<size_t, std::unique_ptr<uint8_t[]>>& sectors = sectors_[func];

  const uint8_t* data_ptr = data.data();
  size_t start = address;
  for (; start < address + data.size(); start = (start & kBlockMask) + kBlockSize) {
    if (sectors.find(start & kBlockMask) == sectors.end()) {
      sectors[start & kBlockMask].reset(new uint8_t[kBlockSize]);
      memset(sectors[start & kBlockMask].get(), 0xff, kBlockSize);
    }

    const size_t write_offset = start - (start & kBlockMask);
    const size_t write_size = std::min(kBlockSize - write_offset, data.size() - start + address);
    memcpy(sectors[start & kBlockMask].get() + write_offset, data_ptr, write_size);

    data_ptr += write_size;
  }
}

void FakeSdmmcDevice::Erase(size_t address, size_t size, uint8_t func) {
  std::map<size_t, std::unique_ptr<uint8_t[]>>& sectors = sectors_[func];
  for (size_t start = address; start < address + size; start = (start & kBlockMask) + kBlockSize) {
    if (sectors.find(start & kBlockMask) == sectors.end()) {
      continue;
    }

    const size_t erase_offset = start - (start & kBlockMask);
    const size_t erase_size = std::min(kBlockSize - erase_offset, size - start + address);
    if (erase_offset == 0 && erase_size == kBlockSize) {
      sectors.erase(start & kBlockMask);
    } else {
      memset(sectors[start & kBlockMask].get() + erase_offset, 0xff, erase_size);
    }
  }
}

void FakeSdmmcDevice::TriggerInBandInterrupt() const {
  interrupt_cb_.ops->callback(interrupt_cb_.ctx);
}

zx_status_t FakeSdmmcDevice::CopySdmmcRegions(cpp20::span<const sdmmc_buffer_region_t> regions,
                                              SdmmcVmoStore& vmos, uint8_t* buffer,
                                              const bool copy_to_regions) {
  for (const sdmmc_buffer_region_t& region : regions) {
    zx::unowned_vmo vmo;
    uint64_t offset = 0;
    if (region.type == SDMMC_BUFFER_TYPE_VMO_HANDLE) {
      vmo = zx::unowned_vmo(region.buffer.vmo);
    } else if (region.type == SDMMC_BUFFER_TYPE_VMO_ID) {
      vmo_store::StoredVmo<OwnedVmoInfo>* const stored_vmo = vmos.GetVmo(region.buffer.vmo_id);
      if (stored_vmo == nullptr) {
        return ZX_ERR_NOT_FOUND;
      }
      if (region.size + region.offset > stored_vmo->meta().size) {
        return ZX_ERR_OUT_OF_RANGE;
      }

      vmo = stored_vmo->vmo();
      offset = stored_vmo->meta().offset;
    }

    zx_status_t status;
    if (copy_to_regions) {
      status = vmo->write(buffer, offset + region.offset, region.size);
    } else {
      status = vmo->read(buffer, offset + region.offset, region.size);
    }
    if (status != ZX_OK) {
      return status;
    }

    buffer += region.size;
  }

  return ZX_OK;
}

}  // namespace sdmmc
