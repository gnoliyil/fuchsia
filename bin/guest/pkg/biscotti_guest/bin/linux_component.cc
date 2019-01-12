// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/pkg/biscotti_guest/bin/linux_component.h"

#include <lib/async/default.h>
#include <zircon/status.h>

namespace biscotti {

// static
std::unique_ptr<LinuxComponent> LinuxComponent::Create(
    TerminationCallback termination_callback, fuchsia::sys::Package package,
    fuchsia::sys::StartupInfo startup_info,
    fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller,
    fuchsia::ui::app::ViewProviderPtr remote_view_provider) {
  FXL_DCHECK(remote_view_provider) << "Missing remote_view_provider";
  return std::unique_ptr<LinuxComponent>(
      new LinuxComponent(std::move(termination_callback), std::move(package),
                         std::move(startup_info), std::move(controller),
                         std::move(remote_view_provider)));
}

LinuxComponent::LinuxComponent(
    TerminationCallback termination_callback, fuchsia::sys::Package package,
    fuchsia::sys::StartupInfo startup_info,
    fidl::InterfaceRequest<fuchsia::sys::ComponentController>
        application_controller_request,
    fuchsia::ui::app::ViewProviderPtr remote_view_provider)
    : termination_callback_(std::move(termination_callback)),
      application_controller_(this),
      remote_view_provider_(std::move(remote_view_provider)) {
  application_controller_.set_error_handler(
      [this](zx_status_t status) { Kill(); });

  auto& launch_info = startup_info.launch_info;
  if (launch_info.directory_request) {
    outgoing_.Serve(std::move(launch_info.directory_request));
  }
  outgoing_.AddPublicService<fuchsia::ui::app::ViewProvider>(
      view_bindings_.GetHandler(this));
  outgoing_.AddPublicService<fuchsia::ui::viewsv1::ViewProvider>(
      v1_view_bindings_.GetHandler(this));

  // TODO(CF-268): Remove once services are pulled from the public directory.
  outgoing_.root_dir()->AddEntry(
      fuchsia::ui::app::ViewProvider::Name_,
      fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        view_bindings_.AddBinding(
            this, fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>(
                      std::move(channel)));
        return ZX_OK;
      })));
  outgoing_.root_dir()->AddEntry(
      fuchsia::ui::viewsv1::ViewProvider::Name_,
      fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        v1_view_bindings_.AddBinding(
            this, fidl::InterfaceRequest<fuchsia::ui::viewsv1::ViewProvider>(
                      std::move(channel)));
        return ZX_OK;
      })));
}

LinuxComponent::~LinuxComponent() = default;

// |fuchsia::sys::ComponentController|
void LinuxComponent::Kill() {
  application_controller_.events().OnTerminated(
      0, fuchsia::sys::TerminationReason::EXITED);

  termination_callback_(this);
  // WARNING: Don't do anything past this point as this instance may have been
  // collected.
}

// |fuchsia::sys::ComponentController|
void LinuxComponent::Detach() {
  application_controller_.set_error_handler(nullptr);
}

// |fuchsia::ui::viewsv1::ViewProvider|
void LinuxComponent::CreateView(
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> view_owner,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> services) {
  CreateView(zx::eventpair(view_owner.TakeChannel().release()),
             std::move(services), nullptr);
}

// |fuchsia::ui::app::ViewProvider|
void LinuxComponent::CreateView(
    zx::eventpair view_token,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
    fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services) {
  remote_view_provider_->CreateView(std::move(view_token),
                                    std::move(incoming_services),
                                    std::move(outgoing_services));
}

}  // namespace biscotti
