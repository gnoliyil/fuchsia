// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ram-nand.h"

#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/zbi-format/partition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <utility>

#include <ddk/metadata/bad-block.h>
#include <ddk/metadata/nand.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>

namespace {

struct RamNandOp {
  nand_operation_t op;
  nand_queue_callback completion_cb;
  void* cookie;
  list_node_t node;
};

static_assert(ZBI_PARTITION_NAME_LEN == fuchsia_hardware_nand::wire::kNameLen, "bad fidl name");
static_assert(ZBI_PARTITION_GUID_LEN == fuchsia_hardware_nand::wire::kGuidLen, "bad fidl guid");

uint32_t GetNumPartitions(const fuchsia_hardware_nand::wire::RamNandInfo& info) {
  return std::min(info.partition_map.partition_count, fuchsia_hardware_nand::wire::kMaxPartitions);
}

void ExtractNandConfig(const fuchsia_hardware_nand::wire::RamNandInfo& info,
                       nand_config_t* config) {
  config->bad_block_config.type = kAmlogicUboot;

  uint32_t extra_count = 0;
  for (uint32_t i = 0; i < GetNumPartitions(info); i++) {
    const auto& partition = info.partition_map.partitions[i];
    if (partition.hidden && partition.bbt) {
      config->bad_block_config.aml_uboot.table_start_block = partition.first_block;
      config->bad_block_config.aml_uboot.table_end_block = partition.last_block;
    } else if (!partition.hidden) {
      if (partition.copy_count > 1) {
        nand_partition_config_t* extra = &config->extra_partition_config[extra_count];

        memcpy(extra->type_guid, partition.unique_guid.data(), ZBI_PARTITION_GUID_LEN);
        extra->copy_count = partition.copy_count;
        extra->copy_byte_offset = partition.copy_byte_offset;
        extra_count++;
      }
    }
  }
  config->extra_partition_config_count = extra_count;
}

fbl::Array<char> ExtractPartitionMap(const fuchsia_hardware_nand::wire::RamNandInfo& info) {
  uint32_t num_partitions = GetNumPartitions(info);
  uint32_t dest_partitions = num_partitions;
  for (uint32_t i = 0; i < num_partitions; i++) {
    if (info.partition_map.partitions[i].hidden) {
      dest_partitions--;
    }
  }

  size_t len = sizeof(zbi_partition_map_t) + sizeof(zbi_partition_t) * dest_partitions;
  fbl::Array<char> buffer(new char[len], len);
  memset(buffer.data(), 0, len);
  zbi_partition_map_t* map = reinterpret_cast<zbi_partition_map_t*>(buffer.data());

  map->block_count = info.nand_info.num_blocks;
  map->block_size = info.nand_info.page_size * info.nand_info.pages_per_block;
  map->partition_count = dest_partitions;
  memcpy(map->guid, info.partition_map.device_guid.data(), sizeof(map->guid));

  zbi_partition_t* dest = map->partitions;
  for (uint32_t i = 0; i < num_partitions; i++) {
    const auto& src = info.partition_map.partitions[i];
    if (!src.hidden) {
      memcpy(dest->type_guid, src.type_guid.data(), sizeof(dest->type_guid));
      memcpy(dest->uniq_guid, src.unique_guid.data(), sizeof(dest->uniq_guid));
      dest->first_block = src.first_block;
      dest->last_block = src.last_block;
      memcpy(dest->name, src.name.data(), sizeof(dest->name));
      dest++;
    }
  }
  return buffer;
}

}  // namespace

NandDevice::NandDevice(const NandParams& params, zx_device_t* parent)
    : DeviceType(parent), params_(params), export_nand_config_{} {}

NandDevice::~NandDevice() {
  if (thread_created_) {
    Kill();
    sync_completion_signal(&wake_signal_);
    int result_code;
    thrd_join(worker_, &result_code);
  }
  ZX_ASSERT(list_is_empty(&txn_list_));
  if (mapped_addr_) {
    zx_vmar_unmap(zx_vmar_root_self(), mapped_addr_, params_.GetSize());
  }
}

