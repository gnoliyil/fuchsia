// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "adb-shell.h"

#include <fidl/fuchsia.dash/cpp/wire.h>
#include <fidl/fuchsia.hardware.adb/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.sys2/cpp/wire.h>
#include <lib/async/cpp/task.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/syslog/cpp/macros.h>
#include <stdio.h>
#include <zircon/types.h>

#include "src/developer/adb/bin/adb-shell/adb_shell_config.h"

namespace adb_shell {

void AdbShell::ConnectToService(ConnectToServiceRequestView request,
                                ConnectToServiceCompleter::Sync& completer) {
  auto status = AddShell({std::string(request->args.get())}, std::move(request->socket));
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess();
}

zx_status_t AdbShell::AddShell(std::optional<std::string> args, zx::socket server) {
  auto impl = std::make_unique<AdbShellImpl>(svc_.borrow(), dispatcher_);
  auto impl_ptr = impl.get();
  {
    fbl::AutoLock lock(&shell_lock_);
    shells_.emplace_back(std::move(impl));
  }

  auto status = impl_ptr->Start(std::move(server), config_.dash_moniker(), args,
                                [this](AdbShellImpl* adb) { RemoveShell(adb); });
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to start adb shell instance -  " << status;
    return status;
  }
  return ZX_OK;
}

void AdbShell::RemoveShell(AdbShellImpl* impl) {
  fbl::AutoLock lock(&shell_lock_);
  auto itr = shells_.begin();
  for (; itr != shells_.end(); itr++) {
    if (itr->get() == impl) {
      break;
    }
  }

  if (itr != shells_.end()) {
    shells_.erase(itr);
  } else {
    FX_LOGS(ERROR) << "Trying to erase a non existing shell instance " << impl;
  }
}

zx_status_t AdbShellImpl::ResolveMoniker(std::string moniker) {
  auto client_end = component::ConnectAt<fuchsia_sys2::LifecycleController>(
      svc_, "fuchsia.sys2.LifecycleController.root");
  if (client_end.is_error()) {
    FX_LOGS(ERROR) << "Could not connect to dash launcher " << client_end.status_string();
    return client_end.status_value();
  }

  fidl::WireSyncClient<fuchsia_sys2::LifecycleController> lifecycle_controller(
      std::move(*client_end));
  auto result = lifecycle_controller->ResolveInstance(fidl::StringView::FromExternal(moniker));
  if (!result.ok()) {
    FX_LOGS(ERROR) << "FIDL call to resolve moniker failed" << result.status();
    return result.status();
  }
  if (result->is_error()) {
    FX_LOGS(ERROR) << "Could not resolve moniker "
                   << static_cast<uint32_t>(result.value().error_value());
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t AdbShellImpl::Start(zx::socket shell_server, std::string moniker,
                                std::optional<std::string> args,
                                fit::callback<void(AdbShellImpl*)> on_dead) {
  auto resolve_result = ResolveMoniker(moniker);
  if (resolve_result != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to resolve moniker " << moniker;
    return resolve_result;
  }

  on_dead_ = std::move(on_dead);

  auto client_end = component::ConnectAt<fuchsia_dash::Launcher>(svc_);
  if (client_end.is_error()) {
    FX_LOGS(ERROR) << "Could not connect to dash launcher " << client_end.status_string();
    return client_end.status_value();
  }

  dash_client_.Bind(std::move(*client_end), dispatcher_, this);
  fidl::StringView cmd = {};
  if (args.has_value() && !args.value().empty()) {
    cmd = fidl::StringView::FromExternal(args.value());
  }

  FX_LOGS(DEBUG) << "Calling Launch Socket with moniker " << moniker;

  auto result = dash_client_.sync()->ExploreComponentOverSocket(
      fidl::StringView::FromExternal(moniker), std::move(shell_server), {}, cmd,
      fuchsia_dash::DashNamespaceLayout::kInstanceNamespaceIsRoot);

  if (!result.ok()) {
    FX_LOGS(ERROR) << "FIDL call to ExploreComponentOverSocket failed" << result.status();
    return result.status();
  }

  if (result->is_error()) {
    FX_LOGS(ERROR) << "ExploreComponentOverSocket failed "
                   << static_cast<uint32_t>(result.value().error_value());
    return static_cast<zx_status_t>(result->error_value());
  }

  return ZX_OK;
}

void AdbShellImpl::OnTerminated(fidl::WireEvent<fuchsia_dash::Launcher::OnTerminated>* event) {
  FX_LOGS(DEBUG) << "Got OnTerminate Event.";
  if (on_dead_) {
    async::PostTask(dispatcher_,
                    [on_dead = std::move(on_dead_), impl = this]() mutable { on_dead(impl); });
  }
}

}  // namespace adb_shell
