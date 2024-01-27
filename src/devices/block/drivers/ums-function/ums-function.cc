
// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fidl/fuchsia.hardware.usb.peripheral.block/cpp/wire.h>
#include <fuchsia/hardware/usb/function/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fit/defer.h>
#include <lib/scsi/scsilib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <usb/peripheral.h>
#include <usb/ums.h>
#include <usb/usb-request.h>

#include "src/devices/block/drivers/ums-function/usb_ums_bind.h"

#define BLOCK_SIZE 512L
#define STORAGE_SIZE (4L * 1024L * 1024L * 1024L)
#define BLOCK_COUNT (STORAGE_SIZE / BLOCK_SIZE)
#define DATA_REQ_SIZE 16384
#define BULK_MAX_PACKET 512

typedef enum {
  DATA_STATE_NONE,
  DATA_STATE_READ,
  DATA_STATE_WRITE,
  DATA_STATE_FAILED
} ums_data_state_t;

static struct {
  usb_interface_descriptor_t intf;
  usb_endpoint_descriptor_t out_ep;
  usb_endpoint_descriptor_t in_ep;
} descriptors = {
    .intf =
        {
            .b_length = sizeof(usb_interface_descriptor_t),
            .b_descriptor_type = USB_DT_INTERFACE,
            //      .b_interface_number set later
            .b_alternate_setting = 0,
            .b_num_endpoints = 2,
            .b_interface_class = USB_CLASS_MSC,
            .b_interface_sub_class = USB_SUBCLASS_MSC_SCSI,
            .b_interface_protocol = USB_PROTOCOL_MSC_BULK_ONLY,
            .i_interface = 0,
        },
    .out_ep =
        {
            .b_length = sizeof(usb_endpoint_descriptor_t),
            .b_descriptor_type = USB_DT_ENDPOINT,
            //      .b_endpoint_address set later
            .bm_attributes = USB_ENDPOINT_BULK,
            .w_max_packet_size = htole16(BULK_MAX_PACKET),
            .b_interval = 0,
        },
    .in_ep =
        {
            .b_length = sizeof(usb_endpoint_descriptor_t),
            .b_descriptor_type = USB_DT_ENDPOINT,
            //      .b_endpoint_address set later
            .bm_attributes = USB_ENDPOINT_BULK,
            .w_max_packet_size = htole16(BULK_MAX_PACKET),
            .b_interval = 0,
        },
};

struct usb_ums_t : fidl::WireServer<fuchsia_hardware_usb_peripheral_block::Device> {
  void EnableWritebackCache(EnableWritebackCacheCompleter::Sync& completer) override {
    writeback_cache = true;
    completer.Reply(ZX_OK);
  }

  void DisableWritebackCache(DisableWritebackCacheCompleter::Sync& completer) override {
    writeback_cache = false;
    completer.Reply(ZX_OK);
  }

  void SetWritebackCacheReported(SetWritebackCacheReportedRequestView request,
                                 SetWritebackCacheReportedCompleter::Sync& completer) override {
    writeback_cache_report = request->report;
    completer.Reply(ZX_OK);
  }

  zx_device_t* zxdev;
  usb_function_protocol_t function;
  usb_request_t* cbw_req;
  bool cbw_req_complete;
  usb_request_t* data_req;
  bool data_req_complete;
  usb_request_t* csw_req;
  bool csw_req_complete;

  // vmo for backing storage
  zx_handle_t storage_handle;
  void* storage;

  // command we are currently handling
  ums_cbw_t current_cbw;
  // data transferred for the current command
  uint32_t data_length;

  // state for data transfers
  ums_data_state_t data_state;
  // state for reads and writes
  zx_off_t data_offset;
  size_t data_remaining;

  uint8_t bulk_out_addr;
  uint8_t bulk_in_addr;
  size_t parent_req_size;
  bool writeback_cache;
  bool writeback_cache_report;
  thrd_t thread;
  bool active;
  cnd_t event;
  mtx_t mtx;
  std::atomic_int pending_request_count;
};

