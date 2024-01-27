// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_CONTEXT_H_
#define SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_CONTEXT_H_

#include <lib/fit/function.h>
#include <lib/fpromise/bridge.h>
#include <lib/synchronous-executor/executor.h>

#include <memory>

#include <fbl/intrusive_double_list.h>
#include <fbl/ref_counted.h>
#include <fbl/slab_allocator.h>
#include <usb/request-cpp.h>

#include "lib/fpromise/promise.h"
#include "src/devices/usb/drivers/xhci/registers.h"
#include "src/devices/usb/drivers/xhci/xhci-hub.h"
#include "src/devices/usb/lib/usb-endpoint/include/usb-endpoint/usb-endpoint.h"

namespace usb_xhci {
struct TRBContext;

using Request = usb::BorrowedRequest<void>;
using OwnedRequest = usb::Request<void>;

using TRBPromise = fpromise::promise<TRB*, zx_status_t>;
using AllocatorTraits = fbl::InstancedSlabAllocatorTraits<std::unique_ptr<TRBContext>, 4096U>;
using AllocatorType = fbl::SlabAllocator<AllocatorTraits>;
struct TRBContext : fbl::DoublyLinkedListable<std::unique_ptr<TRBContext>>,
                    fbl::SlabAllocated<AllocatorTraits> {
  // Root hub port number
  uint8_t port_number = 0;
  std::optional<HubInfo> hub_info;
  std::optional<usb_endpoint::RequestVariant> request;
  std::optional<fpromise::completer<TRB*, zx_status_t>> completer;
  uint64_t token;

  // The last TRB in the TD, i.e. the TRB with IOC (interrupt on completion) set to true. This is
  // the TRB that will pointed to by a transfer event on completion.
  TRB* trb = nullptr;

  // The first TRB in the TD.
  TRB* first_trb = nullptr;

  // When the TD includes a short TRB, this will be set to the actual length of data transferred.
  std::optional<size_t> short_transfer_len;
};

}  // namespace usb_xhci

// Specializations of some fpromise methods to make code more ergnomic.
namespace fpromise {
inline promise_impl<::fpromise::internal::result_continuation<usb_xhci::TRB*, zx_status_t>>
make_error_promise(zx_status_t error) {
  return make_result_promise<usb_xhci::TRB*, zx_status_t>(fpromise::error(error));
}

inline promise_impl<::fpromise::internal::result_continuation<usb_xhci::TRB*, zx_status_t>>
make_ok_promise(usb_xhci::TRB* trb) {
  return make_result_promise<usb_xhci::TRB*, zx_status_t>(fpromise::ok(trb));
}
}  // namespace fpromise

#endif  // SRC_DEVICES_USB_DRIVERS_XHCI_XHCI_CONTEXT_H_