zx_status_t NandDevice::Bind(fuchsia_hardware_nand::wire::RamNandInfo& info) {
  zx::result<DeviceNameType> device_name = Init(std::move(info.vmo));
  if (device_name.is_error()) {
    return device_name.status_value();
  }

  if (info.export_nand_config) {
    export_nand_config_ = std::make_optional<nand_config_t>();
    ExtractNandConfig(info, &*export_nand_config_);
  }
  if (info.export_partition_map) {
    export_partition_map_ = ExtractPartitionMap(info);
  }
  zx_device_prop_t props[] = {
      {BIND_PROTOCOL, 0, ZX_PROTOCOL_NAND},
      {BIND_NAND_CLASS, 0, params_.nand_class},
  };

  fail_after_ = static_cast<uint64_t>(info.fail_after) * params_.page_size;
  if (fail_after_ > 0) {
    zxlogf(INFO, "fail-after: %lu", fail_after_);
  }

  return DdkAdd(ddk::DeviceAddArgs(device_name->data()).set_props(props));
}

void NandDevice::DdkInit(ddk::InitTxn txn) {
  if (export_nand_config_) {
    zx_status_t status = DdkAddMetadata(DEVICE_METADATA_PRIVATE, &*export_nand_config_,
                                        sizeof(*export_nand_config_));
    if (status != ZX_OK) {
      return txn.Reply(status);
    }
  }
  if (export_partition_map_) {
    zx_status_t status = DdkAddMetadata(DEVICE_METADATA_PARTITION_MAP, export_partition_map_.data(),
                                        export_partition_map_.size());
    if (status != ZX_OK) {
      return txn.Reply(status);
    }
  }

  return txn.Reply(ZX_OK);
}

