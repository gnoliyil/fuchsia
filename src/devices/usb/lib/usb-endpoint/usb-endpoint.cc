// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/lib/usb-endpoint/include/usb-endpoint/usb-endpoint.h"

#include <lib/fit/defer.h>

#include <variant>

namespace usb_endpoint {

namespace {

ddk::PhysIter phys_iter(uint64_t* phys_list, size_t phys_count, zx_off_t length,
                        size_t max_length) {
  static_assert(sizeof(phys_iter_sg_entry_t) == sizeof(sg_entry_t) &&
                offsetof(phys_iter_sg_entry_t, length) == offsetof(sg_entry_t, length) &&
                offsetof(phys_iter_sg_entry_t, offset) == offsetof(sg_entry_t, offset));
  phys_iter_buffer_t buf = {.phys = phys_list,
                            .phys_count = phys_count,
                            .length = length,
                            .vmo_offset = 0,
                            .sg_list = nullptr,
                            .sg_count = 0};
  return ddk::PhysIter(buf, max_length);
}

}  // namespace

zx::result<std::vector<ddk::PhysIter>> UsbEndpoint::get_iter(RequestVariant& req,
                                                             size_t max_length) const {
  std::vector<ddk::PhysIter> iters;
  if (std::holds_alternative<usb::BorrowedRequest<void>>(req)) {
    iters.push_back(std::get<usb::BorrowedRequest<void>>(req).phys_iter(max_length));
  } else {
    const auto& fidl_request = std::get<usb::FidlRequest>(req);
    size_t i = 0;
    for (const auto& d : *fidl_request.request().data()) {
      switch (d.buffer()->Which()) {
        case fuchsia_hardware_usb_request::Buffer::Tag::kVmoId:
          iters.push_back(phys_iter(registered_vmos_.at(d.buffer()->vmo_id().value()).phys_list,
                                    registered_vmos_.at(d.buffer()->vmo_id().value()).phys_count,
                                    *d.size(), max_length));
          break;
        case fuchsia_hardware_usb_request::Buffer::Tag::kVmo:
        case fuchsia_hardware_usb_request::Buffer::Tag::kData:
          iters.push_back(fidl_request.phys_iter(i, max_length));
          break;
        default:
          zxlogf(ERROR, "Not supported buffer type");
          return zx::error(ZX_ERR_NOT_SUPPORTED);
      }
      i++;
    }
  }
  return zx::success(std::move(iters));
}

void UsbEndpoint::Connect(async_dispatcher_t* dispatcher,
                          fidl::ServerEnd<fuchsia_hardware_usb_endpoint::Endpoint> server_end) {
  binding_ref_.emplace(fidl::BindServer(dispatcher, std::move(server_end), this,
                                        std::mem_fn(&UsbEndpoint::OnUnbound)));
}

void UsbEndpoint::OnUnbound(fidl::UnbindInfo info,
                            fidl::ServerEnd<fuchsia_hardware_usb_endpoint::Endpoint> server_end) {
  // Unregister VMOs
  auto registered_vmos = std::move(registered_vmos_);
  for (auto& [id, vmo] : registered_vmos) {
    zx_status_t status = zx_pmt_unpin(vmo.pmt);
    ZX_DEBUG_ASSERT(status == ZX_OK);
    free(vmo.phys_list);
  }

  if (info.is_user_initiated()) {
    return;
  }

  if (info.is_peer_closed()) {
    zxlogf(INFO, "Client disconnected");
  } else {
    zxlogf(ERROR, "Server error: %s", info.ToError().status_string());
  }
}

void UsbEndpoint::RegisterVmos(RegisterVmosRequest& request,
                               RegisterVmosCompleter::Sync& completer) {
  std::vector<zx_status_t> statuses;
  std::vector<fuchsia_hardware_usb_endpoint::VmoInfo> failed_vmos;
  for (auto& v : request.vmos()) {
    ZX_ASSERT(v.vmo_id());
    ZX_ASSERT(v.vmo());

    if (registered_vmos_.find(*v.vmo_id()) != registered_vmos_.end()) {
      statuses.emplace_back(ZX_ERR_ALREADY_EXISTS);
      failed_vmos.emplace_back(std::move(v));
      continue;
    }
    uint64_t size;
    auto status = v.vmo()->get_size(&size);
    if (status != ZX_OK) {
      statuses.emplace_back(status);
      failed_vmos.emplace_back(std::move(v));
      return;
    }
    // Pin VMO. Abusing usb_request_physmap
    usb_request_t req = {
        .vmo_handle = v.vmo()->get(),
        .size = size,
        .offset = 0,
        .pmt = ZX_HANDLE_INVALID,
        .phys_list = nullptr,
        .phys_count = 0,
    };
    status = usb_request_physmap(&req, bti_.get());
    if (status != ZX_OK) {
      statuses.emplace_back(status);
      failed_vmos.emplace_back(std::move(v));
      return;
    }

    // Save
    registered_vmos_[*v.vmo_id()] = {.vmo = std::move(*v.vmo()),
                                     .pmt = req.pmt,
                                     .phys_list = req.phys_list,
                                     .phys_count = req.phys_count};
  }

  completer.Reply({std::move(statuses), std::move(failed_vmos)});
}

void UsbEndpoint::UnregisterVmos(UnregisterVmosRequest& request,
                                 UnregisterVmosCompleter::Sync& completer) {
  std::vector<fuchsia_hardware_usb_endpoint::VmoInfo> ret;
  for (const auto& id : request.vmo_ids()) {
    auto registered_vmo = registered_vmos_.extract(id);
    if (registered_vmo.empty()) {
      continue;
    }

    zx_status_t status = zx_pmt_unpin(registered_vmo.mapped().pmt);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to unpin registered VMO %d", status);
      continue;
    }
    free(registered_vmo.mapped().phys_list);

    ret.emplace_back(std::move(fuchsia_hardware_usb_endpoint::VmoInfo().vmo_id(id).vmo(
        std::move(registered_vmo.mapped().vmo))));
  }
  completer.Reply({std::move(ret)});
}

void UsbEndpoint::RequestComplete(zx_status_t status, size_t actual, RequestVariant request) {
  if (std::holds_alternative<usb::BorrowedRequest<void>>(request)) {
    std::get<usb::BorrowedRequest<void>>(request).Complete(status, actual);
    return;
  }
  auto& req = std::get<usb::FidlRequest>(request);

  auto defer_completion = *req.request().defer_completion();
  completions_.emplace_back(std::move(fuchsia_hardware_usb_endpoint::Completion()
                                          .request(req.take_request())
                                          .status(status)
                                          .transfer_size(actual)));
  if (defer_completion && status == ZX_OK) {
    return;
  }
  if (binding_ref_) {
    std::vector<fuchsia_hardware_usb_endpoint::Completion> completions;
    completions.swap(completions_);

    auto status = fidl::SendEvent(*binding_ref_)->OnCompletion(std::move(completions));
    if (status.is_error()) {
      zxlogf(ERROR, "Error sending event: %s", status.error_value().status_string());
    }
  }
}

}  // namespace usb_endpoint
