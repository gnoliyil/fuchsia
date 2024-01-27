// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/termina_guest_manager/termina_guest_manager.h"

#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/virtualization/bin/termina_guest_manager/block_devices.h"

namespace termina_guest_manager {
namespace {

using ::fuchsia::virtualization::GuestConfig;
using ::fuchsia::virtualization::GuestManagerError;

constexpr std::string_view kLinuxEnvironmentName("termina");
constexpr size_t kBytesToWipe = 1ul * 1024 * 1024;  // 1 MiB

void NotifyClient(fidl::Binding<fuchsia::virtualization::LinuxManager>& binding,
                  GuestInfo& current_info) {
  fuchsia::virtualization::LinuxGuestInfo info;
  info.set_cid(current_info.cid);
  info.set_container_status(current_info.container_status);
  info.set_download_percent(current_info.download_percent);
  info.set_failure_reason(current_info.failure_reason);
  binding.events().OnGuestInfoChanged(std::string(kLinuxEnvironmentName), std::move(info));
}

std::string GuestManagerErrorToString(fuchsia::virtualization::GuestManagerError error) {
  switch (error) {
    case fuchsia::virtualization::GuestManagerError::BAD_CONFIG:
      return "BAD_CONFIG";
    case fuchsia::virtualization::GuestManagerError::ALREADY_RUNNING:
      return "ALREADY_RUNNING";
    case fuchsia::virtualization::GuestManagerError::NOT_RUNNING:
      return "NOT_RUNNING";
    case fuchsia::virtualization::GuestManagerError::START_FAILURE:
      return "START_FAILURE";
    case fuchsia::virtualization::GuestManagerError::NO_STORAGE:
      return "NO_STORAGE";
    default:
      return fxl::StringPrintf("GuestManagerError(%u)", static_cast<int32_t>(error));
  }
}

}  // namespace

TerminaGuestManager::TerminaGuestManager(async_dispatcher_t* dispatcher,
                                         fit::function<void()> stop_manager_callback)
    : TerminaGuestManager(dispatcher, sys::ComponentContext::CreateAndServeOutgoingDirectory(),
                          termina_config::Config::TakeFromStartupHandle(),
                          std::move(stop_manager_callback)) {}

TerminaGuestManager::TerminaGuestManager(async_dispatcher_t* dispatcher,
                                         std::unique_ptr<sys::ComponentContext> context,
                                         termina_config::Config structured_config,
                                         fit::function<void()> stop_manager_callback)
    : GuestManager(dispatcher, context.get()),
      context_(std::move(context)),
      structured_config_(std::move(structured_config)),
      stop_manager_callback_(std::move(stop_manager_callback)),
      dispatcher_(dispatcher) {
  guest_ = CreateGuest();
  context_->outgoing()->AddPublicService<fuchsia::virtualization::LinuxManager>(
      [this](auto request) {
        manager_bindings_.AddBinding(this, std::move(request));
        // If we have an initial status; notify the new connection now.
        if (info_.has_value()) {
          NotifyClient(*manager_bindings_.bindings().back(), *info_);
        }
      });
}

std::unique_ptr<Guest> TerminaGuestManager::CreateGuest() {
  return std::make_unique<Guest>(
      structured_config_,
      [this](GuestInfo guest_info) {
        // OnGuestInfoChanged must be called on the FIDL thread from which it originates since
        // StartAndGetLinuxGuestInfo responses must be sent on the same thread as they were
        // dispatched, while Guest can use this callback from other (e.g. grpc) threads.
        async::PostTask(dispatcher_, [this, guest_info = std::move(guest_info)]() {
          OnGuestInfoChanged(std::move(guest_info));
        });
      },
      [this] { return QueryGuestNetworkState(); });
}

fit::result<GuestManagerError, GuestConfig> TerminaGuestManager::GetDefaultGuestConfig() {
  TRACE_DURATION("termina_guest_manager", "TerminaGuestManager::GetDefaultGuestConfig");

  auto base_config = GuestManager::GetDefaultGuestConfig();
  if (base_config.is_error()) {
    return base_config.take_error();
  }

  auto block_devices_result = GetBlockDevices(structured_config_);
  if (block_devices_result.is_error()) {
    FX_LOGS(ERROR) << "Failed to option block devices: " << block_devices_result.error_value();
    return fit::error(GuestManagerError::NO_STORAGE);
  }

  // Drop /dev from our local namespace. We no longer need this capability so we go ahead and
  // release it.
  DropDevNamespace();

  fuchsia::virtualization::GuestConfig termina_config;
  termina_config.set_virtio_gpu(false);
  termina_config.set_block_devices(std::move(block_devices_result.value()));
  termina_config.set_magma_device(fuchsia::virtualization::MagmaDevice());

  // Include a wayland device.
  fuchsia::wayland::ServerPtr server_proxy;
  termina_config.mutable_wayland_device();

  // Add the vsock listeners for gRPC services.
  *termina_config.mutable_vsock_listeners() = guest_->take_vsock_listeners();

  return fit::ok(guest_config::MergeConfigs(std::move(*base_config), std::move(termina_config)));
}

void TerminaGuestManager::StartGuest() {
  fuchsia::virtualization::GuestConfig cfg;
  Launch(std::move(cfg), guest_controller_.NewRequest(), [this](auto res) {
    if (res.is_err()) {
      FX_LOGS(INFO) << "Termina Guest failed to launch: " << GuestManagerErrorToString(res.err());
      OnGuestInfoChanged(GuestInfo{
          .cid = fuchsia::virtualization::DEFAULT_GUEST_CID,
          .container_status = fuchsia::virtualization::ContainerStatus::FAILED,
          .failure_reason = fxl::StringPrintf("Failed to launch VM: %s",
                                              GuestManagerErrorToString(res.err()).c_str()),
      });
    }
  });
}

void TerminaGuestManager::OnGuestLaunched() {
  if (!guest_controller_) {
    Connect(guest_controller_.NewRequest(), [](auto res) {
      // This should only fail if the guest isn't started, which should not be possible here.
      FX_CHECK(res.is_response());
    });
  }
  guest_->OnGuestLaunched(*this, *guest_controller_.get());
}

void TerminaGuestManager::OnGuestStopped() {
  info_ = std::nullopt;
  guest_ = CreateGuest();

  if (structured_config_.stateful_partition_type() == "fvm") {
    // The termina guest manager is dropping access to /dev preventing further accesses, so we
    // can't restart the guest without restarting the guest manager component unless using fxfs.
    // Eventually we will only be using fxfs, and this check can go away.
    stop_manager_callback_();
  }
}

void TerminaGuestManager::StartAndGetLinuxGuestInfo(std::string label,
                                                    StartAndGetLinuxGuestInfoCallback callback) {
  TRACE_DURATION("termina_guest_manager", "TerminaGuestManager::StartAndGetLinuxGuestInfo");

  // Linux runner is currently limited to a single environment name.
  if (label != kLinuxEnvironmentName) {
    FX_LOGS(ERROR) << "Invalid Linux environment: " << label;
    callback(fpromise::error(ZX_ERR_UNAVAILABLE));
    return;
  }

  if (!is_guest_started()) {
    StartGuest();
  } else if (info_ && info_->container_status == fuchsia::virtualization::ContainerStatus::FAILED) {
    info_ = std::nullopt;
    guest_->RetryContainerStartup();
  }

  if (info_.has_value()) {
    fuchsia::virtualization::LinuxGuestInfo info;
    info.set_cid(info_->cid);
    info.set_container_status(info_->container_status);
    info.set_download_percent(info_->download_percent);
    info.set_failure_reason(info_->failure_reason);
    fuchsia::virtualization::LinuxManager_StartAndGetLinuxGuestInfo_Response response;
    response.info = std::move(info);
    callback(fuchsia::virtualization::LinuxManager_StartAndGetLinuxGuestInfo_Result::WithResponse(
        std::move(response)));
  } else {
    callbacks_.push_back(std::move(callback));
  }
}

void TerminaGuestManager::WipeData(WipeDataCallback callback) {
  if (is_guest_started()) {
    callback(fuchsia::virtualization::LinuxManager_WipeData_Result::WithErr(ZX_ERR_BAD_STATE));
    return;
  }
  // We zero out some bytes at the beginning of the partition to corrupt any filesystem data-
  // structures stored there.
  zx::result<> status = WipeStatefulPartition(kBytesToWipe);
  if (status.is_error()) {
    callback(fuchsia::virtualization::LinuxManager_WipeData_Result::WithErr(status.status_value()));
  } else {
    callback(fuchsia::virtualization::LinuxManager_WipeData_Result::WithResponse(
        fuchsia::virtualization::LinuxManager_WipeData_Response()));
  }
}

void TerminaGuestManager::GracefulShutdown() {
  if (is_guest_started()) {
    guest_->InitiateGuestShutdown();
  }
}

void TerminaGuestManager::OnGuestInfoChanged(GuestInfo info) {
  info_ = info;
  while (!callbacks_.empty()) {
    fuchsia::virtualization::LinuxGuestInfo info;
    info.set_cid(info_->cid);
    info.set_container_status(fuchsia::virtualization::ContainerStatus::TRANSIENT);
    fuchsia::virtualization::LinuxManager_StartAndGetLinuxGuestInfo_Response response;
    response.info = std::move(info);
    callbacks_.front()(
        fuchsia::virtualization::LinuxManager_StartAndGetLinuxGuestInfo_Result::WithResponse(
            std::move(response)));
    callbacks_.pop_front();
  }
  for (auto& binding : manager_bindings_.bindings()) {
    NotifyClient(*binding, *info_);
  }
}

}  // namespace termina_guest_manager
