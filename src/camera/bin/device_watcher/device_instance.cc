// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/device_watcher/device_instance.h"

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/process/cpp/fidl.h>
#include <lib/sys/service/cpp/service.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>
#include <zircon/processargs.h>

#include "fuchsia/io/cpp/fidl.h"
#include "lib/fit/function.h"
#include "lib/sys/cpp/service_directory.h"

namespace camera {

fpromise::result<std::unique_ptr<DeviceInstance>, zx_status_t> DeviceInstance::Create(
    fuchsia::hardware::camera::DeviceHandle camera, const fuchsia::component::RealmPtr& realm,
    async_dispatcher_t* dispatcher, const std::string& collection_name,
    const std::string& child_name, const std::string& url) {
  auto instance = std::make_unique<DeviceInstance>();
  instance->dispatcher_ = dispatcher;
  instance->name_ = child_name;
  instance->collection_name_ = collection_name;

  // Launch the child device.
  fuchsia::component::decl::CollectionRef collection;
  collection.name = collection_name;
  fuchsia::component::decl::Child child;
  child.set_name(child_name);
  child.set_url(url);
  child.set_startup(fuchsia::component::decl::StartupMode::LAZY);
  fuchsia::component::CreateChildArgs args;

  // Pass the camera DeviceHandle to the child so the child can communicate with the correct
  // instance.
  fuchsia::process::HandleInfo handle_info;
  auto channel = camera.TakeChannel();
  zx::handle handle(channel.release());
  ZX_ASSERT(handle.is_valid());
  handle_info.handle = std::move(handle);
  handle_info.id = PA_HND(PA_USER0, 0);
  std::vector<fuchsia::process::HandleInfo> numbered_handles;
  numbered_handles.push_back(std::move(handle_info));
  args.set_numbered_handles(std::move(numbered_handles));

  fuchsia::component::Realm::CreateChildCallback cb =
      [child_name](fuchsia::component::Realm_CreateChild_Result result) {
        if (result.is_err()) {
          FX_LOGS(ERROR) << "Failed to create camera device child. Result: "
                         << static_cast<long>(result.err());
          ZX_ASSERT(false);  // Should never happen.
        }
        FX_LOGS(INFO) << "Created camera device child: " << child_name;
      };
  realm->CreateChild(std::move(collection), std::move(child), std::move(args), std::move(cb));

  // TODO(b/244178394) - Need to have handlers/callbacks for child component exits or crashes.

  return fpromise::ok(std::move(instance));
}

}  // namespace camera
