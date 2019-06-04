// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/appmgr/namespace_builder.h"

#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <zircon/processargs.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "lib/fsl/io/fd.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/path.h"
#include "src/lib/files/unique_fd.h"

namespace component {

NamespaceBuilder::NamespaceBuilder() = default;

NamespaceBuilder::~NamespaceBuilder() = default;

void NamespaceBuilder::AddFlatNamespace(fuchsia::sys::FlatNamespacePtr ns) {
  if (ns && ns->paths.size() == ns->directories.size()) {
    for (size_t i = 0; i < ns->paths.size(); ++i) {
      AddDirectoryIfNotPresent(ns->paths.at(i),
                               std::move(ns->directories.at(i)));
    }
  }
}

void NamespaceBuilder::AddPackage(zx::channel package) {
  PushDirectoryFromChannel("/pkg", std::move(package));
}

void NamespaceBuilder::AddConfigData(const SandboxMetadata& sandbox, const std::string& pkg_name) {
  for (const auto& feature : sandbox.features()) {
    if (feature == "config-data") {
      PushDirectoryFromPathAs("/pkgfs/packages/config-data/0/data/" + pkg_name,
                              "/config/data");
    }
  }
}

void NamespaceBuilder::AddDirectoryIfNotPresent(const std::string& path,
                                                zx::channel directory) {
  if (std::find(paths_.begin(), paths_.end(), path) != paths_.end())
    return;
  PushDirectoryFromChannel(path, std::move(directory));
}

void NamespaceBuilder::AddServices(zx::channel services) {
  PushDirectoryFromChannel("/svc", std::move(services));
}

void NamespaceBuilder::AddSandbox(
    const SandboxMetadata& sandbox,
    const HubDirectoryFactory& hub_directory_factory) {
  AddSandbox(sandbox, hub_directory_factory, [] {
    FXL_NOTREACHED() << "IsolatedDataPathFactory unexpectedly used";
    return "";
  }, [] {
    FXL_NOTREACHED() << "IsolatedCachePathFactory unexpectedly used";
    return "";
  });
}

void NamespaceBuilder::AddSandbox(
    const SandboxMetadata& sandbox,
    const HubDirectoryFactory& hub_directory_factory,
    const IsolatedDataPathFactory& isolated_data_path_factory,
    const IsolatedCachePathFactory& isolated_cache_path_factory) {
  for (const auto& path : sandbox.dev()) {
    if (path == "class") {
      FXL_LOG(WARNING) << "Ignoring request for all device classes";
      continue;
    }
    PushDirectoryFromPath("/dev/" + path);
  }

  for (const auto& path : sandbox.system())
    PushDirectoryFromPath("/system/" + path);

  for (const auto& path : sandbox.pkgfs())
    PushDirectoryFromPath("/pkgfs/" + path);

  // Prioritize isolated persistent storage over shell feature, if both are
  // present.
  if (sandbox.HasFeature("isolated-persistent-storage")) {
    PushDirectoryFromPathAs(isolated_data_path_factory(), "/data");
  }

  if (sandbox.HasFeature("deprecated-misc-storage")) {
    const std::string dataDir = "/data/misc";
    if (files::CreateDirectory(dataDir)) {
      PushDirectoryFromPathAs(dataDir, "/misc");
    } else {
      FXL_LOG(ERROR) << "Failed to create deprecated-misc-storage directory";
    }
  }

  if (sandbox.HasFeature("isolated-cache-storage")) {
    PushDirectoryFromPathAs(isolated_cache_path_factory(), "/cache");
  }

  for (const auto& feature : sandbox.features()) {
    if (feature == "build-info") {
      PushDirectoryFromPathAs("/pkgfs/packages/build-info/0/data",
                              "/config/build-info");
    } else if (feature == "root-ssl-certificates" || feature == "shell" ||
               feature == "deprecated-shell") {
      // "shell" implies "root-ssl-certificates"
      PushDirectoryFromPathAs("/pkgfs/packages/root_ssl_certificates/0/data",
                              "/config/ssl");

      if (feature == "shell" || feature == "deprecated-shell") {
        // TODO(abarth): These permissions should depend on the envionment
        // in some way so that a shell running at a user-level scope doesn't
        // have access to all the device drivers and such.
        PushDirectoryFromPathAs("/pkgfs/packages/shell-commands/0/bin", "/bin");
        PushDirectoryFromPath("/blob");
        PushDirectoryFromPath("/boot");
        // Note that if the 'isolated-persistent-storage' feature is present,
        // this is a no-op since it has handled first above.
        // PushDirectoryFromPath does not override existing directories.
        PushDirectoryFromPath("/data");
        PushDirectoryFromPath("/dev");
        PushDirectoryFromChannel("/hub", hub_directory_factory());
        PushDirectoryFromPath("/install");
        PushDirectoryFromPath("/pkgfs");
        PushDirectoryFromPath("/system");
        PushDirectoryFromPath("/tmp");
        PushDirectoryFromPath("/volume");
      }
    } else if (feature == "shell-commands") {
      PushDirectoryFromPathAs("/pkgfs/packages/shell-commands/0/bin", "/bin");
    } else if (feature == "system-temp") {
      PushDirectoryFromPath("/tmp");
    } else if (feature == "vulkan") {
      PushDirectoryFromPath("/dev/class/goldfish-address-space");
      PushDirectoryFromPath("/dev/class/goldfish-control");
      PushDirectoryFromPath("/dev/class/goldfish-pipe");
      PushDirectoryFromPath("/dev/class/gpu");
      PushDirectoryFromPathAs(
        "/pkgfs/packages/config-data/0/data/vulkan-icd/icd.d",
        "/config/vulkan/icd.d");
    }
  }

  for (const auto& path : sandbox.boot())
    PushDirectoryFromPath("/boot/" + path);
}

void NamespaceBuilder::PushDirectoryFromPath(std::string path) {
  PushDirectoryFromPathAs(path, path);
}

void NamespaceBuilder::PushDirectoryFromPathAs(std::string src_path,
                                               std::string dst_path) {
  if (std::find(paths_.begin(), paths_.end(), dst_path) != paths_.end())
    return;
  fxl::UniqueFD dir(open(src_path.c_str(), O_DIRECTORY | O_RDONLY));
  if (!dir.is_valid())
    return;
  zx::channel handle = fsl::CloneChannelFromFileDescriptor(dir.get());
  if (!handle) {
    FXL_DLOG(WARNING) << "Failed to clone channel for " << src_path;
    return;
  }
  PushDirectoryFromChannel(std::move(dst_path), std::move(handle));
}

void NamespaceBuilder::PushDirectoryFromChannel(std::string path,
                                                zx::channel channel) {
  FXL_DCHECK(std::find(paths_.begin(), paths_.end(), path) == paths_.end());
  types_.push_back(PA_HND(PA_NS_DIR, types_.size()));
  handles_.push_back(channel.get());
  paths_.push_back(std::move(path));

  handle_pool_.push_back(std::move(channel));
}

fdio_flat_namespace_t* NamespaceBuilder::Build() {
  path_data_.resize(paths_.size());
  for (size_t i = 0; i < paths_.size(); ++i)
    path_data_[i] = paths_[i].c_str();

  flat_ns_.count = types_.size();
  flat_ns_.handle = handles_.data();
  flat_ns_.type = types_.data();
  flat_ns_.path = path_data_.data();
  Release();
  return &flat_ns_;
}

fuchsia::sys::FlatNamespace NamespaceBuilder::BuildForRunner() {
  fuchsia::sys::FlatNamespace flat_namespace;
  flat_namespace.paths.clear();
  flat_namespace.directories.clear();

  for (auto& path : paths_) {
    flat_namespace.paths.push_back(std::move(path));
  }

  for (auto& handle : handle_pool_) {
    flat_namespace.directories.push_back(std::move(handle));
  }
  return flat_namespace;
}

void NamespaceBuilder::Release() {
  for (auto& handle : handle_pool_)
    (void)handle.release();
  handle_pool_.clear();
}

}  // namespace component
