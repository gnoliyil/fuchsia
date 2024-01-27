// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/cpp/testing/enclosing_environment.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/service.h>
#include <zircon/assert.h>

#include <memory>
#include <utility>

namespace sys {
namespace testing {

// For Fuchsia in-tree coverage support, the the FIDL service name
// also known as |fuchsia::debugdata::Publisher::Name_|
static const char fuchsia_debugdata_Publisher_Name_[] = "fuchsia.debugdata.Publisher";

EnvironmentServices::ParentOverrides::ParentOverrides(ParentOverrides&&) noexcept = default;

EnvironmentServices::ParentOverrides::ParentOverrides() = default;

EnvironmentServices::EnvironmentServices(const fuchsia::sys::EnvironmentPtr& parent_env,
                                         ParentOverrides parent_overrides,
                                         async_dispatcher_t* dispatcher)
    : svc_(std::make_unique<vfs::PseudoDir>()), dispatcher_(dispatcher) {
#if __Fuchsia_API_level__ < 10
  zx::channel
#else
  fidl::InterfaceRequest<fuchsia::io::Directory>
#endif
      request;
  parent_svc_ = sys::ServiceDirectory::CreateWithRequest(&request);
  parent_env->GetDirectory(std::move(request));
  if (parent_overrides.loader_service_) {
    AddSharedService(parent_overrides.loader_service_, fuchsia::sys::Loader::Name_);
  } else {
    AllowParentService(fuchsia::sys::Loader::Name_);
  }

  if (parent_overrides.debug_data_publisher_service_) {
    AddSharedService(parent_overrides.debug_data_publisher_service_,
                     fuchsia_debugdata_Publisher_Name_);
  } else {
    AllowParentService(fuchsia_debugdata_Publisher_Name_);
  }
}

EnvironmentServices::~EnvironmentServices() = default;

// static
std::unique_ptr<EnvironmentServices> EnvironmentServices::Create(
    const fuchsia::sys::EnvironmentPtr& parent_env, async_dispatcher_t* dispatcher) {
  return std::unique_ptr<EnvironmentServices>(
      new EnvironmentServices(parent_env, ParentOverrides{}, dispatcher));
}

// static
std::unique_ptr<EnvironmentServices> EnvironmentServices::CreateWithParentOverrides(
    const fuchsia::sys::EnvironmentPtr& parent_env, ParentOverrides parent_overrides,
    async_dispatcher_t* dispatcher) {
  return std::unique_ptr<EnvironmentServices>(
      new EnvironmentServices(parent_env, std::move(parent_overrides), dispatcher));
}

zx_status_t EnvironmentServices::AddSharedService(const std::shared_ptr<vfs::Service>& service,
                                                  std::string service_name) {
  svc_names_.push_back(service_name);
  return svc_->AddSharedEntry(std::move(service_name), service);
}

zx_status_t EnvironmentServices::AddService(std::unique_ptr<vfs::Service> service,
                                            std::string service_name) {
  svc_names_.push_back(service_name);
  return svc_->AddEntry(std::move(service_name), std::move(service));
}

zx_status_t EnvironmentServices::AddService(Connector connector, std::string service_name) {
  return AddService(std::make_unique<vfs::Service>(std::move(connector)), std::move(service_name));
}

zx_status_t EnvironmentServices::AddServiceWithLaunchInfo(fuchsia::sys::LaunchInfo launch_info,
                                                          std::string service_name) {
  return AddServiceWithLaunchInfo(
      launch_info.url,
      [launch_info = std::move(launch_info)]() {
        // clone only URL and Arguments
        fuchsia::sys::LaunchInfo dup_launch_info;
        fidl::Clone(launch_info.url, &dup_launch_info.url);
        fidl::Clone(launch_info.arguments, &dup_launch_info.arguments);
        return dup_launch_info;
      },
      std::move(service_name));
}

zx_status_t EnvironmentServices::AddServiceWithLaunchInfo(
    std::string singleton_id, fit::function<fuchsia::sys::LaunchInfo()> handler,
    std::string service_name) {
  return AddService(
      std::make_unique<vfs::Service>([this, service_name, handler = std::move(handler),
                                      singleton_id = std::move(singleton_id),
                                      controller = fuchsia::sys::ComponentControllerPtr()](
                                         zx::channel client_handle,
                                         async_dispatcher_t* /*unused*/) mutable {
        auto it = singleton_services_.find(singleton_id);
        if (it == singleton_services_.end()) {
          fuchsia::sys::LaunchInfo launch_info = handler();
          auto services = sys::ServiceDirectory::CreateWithRequest(&launch_info.directory_request);

          enclosing_env_->CreateComponent(std::move(launch_info), controller.NewRequest());
          controller.set_error_handler([this, singleton_id, &controller](zx_status_t /*unused*/) {
            // TODO(unknown): show error? where on stderr?
            controller.Unbind();  // kills the singleton application
            singleton_services_.erase(singleton_id);
          });

          controller.events().OnTerminated =
              [this, singleton_id](int64_t exit_code, fuchsia::sys::TerminationReason reason) {
                if (service_terminated_callback_) {
                  service_terminated_callback_(singleton_id, exit_code, reason);
                }
              };

          std::tie(it, std::ignore) =
              singleton_services_.emplace(singleton_id, std::move(services));
        }

        it->second->Connect(service_name, std::move(client_handle));
      }),
      std::move(service_name));
}

zx_status_t EnvironmentServices::AllowParentService(std::string service_name) {
  return AddService(
      [this, service_name](zx::channel channel, async_dispatcher_t* /*unused*/) {
        parent_svc_->Connect(service_name, std::move(channel));
      },
      std::move(service_name));
}

fidl::InterfaceHandle<fuchsia::io::Directory> EnvironmentServices::ServeServiceDir(
    fuchsia::io::OpenFlags flags) {
  fidl::InterfaceHandle<fuchsia::io::Directory> dir;
  ZX_ASSERT(ServeServiceDir(dir.NewRequest(), flags) == ZX_OK);
  return dir;
}

zx_status_t EnvironmentServices::ServeServiceDir(
    fidl::InterfaceRequest<fuchsia::io::Directory> request, fuchsia::io::OpenFlags flags) {
  return ServeServiceDir(request.TakeChannel(), flags);
}

zx_status_t EnvironmentServices::ServeServiceDir(zx::channel request,
                                                 fuchsia::io::OpenFlags flags) {
  return svc_->Serve(flags, std::move(request), dispatcher_);
}

EnclosingEnvironment::EnclosingEnvironment(std::string label,
                                           const fuchsia::sys::EnvironmentPtr& parent_env,
                                           std::unique_ptr<EnvironmentServices> services,
                                           const fuchsia::sys::EnvironmentOptions& options)
    : label_(std::move(label)), services_(std::move(services)) {
  services_->set_enclosing_env(this);

  // Start environment with services.
  fuchsia::sys::ServiceListPtr service_list(new fuchsia::sys::ServiceList);
  service_list->names = std::move(services_->svc_names_);
  service_list->host_directory = services_
                                     ->ServeServiceDir()
#if __Fuchsia_API_level__ < 10
                                     .TakeChannel()
#endif
      ;
  fuchsia::sys::EnvironmentPtr env;

  parent_env->CreateNestedEnvironment(env.NewRequest(), env_controller_.NewRequest(), label_,
                                      std::move(service_list), options);
  env_controller_.set_error_handler([this](zx_status_t /*unused*/) { SetRunning(false); });
  // Connect to launcher
  env->GetLauncher(launcher_.NewRequest());

#if __Fuchsia_API_level__ < 10
  zx::channel
#else
  fidl::InterfaceRequest<fuchsia::io::Directory>
#endif
      request;
  service_provider_ = sys::ServiceDirectory::CreateWithRequest(&request);
  // Connect to service
  env->GetDirectory(std::move(request));

  env_controller_.events().OnCreated = [this]() { SetRunning(true); };
}

// static
std::unique_ptr<EnclosingEnvironment> EnclosingEnvironment::Create(
    std::string label, const fuchsia::sys::EnvironmentPtr& parent_env,
    std::unique_ptr<EnvironmentServices> services,
    const fuchsia::sys::EnvironmentOptions& options) {
  auto* env = new EnclosingEnvironment(std::move(label), parent_env, std::move(services), options);
  return std::unique_ptr<EnclosingEnvironment>(env);
}

EnclosingEnvironment::~EnclosingEnvironment() {
  auto channel = env_controller_.Unbind();
  if (channel) {
    fuchsia::sys::EnvironmentControllerSyncPtr controller;
    controller.Bind(std::move(channel));
    controller->Kill();
  }
}

void EnclosingEnvironment::Kill(fit::function<void()> callback) {
  env_controller_->Kill([callback = std::move(callback)]() {
    if (callback) {
      callback();
    }
  });
}

std::unique_ptr<EnclosingEnvironment> EnclosingEnvironment::CreateNestedEnclosingEnvironment(
    std::string label) {
  fuchsia::sys::EnvironmentPtr env;
  service_provider_->Connect(env.NewRequest());
  return Create(std::move(label), env, EnvironmentServices::Create(env));
}

void EnclosingEnvironment::CreateComponent(
    fuchsia::sys::LaunchInfo launch_info,
    fidl::InterfaceRequest<fuchsia::sys::ComponentController> request) {
  launcher_.CreateComponent(std::move(launch_info), std::move(request));
}

fuchsia::sys::ComponentControllerPtr EnclosingEnvironment::CreateComponent(
    fuchsia::sys::LaunchInfo launch_info) {
  fuchsia::sys::ComponentControllerPtr controller;
  CreateComponent(std::move(launch_info), controller.NewRequest());
  return controller;
}

fuchsia::sys::ComponentControllerPtr EnclosingEnvironment::CreateComponentFromUrl(
    std::string component_url) {
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = std::move(component_url);

  return CreateComponent(std::move(launch_info));
}

void EnclosingEnvironment::SetRunning(bool running) {
  running_ = running;
  if (running_changed_callback_) {
    running_changed_callback_(running_);
  }
}

}  // namespace testing
}  // namespace sys