static void ums_cbw_complete(void* ctx, usb_request_t* req);
static void ums_data_complete(void* ctx, usb_request_t* req);
static void ums_csw_complete(void* ctx, usb_request_t* req);

static void ums_request_queue(void* ctx, usb_function_protocol_t* function, usb_request_t* req,
                              const usb_request_complete_callback_t* completion) {
  usb_ums_t* ums = (usb_ums_t*)ctx;
  atomic_fetch_add(&ums->pending_request_count, 1);
  usb_function_request_queue(function, req, completion);
}

static void ums_completion_callback(void* ctx, usb_request_t* req) {
  usb_ums_t* ums = (usb_ums_t*)ctx;
  mtx_lock(&ums->mtx);
  if (req == ums->cbw_req) {
    ums->cbw_req_complete = true;
  } else {
    if (req == ums->data_req) {
      ums->data_req_complete = true;
    } else {
      ums->csw_req_complete = true;
    }
  }
  cnd_signal(&ums->event);
  mtx_unlock(&ums->mtx);
}

static void ums_function_queue_data(usb_ums_t* ums, usb_request_t* req) {
  ums->data_length += req->header.length;
  req->header.ep_address =
      ums->current_cbw.bmCBWFlags & USB_DIR_IN ? ums->bulk_in_addr : ums->bulk_out_addr;
  usb_request_complete_callback_t complete = {
      .callback = ums_completion_callback,
      .ctx = ums,
  };
  ums_request_queue(ums, &ums->function, req, &complete);
}

static void ums_queue_csw(usb_ums_t* ums, uint8_t status) {
  // first queue next cbw so it is ready to go
  usb_request_complete_callback_t cbw_complete = {
      .callback = ums_completion_callback,
      .ctx = ums,
  };
  ums_request_queue(ums, &ums->function, ums->cbw_req, &cbw_complete);

  usb_request_t* req = ums->csw_req;
  ums_csw_t* csw;
  usb_request_mmap(req, (void**)&csw);

  csw->dCSWSignature = htole32(CSW_SIGNATURE);
  csw->dCSWTag = ums->current_cbw.dCBWTag;
  csw->dCSWDataResidue =
      htole32(le32toh(ums->current_cbw.dCBWDataTransferLength) - ums->data_length);
  csw->bmCSWStatus = status;

  req->header.length = sizeof(ums_csw_t);
  usb_request_complete_callback_t csw_complete = {
      .callback = ums_completion_callback,
      .ctx = ums,
  };
  ums_request_queue(ums, &ums->function, ums->csw_req, &csw_complete);
}

static void ums_continue_transfer(usb_ums_t* ums) {
  usb_request_t* req = ums->data_req;

  size_t length = ums->data_remaining;
  if (length > DATA_REQ_SIZE) {
    length = DATA_REQ_SIZE;
  }
  req->header.length = length;

  if (ums->data_state == DATA_STATE_READ) {
    size_t result =
        usb_request_copy_to(req, static_cast<char*>(ums->storage) + ums->data_offset, length, 0);
    ZX_ASSERT(result == length);
    ums_function_queue_data(ums, req);
  } else if (ums->data_state == DATA_STATE_WRITE) {
    ums_function_queue_data(ums, req);
  } else {
    zxlogf(ERROR, "ums_continue_transfer: bad data state %d", ums->data_state);
  }
}

static void ums_start_transfer(usb_ums_t* ums, ums_data_state_t state, uint64_t lba,
                               uint32_t blocks) {
  zx_off_t offset = lba * BLOCK_SIZE;
  size_t length = blocks * BLOCK_SIZE;

  if (offset + length > STORAGE_SIZE) {
    zxlogf(ERROR, "ums_start_transfer: transfer out of range state: %d, lba: %zu blocks: %u", state,
           lba, blocks);
    // TODO(voydanoff) report error to host
    return;
  }

  ums->data_state = state;
  ums->data_offset = offset;
  ums->data_remaining = length;

  ums_continue_transfer(ums);
}

