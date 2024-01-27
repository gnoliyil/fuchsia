// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/nand/drivers/nand/nand.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/io-buffer.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>
#include <zircon/status.h>
#include <zircon/threads.h>

#include <algorithm>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>

#include "src/devices/nand/drivers/nand/nand-bind.h"

// TODO: Investigate elimination of unmap.
// This code does vx_vmar_map/unmap and copies data in/out of the
// mapped virtual address. Unmapping is expensive, but required (a closing
// of the vmo does not unmap, so not unmapping will quickly lead to memory
// exhaustion. Check to see if we can do something different - is vmo_read/write
// cheaper than mapping and unmapping (which will cause TLB flushes) ?

namespace nand {

zx_status_t NandDevice::ReadPage(uint8_t* data, uint8_t* oob, uint32_t nand_page,
                                 uint32_t* corrected_bits, size_t retries) {
  // Always grab the oob, whether the caller wants it or not.
  uint8_t* oob_result = oob ? oob : oob_buffer_.get();
  if (data && dangerous_reads_cache_->GetPage(nand_page, data, oob_result)) {
    *corrected_bits = 0;
    return ZX_OK;
  }

  zx_status_t status = ZX_ERR_INTERNAL;
  size_t retry = 0;
  bool ecc_failure = false;
  for (; status != ZX_OK && retry < retries; retry++) {
    status = raw_nand_.ReadPageHwecc(nand_page, data, nand_info_.page_size, nullptr, oob_result,
                                     nand_info_.oob_size, nullptr, corrected_bits);
    if (status == ZX_OK) {
      // Only record the returned corrected bits number on success, otherwise it is undefined.
      read_ecc_bit_flips_.Insert(*corrected_bits);
    } else {
      read_internal_failure_.Add(1);
      zxlogf(WARNING, "%s: Retrying Read@%u", __func__, nand_page);
      if (status == ZX_ERR_IO_DATA_INTEGRITY) {
        ecc_failure = true;
        read_ecc_bit_flips_.Insert(nand_info_.ecc_bits + 1);
      }
    }
  }

  if (status != ZX_OK) {
    read_failure_.Add(1);
    read_attempts_.Insert(ULONG_MAX);
    zxlogf(WARNING, "%s: Read error %d, exhausted all retries", __func__, status);
  } else {
    read_attempts_.Insert(retry);
    if (retry > 1) {
      zxlogf(INFO, "%s: Successfully read@%u on retry %zd", __func__, nand_page, retry - 1);
    }
  }
  // If we get a failed ECC from the nand device, report up the stack that
  // things are going badly, in case the repeated read goes inexplicably better.
  if (ecc_failure) {
    *corrected_bits = nand_info_.ecc_bits;
  }
  if (status == ZX_OK && data && *corrected_bits > nand_info_.ecc_bits / 2) {
    // Cache this page, since we should re-read it for a block transfer soon.
    dangerous_reads_cache_->Insert(nand_page, data, oob_result);
  }
  return status;
}

zx_status_t NandDevice::EraseOp(nand_operation_t* nand_op) {
  uint32_t nand_page;

  for (uint32_t i = 0; i < nand_op->erase.num_blocks; i++) {
    nand_page = (nand_op->erase.first_block + i) * nand_info_.pages_per_block;
    // Purge cache for deleted content.
    dangerous_reads_cache_->PurgeRange(nand_page, nand_info_.pages_per_block);
    zx_status_t status = raw_nand_.EraseBlock(nand_page);
    if (status != ZX_OK) {
      zxlogf(ERROR, "nand: Erase of block %u failed", nand_op->erase.first_block + i);
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t NandDevice::MapVmos(const nand_operation_t& nand_op, fzl::VmoMapper* data,
                                uint8_t** vaddr_data, fzl::VmoMapper* oob, uint8_t** vaddr_oob) {
  zx_status_t status;
  if (nand_op.rw.data_vmo != ZX_HANDLE_INVALID) {
    const auto vmo = zx::unowned_vmo(nand_op.rw.data_vmo);
    const size_t offset_bytes = nand_op.rw.offset_data_vmo * nand_info_.page_size;
    const size_t aligned_offset_bytes =
        fbl::round_down(offset_bytes, static_cast<size_t>(PAGE_SIZE));
    const size_t page_offset_bytes = offset_bytes - aligned_offset_bytes;
    status = data->Map(*vmo, aligned_offset_bytes,
                       nand_op.rw.length * nand_info_.page_size + page_offset_bytes,
                       ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (status != ZX_OK) {
      zxlogf(ERROR, "nand: Cannot map data vmo: %s", zx_status_get_string(status));
      return status;
    }
    *vaddr_data = reinterpret_cast<uint8_t*>(data->start()) + page_offset_bytes;
  }

  // Map oob.
  if (nand_op.rw.oob_vmo != ZX_HANDLE_INVALID) {
    const auto vmo = zx::unowned_vmo(nand_op.rw.oob_vmo);
    const size_t offset_bytes = nand_op.rw.offset_oob_vmo * nand_info_.page_size;
    const size_t aligned_offset_bytes =
        fbl::round_down(offset_bytes, static_cast<size_t>(PAGE_SIZE));
    const size_t page_offset_bytes = offset_bytes - aligned_offset_bytes;
    status = oob->Map(*vmo, aligned_offset_bytes,
                      nand_op.rw.length * nand_info_.oob_size + page_offset_bytes,
                      ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (status != ZX_OK) {
      zxlogf(ERROR, "nand: Cannot map oob vmo: %s", zx_status_get_string(status));
      return status;
    }
    *vaddr_oob = reinterpret_cast<uint8_t*>(oob->start()) + page_offset_bytes;
  }
  return ZX_OK;
}

zx_status_t NandDevice::ReadOp(nand_operation_t* nand_op) {
  fzl::VmoMapper data;
  fzl::VmoMapper oob;
  uint8_t* vaddr_data = nullptr;
  uint8_t* vaddr_oob = nullptr;

  zx_status_t status = MapVmos(*nand_op, &data, &vaddr_data, &oob, &vaddr_oob);
  if (status != ZX_OK) {
    return status;
  }

  uint32_t max_corrected_bits = 0;
  for (uint32_t i = 0; i < nand_op->rw.length; i++) {
    uint32_t ecc_correct = 0;
    status = ReadPage(vaddr_data, vaddr_oob, nand_op->rw.offset_nand + i, &ecc_correct,
                      kNandReadRetries);
    if (status != ZX_OK) {
      zxlogf(ERROR, "nand: Read data error %d at page offset %u", status,
             nand_op->rw.offset_nand + i);
      break;
    } else {
      max_corrected_bits = std::max(max_corrected_bits, ecc_correct);
    }

    if (vaddr_data) {
      vaddr_data += nand_info_.page_size;
    }
    if (vaddr_oob) {
      vaddr_oob += nand_info_.oob_size;
    }
  }
  nand_op->rw.corrected_bit_flips = max_corrected_bits;

  return status;
}

zx_status_t NandDevice::WriteOp(nand_operation_t* nand_op) {
  fzl::VmoMapper data;
  fzl::VmoMapper oob;
  uint8_t* vaddr_data = nullptr;
  uint8_t* vaddr_oob = nullptr;

  zx_status_t status = MapVmos(*nand_op, &data, &vaddr_data, &oob, &vaddr_oob);
  if (status != ZX_OK) {
    return status;
  }

  for (uint32_t i = 0; i < nand_op->rw.length; i++) {
    status = raw_nand_.WritePageHwecc(vaddr_data, nand_info_.page_size, vaddr_oob,
                                      nand_info_.oob_size, nand_op->rw.offset_nand + i);
    if (status != ZX_OK) {
      zxlogf(ERROR, "nand: Write data error %d at page offset %u", status,
             nand_op->rw.offset_nand + i);
      break;
    }

    if (vaddr_data) {
      vaddr_data += nand_info_.page_size;
    }
    if (vaddr_oob) {
      vaddr_oob += nand_info_.oob_size;
    }
  }

  return status;
}

void NandDevice::DoIo(Transaction txn) {
  zx_status_t status = ZX_OK;

  switch (txn.operation()->command) {
    case NAND_OP_READ:
      status = ReadOp(txn.operation());
      break;
    case NAND_OP_WRITE:
      status = WriteOp(txn.operation());
      break;
    case NAND_OP_ERASE:
      status = EraseOp(txn.operation());
      break;
    default:
      status = ZX_ERR_NOT_SUPPORTED;
      break;
  }
  txn.Complete(status);
}

// Initialization is complete by the time the thread starts.
zx_status_t NandDevice::WorkerThread() {
  for (;;) {
    fbl::AutoLock al(&lock_);
    while (txn_queue_.is_empty() && !shutdown_) {
      worker_event_.Wait(&lock_);
    }
    if (shutdown_) {
      break;
    }
    nand::BorrowedOperationQueue<> queue(std::move(txn_queue_));
    al.release();
    auto txn = queue.pop();
    while (txn != std::nullopt) {
      DoIo(*std::move(txn));
      txn = queue.pop();
    }
  }

  zxlogf(DEBUG, "nand: worker thread terminated");
  return ZX_OK;
}

void NandDevice::Shutdown() {
  // Signal the worker thread and wait for it to terminate.
  {
    fbl::AutoLock al(&lock_);
    if (shutdown_) {
      return;
    }

    shutdown_ = true;
    worker_event_.Signal();
  }
  thrd_join(worker_thread_, nullptr);

  // Error out all pending requests.
  fbl::AutoLock al(&lock_);
  txn_queue_.Release();
}

void NandDevice::NandQuery(nand_info_t* info_out, size_t* nand_op_size_out) {
  memcpy(info_out, &nand_info_, sizeof(*info_out));
  *nand_op_size_out = Transaction::OperationSize(sizeof(nand_operation_t));
}

void NandDevice::NandQueue(nand_operation_t* op, nand_queue_callback completion_cb, void* cookie) {
  if (completion_cb == nullptr) {
    zxlogf(DEBUG, "nand: nand op %p completion_cb unset!", op);
    zxlogf(DEBUG, "nand: cannot queue command!");
    return;
  }

  Transaction txn(op, completion_cb, cookie, sizeof(nand_operation_t));

  switch (op->command) {
    case NAND_OP_READ:
    case NAND_OP_WRITE: {
      if (op->rw.offset_nand >= num_nand_pages_ || !op->rw.length ||
          (num_nand_pages_ - op->rw.offset_nand) < op->rw.length) {
        txn.Complete(ZX_ERR_OUT_OF_RANGE);
        return;
      }
      if (op->rw.data_vmo == ZX_HANDLE_INVALID && op->rw.oob_vmo == ZX_HANDLE_INVALID) {
        txn.Complete(ZX_ERR_BAD_HANDLE);
        return;
      }
      break;
    }
    case NAND_OP_ERASE:
      if (!op->erase.num_blocks || op->erase.first_block >= nand_info_.num_blocks ||
          (op->erase.num_blocks > (nand_info_.num_blocks - op->erase.first_block))) {
        txn.Complete(ZX_ERR_OUT_OF_RANGE);
        return;
      }
      break;

    default:
      txn.Complete(ZX_ERR_NOT_SUPPORTED);
      return;
  }

  // TODO: UPDATE STATS HERE.
  fbl::AutoLock al(&lock_);
  if (shutdown_) {
    txn.Complete(ZX_ERR_CANCELED);
  } else {
    txn_queue_.push(std::move(txn));
    worker_event_.Signal();
  }
}

zx_status_t NandDevice::NandGetFactoryBadBlockList(uint32_t* bad_blocks, size_t bad_block_len,
                                                   size_t* num_bad_blocks) {
  *num_bad_blocks = 0;
  return ZX_ERR_NOT_SUPPORTED;
}

void NandDevice::DdkSuspend(ddk::SuspendTxn txn) {
  const uint8_t suspend_reason = txn.suspend_reason() & DEVICE_MASK_SUSPEND_REASON;
  if (suspend_reason != DEVICE_SUSPEND_REASON_SUSPEND_RAM) {
    Shutdown();
  }
  txn.Reply(ZX_OK, txn.requested_state());
}

void NandDevice::DdkRelease() { delete this; }

NandDevice::~NandDevice() { Shutdown(); }

// static
zx_status_t NandDevice::Create(void* ctx, zx_device_t* parent) {
  zxlogf(INFO, "NandDevice::Create: Starting...!");

  fbl::AllocChecker ac;
  std::unique_ptr<NandDevice> dev(new (&ac) NandDevice(parent));
  if (!ac.check()) {
    zxlogf(ERROR, "nand: no memory to allocate nand device!");
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status;
  if ((status = dev->Init()) != ZX_OK) {
    return status;
  }

  if ((status = dev->Bind()) != ZX_OK) {
    dev.release()->DdkRelease();
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = dev.release();

  return ZX_OK;
}

zx::vmo NandDevice::GetDuplicateInspectVmoForTest() const { return inspect_.DuplicateVmo(); }

zx_status_t NandDevice::Init() {
  if (!raw_nand_.is_valid()) {
    zxlogf(ERROR, "nand: failed to get raw_nand protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status = raw_nand_.GetNandInfo(&nand_info_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "nand: get_nand_info returned error %d", status);
    return status;
  }

  dangerous_reads_cache_ =
      std::make_unique<ReadCache>(8, nand_info_.page_size, nand_info_.oob_size);
  oob_buffer_ = std::make_unique<uint8_t[]>(nand_info_.oob_size);

  num_nand_pages_ = nand_info_.num_blocks * nand_info_.pages_per_block;

  int rc = thrd_create_with_name(
      &worker_thread_, [](void* arg) { return static_cast<NandDevice*>(arg)->WorkerThread(); },
      this, "nand-worker");

  if (rc != thrd_success) {
    return thrd_status_to_zx_status(rc);
  }

  root_ = inspect_.GetRoot().CreateChild("nand");
  // 32 buckets: 0-31. Current devices only use up to BCH30.
  // Will populate read failures as ecc bits + 1.
  read_ecc_bit_flips_ = root_.CreateLinearUintHistogram("read_ecc_bit_flips", 0, 1, 32);
  // Buckets 0, 1, 2, 4...128. Failures will be maxint and dump in the overflow bucket.
  read_attempts_ = root_.CreateExponentialUintHistogram("read_attempts", 0, 1, 2, 9);
  read_internal_failure_ = root_.CreateUint("read_internal_failure", 0);
  read_failure_ = root_.CreateUint("read_failure", 0);

  // Set a scheduling role for the nand-worker thread.
  // This is required in order to service the blobfs-pager-thread, which is on a deadline profile.
  // This will no longer be needed once we have the ability to propagate deadlines. Until then, we
  // need to set deadline profiles for all threads that the blobfs-pager-thread interacts with in
  // order to service page requests.
  const char* role_name = "fuchsia.devices.nand.drivers.nand.device";
  status = device_set_profile_by_role(this->zxdev(), thrd_get_zx_handle(worker_thread_), role_name,
                                      strlen(role_name));
  if (status != ZX_OK) {
    zxlogf(WARNING, "nand: failed to apply role to worker: %d\n", status);
  }

  return ZX_OK;
}

zx_status_t NandDevice::Bind() {
  zx_device_prop_t props[] = {
      {BIND_PROTOCOL, 0, ZX_PROTOCOL_NAND},
      {BIND_NAND_CLASS, 0, NAND_CLASS_PARTMAP},
  };

  return DdkAdd(
      ddk::DeviceAddArgs("nand").set_props(props).set_inspect_vmo(inspect_.DuplicateVmo()));
}

static constexpr zx_driver_ops_t nand_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = NandDevice::Create;
  return ops;
}();

}  // namespace nand

ZIRCON_DRIVER(nand, nand::nand_driver_ops, "zircon", "0.1");
