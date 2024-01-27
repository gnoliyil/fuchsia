// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_V2_DRIVER_HOST_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_V2_DRIVER_HOST_H_

#include <fidl/fuchsia.driver.host/cpp/fidl.h>
#include <fidl/fuchsia.driver.host/cpp/wire.h>
#include <lib/vfs/cpp/pseudo_dir.h>

#include <fbl/intrusive_double_list.h>

namespace dfv2 {

class DriverHost {
 public:
  using StartCallback = fit::callback<void(zx::result<>)>;
  virtual void Start(fidl::ClientEnd<fuchsia_driver_framework::Node> client_end,
                     std::string node_node,
                     fidl::VectorView<fuchsia_driver_framework::wire::NodeSymbol> symbols,
                     fuchsia_component_runner::wire::ComponentStartInfo start_info,
                     fidl::ServerEnd<fuchsia_driver_host::Driver> driver, StartCallback cb) = 0;

  virtual zx::result<uint64_t> GetProcessKoid() const = 0;
};

class DriverHostComponent final
    : public DriverHost,
      public fbl::DoublyLinkedListable<std::unique_ptr<DriverHostComponent>> {
 public:
  DriverHostComponent(fidl::ClientEnd<fuchsia_driver_host::DriverHost> driver_host,
                      async_dispatcher_t* dispatcher,
                      fbl::DoublyLinkedList<std::unique_ptr<DriverHostComponent>>* driver_hosts);

  void Start(fidl::ClientEnd<fuchsia_driver_framework::Node> client_end, std::string node_name,
             fidl::VectorView<fuchsia_driver_framework::wire::NodeSymbol> symbols,
             fuchsia_component_runner::wire::ComponentStartInfo start_info,
             fidl::ServerEnd<fuchsia_driver_host::Driver> driver, StartCallback cb) override;

  zx::result<fuchsia_driver_host::ProcessInfo> GetProcessInfo() const;
  zx::result<uint64_t> GetProcessKoid() const override;
  zx::result<uint64_t> GetJobKoid() const;

  zx::result<> InstallLoader(fidl::ClientEnd<fuchsia_ldsvc::Loader> loader_client) const;

 private:
  void InitializeElfDir();

  fidl::WireSharedClient<fuchsia_driver_host::DriverHost> driver_host_;
  mutable std::optional<fuchsia_driver_host::ProcessInfo> process_info_;
  vfs::PseudoDir runtime_dir_;
  async_dispatcher_t* dispatcher_;
};

zx::result<> SetEncodedConfig(
    fidl::WireTableBuilder<fuchsia_driver_framework::wire::DriverStartArgs>& args,
    fuchsia_component_runner::wire::ComponentStartInfo& start_info);

}  // namespace dfv2

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_V2_DRIVER_HOST_H_
