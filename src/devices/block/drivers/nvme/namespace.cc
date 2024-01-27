// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/block/drivers/nvme/namespace.h"

#include <fuchsia/hardware/block/cpp/banjo.h>
#include <lib/fzl/vmo-mapper.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <hwreg/bitfields.h>

#include "src/devices/block/drivers/nvme/commands/identify.h"
#include "src/devices/block/drivers/nvme/nvme.h"
#include "src/devices/block/drivers/nvme/queue-pair.h"

namespace nvme {

zx_status_t Namespace::AddNamespace() {
  return DdkAdd(ddk::DeviceAddArgs(NamespaceName().c_str()));
}

zx_status_t Namespace::Bind(Nvme* controller, uint32_t namespace_id) {
  if (namespace_id == 0 || namespace_id == ~0u) {
    zxlogf(ERROR, "Attempted to create namespace with invalid id %u.", namespace_id);
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AllocChecker ac;
  auto ns = fbl::make_unique_checked<Namespace>(&ac, controller->zxdev(), controller, namespace_id);
  if (!ac.check()) {
    zxlogf(ERROR, "Failed to allocate memory for namespace %u.", namespace_id);
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = ns->AddNamespace();
  if (status != ZX_OK) {
    return status;
  }

  // The DDK takes ownership of the device.
  [[maybe_unused]] auto placeholder = ns.release();
  return ZX_OK;
}

void Namespace::DdkRelease() {
  zxlogf(DEBUG, "Releasing driver.");
  delete this;
}

void Namespace::DdkInit(ddk::InitTxn txn) {
  // The driver initialization has numerous error conditions. Wrap the initialization here to ensure
  // we always call txn.Reply() in any outcome.
  zx_status_t status = Init();
  if (status != ZX_OK) {
    zxlogf(ERROR, "Driver initialization failed: %s", zx_status_get_string(status));
  }
  txn.Reply(status);
}

static void PopulateNamespaceInspect(const IdentifyNvmeNamespace& ns,
                                     const fbl::String& namespace_name,
                                     uint16_t atomic_write_unit_normal,
                                     uint16_t atomic_write_unit_power_fail,
                                     uint32_t max_transfer_bytes, uint32_t block_size_bytes,
                                     inspect::Node* inspect_node, inspect::Inspector* inspector) {
  auto inspect_ns = inspect_node->CreateChild(namespace_name);
  uint16_t nawun = ns.ns_atomics() ? ns.n_aw_un + 1 : atomic_write_unit_normal;
  uint16_t nawupf = ns.ns_atomics() ? ns.n_aw_u_pf + 1 : atomic_write_unit_power_fail;
  inspect_ns.CreateInt("atomic_write_unit_normal_blocks", nawun, inspector);
  inspect_ns.CreateInt("atomic_write_unit_power_fail_blocks", nawupf, inspector);
  inspect_ns.CreateInt("namespace_atomic_boundary_size_normal_blocks", ns.n_abs_n, inspector);
  inspect_ns.CreateInt("namespace_atomic_boundary_offset_blocks", ns.n_ab_o, inspector);
  inspect_ns.CreateInt("namespace_atomic_boundary_size_power_fail_blocks", ns.n_abs_pf, inspector);
  inspect_ns.CreateInt("namespace_optimal_io_boundary_blocks", ns.n_oio_b, inspector);
  // table of block formats
  for (int i = 0; i < ns.n_lba_f; i++) {
    if (ns.lba_formats[i].value) {
      auto& fmt = ns.lba_formats[i];
      inspect_ns.CreateInt(fbl::StringPrintf("lba_format_%u_block_size_bytes", i),
                           fmt.lba_data_size_bytes(), inspector);
      inspect_ns.CreateInt(fbl::StringPrintf("lba_format_%u_relative_performance", i),
                           fmt.relative_performance(), inspector);
      inspect_ns.CreateInt(fbl::StringPrintf("lba_format_%u_metadata_size_bytes", i),
                           fmt.metadata_size_bytes(), inspector);
    }
  }
  inspect_ns.CreateInt("active_lba_format_index", ns.lba_format_index(), inspector);
  inspect_ns.CreateInt("data_protection_caps", ns.dpc & 0x3F, inspector);
  inspect_ns.CreateInt("data_protection_set", ns.dps & 3, inspector);
  inspect_ns.CreateInt("namespace_size_blocks", ns.n_sze, inspector);
  inspect_ns.CreateInt("namespace_cap_blocks", ns.n_cap, inspector);
  inspect_ns.CreateInt("namespace_util_blocks", ns.n_use, inspector);
  inspect_ns.CreateInt("max_transfer_bytes", max_transfer_bytes, inspector);
  inspect_ns.CreateInt("block_size_bytes", block_size_bytes, inspector);
  inspector->emplace(std::move(inspect_ns));
}

zx_status_t Namespace::Init() {
  zx::vmo admin_data;
  const uint32_t kPageSize = zx_system_get_page_size();
  zx_status_t status = zx::vmo::create(kPageSize, 0, &admin_data);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to create vmo: %s", zx_status_get_string(status));
    return status;
  }

  fzl::VmoMapper mapper;
  status = mapper.Map(admin_data);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to map vmo: %s", zx_status_get_string(status));
    return status;
  }

  // Identify namespace.
  IdentifySubmission identify_ns;
  identify_ns.namespace_id = namespace_id_;
  identify_ns.set_structure(IdentifySubmission::IdentifyCns::kIdentifyNamespace);
  status = controller_->DoAdminCommandSync(identify_ns, admin_data.borrow());
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to identify namespace %u: %s", namespace_id_,
           zx_status_get_string(status));
    return status;
  }

  auto ns = static_cast<IdentifyNvmeNamespace*>(mapper.start());

  block_info_.flags |= BLOCK_FLAG_FUA_SUPPORT;
  block_info_.block_count = ns->n_sze;
  auto& fmt = ns->lba_formats[ns->lba_format_index()];
  block_info_.block_size = fmt.lba_data_size_bytes();

  if (fmt.metadata_size_bytes()) {
    zxlogf(ERROR, "NVMe drive uses LBA format with metadata (%u bytes), which we do not support.",
           fmt.metadata_size_bytes());
    return ZX_ERR_NOT_SUPPORTED;
  }
  // The NVMe spec only mentions a lower bound. The upper bound may be a false requirement.
  if ((block_info_.block_size < 512) || (block_info_.block_size > 32768)) {
    zxlogf(ERROR, "Cannot handle LBA size of %u.", block_info_.block_size);
    return ZX_ERR_NOT_SUPPORTED;
  }

  // NVME r/w commands operate in block units, maximum of 64K blocks.
  const uint32_t max_bytes_per_cmd = block_info_.block_size * 65536;
  uint32_t max_transfer_bytes = controller_->max_data_transfer_bytes();
  if (max_transfer_bytes == 0) {
    max_transfer_bytes = max_bytes_per_cmd;
  } else {
    max_transfer_bytes = std::min(max_transfer_bytes, max_bytes_per_cmd);
  }

  // Limit maximum transfer size to 1MB which fits comfortably within our single PRP page per
  // QueuePair setup.
  const uint32_t prp_restricted_transfer_bytes = QueuePair::kMaxTransferPages * kPageSize;
  if (max_transfer_bytes > prp_restricted_transfer_bytes) {
    max_transfer_bytes = prp_restricted_transfer_bytes;
  }

  block_info_.max_transfer_size = max_transfer_bytes;

  // Convert to block units.
  max_transfer_blocks_ = max_transfer_bytes / block_info_.block_size;

  PopulateNamespaceInspect(*ns, NamespaceName(), controller_->atomic_write_unit_normal(),
                           controller_->atomic_write_unit_power_fail(), max_transfer_bytes,
                           block_info_.block_size, &controller_->inspect_node(),
                           &controller_->inspector());

  return ZX_OK;
}

