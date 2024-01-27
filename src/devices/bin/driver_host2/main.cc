// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.driver.framework/cpp/wire.h>
#include <fidl/fuchsia.inspect/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fdf/env.h>
#include <lib/inspect/service/cpp/service.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-provider/provider.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/service.h>
#include <zircon/status.h>

#include "driver_host.h"
#include "src/devices/lib/log/log.h"
#include "src/sys/lib/stdout-to-debuglog/cpp/stdout-to-debuglog.h"

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf

namespace fi = fuchsia::inspect;

constexpr char kDiagnosticsDir[] = "diagnostics";

int main(int argc, char** argv) {
  fuchsia_logging::SetTags({"driver_host2", "driver"});
  driver_logger::GetLogger().AddTag("driver_host2").AddTag("driver");
  // TODO(fxbug.dev/33183): Lock down job.
  zx_status_t status = StdoutToDebuglog::Init();
  if (status != ZX_OK) {
    FX_SLOG(INFO,
            "Failed to redirect stdout to debuglog, assuming test environment and continuing");
  }

  if (zx_status_t status = fdf_env_start(); status != ZX_OK) {
    FX_SLOG(ERROR, "Failed to create the initial dispatcher thread");
    return status;
  }

  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  auto outgoing = component::OutgoingDirectory(loop.dispatcher());

  auto serve = outgoing.ServeFromStartupInfo();
  if (serve.is_error()) {
    FX_SLOG(ERROR, "Failed to serve outgoing directory", KV("status", serve.status_string()));
    return serve.status_value();
  }

  // Setup inspect.
  inspect::Inspector inspector;
  if (!inspector) {
    FX_SLOG(ERROR, "Failed to allocate VMO for inspector");
    return ZX_ERR_NO_MEMORY;
  }
  auto tree_handler = inspect::MakeTreeHandler(&inspector, loop.dispatcher());
  auto tree_service = std::make_unique<vfs::Service>(std::move(tree_handler));
  vfs::PseudoDir diagnostics_dir;
  status = diagnostics_dir.AddEntry(fi::Tree::Name_, std::move(tree_service));
  if (status != ZX_OK) {
    FX_SLOG(ERROR, "Failed to add directory entry", KV("name", fi::Tree::Name_),
            KV("status_str", zx_status_get_string(status)));
    return status;
  }

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  diagnostics_dir.Serve(
      fuchsia::io::OpenFlags::RIGHT_WRITABLE | fuchsia::io::OpenFlags::RIGHT_READABLE |
          fuchsia::io::OpenFlags::RIGHT_EXECUTABLE | fuchsia::io::OpenFlags::DIRECTORY,
      endpoints->server.TakeChannel(), loop.dispatcher());
  zx::result<> status_result = outgoing.AddDirectory(std::move(endpoints->client), kDiagnosticsDir);
  if (status_result.is_error()) {
    FX_SLOG(ERROR, "Failed to add directory entry", KV("name", kDiagnosticsDir),
            KV("status_str", status_result.status_string()));
    return status_result.status_value();
  }

  dfv2::DriverHost driver_host(inspector, loop);
  auto init = driver_host.PublishDriverHost(outgoing);
  if (init.is_error()) {
    return init.error_value();
  }

  status = loop.Run();
  // All drivers should now be shutdown and stopped.
  // Destroy all dispatchers in case any weren't freed correctly.
  // This will block until all dispatcher callbacks complete.
  fdf_env_destroy_all_dispatchers();
  return status;
}
