// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_EVENT_RING_H_
#define SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_EVENT_RING_H_

#include <lib/dma-buffer/buffer.h>
#include <lib/fpromise/promise.h>
#include <lib/mmio/mmio.h>
#include <lib/synchronous-executor/executor.h>
#include <lib/trace/event.h>
#include <lib/zx/bti.h>
#include <zircon/errors.h>

#include <optional>

#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <usb/usb.h>

#include "src/devices/usb/drivers/xhci/xhci-context.h"
#include "src/devices/usb/drivers/xhci/xhci-hub.h"
#include "src/devices/usb/drivers/xhci/xhci-port-state.h"
#include "zircon/system/ulib/inspect/include/lib/inspect/cpp/vmo/types.h"

namespace usb_xhci {

// Event Ring Segment table entry (6.5)
struct ERSTEntry {
  uint32_t address_low;
  uint32_t address_high;
  uint32_t size;
  uint32_t rsvd;
};

// Used for managing event ring segments.
// This table can be expanded and shrunk as event ring
// segments are added and removed.
class EventRingSegmentTable {
 public:
  zx_status_t Init(size_t page_size, const zx::bti& bti, bool is_32bit, uint32_t erst_max,
                   ERSTSZ erst_size, const dma_buffer::BufferFactory& factory,
                   fdf::MmioBuffer* mmio);
  zx_status_t AddSegment(zx_paddr_t paddr);
  ERSTEntry* entries() { return entries_; }
  zx_paddr_t erst() { return erst_->phys()[0]; }
  // Returns the number of segments in this ERST
  uint32_t SegmentCount() const { return offset_; }
  uint64_t TrbCount() const { return (SegmentCount() * page_size_) / sizeof(TRB); }
  bool CanGrow() const { return offset_ < count_; }
  void AddPressure() { erst_pressure_++; }
  size_t Pressure() const { return erst_pressure_; }
  void RemovePressure() { erst_pressure_--; }

 private:
  size_t erst_pressure_ = 0;
  ERSTSZ erst_size_;
  std::unique_ptr<dma_buffer::PagedBuffer> erst_;
  // Entries in the event ring segment table.
  // This is valid after Init() is called which
  // allocates the event ring segment table.
  ERSTEntry* entries_;
  // Number of ERST entries
  size_t count_ = 0;
  // Offset in ERST table
  uint32_t offset_ = 0;
  // BTI used for obtaining physical memory addresses.
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  const zx::bti* bti_;
  size_t page_size_;
  bool is_32bit_;
  std::optional<fdf::MmioView> mmio_;
};

struct PortStatusChangeState : public fbl::RefCounted<PortStatusChangeState> {
  size_t port_index;
  size_t port_count;
  PortStatusChangeState(size_t i, size_t port_count) : port_index(i), port_count(port_count) {}
};

// Keeps track of events received from the XHCI controller
class UsbXhci;
class CommandRing;
class EventRing {
 public:
  // Adds a segment to the event ring.
  zx_status_t Init(size_t page_size, const zx::bti& bti, fdf::MmioBuffer* buffer, bool is_32bit,
                   uint32_t erst_max, ERSTSZ erst_size, ERDP erdp_reg, IMAN iman_reg,
                   uint8_t cap_length, HCSPARAMS1 hcs_params_1, CommandRing* command_ring,
                   DoorbellOffset doorbell_offset, UsbXhci* hci, HCCPARAMS1 hcc_params_1,
                   uint64_t* dcbaa, uint16_t interrupter, inspect::Node* interrupter_node);
  // Disable thread safety analysis here.
  // We don't need to hold the mutex just to read the ERST
  // paddr, as this will never change (it is effectively a constant).
  // We don't need to incurr the overhead of acquiring the mutex for this.
  zx_paddr_t erst() __TA_NO_THREAD_SAFETY_ANALYSIS { return segments_.erst(); }
  void RemovePressure();
  size_t GetPressure();
  zx_status_t AddSegmentIfNoneLock() {
    fbl::AutoLock l(&segment_mutex_);
    return AddSegmentIfNone();
  }
  zx_paddr_t erdp_phys() { return erdp_phys_; }
  TRB* erdp_virt() { return erdp_virt_; }
  zx_status_t HandleIRQ();
  zx_status_t Ring0Bringup();

  void ScheduleTask(TRBPromise promise) { ScheduleTask(promise.discard_value()); }
  void ScheduleTask(fpromise::promise<void, zx_status_t> promise);

  void RunUntilIdle();

 private:
  void SchedulePortStatusChange(uint8_t port_id, bool preempt = false);
  fpromise::promise<void, zx_status_t> HandlePortStatusChangeEvent(uint8_t port_id);
  fpromise::promise<void, zx_status_t> WaitForPortStatusChange(uint8_t port_id);

  fpromise::promise<void, zx_status_t> LinkUp(uint8_t port_id);
  void CallPortStatusChanged(fbl::RefPtr<PortStatusChangeState> state);

