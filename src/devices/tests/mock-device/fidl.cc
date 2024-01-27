// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl.h"

#include <fuchsia/device/mock/cpp/fidl.h>
#include <lib/fidl/cpp/message.h>
#include <lib/fidl/cpp/message_part.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <zircon/fidl.h>

#include <utility>

#include <fbl/algorithm.h>

namespace mock_device {

zx_status_t WaitForPerformActions(const zx::channel& c,
                                  fbl::Array<device_mock::wire::Action>* actions_out) {
  zx_signals_t signals;
  zx_status_t status =
      c.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &signals);
  if (status != ZX_OK) {
    return status;
  }
  if (!(signals & ZX_CHANNEL_READABLE)) {
    return ZX_ERR_STOP;
  }

  FIDL_ALIGNDECL uint8_t request_buf[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_info_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl::HLCPPIncomingMessage request(fidl::BytePart(request_buf, sizeof(request_buf)),
                                     fidl::HandleInfoPart(handles, std::size(handles)));
  status = request.Read(c.get(), 0);
  if (status != ZX_OK) {
    return status;
  }

  const char* err_out = nullptr;
  status = request.Decode(fuchsia::device::mock::MockDeviceThreadPerformActionsRequest::FidlType,
                          &err_out);
  if (status != ZX_OK) {
    printf("mock-device-thread: Failed to decode actions: %s\n", err_out);
    return status;
  }
  auto payload =
      request.GetBytesAs<fidl::WireRequest<device_mock::MockDeviceThread::PerformActions>>();
  auto array = std::make_unique<device_mock::wire::Action[]>(payload->actions.count());
  memcpy(reinterpret_cast<void*>(array.get()),
         reinterpret_cast<const void*>(payload->actions.data()),
         payload->actions.count() * sizeof(device_mock::wire::Action));
  actions_out->reset(array.release(), payload->actions.count());
  return ZX_OK;
}

}  // namespace mock_device
