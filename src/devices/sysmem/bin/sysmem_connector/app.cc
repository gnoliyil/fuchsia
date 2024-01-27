// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "app.h"

#include <fuchsia/metrics/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/sysmem-connector/sysmem-connector.h>
#include <lib/vfs/cpp/pseudo_dir.h>

#include <sdk/lib/syslog/cpp/macros.h>

const char* kSysmemClassPath = "/dev/class/sysmem";

App::App(async_dispatcher_t* dispatcher)
    : dispatcher_(dispatcher),
      component_context_(sys::ComponentContext::CreateAndServeOutgoingDirectory()) {
  zx_status_t status = sysmem_connector_init(kSysmemClassPath, true, &sysmem_connector_);
  if (status != ZX_OK) {
    printf(
        "sysmem_connector sysmem_connector_init() failed - exiting - status: "
        "%d\n",
        status);
    // Without sysmem_connector_init() success, we can't serve anything, so assert.
    ZX_ASSERT(status == ZX_OK);
  }
  status = outgoing_aux_service_directory_parent_.AddPublicService<
      fuchsia::metrics::MetricEventLoggerFactory>(
      [this](fidl::InterfaceRequest<fuchsia::metrics::MetricEventLoggerFactory> request) {
        ZX_DEBUG_ASSERT(component_context_);
        FX_LOGS(INFO)
            << "sysmem_connector handling request for MetricEventLoggerFactory -- handle value: "
            << request.channel().get();
        component_context_->svc()->Connect(std::move(request));
      });
  outgoing_aux_service_directory_ =
      outgoing_aux_service_directory_parent_.GetOrCreateDirectory("svc");

  // Else sysmem_connector won't be able to provide what sysmem expects to be able to rely on.
  ZX_ASSERT(status == ZX_OK);

  fidl::InterfaceHandle<fuchsia::io::Directory> aux_service_directory;
  status = outgoing_aux_service_directory_->Serve(
      fuchsia::io::OpenFlags::RIGHT_READABLE | fuchsia::io::OpenFlags::RIGHT_WRITABLE |
          fuchsia::io::OpenFlags::DIRECTORY,
      aux_service_directory.NewRequest().TakeChannel(), dispatcher_);
  if (status != ZX_OK) {
    printf("outgoing_aux_service_directory_.Serve() failed - status: %d\n", status);
    // We expect this to work.
    ZX_ASSERT(status == ZX_OK);
  }

  sysmem_connector_queue_service_directory(sysmem_connector_,
                                           aux_service_directory.TakeChannel().release());

  component_context_->outgoing()->AddPublicService<fuchsia::sysmem::Allocator>(
      [this](fidl::InterfaceRequest<fuchsia::sysmem::Allocator> request) {
        // Normally a service would directly serve the server end of the
        // channel, but in this case we forward the service request to the
        // sysmem driver.  We do the forwarding via code we share with a similar
        // Zircon service.
        sysmem_connector_queue_connection_request_v1(sysmem_connector_,
                                                     request.TakeChannel().release());
      });
  component_context_->outgoing()->AddPublicService<fuchsia::sysmem2::Allocator>(
      [this](fidl::InterfaceRequest<fuchsia::sysmem2::Allocator> request) {
        // Normally a service would directly serve the server end of the
        // channel, but in this case we forward the service request to the
        // sysmem driver.  We do the forwarding via code we share with a similar
        // Zircon service.
        sysmem_connector_queue_connection_request_v2(sysmem_connector_,
                                                     request.TakeChannel().release());
      });
}

App::~App() {
  sysmem_connector_release(sysmem_connector_);
  sysmem_connector_ = nullptr;
}