static void ums_handle_inquiry(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_inquiry");

  usb_request_t* req = ums->data_req;
  uint8_t* buffer;
  usb_request_mmap(req, (void**)&buffer);
  memset(buffer, 0, UMS_INQUIRY_TRANSFER_LENGTH);
  req->header.length = UMS_INQUIRY_TRANSFER_LENGTH;

  // fill in inquiry result
  buffer[0] = 0;     // Peripheral Device Type: Direct access block device
  buffer[1] = 0x80;  // Removable
  buffer[2] = 6;     // Version SPC-4
  buffer[3] = 0x12;  // Response Data Format
  memcpy(buffer + 8, "Google  ", 8);
  memcpy(buffer + 16, "Zircon UMS      ", 16);
  memcpy(buffer + 32, "1.00", 4);

  ums_function_queue_data(ums, req);
}

static void ums_handle_test_unit_ready(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_test_unit_ready");

  // no data phase here. Just return status OK
  ums_queue_csw(ums, CSW_SUCCESS);
}

static void ums_handle_request_sense(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_request_sense");

  usb_request_t* req = ums->data_req;
  uint8_t* buffer;
  usb_request_mmap(req, (void**)&buffer);
  memset(buffer, 0, UMS_REQUEST_SENSE_TRANSFER_LENGTH);
  req->header.length = UMS_REQUEST_SENSE_TRANSFER_LENGTH;

  // TODO(voydanoff) This is a hack. Figure out correct values to return here.
  buffer[0] = 0x70;   // Response Code
  buffer[2] = 5;      // Illegal Request
  buffer[7] = 10;     // Additional Sense Length
  buffer[12] = 0x20;  // Additional Sense Code

  ums_function_queue_data(ums, req);
}

static void ums_handle_read_capacity10(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_read_capacity10");

  usb_request_t* req = ums->data_req;
  scsi::ReadCapacity10ParameterData* data;
  usb_request_mmap(req, (void**)&data);

  uint64_t lba = BLOCK_COUNT - 1;
  if (lba > UINT32_MAX) {
    data->returned_logical_block_address = htobe32(UINT32_MAX);
  } else {
    data->returned_logical_block_address = htobe32(lba);
  }
  data->block_length_in_bytes = htobe32(BLOCK_SIZE);

  req->header.length = sizeof(*data);
  ums_function_queue_data(ums, req);
}

static void ums_handle_read_capacity16(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_read_capacity16");

  usb_request_t* req = ums->data_req;
  scsi::ReadCapacity16ParameterData* data;
  usb_request_mmap(req, (void**)&data);
  memset(data, 0, sizeof(*data));

  data->returned_logical_block_address = htobe64(BLOCK_COUNT - 1);
  data->block_length_in_bytes = htobe32(BLOCK_SIZE);

  req->header.length = sizeof(*data);
  ums_function_queue_data(ums, req);
}

static void ums_handle_mode_sense6(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_mode_sense6");
  scsi::ModeSense6CDB command;
  memcpy(&command, cbw->CBWCB, sizeof(command));
  usb_request_t* req = ums->data_req;
  scsi::ModeSense6ParameterHeader* data;
  usb_request_mmap(req, (void**)&data);
  memset(data, 0, sizeof(*data));
  req->header.length = sizeof(*data);
  if (command.page_code == 0x3F && ums->writeback_cache_report) {
    // Special request (cache page)
    // 20 byte response.
    ((unsigned char*)data)[6] = 1 << 2;  // Write Cache enable bit
    req->header.length = 20;
  }
  ums_function_queue_data(ums, req);
}

