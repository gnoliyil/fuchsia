// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_NAMESPACE_BUILDER_H_
#define SRC_SYS_APPMGR_NAMESPACE_BUILDER_H_

#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fdio/namespace.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>

#include <vector>

#include <fbl/unique_fd.h>

#include "src/lib/cmx/sandbox.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/pkg_url/fuchsia_pkg_url.h"

namespace component {

class NamespaceBuilder {
 public:
  NamespaceBuilder(fbl::unique_fd dir, const std::string namespace_id)
      : appmgr_config_dir_(std::move(dir)), ns_id(namespace_id) {}
  ~NamespaceBuilder();

  void AddFlatNamespace(fuchsia::sys::FlatNamespacePtr flat_namespace);
  void AddPackage(fidl::InterfaceHandle<fuchsia::io::Directory> package);
  void AddConfigData(const SandboxMetadata& sandbox, const std::string& component_name);
  void AddDirectoryIfNotPresent(const std::string& path,
                                fidl::InterfaceHandle<fuchsia::io::Directory> directory);
  void AddServices(fidl::InterfaceHandle<fuchsia::io::Directory> services);

  // A factory function that returns a new directory that /hub points to.
  using HubDirectoryFactory = fit::function<fidl::InterfaceHandle<fuchsia::io::Directory>()>;
  // A factory function that returns a new path for /data to point to when it
  // should be isolated from other components and realms
  using IsolatedDataPathFactory = fit::function<fpromise::result<std::string, zx_status_t>()>;
  // A factory function that returns a new path for /tmp to point to when it
  // should be isolated from other components and realms
  using IsolatedTempPathFactory = fit::function<fpromise::result<std::string, zx_status_t>()>;
  // Returns a non-ZX_OK status if the sandbox cannot be made.
  [[nodiscard]] zx_status_t AddSandbox(const SandboxMetadata& sandbox,
                                       const HubDirectoryFactory& hub_directory_factory);
  // Returns a non-ZX_OK status if the sandbox cannot be made.
  [[nodiscard]] zx_status_t AddSandbox(const SandboxMetadata& sandbox,
                                       const HubDirectoryFactory& hub_directory_factory,
                                       const IsolatedDataPathFactory& isolated_data_path_factory,
                                       const IsolatedTempPathFactory& isolated_temp_path_factory);

  // Returns an fdio_flat_namespace_t representing the built namespace.
  //
  // The returned fdio_flat_namespace_t has ownership of the zx::channel objects
  // added to the namespace but does not have ownership of the memory for the
  // fdio_flat_namespace_t or the memory pointed to by its |handle|, |type|, or
  // |path| properties. The NamespaceBuilder will free that memory in its
  // destructor.
  //
  // Build() can be called only once for each builder. None of the "add" methods
  // can be called after Build().
  fdio_flat_namespace_t* Build();

  // Similar to Build() but returns a FIDL struct with ownership of all
  // zx:channel that are part of this namespace.
  fuchsia::sys::FlatNamespace BuildForRunner();

 private:
  fbl::unique_fd appmgr_config_dir_;
  std::string ns_id;
  void AddHub(const HubDirectoryFactory& hub_directory_factory);
  void PushDirectoryFromPath(std::string path);
  void PushDirectoryFromPathAs(std::string src_path, std::string dst_path);
  void PushDirectoryFromPathAsWithPermissions(std::string src_path, std::string dst_path,
                                              fuchsia_io::wire::OpenFlags flags);
  void PushDirectoryFromChannel(std::string path,
                                fidl::InterfaceHandle<fuchsia::io::Directory> channel);
  void Release();

  std::vector<zx_handle_t> handles_;
  std::vector<std::string> paths_;

  std::vector<fidl::InterfaceHandle<fuchsia::io::Directory>> handle_pool_;
  std::vector<const char*> path_data_;
  fdio_flat_namespace_t flat_ns_;

  FXL_DISALLOW_COPY_AND_ASSIGN(NamespaceBuilder);
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_NAMESPACE_BUILDER_H_
