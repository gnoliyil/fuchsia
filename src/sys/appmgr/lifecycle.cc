// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/lifecycle.h"

#include "src/sys/appmgr/appmgr.h"

namespace component {

zx_status_t LifecycleServer::Create(
    async_dispatcher_t* dispatcher,
    fidl::ServerEnd<fuchsia_process_lifecycle::Lifecycle> server_end) {
  lifecycle_ = fidl::BindServer(dispatcher, std::move(server_end), this);
  return ZX_OK;
}

void LifecycleServer::Close(zx_status_t status) {
  FX_LOGS(INFO) << "Closing appmgr lifecycle channel.";
  if (lifecycle_) {
    lifecycle_->Close(status);
  } else {
    FX_LOGS(ERROR) << "Appmgr lifecycle not bound.";
  }
}

void LifecycleServer::Stop(StopCompleter::Sync& completer) {
  FX_LOGS(INFO) << "appmgr: received shutdown command over lifecycle interface";
  child_lifecycles_ = appmgr_->Shutdown([this](zx_status_t status) mutable {
    FX_LOGS(INFO) << "Lifecycle Server complete callback";
    child_lifecycles_.clear();
    Close(status);
    stop_callback_(status);
  });
}

}  // namespace component
