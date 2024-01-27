// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/cpp/fidl.h>
#include <lib/sys/cpp/testing/fake_component.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/service.h>

#include <utility>

namespace sys {
namespace testing {

FakeComponent::FakeComponent() : directory_(std::make_unique<vfs::PseudoDir>()) {}

FakeComponent::~FakeComponent() = default;

void FakeComponent::Register(std::string url, FakeLauncher& fake_launcher,
                             async_dispatcher_t* dispatcher) {
  fake_launcher.RegisterComponent(
      std::move(url),
      [this, dispatcher](fuchsia::sys::LaunchInfo launch_info,
                         fidl::InterfaceRequest<fuchsia::sys::ComponentController> ctrl) {
        ctrls_.push_back(std::move(ctrl));
        zx_status_t status = directory_->Serve({},
#if __Fuchsia_API_level__ < 10
                                               std::move(launch_info.directory_request)
#else
                                               launch_info.directory_request.TakeChannel()
#endif
                                                   ,
                                               dispatcher);
        ZX_ASSERT(status == ZX_OK);
      });
}

zx_status_t FakeComponent::AddPublicService(Connector connector, std::string service_name) {
  return directory_->AddEntry(std::move(service_name),
                              std::make_unique<vfs::Service>(std::move(connector)));
}

}  // namespace testing
}  // namespace sys
