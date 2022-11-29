// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xhci-enumeration.h"

#include <endian.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "fuchsia/hardware/usb/descriptor/c/banjo.h"
#include "src/devices/usb/drivers/xhci/registers.h"
#include "src/devices/usb/drivers/xhci/xhci-context.h"
#include "usb-xhci.h"
#include "xhci-async-auto-call.h"
#include "zircon/status.h"

namespace usb_xhci {

fpromise::promise<usb_device_descriptor_t, zx_status_t> GetDeviceDescriptor(UsbXhci* hci,
                                                                            uint8_t slot_id,
                                                                            uint16_t length) {
  std::optional<OwnedRequest> request_wrapper;
  OwnedRequest::Alloc(&request_wrapper, length, 0, hci->UsbHciGetRequestSize());
  usb_request_t* request = request_wrapper->request();
  request->header.device_id = slot_id - 1;
  request->header.ep_address = 0;
  request->setup.bm_request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
  request->setup.w_value = USB_DT_DEVICE << 8;
  request->setup.w_index = 0;
  request->setup.b_request = USB_REQ_GET_DESCRIPTOR;
  request->setup.w_length = length;
  request->direct = true;
  return hci->UsbHciRequestQueue(std::move(*request_wrapper))
      .then([=](fpromise::result<OwnedRequest, void>& result)
                -> fpromise::result<usb_device_descriptor_t, zx_status_t> {
        auto& request = result.value();
        zx_status_t status = request.request()->response.status;
        if (status != ZX_OK) {
          zxlogf(ERROR, "GetDeviceDescriptor request failed %s", zx_status_get_string(status));
          return fpromise::error(status);
        }
        usb_device_descriptor_t descriptor;
        ssize_t actual = request.CopyFrom(&descriptor, length, 0);
        if (actual != length) {
          zxlogf(ERROR, "GetDeviceDescriptor request expected %u bytes, got %zd", length, actual);
          return fpromise::error(ZX_ERR_IO);
        }
        if (descriptor.b_descriptor_type != USB_DT_DEVICE) {
          zxlogf(ERROR, "GetDeviceDescriptor got bad descriptor type: %u",
                 descriptor.b_descriptor_type);
          return fpromise::error(ZX_ERR_IO);
        }
        return fpromise::ok(descriptor);
      })
      .box();
}

fpromise::promise<uint8_t, zx_status_t> GetMaxPacketSize(UsbXhci* hci, uint8_t slot_id) {
  // Read the first 8 bytes of the descriptor only, to get the max packet size.
  return GetDeviceDescriptor(hci, slot_id, 8)
      .and_then(
          [=](const usb_device_descriptor_t& descriptor) -> fpromise::result<uint8_t, zx_status_t> {
            return fpromise::ok(descriptor.b_max_packet_size0);
          })
      .box();
}

TRBPromise UpdateMaxPacketSize(UsbXhci* hci, uint8_t slot_id) {
  return GetMaxPacketSize(hci, slot_id)
      .and_then([=](const uint8_t& packet_size) {
        return hci->SetMaxPacketSizeCommand(slot_id, packet_size);
      })
      .box();
}

struct AsyncState : public fbl::RefCounted<AsyncState> {
  // The current slot that is being enumerated
  uint8_t slot;

  // Block Set Request -- set to true
  // if a SET_ADDRESS command shouldn't be sent
  // when addressing a device.
  bool bsr = false;

