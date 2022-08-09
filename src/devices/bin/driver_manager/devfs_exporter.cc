// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/devfs_exporter.h"

#include "src/devices/lib/log/log.h"
#include "src/lib/storage/vfs/cpp/service.h"

namespace fdfs = fuchsia_device_fs;

namespace driver_manager {

zx::status<std::unique_ptr<ExportWatcher>> ExportWatcher::Create(
    async_dispatcher_t* dispatcher, Devnode* root,
    fidl::ClientEnd<fuchsia_io::Directory> service_dir, std::string_view service_path,
    std::string_view devfs_path, uint32_t protocol_id,
    fuchsia_device_fs::wire::ExportOptions options) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Node>();
  if (endpoints.is_error()) {
    return zx::error(endpoints.error_value());
  }

  auto response =
      fidl::WireCall(service_dir)
          ->Open(fuchsia_io::wire::OpenFlags::kRightReadable |
                     fuchsia_io::wire::OpenFlags::kRightWritable,
                 0, fidl::StringView::FromExternal(service_path), std::move(endpoints->server));
  if (!response.ok()) {
    return zx::error(response.error().status());
  }

  auto watcher = std::make_unique<ExportWatcher>();
  watcher->devfs_path_ = std::string(devfs_path);
  watcher->client_ = fidl::WireClient(std::move(endpoints->client), dispatcher, watcher.get());

  zx_status_t status = devfs_export(root, std::move(service_dir), service_path, devfs_path,
                                    protocol_id, options, watcher->devnodes_);
  if (status != ZX_OK) {
    return zx::error(status);
  }

  return zx::ok(std::move(watcher));
}

zx_status_t ExportWatcher::MakeVisible() {
  for (auto& node : devnodes_) {
    if (node->service_options != fuchsia_device_fs::wire::ExportOptions::kInvisible) {
      return ZX_ERR_BAD_STATE;
    }
    node->service_options &= ~fuchsia_device_fs::wire::ExportOptions::kInvisible;
    devfs_notify(node->parent, node->name, fuchsia_io::wire::WatchEvent::kAdded);
  }

  return ZX_OK;
}

DevfsExporter::DevfsExporter(Devnode* root, async_dispatcher_t* dispatcher)
    : root_(root), dispatcher_(dispatcher) {}

zx::status<> DevfsExporter::PublishExporter(const fbl::RefPtr<fs::PseudoDir>& svc_dir) {
  const auto service = [this](fidl::ServerEnd<fdfs::Exporter> request) {
    fidl::BindServer(dispatcher_, std::move(request), this);
    return ZX_OK;
  };
  zx_status_t status = svc_dir->AddEntry(fidl::DiscoverableProtocolName<fdfs::Exporter>,
                                         fbl::MakeRefCounted<fs::Service>(service));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to add directory entry '%s': %s",
         fidl::DiscoverableProtocolName<fdfs::Exporter>, zx_status_get_string(status));
  }
  return zx::make_status(status);
}

void DevfsExporter::Export(ExportRequestView request, ExportCompleter::Sync& completer) {
  auto result = ExportWatcher::Create(
      dispatcher_, root_, std::move(request->service_dir), request->service_path.get(),
      request->devfs_path.get(), request->protocol_id, fuchsia_device_fs::wire::ExportOptions());
  if (result.is_error()) {
    LOGF(ERROR, "Failed to export service to devfs path \"%.*s\": %s",
         static_cast<int>(request->devfs_path.size()), request->devfs_path.data(),
         result.status_string());
    completer.ReplyError(result.error_value());
    return;
  }

  ExportWatcher* export_ptr = result.value().get();
  exports_.push_back(std::move(result.value()));

  // Set a callback so when the ExportWatcher sees its connection closed, it
  // will delete itself and its devnodes.
  export_ptr->set_on_close_callback([this, export_ptr]() {
    exports_.erase(std::remove_if(exports_.begin(), exports_.end(),
                                  [&](const auto& it) { return it.get() == export_ptr; }),
                   exports_.end());
  });

  completer.ReplySuccess();
}

void DevfsExporter::ExportOptions(ExportOptionsRequestView request,
                                  ExportOptionsCompleter::Sync& completer) {
  auto result = ExportWatcher::Create(dispatcher_, root_, std::move(request->service_dir),
                                      request->service_path.get(), request->devfs_path.get(),
                                      request->protocol_id, request->options);
  if (result.is_error()) {
    LOGF(ERROR, "Failed to export service to devfs path \"%.*s\": %s",
         static_cast<int>(request->devfs_path.size()), request->devfs_path.data(),
         result.status_string());
    completer.ReplyError(result.error_value());
    return;
  }

  ExportWatcher* export_ptr = result.value().get();
  exports_.push_back(std::move(result.value()));

  // Set a callback so when the ExportWatcher sees its connection closed, it
  // will delete itself and its devnodes.
  export_ptr->set_on_close_callback([this, export_ptr]() {
    exports_.erase(std::remove_if(exports_.begin(), exports_.end(),
                                  [&](const auto& it) { return it.get() == export_ptr; }),
                   exports_.end());
  });

  completer.ReplySuccess();
}

void DevfsExporter::MakeVisible(MakeVisibleRequestView request,
                                MakeVisibleCompleter::Sync& completer) {
  for (auto& e : exports_) {
    if (e->devfs_path().compare(request->devfs_path.get()) != 0) {
      continue;
    }
    zx_status_t status = e->MakeVisible();
    if (status != ZX_OK) {
      completer.ReplyError(status);
      return;
    }
    completer.ReplySuccess();
    return;
  }
  completer.ReplyError(ZX_ERR_NOT_FOUND);
}

}  // namespace driver_manager