static void ums_handle_read10(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_read10");

  scsi::Read10CDB* command = reinterpret_cast<scsi::Read10CDB*>(cbw->CBWCB);
  uint64_t lba = be32toh(command->logical_block_address);
  uint32_t blocks = be16toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_read12(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_read12");

  scsi::Read12CDB* command = reinterpret_cast<scsi::Read12CDB*>(cbw->CBWCB);
  uint64_t lba = be32toh(command->logical_block_address);
  uint32_t blocks = be32toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_read16(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_read16");

  scsi::Read16CDB* command = reinterpret_cast<scsi::Read16CDB*>(cbw->CBWCB);
  uint64_t lba = be64toh(command->logical_block_address);
  uint32_t blocks = be32toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_write10(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_write10");

  scsi::Write10CDB* command = reinterpret_cast<scsi::Write10CDB*>(cbw->CBWCB);
  uint64_t lba = be32toh(command->logical_block_address);
  uint32_t blocks = be16toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_write12(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_write12");

  scsi::Write12CDB* command = reinterpret_cast<scsi::Write12CDB*>(cbw->CBWCB);
  uint64_t lba = be32toh(command->logical_block_address);
  uint32_t blocks = be32toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_write16(usb_ums_t* ums, ums_cbw_t* cbw) {
  zxlogf(DEBUG, "ums_handle_write16");

  scsi::Write16CDB* command = reinterpret_cast<scsi::Write16CDB*>(cbw->CBWCB);
  uint64_t lba = be64toh(command->logical_block_address);
  uint32_t blocks = be32toh(command->transfer_length);
  ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_cbw(usb_ums_t* ums, ums_cbw_t* cbw) {
  if (le32toh(cbw->dCBWSignature) != CBW_SIGNATURE) {
    zxlogf(ERROR, "ums_handle_cbw: bad dCBWSignature 0x%x", le32toh(cbw->dCBWSignature));
    return;
  }

  // reset data length for computing residue
  ums->data_length = 0;

  // all SCSI commands have opcode in the first byte.
  auto opcode = static_cast<scsi::Opcode>(cbw->CBWCB[0]);
  switch (opcode) {
    case scsi::Opcode::INQUIRY:
      ums_handle_inquiry(ums, cbw);
      break;
    case scsi::Opcode::TEST_UNIT_READY:
      ums_handle_test_unit_ready(ums, cbw);
      break;
    case scsi::Opcode::REQUEST_SENSE:
      ums_handle_request_sense(ums, cbw);
      break;
    case scsi::Opcode::READ_CAPACITY_10:
      ums_handle_read_capacity10(ums, cbw);
      break;
    case scsi::Opcode::READ_CAPACITY_16:
      ums_handle_read_capacity16(ums, cbw);
      break;
    case scsi::Opcode::MODE_SENSE_6:
      ums_handle_mode_sense6(ums, cbw);
      break;
    case scsi::Opcode::READ_10:
      ums_handle_read10(ums, cbw);
      break;
    case scsi::Opcode::READ_12:
      ums_handle_read12(ums, cbw);
      break;
    case scsi::Opcode::READ_16:
      ums_handle_read16(ums, cbw);
      break;
    case scsi::Opcode::WRITE_10:
      ums_handle_write10(ums, cbw);
      break;
    case scsi::Opcode::WRITE_12:
      ums_handle_write12(ums, cbw);
      break;
    case scsi::Opcode::WRITE_16:
      ums_handle_write16(ums, cbw);
      break;
    case scsi::Opcode::SYNCHRONIZE_CACHE_10:
      // TODO: This is presently untestable.
      // Implement this once we have a means of testing this.
      break;
    default:
      zxlogf(DEBUG, "ums_handle_cbw: unsupported opcode %02Xh", cbw->CBWCB[0]);
      if (cbw->dCBWDataTransferLength) {
        // queue zero length packet to satisfy data phase
        usb_request_t* req = ums->data_req;
        req->header.length = 0;
        ums->data_state = DATA_STATE_FAILED;
        ums_function_queue_data(ums, req);
      }
      ums_queue_csw(ums, CSW_FAILED);
      break;
  }
}

static void ums_cbw_complete(void* ctx, usb_request_t* req) {
  usb_ums_t* ums = static_cast<usb_ums_t*>(ctx);

  zxlogf(DEBUG, "ums_cbw_complete %d %ld", req->response.status, req->response.actual);

  if (req->response.status == ZX_OK && req->response.actual == sizeof(ums_cbw_t)) {
    ums_cbw_t* cbw = &ums->current_cbw;
    memset(cbw, 0, sizeof(*cbw));
    [[maybe_unused]] size_t result = usb_request_copy_from(req, cbw, sizeof(*cbw), 0);
    ums_handle_cbw(ums, cbw);
  }
}

static void ums_data_complete(void* ctx, usb_request_t* req) {
  usb_ums_t* ums = static_cast<usb_ums_t*>(ctx);

  zxlogf(DEBUG, "ums_data_complete %d %ld", req->response.status, req->response.actual);

  if (ums->data_state == DATA_STATE_WRITE) {
    size_t result = usb_request_copy_from(req, static_cast<char*>(ums->storage) + ums->data_offset,
                                          req->response.actual, 0);
    ZX_ASSERT(result == req->response.actual);
  } else if (ums->data_state == DATA_STATE_FAILED) {
    ums->data_state = DATA_STATE_NONE;
    ums_queue_csw(ums, CSW_FAILED);
    return;
  } else {
    ums->data_state = DATA_STATE_NONE;
    ums_queue_csw(ums, CSW_SUCCESS);
    return;
  }

  ums->data_offset += req->response.actual;
  if (ums->data_remaining > req->response.actual) {
    ums->data_remaining -= req->response.actual;
  } else {
    ums->data_remaining = 0;
  }

  if (ums->data_remaining > 0) {
    ums_continue_transfer(ums);
  } else {
    ums->data_state = DATA_STATE_NONE;
    ums_queue_csw(ums, CSW_SUCCESS);
  }
}

static void ums_csw_complete(void* ctx, usb_request_t* req) {
  zxlogf(DEBUG, "ums_csw_complete %d %ld", req->response.status, req->response.actual);
}

static size_t ums_get_descriptors_size(void* ctx) { return sizeof(descriptors); }

static void ums_get_descriptors(void* ctx, uint8_t* buffer, size_t buffer_size,
                                size_t* out_actual) {
  size_t length = sizeof(descriptors);
  if (length > buffer_size) {
    length = buffer_size;
  }
  memcpy(buffer, &descriptors, length);
  *out_actual = length;
}

static zx_status_t ums_control(void* ctx, const usb_setup_t* setup, const uint8_t* write_buffer,
                               size_t write_size, uint8_t* out_read_buffer, size_t read_size,
                               size_t* out_read_actual) {
  if (setup->bm_request_type == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) &&
      setup->b_request == USB_REQ_GET_MAX_LUN && setup->w_value == 0 && setup->w_index == 0 &&
      setup->w_length >= sizeof(uint8_t)) {
    *((uint8_t*)out_read_buffer) = 0;
    *out_read_actual = sizeof(uint8_t);
    return ZX_OK;
  }

  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t ums_set_configured(void* ctx, bool configured, usb_speed_t speed) {
  zxlogf(DEBUG, "ums_set_configured %d %d", configured, speed);
  usb_ums_t* ums = static_cast<usb_ums_t*>(ctx);
  zx_status_t status;

  // TODO(voydanoff) fullspeed and superspeed support
  if (configured) {
    if ((status = usb_function_config_ep(&ums->function, &descriptors.out_ep, NULL)) != ZX_OK ||
        (status = usb_function_config_ep(&ums->function, &descriptors.in_ep, NULL)) != ZX_OK) {
      zxlogf(ERROR, "ums_set_configured: usb_function_config_ep failed");
    }
  } else {
    if ((status = usb_function_disable_ep(&ums->function, ums->bulk_out_addr)) != ZX_OK ||
        (status = usb_function_disable_ep(&ums->function, ums->bulk_in_addr)) != ZX_OK) {
      zxlogf(ERROR, "ums_set_configured: usb_function_disable_ep failed");
    }
  }

  if (configured && status == ZX_OK) {
    // queue first read on OUT endpoint
    usb_request_complete_callback_t cbw_complete = {
        .callback = ums_completion_callback,
        .ctx = ums,
    };
    ums_request_queue(ums, &ums->function, ums->cbw_req, &cbw_complete);
  }
  return status;
}

static zx_status_t ums_set_interface(void* ctx, uint8_t interface, uint8_t alt_setting) {
  return ZX_ERR_NOT_SUPPORTED;
}

usb_function_interface_protocol_ops_t ums_device_ops = {
    .get_descriptors_size = ums_get_descriptors_size,
    .get_descriptors = ums_get_descriptors,
    .control = ums_control,
    .set_configured = ums_set_configured,
    .set_interface = ums_set_interface,
};

static void usb_ums_unbind(void* ctx) {
  zxlogf(DEBUG, "usb_ums_unbind");
  usb_ums_t* ums = static_cast<usb_ums_t*>(ctx);

  usb_function_cancel_all(&ums->function, ums->bulk_out_addr);
  usb_function_cancel_all(&ums->function, ums->bulk_in_addr);
  usb_function_cancel_all(&ums->function, descriptors.intf.b_interface_number);

  mtx_lock(&ums->mtx);
  ums->active = false;
  cnd_signal(&ums->event);
  mtx_unlock(&ums->mtx);
  int retval;
  thrd_join(ums->thread, &retval);
  device_unbind_reply(ums->zxdev);
}

static void usb_ums_release(void* ctx) {
  zxlogf(DEBUG, "usb_ums_release");
  usb_ums_t* ums = static_cast<usb_ums_t*>(ctx);

  if (ums->storage) {
    zx_vmar_unmap(zx_vmar_root_self(), (uintptr_t)ums->storage, STORAGE_SIZE);
  }
  if (ums->cbw_req) {
    usb_request_release(ums->cbw_req);
  }
  if (ums->data_req) {
    usb_request_release(ums->data_req);
  }
  if (ums->cbw_req) {
    usb_request_release(ums->csw_req);
  }
  cnd_destroy(&ums->event);
  mtx_destroy(&ums->mtx);
  free(ums);
}

static zx_protocol_device_t usb_ums_proto = {
    .version = DEVICE_OPS_VERSION,
    .unbind = usb_ums_unbind,
    .release = usb_ums_release,
    .message =
        [](void* ctx, fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
          usb_ums_t* thiz = static_cast<usb_ums_t*>(ctx);
          DdkTransaction transaction(txn);
          fidl::WireDispatch<fuchsia_hardware_usb_peripheral_block::Device>(
              thiz, fidl::IncomingHeaderAndMessage::FromEncodedCMessage(msg), &transaction);
          return transaction.Status();
        },
};
static zx_handle_t vmo = 0;

static int usb_ums_thread(void* ctx) {
  usb_ums_t* ums = (usb_ums_t*)ctx;
  ZX_ASSERT(ums->active);
  while (ums->active) {
    mtx_lock(&ums->mtx);
    if (!(ums->cbw_req_complete || ums->csw_req_complete || ums->data_req_complete ||
          (!ums->active))) {
      cnd_wait(&ums->event, &ums->mtx);
    }
    mtx_unlock(&ums->mtx);
    if (!ums->active && !atomic_load(&ums->pending_request_count)) {
      return 0;
    }
    if (ums->cbw_req_complete) {
      atomic_fetch_add(&ums->pending_request_count, -1);
      ums->cbw_req_complete = false;
      ums_cbw_complete(ums, ums->cbw_req);
    }
    if (ums->csw_req_complete) {
      atomic_fetch_add(&ums->pending_request_count, -1);
      ums->csw_req_complete = false;
      ums_csw_complete(ums, ums->csw_req);
    }
    if (ums->data_req_complete) {
      atomic_fetch_add(&ums->pending_request_count, -1);
      ums->data_req_complete = false;
      ums_data_complete(ums, ums->data_req);
    }
  }
  return 0;
}

zx_status_t usb_ums_bind(void* ctx, zx_device_t* parent) {
  zxlogf(INFO, "usb_ums_bind");

  usb_ums_t* ums = static_cast<usb_ums_t*>(calloc(1, sizeof(usb_ums_t)));
  if (!ums) {
    return ZX_ERR_NO_MEMORY;
  }
  ums->data_state = DATA_STATE_NONE;
  ums->active = true;
  mtx_init(&ums->mtx, 0);
  atomic_init(&ums->pending_request_count, 0);
  cnd_init(&ums->event);
  zx_status_t status = ZX_OK;
  ums->writeback_cache = false;
  ums->writeback_cache_report = false;

  auto cleanup = fit::defer([&] { usb_ums_release(ums); });

  status = device_get_protocol(parent, ZX_PROTOCOL_USB_FUNCTION, &ums->function);
  if (status != ZX_OK) {
    return status;
  }

  ums->parent_req_size = usb_function_get_request_size(&ums->function);
  ZX_DEBUG_ASSERT(ums->parent_req_size != 0);

  status = usb_function_alloc_interface(&ums->function, &descriptors.intf.b_interface_number);
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_ums_bind: usb_function_alloc_interface failed");
    return status;
  }
  status = usb_function_alloc_ep(&ums->function, USB_DIR_OUT, &ums->bulk_out_addr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_ums_bind: usb_function_alloc_ep failed");
    return status;
  }
  status = usb_function_alloc_ep(&ums->function, USB_DIR_IN, &ums->bulk_in_addr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_ums_bind: usb_function_alloc_ep failed");
    return status;
  }
  descriptors.out_ep.b_endpoint_address = ums->bulk_out_addr;
  descriptors.in_ep.b_endpoint_address = ums->bulk_in_addr;

  status =
      usb_request_alloc(&ums->cbw_req, BULK_MAX_PACKET, ums->bulk_out_addr, ums->parent_req_size);
  if (status != ZX_OK) {
    return status;
  }
  // Endpoint for data_req depends on current_cbw.bmCBWFlags,
  // and will be set in ums_function_queue_data.
  status = usb_request_alloc(&ums->data_req, DATA_REQ_SIZE, 0, ums->parent_req_size);
  if (status != ZX_OK) {
    return status;
  }
  status =
      usb_request_alloc(&ums->csw_req, BULK_MAX_PACKET, ums->bulk_in_addr, ums->parent_req_size);
  if (status != ZX_OK) {
    return status;
  }
  // create and map a VMO
  if (!vmo) {
    status = zx_vmo_create(STORAGE_SIZE, 0, &vmo);
    if (status != ZX_OK) {
      return status;
    }
  }
  ums->storage_handle = vmo;
  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0,
                       ums->storage_handle, 0, STORAGE_SIZE, (zx_vaddr_t*)&ums->storage);
  if (status != ZX_OK) {
    return status;
  }

  ums->csw_req->header.length = sizeof(ums_csw_t);

  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "usb-ums-function",
      .ctx = ums,
      .ops = &usb_ums_proto,
  };
  args.proto_id = ZX_PROTOCOL_CACHE_TEST;

  status = device_add(parent, &args, &ums->zxdev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_device_bind add_device failed %d", status);
    return status;
  }

  usb_function_set_interface(&ums->function, ums, &ums_device_ops);
  thrd_create_with_name(&ums->thread, usb_ums_thread, ums, "ums_worker");
  cleanup.cancel();
  return ZX_OK;
}

static zx_driver_ops_t usb_ums_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_ums_bind,
};

// clang-format off
ZIRCON_DRIVER(usb_ums, usb_ums_ops, "zircon", "0.1");
