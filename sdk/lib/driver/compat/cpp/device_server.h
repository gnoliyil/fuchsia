// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_DRIVER_COMPAT_CPP_DEVICE_SERVER_H_
#define LIB_DRIVER_COMPAT_CPP_DEVICE_SERVER_H_

#include <fidl/fuchsia.component.decl/cpp/fidl.h>
#include <fidl/fuchsia.driver.compat/cpp/wire.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/driver/compat/cpp/service_offers.h>
#include <lib/driver/outgoing/cpp/handlers.h>
#include <lib/driver/outgoing/cpp/outgoing_directory.h>

namespace compat {

using Metadata = std::vector<uint8_t>;
using MetadataMap = std::unordered_map<uint32_t, const Metadata>;

// The DeviceServer class vends the fuchsia_driver_compat::Device interface.
// It represents a single device.
class DeviceServer : public fidl::WireServer<fuchsia_driver_compat::Device> {
 public:
  struct GenericProtocol {
    void* ops;
    void* ctx;
  };

  using GetBanjoProtoCb = fit::function<zx::result<GenericProtocol>(uint32_t)>;

  DeviceServer() = default;

  DeviceServer(std::string name, uint32_t proto_id, std::string topological_path,
               std::optional<ServiceOffersV1> service_offers = std::nullopt,
               GetBanjoProtoCb get_banjo_protocol = {})
      : name_(std::move(name)),
        topological_path_(std::move(topological_path)),
        proto_id_(proto_id),
        service_offers_(std::move(service_offers)),
        get_banjo_protocol_(std::move(get_banjo_protocol)) {}

  // Functions to implement the DFv1 device API.
  zx_status_t AddMetadata(uint32_t type, const void* data, size_t size);
  zx_status_t GetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual);
  zx_status_t GetMetadataSize(uint32_t type, size_t* out_size);

  // Serve this interface in an outgoing directory.
  zx_status_t Serve(async_dispatcher_t* dispatcher, component::OutgoingDirectory* outgoing);
  zx_status_t Serve(async_dispatcher_t* dispatcher, fdf::OutgoingDirectory* outgoing);

  // Create offers to offer this interface to another component.
  std::vector<fuchsia_component_decl::wire::Offer> CreateOffers(fidl::ArenaBase& arena);
  std::vector<fuchsia_component_decl::Offer> CreateOffers();

  std::string_view name() const { return name_; }
  std::string_view topological_path() const { return topological_path_; }
  uint32_t proto_id() const { return proto_id_; }

 private:
  // fuchsia.driver.compat.Compat
  void GetTopologicalPath(GetTopologicalPathCompleter::Sync& completer) override;
  void GetMetadata(GetMetadataCompleter::Sync& completer) override;
  void GetBanjoProtocol(GetBanjoProtocolRequestView request,
                        GetBanjoProtocolCompleter::Sync& completer) override;

  std::string name_;
  std::string topological_path_;
  uint32_t proto_id_ = 0;
  MetadataMap metadata_;
  std::optional<ServiceOffersV1> service_offers_;
  GetBanjoProtoCb get_banjo_protocol_;

  // This callback is called when the class is destructed and it will stop serving the protocol.
  fit::deferred_callback stop_serving_;
};

}  // namespace compat

#endif  // LIB_DRIVER_COMPAT_CPP_DEVICE_SERVER_H_