zx::result<NandDevice::DeviceNameType> NandDevice::Init(zx::vmo vmo) {
  ZX_DEBUG_ASSERT(!thread_created_);
  static uint64_t dev_count = 0;

  DeviceNameType name;
  snprintf(name.data(), name.size(), "ram-nand-%" PRIu64, dev_count++);

  zx_status_t status;
  const bool use_vmo = vmo.is_valid();
  if (use_vmo) {
    vmo_ = std::move(vmo);

    uint64_t size;
    status = vmo_.get_size(&size);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    if (size < params_.GetSize()) {
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
  } else {
    status = zx::vmo::create(params_.GetSize(), 0, &vmo_);
    if (status != ZX_OK) {
      return zx::error(status);
    }
  }

  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo_.get(), 0,
                       params_.GetSize(), &mapped_addr_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  if (!use_vmo) {
    memset(reinterpret_cast<char*>(mapped_addr_), 0xff, params_.GetSize());
  }

  if (thrd_create(&worker_, WorkerThreadStub, this) != thrd_success) {
    return zx::error(ZX_ERR_NO_RESOURCES);
  }
  thread_created_ = true;

  return zx::ok(name);
}

void NandDevice::DdkUnbind(ddk::UnbindTxn txn) {
  Kill();
  sync_completion_signal(&wake_signal_);
  txn.Reply();
}

void NandDevice::Unlink(UnlinkCompleter::Sync& completer) {
  fbl::AutoLock lock(&lock_);
  if (dead_) {
    completer.Close(ZX_ERR_BAD_STATE);
    return;
  }
  DdkAsyncRemove();
  completer.Reply(ZX_OK);
}

void NandDevice::NandQuery(nand_info_t* info_out, size_t* nand_op_size_out) {
  *info_out = params_;
  *nand_op_size_out = sizeof(RamNandOp);
}

void NandDevice::NandQueue(nand_operation_t* operation, nand_queue_callback completion_cb,
                           void* cookie) {
  uint32_t max_pages = params_.NumPages();
  switch (operation->command) {
    case NAND_OP_READ_BYTES:
    case NAND_OP_WRITE_BYTES:
      if (operation->rw_bytes.offset_nand >= params_.GetSize() || !operation->rw_bytes.length ||
          (params_.GetSize() - operation->rw_bytes.offset_nand) < operation->rw_bytes.length) {
        completion_cb(cookie, ZX_ERR_OUT_OF_RANGE, operation);
        return;
      }
      if (operation->rw_bytes.data_vmo == ZX_HANDLE_INVALID) {
        completion_cb(cookie, ZX_ERR_BAD_HANDLE, operation);
        return;
      }
      break;
    case NAND_OP_READ:
    case NAND_OP_WRITE: {
      if (operation->rw.offset_nand >= max_pages || !operation->rw.length ||
          (max_pages - operation->rw.offset_nand) < operation->rw.length) {
        completion_cb(cookie, ZX_ERR_OUT_OF_RANGE, operation);
        return;
      }
      if (operation->rw.data_vmo == ZX_HANDLE_INVALID &&
          operation->rw.oob_vmo == ZX_HANDLE_INVALID) {
        completion_cb(cookie, ZX_ERR_BAD_HANDLE, operation);
        return;
      }
      break;
    }
    case NAND_OP_ERASE:
      if (!operation->erase.num_blocks || operation->erase.first_block >= params_.num_blocks ||
          params_.num_blocks - operation->erase.first_block < operation->erase.num_blocks) {
        completion_cb(cookie, ZX_ERR_OUT_OF_RANGE, operation);
        return;
      }
      break;

    default:
      completion_cb(cookie, ZX_ERR_NOT_SUPPORTED, operation);
      return;
  }

  if (AddToList(operation, completion_cb, cookie)) {
    sync_completion_signal(&wake_signal_);
  } else {
    completion_cb(cookie, ZX_ERR_BAD_STATE, operation);
  }
}

zx_status_t NandDevice::NandGetFactoryBadBlockList(uint32_t* bad_blocks, size_t bad_block_len,
                                                   size_t* num_bad_blocks) {
  *num_bad_blocks = 0;
  return ZX_OK;
}

void NandDevice::Kill() {
  fbl::AutoLock lock(&lock_);
  dead_ = true;
}

bool NandDevice::AddToList(nand_operation_t* operation, nand_queue_callback completion_cb,
                           void* cookie) {
  fbl::AutoLock lock(&lock_);
  bool is_dead = dead_;
  if (!dead_) {
    RamNandOp* nand_op = reinterpret_cast<RamNandOp*>(operation);
    nand_op->completion_cb = completion_cb;
    nand_op->cookie = cookie;
    list_add_tail(&txn_list_, &nand_op->node);
  }
  return !is_dead;
}

bool NandDevice::RemoveFromList(nand_operation_t** operation) {
  fbl::AutoLock lock(&lock_);
  RamNandOp* nand_op = list_remove_head_type(&txn_list_, RamNandOp, node);
  *operation = reinterpret_cast<nand_operation_t*>(nand_op);
  return !dead_;
}

int NandDevice::WorkerThread() {
  for (;;) {
    nand_operation_t* operation;
    for (;;) {
      bool alive = RemoveFromList(&operation);
      if (operation) {
        if (alive) {
          sync_completion_reset(&wake_signal_);
          break;
        } else {
          auto* op = reinterpret_cast<RamNandOp*>(operation);
          op->completion_cb(op->cookie, ZX_ERR_BAD_STATE, operation);
        }
      } else if (alive) {
        sync_completion_wait(&wake_signal_, ZX_TIME_INFINITE);
      } else {
        return 0;
      }
    }

    zx_status_t status = ZX_OK;

    switch (operation->command) {
      case NAND_OP_WRITE_BYTES:
        if (fail_after_ > 0) {
          if (write_count_ >= fail_after_) {
            status = ZX_ERR_IO;
            break;
          }
          if (write_count_ + operation->rw_bytes.length > fail_after_) {
            const uint64_t old_length = operation->rw_bytes.length;
            operation->rw_bytes.length = fail_after_ - write_count_;
            status = ReadWriteData(operation, true);
            if (status == ZX_OK) {
              write_count_ = fail_after_;
              status = ZX_ERR_IO;
            }
            operation->rw.length = old_length;
            break;
          }
        }
        __FALLTHROUGH;
      case NAND_OP_READ_BYTES:
        status = ReadWriteData(operation, true);

        if (status == ZX_OK && operation->command == NAND_OP_WRITE_BYTES) {
          write_count_ += operation->rw_bytes.length;
        }
        break;
      case NAND_OP_WRITE:
        if (fail_after_ > 0) {
          if (write_count_ >= fail_after_) {
            status = ZX_ERR_IO;
            break;
          }
          if (write_count_ + (static_cast<uint64_t>(operation->rw.length) * params_.page_size) >
              fail_after_) {
            const uint32_t old_length = operation->rw.length;
            operation->rw.length = (fail_after_ - write_count_) / params_.page_size;
            status = ReadWriteData(operation, false);
            if (status == ZX_OK) {
              status = ReadWriteOob(operation);
            }
            if (status == ZX_OK) {
              write_count_ = fail_after_;
              status = ZX_ERR_IO;
            }
            operation->rw.length = old_length;
            break;
          }
        }
        __FALLTHROUGH;
      case NAND_OP_READ:
        status = ReadWriteData(operation, false);
        if (status == ZX_OK) {
          status = ReadWriteOob(operation);
        }
        if (status == ZX_OK && operation->command == NAND_OP_WRITE) {
          write_count_ += static_cast<uint64_t>(operation->rw.length) * params_.page_size;
        }
        break;

      case NAND_OP_ERASE: {
        status = Erase(operation);
        break;
      }
      default:
        ZX_DEBUG_ASSERT(false);  // Unexpected.
    }

    auto* op = reinterpret_cast<RamNandOp*>(operation);
    op->completion_cb(op->cookie, status, operation);
  }
}

int NandDevice::WorkerThreadStub(void* arg) {
  NandDevice* device = static_cast<NandDevice*>(arg);
  return device->WorkerThread();
}

zx_status_t NandDevice::ReadWriteData(nand_operation_t* operation, bool bytes) {
  if (operation->rw.data_vmo == ZX_HANDLE_INVALID) {
    return ZX_OK;
  }

  uint32_t nand_addr;
  uint64_t vmo_addr;
  uint32_t length;
  if (bytes) {
    nand_addr = operation->rw_bytes.offset_nand;
    vmo_addr = operation->rw_bytes.offset_data_vmo;
    length = operation->rw_bytes.length;
  } else {
    nand_addr = operation->rw.offset_nand * params_.page_size;
    vmo_addr = operation->rw.offset_data_vmo * params_.page_size;
    length = operation->rw.length * params_.page_size;
  }
  void* addr = reinterpret_cast<char*>(mapped_addr_) + nand_addr;

  if (operation->command == NAND_OP_READ || operation->command == NAND_OP_READ_BYTES) {
    operation->rw.corrected_bit_flips = 0;
    return zx_vmo_write(operation->rw.data_vmo, addr, vmo_addr, length);
  }

  if (bytes) {
    ZX_DEBUG_ASSERT(operation->command == NAND_OP_WRITE_BYTES);
  } else {
    ZX_DEBUG_ASSERT(operation->command == NAND_OP_WRITE);
    // Likely something bad is going on if writing multiple blocks.
    ZX_DEBUG_ASSERT_MSG(operation->rw.length <= params_.pages_per_block, "Writing multiple blocks");
    ZX_DEBUG_ASSERT_MSG(
        operation->rw.offset_nand / params_.pages_per_block ==
            (operation->rw.offset_nand + operation->rw.length - 1) / params_.pages_per_block,
        "Writing multiple blocks");
  }

  return zx_vmo_read(operation->rw.data_vmo, addr, vmo_addr, length);
}

zx_status_t NandDevice::ReadWriteOob(nand_operation_t* operation) {
  if (operation->rw.oob_vmo == ZX_HANDLE_INVALID) {
    return ZX_OK;
  }

  uint32_t nand_addr = MainDataSize() + operation->rw.offset_nand * params_.oob_size;
  uint64_t vmo_addr = operation->rw.offset_oob_vmo * params_.page_size;
  uint32_t length = operation->rw.length * params_.oob_size;
  void* addr = reinterpret_cast<char*>(mapped_addr_) + nand_addr;

  if (operation->command == NAND_OP_READ) {
    operation->rw.corrected_bit_flips = 0;
    return zx_vmo_write(operation->rw.oob_vmo, addr, vmo_addr, length);
  }

  ZX_DEBUG_ASSERT(operation->command == NAND_OP_WRITE);
  return zx_vmo_read(operation->rw.oob_vmo, addr, vmo_addr, length);
}

zx_status_t NandDevice::Erase(nand_operation_t* operation) {
  ZX_DEBUG_ASSERT(operation->command == NAND_OP_ERASE);

  uint32_t block_size = params_.page_size * params_.pages_per_block;
  uint32_t nand_addr = operation->erase.first_block * block_size;
  uint32_t length = operation->erase.num_blocks * block_size;
  void* addr = reinterpret_cast<char*>(mapped_addr_) + nand_addr;

  memset(addr, 0xff, length);

  // Clear the OOB area:
  uint32_t oob_per_block = params_.oob_size * params_.pages_per_block;
  length = operation->erase.num_blocks * oob_per_block;
  nand_addr = MainDataSize() + operation->erase.first_block * oob_per_block;
  addr = reinterpret_cast<char*>(mapped_addr_) + nand_addr;

  memset(addr, 0xff, length);

  return ZX_OK;
}
