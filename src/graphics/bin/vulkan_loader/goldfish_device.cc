// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/bin/vulkan_loader/goldfish_device.h"

#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <lib/component/incoming/cpp/service_client.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <lib/fdio/unsafe.h>
#include <lib/syslog/cpp/macros.h>

#include "src/graphics/bin/vulkan_loader/app.h"

// static
std::unique_ptr<GoldfishDevice> GoldfishDevice::Create(LoaderApp* app, int dir_fd, std::string name,
                                                       inspect::Node* parent) {
  std::unique_ptr<GoldfishDevice> device(new GoldfishDevice(app));
  if (!device->Initialize(dir_fd, name, parent))
    return nullptr;
  return device;
}

bool GoldfishDevice::Initialize(int dir_fd, std::string name, inspect::Node* parent) {
  node() = parent->CreateChild("goldfish-" + name);
  icd_list_.Initialize(&node());
  auto pending_action_token = app()->GetPendingActionToken();

  fdio_cpp::UnownedFdioCaller caller(dir_fd);
  zx::result controller =
      component::ConnectAt<fuchsia_hardware_goldfish::Controller>(caller.directory(), name);
  if (controller.is_error()) {
    FX_PLOGS(ERROR, controller.error_value()) << "Failed to connect to service";
    return false;
  }

  if (fidl::Status status =
          fidl::WireCall(controller.value())
              ->OpenSession(fidl::ServerEnd<fuchsia_hardware_goldfish::PipeDevice>(
                  device_.NewRequest().TakeChannel()));
      !status.ok()) {
    FX_PLOGS(ERROR, status.status()) << "Failed to open session";
    return false;
  }
  device_.set_error_handler([this](zx_status_t status) {
    // Deletes |this|.
    app()->RemoveDevice(this);
  });

  auto data = node().CreateChild("0");
  std::string component_url = "fuchsia-pkg://fuchsia.com/libvulkan_goldfish#meta/vulkan.cm";
  data.RecordString("component_url", component_url);

  icd_list_.Add(app()->CreateIcdComponent(component_url));
  icds().push_back(std::move(data));
  return true;
}
