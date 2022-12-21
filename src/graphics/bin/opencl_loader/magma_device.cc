// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/bin/opencl_loader/magma_device.h"

#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fit/thread_checker.h>
#include <lib/syslog/cpp/macros.h>

#include "src/graphics/bin/opencl_loader/app.h"

// static
std::unique_ptr<MagmaDevice> MagmaDevice::Create(LoaderApp* app, int dir_fd, std::string name,
                                                 inspect::Node* parent) {
  std::unique_ptr<MagmaDevice> device(new MagmaDevice(app));
  if (!device->Initialize(dir_fd, name, parent))
    return nullptr;
  return device;
}

bool MagmaDevice::Initialize(int dir_fd, const std::string& name, inspect::Node* parent) {
  FIT_DCHECK_IS_THREAD_VALID(main_thread_);
  node() = parent->CreateChild("magma-" + name);
  icd_list_.Initialize(&node());
  auto pending_action_token = app()->GetPendingActionToken();

  fdio_cpp::UnownedFdioCaller caller(dir_fd);
  zx_status_t status = fdio_service_connect_at(caller.directory().channel()->get(), name.c_str(),
                                               device_.NewRequest().TakeChannel().release());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to connect to service";
    return false;
  }
  device_.set_error_handler([this](zx_status_t status) {
    // Deletes |this|.
    app()->RemoveDevice(this);
  });

  device_->GetIcdList([this, name, pending_action_token = std::move(pending_action_token)](
                          std::vector<fuchsia::gpu::magma::IcdInfo> icd_info) mutable {
    FIT_DCHECK_IS_THREAD_VALID(main_thread_);
    uint32_t i = 0;
    for (auto& icd : icd_info) {
      if (!icd.has_component_url()) {
        FX_LOGS(ERROR) << "ICD missing component URL";
        continue;
      }
      if (!icd.has_flags()) {
        FX_LOGS(ERROR) << "ICD missing flags";
        continue;
      }
      IcdData data;
      data.node = node().CreateChild(std::to_string(i++));
      data.node.RecordString("component_url", icd.component_url());
      data.node.RecordUint("flags", static_cast<uint32_t>(icd.flags()));
      if (icd.flags() & fuchsia::gpu::magma::IcdFlags::SUPPORTS_OPENCL) {
        icd_list_.Add(app()->CreateIcdComponent(icd.component_url()));
      }

      icds().push_back(std::move(data));
    }
  });
  return true;
}