  // Whether or not we are retrying enumeration
  bool retry_ctx = false;
};

TRBPromise EnumerateDeviceInternal(UsbXhci* hci, uint8_t port, std::optional<HubInfo> hub_info,
                                   fbl::RefPtr<AsyncState> state);

// Retries enumeration if we get a USB transaction error.
// See section 4.3 of revision 1.2 of the xHCI specification for details
TRBPromise RetryEnumeration(UsbXhci* hci, uint8_t port, uint8_t old_slot,
                            std::optional<HubInfo> hub_info, fbl::RefPtr<AsyncState> state) {
  // Disabling the slot is required due to fxbug.dev/41924
  return hci->DisableSlotCommand(old_slot)
      .and_then([=](TRB*& result) {
        // DisableSlotCommand will never return an error in the TRB.
        // Failure to disable a slot is considered a fatal error, and will result in
        // ZX_ERR_BAD_STATE being returned.
        return EnumerateDeviceInternal(hci, port, hub_info, state);
      })
      .box();
}

TRBPromise EnumerateDeviceInternal(UsbXhci* hci, uint8_t port, std::optional<HubInfo> hub_info,
                                   fbl::RefPtr<AsyncState> state) {
  // Error handler responsible for teardown in the event of an error.
  auto error_handler = fbl::MakeRefCounted<AsyncAutoCall>(hci);
  if (state->bsr) {
    state->retry_ctx = true;
  }
  // Obtain a Device Slot for the newly attached device
  auto address_device =
      hci->EnableSlotCommand()
          .and_then([=](TRB*& result) -> TRBPromise {
            auto completion_event = static_cast<CommandCompletionEvent*>(result);
            if (completion_event->CompletionCode() != CommandCompletionEvent::Success) {
              return fpromise::make_error_promise(ZX_ERR_IO);
            }
            // After successfully obtaining a device slot,
            // issue an Address Device command and enable its default control endpoint.
            uint8_t slot = static_cast<uint8_t>(completion_event->SlotID());
            state->slot = slot;
            hci->SetDeviceInformation(slot, port, hub_info);
            if (!state->retry_ctx) {
              // On failure, ensure that the slot gets disabled.
              // If we're in a retry context, it is the caller's responsibility to clean up.
              error_handler->GivebackPromise(error_handler->BorrowPromise()
                                                 .then([=](fpromise::result<void, void>& result) {
                                                   hci->ScheduleTask(kPrimaryInterrupter,
                                                                     hci->DisableSlotCommand(slot));
                                                 })
                                                 .box());
            }
            return hci->AddressDeviceCommand(slot, port, hub_info, state->bsr);
          })
          .and_then([=](TRB*& result) -> TRBPromise {
            // Check for errors and retry if the device refuses the SET_ADDRESS command
            auto completion_event = static_cast<CommandCompletionEvent*>(result);
            if (completion_event->CompletionCode() == CommandCompletionEvent::UsbTransactionError) {
              // Retry at most once
              if (!hci->IsDeviceConnected(state->slot) || state->retry_ctx) {
                return fpromise::make_error_promise(ZX_ERR_IO);
              }
              state->bsr = true;
              return RetryEnumeration(hci, port, state->slot, hub_info, state);
            }
            if (completion_event->CompletionCode() != CommandCompletionEvent::Success) {
              return fpromise::make_error_promise(ZX_ERR_IO);
            }
            return fpromise::make_ok_promise(result);
          })
          .and_then([=](TRB*& result) {
            // If retry was successful, re-initialize the error handler with the new slot
            if (state->bsr && !state->retry_ctx) {
              state->bsr = false;
              error_handler->Reinit();
              error_handler->GivebackPromise(error_handler->BorrowPromise()
                                                 .then([=](fpromise::result<void, void>& result) {
                                                   hci->ScheduleTask(
                                                       kPrimaryInterrupter,
                                                       hci->DisableSlotCommand(state->slot));
                                                 })
                                                 .box());
            }
            return fpromise::ok(result);
          });

  // We're being invoked from a retry context. Return to the original caller.
  if (state->retry_ctx) {
    return address_device
        .and_then([=](TRB*& result) -> TRBPromise {
          // Clear the retry_ctx field before returning to the caller
          state->retry_ctx = false;
          auto completion_event = static_cast<CommandCompletionEvent*>(result);
          if (completion_event->CompletionCode() != CommandCompletionEvent::Success) {
            return fpromise::make_ok_promise(result);
          }
          // Update the maximum packet size
          return UpdateMaxPacketSize(hci, state->slot);
        })
        .and_then([=](TRB*& result) -> TRBPromise {
          auto completion_event = static_cast<CommandCompletionEvent*>(result);
          if (completion_event->CompletionCode() != CommandCompletionEvent::Success) {
            return fpromise::make_ok_promise(result);
          }
          // Issue a SET_ADDRESS request to the device
          return hci->AddressDeviceCommand(state->slot);
        })
        .and_then([=](TRB*& result) -> fpromise::result<TRB*, zx_status_t> {
          auto completion_event = static_cast<CommandCompletionEvent*>(result);
          if (completion_event->CompletionCode() != CommandCompletionEvent::Success) {
            return fpromise::ok(result);
          }
          error_handler->Cancel();
          return fpromise::ok(result);
        })
        .box();
  }

  // We're NOT being invoked from a retry context -- finish device initialization
  return address_device
      .and_then([=](TRB*& result) -> TRBPromise {
        auto speed = hci->GetDeviceSpeed(state->slot);
        if (!speed.has_value()) {
          return fpromise::make_error_promise(ZX_ERR_BAD_STATE);
        }
        if (speed.value() != USB_SPEED_SUPER) {
          // See USB 2.0 specification (revision 2.0) section 9.2.6
          return hci->Timeout(kPrimaryInterrupter, zx::deadline_after(zx::msec(10)));
        }
        return fpromise::make_ok_promise(result);
      })
      .and_then([=](TRB*& result) -> fpromise::promise<uint8_t, zx_status_t> {
        // For full-speed devices, system software should read the first 8 bytes
        // of the device descriptor to determine the max packet size of the default control
        // endpoint. Additionally, certain devices may require the controller to read this value
        // before fetching the full descriptor; so we always read the max packet size in order to
        // prevent later enumeration failures.
        return GetMaxPacketSize(hci, state->slot);
      })
      .and_then([=](uint8_t& max_packet_size) -> fpromise::promise<void, zx_status_t> {
        // Set the max packet size if the device is a full speed device.
        auto speed = hci->GetDeviceSpeed(state->slot);
        if (!speed.has_value()) {
          return fpromise::make_error_promise<zx_status_t>(ZX_ERR_BAD_STATE);
        }
        if (speed.value() != USB_SPEED_FULL) {
          return fpromise::make_result_promise<void, zx_status_t>(fpromise::ok());
        }
        return hci->SetMaxPacketSizeCommand(state->slot, max_packet_size)
            .and_then([=](TRB*& result) -> fpromise::promise<void, zx_status_t> {
              // Discard TRB result.
              return fpromise::make_result_promise<void, zx_status_t>(fpromise::ok());
            });
      })
      .and_then([=]() -> fpromise::promise<usb_device_descriptor_t, zx_status_t> {
        // Now read the whole descriptor.
        return GetDeviceDescriptor(hci, state->slot, sizeof(usb_device_descriptor_t));
      })
      .and_then([=](usb_device_descriptor_t& descriptor) -> fpromise::result<TRB*, zx_status_t> {
        hci->CreateDeviceInspectNode(state->slot, le16toh(descriptor.id_vendor),
                                     le16toh(descriptor.id_product));

        // Online the device, making it visible to the DDK (enumeration has completed)
        auto speed = hci->GetDeviceSpeed(state->slot);
        if (!speed.has_value()) {
          return fpromise::error(ZX_ERR_BAD_STATE);
        }
        zx_status_t status = hci->DeviceOnline(state->slot, port, speed.value());
        if (status == ZX_OK) {
          error_handler->Cancel();
          return fpromise::ok(nullptr);
        }
        return fpromise::error(status);
      })
      .box();
}

TRBPromise EnumerateDevice(UsbXhci* hci, uint8_t port, std::optional<HubInfo> hub_info) {
  return EnumerateDeviceInternal(hci, port, hub_info, fbl::MakeRefCounted<AsyncState>());
}
}  // namespace usb_xhci
