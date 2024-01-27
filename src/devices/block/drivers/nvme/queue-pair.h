// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_NVME_QUEUE_PAIR_H_
#define SRC_DEVICES_BLOCK_DRIVERS_NVME_QUEUE_PAIR_H_

#include <lib/ddk/io-buffer.h>
#include <lib/fpromise/bridge.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/stdcompat/span.h>
#include <zircon/status.h>

#include <fbl/alloc_checker.h>
#include <fbl/vector.h>

#include "src/devices/block/drivers/nvme/commands.h"
#include "src/devices/block/drivers/nvme/queue.h"
#include "src/devices/block/drivers/nvme/registers.h"

namespace nvme {

struct IoCommand;

// Data associated with a transaction.
struct TransactionData {
  void ClearExceptPrp() {
    io_cmd = nullptr;
    pmt.reset();
    active = false;
    data_vmo = std::nullopt;
  }

  // The IoCommand consists of one or more transactions.
  IoCommand* io_cmd;
  // Used to pin the pages relevant to this transaction.
  zx::pmt pmt;
  // Described by NVM Express Base Specification 2.0 Section 4.1.1, "Physical Region Page Entry and
  // List"
  ddk::IoBuffer prp_buffer;
  // Set to true when a transaction is submitted, and set to false when it is completed.
  bool active = false;
  // VMO for the data that is read from or written to the device.
  std::optional<zx::unowned_vmo> data_vmo;
};

// A QueuePair represents a completion and submission queue that are paired together.
// It manages the relationship between the two.
// While the spec allows many submission queues to map to one completion queue, for simplicity
// we always assume there is a 1:1 relationship between the two.
// This class is thread-unsafe.
class QueuePair {
 public:
  // TODO(fxbug.dev/102133): Tune kMaxTransferPages vs. preallocated PRP buffer usage.
  // Limits the PRP buffer size to a single page.
  static constexpr uint32_t kMaxTransferPages = 256;

  static zx::result<std::unique_ptr<QueuePair>> Create(zx::unowned_bti bti, uint16_t queue_id,
                                                       uint32_t max_entries, CapabilityReg& reg,
                                                       fdf::MmioBuffer& mmio, bool prealloc_prp);

  // Prefer |QueuePair::Create|.
  QueuePair(Queue completion, Queue submission, fbl::Vector<TransactionData> txns,
            zx::unowned_bti bti, fdf::MmioBuffer& mmio, DoorbellReg completion_doorbell,
            DoorbellReg submission_doorbell)
      : kPageSize(zx_system_get_page_size()),
        kPageMask(zx_system_get_page_size() - 1),
        kPageShift(__builtin_ctzl(zx_system_get_page_size())),
        completion_(std::move(completion)),
        submission_(std::move(submission)),
        txns_(std::move(txns)),
        bti_(std::move(bti)),
        mmio_(mmio),
        completion_doorbell_(completion_doorbell),
        submission_doorbell_(submission_doorbell) {}

  const Queue& completion() { return completion_; }
  const Queue& submission() { return submission_; }
  const fbl::Vector<TransactionData>& txn_data() { return txns_; }

  // Preallocates PRP buffers to avoid repeatedly allocating and freeing them for every transaction.
  zx_status_t PreallocatePrpBuffers();

  // Check the completion queue for any new completed elements. Should be called from an async task
  // posted by the interrupt handler.
  zx_status_t CheckForNewCompletion(Completion** completion, IoCommand** io_cmd = nullptr);
  void RingCompletionDb();

  // When submitting an admin command, io_cmd need not be supplied.
  zx_status_t Submit(Submission& submission, std::optional<zx::unowned_vmo> data,
                     zx_off_t vmo_offset, size_t bytes, IoCommand* io_cmd = nullptr) {
    return Submit(cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(&submission), sizeof(submission)),
                  std::move(data), vmo_offset, bytes, io_cmd);
  }

 private:
  friend class QueuePairTest;

  // Raw implementation of submit that operates on a byte span rather than a submission.
  zx_status_t Submit(cpp20::span<uint8_t> submission, std::optional<zx::unowned_vmo> data,
                     zx_off_t vmo_offset, size_t bytes, IoCommand* io_cmd);

  // TODO(fxbug.dev/102133): Use this if setting up PRP lists that span more than one page. See
  // QueuePair::kMaxTransferPages.
  // Puts a PRP list in |buf| containing the given addresses.
  zx_status_t PreparePrpList(ddk::IoBuffer& buf, cpp20::span<const zx_paddr_t> pages);

  // System parameters.
  const uint64_t kPageSize;
  const uint64_t kPageMask;
  const uint64_t kPageShift;

  // Completion queue.
  Queue completion_;
  // Submission queue.
  Queue submission_;
  // This is an array of data associated with each transaction.
  // Each transaction's ID is equal to its index in the queue, and this array works the same way.
  fbl::Vector<TransactionData> txns_;
  // Entries in the completion queue with phase equal to this are done.
  uint8_t completion_ready_phase_ = 1;
  // Last position the controller reported it was up to in the submission queue.
  size_t sq_head_ = submission_.entry_count() - 1;

  zx::unowned_bti bti_;

  fdf::MmioBuffer& mmio_;
  DoorbellReg completion_doorbell_;
  DoorbellReg submission_doorbell_;
};

}  // namespace nvme

#endif  // SRC_DEVICES_BLOCK_DRIVERS_NVME_QUEUE_PAIR_H_
