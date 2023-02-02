// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sysmem_fuzz_common.h"

#include <fuchsia/sysmem/c/banjo.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include "fidl/fuchsia.sysmem2/cpp/markers.h"
#include "log_rtn.h"

MockDdkSysmem::~MockDdkSysmem() {
  if (initialized_) {
    loop_.Shutdown();
    sysmem_.DdkAsyncRemove();
    mock_ddk::ReleaseFlaggedDevices(root_.get());
    sysmem_.ResetThreadCheckerForTesting();
    ZX_ASSERT(sysmem_.logical_buffer_collections().size() == 0);
    initialized_ = false;
  }
}
bool MockDdkSysmem::Init() {
  if (initialized_) {
    fprintf(stderr, "MockDdkSysmem already initialized.\n");
    fflush(stderr);
    return false;
  }
  // Avoid wasting fuzzer time outputting logs.
  mock_ddk::SetMinLogSeverity(FX_LOG_FATAL);
  // Pick a platform where AFBC textures will be used. Also add a protected pool to test code that
  // handles that specially (though protected allocations will always fail because the pool is never
  // marked ready).
  static const sysmem_metadata_t metadata{
      .vid = PDEV_VID_AMLOGIC,
      .pid = PDEV_PID_AMLOGIC_S905D2,
      .protected_memory_size = 1024 * 1024,
      .contiguous_memory_size = 0,
  };
  root_->SetMetadata(SYSMEM_METADATA_TYPE, &metadata, sizeof(metadata));

  pdev_.UseFakeBti();

  root_->AddProtocol(ZX_PROTOCOL_PDEV, pdev_.proto()->ops, pdev_.proto()->ctx);
  if (ZX_OK == sysmem_.Bind()) {
    initialized_ = true;
  }
  sysmem_.set_settings(sysmem_driver::Settings{.max_allocation_size = 256 * 1024});

  loop_.StartThread();
  return initialized_;
}

zx::result<fidl::ClientEnd<fuchsia_sysmem::Allocator>> MockDdkSysmem::Connect() {
  auto driver_endpoints = fidl::CreateEndpoints<fuchsia_sysmem2::DriverConnector>();
  if (driver_endpoints.is_error()) {
    return zx::error(driver_endpoints.status_value());
  }

  fidl::BindServer(loop_.dispatcher(), std::move(driver_endpoints->server), &sysmem_);

  zx::result allocator_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::Allocator>();
  if (allocator_endpoints.is_error()) {
    return zx::error(allocator_endpoints.status_value());
  }

  auto [allocator_client_end, allocator_server_end] = std::move(*allocator_endpoints);

  fidl::WireSyncClient<fuchsia_sysmem2::DriverConnector> driver_client(
      std::move(driver_endpoints->client));
  fidl::Status result = driver_client->ConnectV1(std::move(allocator_server_end));
  if (!result.ok()) {
    return zx::error(result.status());
  }
  return zx::ok(std::move(allocator_client_end));
}