void Namespace::BlockImplQuery(block_info_t* out_info, uint64_t* out_block_op_size) {
  *out_info = block_info_;
  *out_block_op_size = sizeof(IoCommand);
}

zx_status_t Namespace::IsValidIoRwCommand(const block_op_t& op) const {
  if (op.rw.length == 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (op.rw.length > max_transfer_blocks_) {
    zxlogf(ERROR, "Request exceeding max transfer size.");
    return ZX_ERR_INVALID_ARGS;
  }
  // IO address range must fit within device.
  if ((op.rw.offset_dev >= block_info_.block_count) ||
      (block_info_.block_count - op.rw.offset_dev < op.rw.length)) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  return ZX_OK;
}

void Namespace::BlockImplQueue(block_op_t* op, block_impl_queue_callback callback, void* cookie) {
  IoCommand* io_cmd = containerof(op, IoCommand, op);
  io_cmd->completion_cb = callback;
  io_cmd->cookie = cookie;
  io_cmd->namespace_id = namespace_id_;
  io_cmd->block_size_bytes = block_info_.block_size;

  const auto opcode = io_cmd->op.command & BLOCK_OP_MASK;
  switch (opcode) {
    case BLOCK_OP_READ:
    case BLOCK_OP_WRITE:
      if (zx_status_t status = IsValidIoRwCommand(io_cmd->op); status != ZX_OK) {
        io_cmd->Complete(status);
        return;
      }
      zxlogf(TRACE, "Block IO: %s: %u blocks @ LBA %zu", opcode == BLOCK_OP_WRITE ? "wr" : "rd",
             io_cmd->op.rw.length, io_cmd->op.rw.offset_dev);
      break;
    case BLOCK_OP_FLUSH:
      zxlogf(TRACE, "Block IO: flush");
      break;
    default:
      io_cmd->Complete(ZX_ERR_NOT_SUPPORTED);
      return;
  }

  controller_->QueueIoCommand(io_cmd);
}

}  // namespace nvme