  // Advance ERDP according to Section 4.9.4.1. Evaluates the validity of the current TRB and the
  // direction of the next TRB. Possible directions:
  //   - If not at the end of the segment, next TRB is consecutive in address.
  //       - If we are in a new segment, CCS bit is invalid. Evaluate if we stop by checking the
  //         next TRB's completion code. Only continue moving on if completion code is not INVALID.
  //   - If at the end of a segment
  //       - When there are no new segments, next TRB is the beginning of the next segment..
  //       - When there are new segments, but we're not about to enter into the new segment, next
  //         TRB is still the beginning of the next segment.
  //       - When there are new segments and we are about to enter into the new segment, checks if
  //         the new segment is being used.
  //           - If the new segment is being used, go to the beginning of the new segment.
  //           - If the new segment is not being used yet, check if the event ring is empty.
  //              - If the event ring is not empty, go to the beginning of the 0th segment (because
  //                the new segment is always the last segment)
  //              - If the event ring is empty, reevaluate next time (by setting the reevaluate_ bit
  //                to true). In this case ERDP points not to the next TRB to be evaluated (as we
  //                usually expect), but the current TRB already evaluated.
  // Returns the next TRB pointed to by ERDP.
  void AdvanceErdp();
  zx_paddr_t UpdateErdpReg(zx_paddr_t last_phys, size_t processed_trb_count);

  std::optional<Control> CurrentErdp();

  // Interrupt handlers.
  void HandlePortStatusChangeInterrupt();
  zx_status_t HandleCommandCompletionInterrupt();
  void HandleTransferInterrupt();

  // USB 3.0 device attach
  void Usb3DeviceAttach(uint16_t port_id);
  // USB 2.0 device attach
  void Usb2DeviceAttach(uint16_t port_id);
  zx_status_t AddSegmentIfNone() __TA_REQUIRES(segment_mutex_);
  zx_status_t AddSegment() __TA_REQUIRES(segment_mutex_);

  bool StallWorkaroundForDefectiveHubs(std::unique_ptr<TRBContext>& context);

  struct SegmentBuf : fbl::DoublyLinkedListable<std::unique_ptr<SegmentBuf>> {
    SegmentBuf(std::unique_ptr<dma_buffer::ContiguousBuffer> b, bool n)
        : buf(std::move(b)), new_segment(n) {}

    std::unique_ptr<dma_buffer::ContiguousBuffer> buf;
    bool new_segment;
  };
  fbl::DoublyLinkedList<std::unique_ptr<SegmentBuf>> buffers_ __TA_GUARDED(segment_mutex_);
  fbl::DoublyLinkedList<std::unique_ptr<SegmentBuf>>::iterator buffers_it_
      __TA_GUARDED(segment_mutex_);

  // List of pending enumeration tasks
  fbl::DoublyLinkedList<std::unique_ptr<TRBContext>> enumeration_queue_;
  // Whether or not we're currently enumerating a device
  bool enumerating_ = false;
  synchronous_executor::synchronous_executor executor_;

  // Virtual address of the event ring dequeue pointer
  TRB* erdp_virt_ = nullptr;

  // Event ring dequeue pointer (physical address)
  zx_paddr_t erdp_phys_ = 0;

  // Current Cycle State
  bool ccs_ = true;
  fbl::Mutex segment_mutex_;
  EventRingSegmentTable segments_ __TA_GUARDED(segment_mutex_);
  // BTI used for obtaining physical memory addresses.
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  const zx::bti* bti_;
  size_t page_size_;
  bool is_32bit_;
  // Pointer to the MMIO buffer for writing to xHCI registers
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  fdf::MmioBuffer* mmio_;

  // Event ring dequeue pointer register
  ERDP erdp_reg_;

  // Interrupt management register
  IMAN iman_reg_;
  uint8_t segment_index_ __TA_GUARDED(segment_mutex_) = 0;
  UsbXhci* hci_;
  uint8_t cap_length_;
  HCSPARAMS1 hcs_params_1_;
  CommandRing* command_ring_;
  DoorbellOffset doorbell_offset_;
  HCCPARAMS1 hcc_params_1_;

  // Device context base address array
  // This is a pointer into the buffer
  // owned by UsbXhci, which this is a child of.
  // When xHCI shuts down, this pointer will be invalid.
  uint64_t* dcbaa_;

  uint16_t interrupter_;
  bool reevaluate_ = false;
  bool resynchronize_ = false;

  // inspect properties do not allow us to read the values we have published,
  // nor do they provide any sort of "max" functionality (only add, subtract,
  // and set).  Because of this, we need to keep our own shadow copy of our max
  // observed value in order to properly update the published value.
  inspect::UintProperty total_event_trbs_;
  inspect::UintProperty max_single_irq_event_trbs_;
  uint64_t max_single_irq_event_trbs_value_{0};
  inspect::Node events_;
  std::optional<inspect::UintProperty> port_status_change_event_;
  std::optional<inspect::LinearUintHistogram> command_completion_event_;
  std::optional<inspect::LinearUintHistogram> transfer_event_;
  std::optional<inspect::UintProperty> mf_index_wrap_event_;
  std::optional<inspect::UintProperty> host_controller_event_;
  std::optional<inspect::LinearUintHistogram> unhandled_events_;

  std::optional<trace_async_id_t> async_id_;
};
}  // namespace usb_xhci

#endif  // SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_EVENT_RING_H_
