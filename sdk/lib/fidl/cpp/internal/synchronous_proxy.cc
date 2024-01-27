// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fidl/cpp/internal/synchronous_proxy.h"

#include <memory>
#include <utility>

#include "lib/fidl/cpp/internal/logging.h"

namespace fidl {
namespace internal {

SynchronousProxy::SynchronousProxy(zx::channel channel) : channel_(std::move(channel)) {}

SynchronousProxy::~SynchronousProxy() = default;

zx::channel SynchronousProxy::TakeChannel() { return std::move(channel_); }

zx_status_t SynchronousProxy::Send(const fidl_type_t* type, HLCPPOutgoingMessage message) {
  return fidl::internal::SendMessage(channel_, type, std::move(message));
}

zx_status_t SynchronousProxy::Call(const fidl_type_t* request_type,
                                   const fidl_type_t* response_type, HLCPPOutgoingMessage request,
                                   HLCPPIncomingMessage* response) {
  const char* error_msg = nullptr;
  if (request_type != nullptr) {
    zx_status_t status = request.Validate(request_type, &error_msg);
    if (status != ZX_OK) {
      FIDL_REPORT_ENCODING_ERROR(request, request_type, error_msg);
      return status;
    }
  } else if (!request.has_only_header()) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status = request.Call(channel_.get(), 0, ZX_TIME_INFINITE, response);
  if (status != ZX_OK)
    return status;

  if (response_type != nullptr) {
    status = response->Decode(response_type, &error_msg);
    if (status != ZX_OK) {
      FIDL_REPORT_DECODING_ERROR(*response, response_type, error_msg);
      return status;
    }
  } else if (!response->has_only_header()) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_OK;
}

}  // namespace internal
}  // namespace fidl
