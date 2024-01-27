// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_USB_MASS_STORAGE_USB_MASS_STORAGE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_USB_MASS_STORAGE_USB_MASS_STORAGE_H_

#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <fuchsia/hardware/block/driver/cpp/banjo.h>
#include <fuchsia/hardware/usb/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/scsi/controller.h>
#include <lib/scsi/disk.h>
#include <lib/sync/completion.h>
#include <threads.h>
#include <zircon/assert.h>
#include <zircon/listnode.h>

#include <atomic>
#include <memory>
#include <thread>

#include <ddktl/device.h>
#include <fbl/array.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <usb/ums.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

namespace ums {

// Abstract waiter class for waiting on a sync_completion_t.
// This is necessary to allow injection of a timer by a test
// into the UsbMassStorageDevice class, allowing for a simulated clock.
class WaiterInterface : public fbl::RefCounted<WaiterInterface> {
 public:
  virtual zx_status_t Wait(sync_completion_t* completion, zx_duration_t duration) = 0;
  virtual ~WaiterInterface() = default;
};

// struct representing a block device for a logical unit
struct Transaction {
  scsi::DiskOp disk_op;

  // UsbMassStorageDevice::ExecuteCommandAsync() checks that the incoming CDB's size does not exceed
  // this buffer's.
  uint8_t cdb_buffer[16];
  uint8_t cdb_length;
  uint8_t lun;
  uint32_t block_size_bytes;

  list_node_t node;
};

class UsbMassStorageDevice;

struct UsbRequestContext {
  usb_request_complete_callback_t completion;
};

using MassStorageDeviceType =
    ddk::Device<UsbMassStorageDevice, ddk::Unbindable, ddk::Initializable>;
class UsbMassStorageDevice : public scsi::Controller, public MassStorageDeviceType {
 public:
  explicit UsbMassStorageDevice(fbl::RefPtr<WaiterInterface> waiter, zx_device_t* parent = nullptr)
      : MassStorageDeviceType(parent), waiter_(waiter) {}

  ~UsbMassStorageDevice() override = default;

  void DdkRelease();
  void DdkInit(ddk::InitTxn txn);

  void DdkUnbind(ddk::UnbindTxn txn);

  // scsi::Controller
  size_t BlockOpSize() override { return sizeof(Transaction); }
  zx_status_t ExecuteCommandSync(uint8_t target, uint16_t lun, iovec cdb, bool is_write,
                                 iovec data) override;
  void ExecuteCommandAsync(uint8_t target, uint16_t lun, iovec cdb, bool is_write,
                           uint32_t block_size_bytes, scsi::DiskOp* disk_op) override;

  // Performs the object initialization.
  zx_status_t Init(bool is_test_mode);
  void Release();  // Visible for testing.

  DISALLOW_COPY_ASSIGN_AND_MOVE(UsbMassStorageDevice);

 private:
  zx_status_t Reset();

  // Sends a Command Block Wrapper (command portion of request)
  // to a USB mass storage device.
  zx_status_t SendCbw(uint8_t lun, uint32_t transfer_length, uint8_t flags, uint8_t command_len,
                      void* command);

  // Reads a Command Status Wrapper from a USB mass storage device
  // and validates that the command index in the response matches the index
  // in the previous request.
  zx_status_t ReadCsw(uint32_t* out_residue);

  // Validates the command index and signature of a command status wrapper.
  csw_status_t VerifyCsw(usb_request_t* csw_request, uint32_t* out_residue);

  zx_status_t ReadSync(size_t transfer_length);

  zx_status_t DataTransfer(zx_handle_t vmo_handle, zx_off_t offset, size_t length,
                           uint8_t ep_address);

  zx_status_t DoTransaction(Transaction* txn, uint8_t flags, uint8_t ep_address,
                            const std::string& action);

  zx_status_t CheckLunsReady();

  int WorkerThread(ddk::InitTxn&& txn);

  void RequestQueue(usb_request_t* request, const usb_request_complete_callback_t* completion);

  usb::UsbDevice usb_;

  uint32_t tag_send_;  // next tag to send in CBW

  uint32_t tag_receive_;  // next tag we expect to receive in CSW

  uint8_t max_lun_;  // index of last logical unit

  uint32_t max_transfer_bytes_;  // maximum transfer size reported by usb_get_max_transfer_size()

  uint8_t interface_number_;
  std::optional<std::thread> worker_thread_;
  uint8_t bulk_in_addr_;

  uint8_t bulk_out_addr_;

  size_t bulk_in_max_packet_;

  size_t bulk_out_max_packet_;

  usb_request_t* cbw_req_;

  usb_request_t* data_req_;

  usb_request_t* csw_req_;

  usb_request_t* data_transfer_req_;  // for use in DataTransfer

  size_t parent_req_size_;

  std::atomic_size_t pending_requests_ = 0;

  fbl::RefPtr<WaiterInterface> waiter_;

  bool dead_;

  // list of queued transactions
  list_node_t queued_txns_;

  sync_completion_t txn_completion_;  // signals WorkerThread when new txns are available
                                      // and when device is dead
  fbl::Mutex txn_lock_;               // protects queued_txns, txn_completion and dead

  fbl::Array<fbl::RefPtr<scsi::Disk>> block_devs_;

  bool is_test_mode_ = false;
};
}  // namespace ums

#endif  // SRC_DEVICES_BLOCK_DRIVERS_USB_MASS_STORAGE_USB_MASS_STORAGE_H_
