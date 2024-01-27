// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.boot/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/wire/client.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fidl/cpp/wire/internal/transport_channel.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/zx/channel.h>
#include <string.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <memory>

#include <fbl/unique_fd.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/sys/early_boot_instrumentation/coverage_source.h"

int main(int argc, char** argv) {
  static const std::string static_dir_name(early_boot_instrumentation::kStaticDir);
  static const std::string dynamic_dir_name(early_boot_instrumentation::kDynamicDir);
  const fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line, {"early-boot-instrumentation"});

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto context = sys::ComponentContext::CreateAndServeOutgoingDirectory();

  // Add prof_data dir.
  early_boot_instrumentation::SinkDirMap sink_map;

  // All but llvm-profile data until its unified.
  vfs::PseudoDir* debug_data = context->outgoing()->GetOrCreateDirectory("debugdata");

  // Get the SvcStash server end.
  zx::channel provider_client, provider_server;
  if (zx_status_t res = zx::channel::create(0, &provider_client, &provider_server); res != ZX_OK) {
    FX_LOGS(ERROR) << "Could not create channel for fuchsia.boot.SvcStashProvider. "
                   << zx_status_get_string(res);
  } else if (zx_status_t res = fdio_service_connect(
                 fidl::DiscoverableProtocolDefaultPath<fuchsia_boot::SvcStashProvider>,
                 provider_server.release());
             res != ZX_OK) {
    FX_LOGS(ERROR) << "Could not obtain handle to fuchsia.boot.SvcStashProvider. "
                   << zx_status_get_string(res);
  } else {  // Successfully connected to the service.
    fidl::WireSyncClient<fuchsia_boot::SvcStashProvider> provider_fidl_client;
    fidl::ClientEnd<fuchsia_boot::SvcStashProvider> provider_client_end(std::move(provider_client));
    provider_fidl_client.Bind(std::move(provider_client_end));

    auto get_response = provider_fidl_client->Get();

    if (get_response.ok() && get_response->is_ok()) {
      auto& stash_svc = get_response->value()->resource;
      sink_map = early_boot_instrumentation::ExtractDebugData(std::move(stash_svc));
    }
  }

  // TODO(fxbug.dev/124317): This code does not create any directories when
  // profraw files are not available. Fix it as per contract.
  fbl::unique_fd kernel_data_dir(open("/boot/kernel/data", O_RDONLY));
  if (!kernel_data_dir) {
    const char* err = strerror(errno);
    FX_LOGS(ERROR) << "Could not obtain handle to '/boot/kernel/data'. " << err;
  } else if (zx::result res =
                 early_boot_instrumentation::ExposeKernelProfileData(kernel_data_dir, sink_map);
             res.is_error()) {
    FX_PLOGS(ERROR, res.status_value()) << "Could not expose kernel profile data. ";
  }

  fbl::unique_fd phys_data_dir(open("/boot/kernel/data/phys", O_RDONLY));
  if (!phys_data_dir) {
    const char* err = strerror(errno);
    FX_LOGS(ERROR) << "Could not obtain handle to '/boot/kernel/data/phys'. " << err;
  } else if (zx::result res =
                 early_boot_instrumentation::ExposePhysbootProfileData(phys_data_dir, sink_map);
             res.is_error()) {
    FX_PLOGS(ERROR, res.status_value()) << "Could not expose physboot profile data. ";
  }

  for (auto& [sink, root] : sink_map) {
    debug_data->AddEntry(sink, std::move(root));
  }
  sink_map.clear();

  loop.Run();
  return 0;
}
