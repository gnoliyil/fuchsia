// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/misc/drivers/compat/device.h"

#include <fidl/fuchsia.device.composite/cpp/wire.h>
#include <fidl/fuchsia.driver.framework/cpp/wire_types.h>
#include <lib/async/cpp/task.h>
#include <lib/ddk/binding_priv.h>
#include <lib/driver/compat/cpp/symbols.h>
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/fdf/cpp/protocol.h>
#include <lib/fit/defer.h>
#include <lib/fpromise/bridge.h>
#include <lib/stdcompat/span.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>

#include "driver.h"
#include "src/devices/bin/driver_host/node_group_desc_util.h"
#include "src/devices/misc/drivers/compat/composite.h"

namespace fdf {
using namespace fuchsia_driver_framework;
}
namespace fcd = fuchsia_component_decl;

namespace {

std::optional<fdf::wire::NodeProperty> fidl_offer_to_device_prop(fidl::AnyArena& arena,
                                                                 const char* fidl_offer) {
  static const std::unordered_map<std::string_view, uint32_t> kPropMap = {
#define DDK_FIDL_PROTOCOL_DEF(tag, val, name) \
  {                                           \
      name,                                   \
      val,                                    \
  },
#include <lib/ddk/fidl-protodefs.h>
  };

  auto prop = kPropMap.find(fidl_offer);
  if (prop == kPropMap.end()) {
    return std::nullopt;
  }

  auto& [key, value] = *prop;
  return fdf::MakeProperty(arena, BIND_FIDL_PROTOCOL, value);
}

// Makes a valid name. This must be a valid component framework instance name.
std::string MakeValidName(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (auto ch : name) {
    switch (ch) {
      case ':':
      case '.':
        out.push_back('_');
        break;
      default:
        out.push_back(ch);
    }
  }
  return out;
}

template <typename T>
bool HasOp(const zx_protocol_device_t* ops, T member) {
  return ops != nullptr && ops->*member != nullptr;
}

std::vector<std::string> MakeServiceOffers(device_add_args_t* zx_args) {
  std::vector<std::string> offers;
  for (const auto& offer :
       cpp20::span(zx_args->fidl_service_offers, zx_args->fidl_service_offer_count)) {
    offers.push_back(std::string(offer));
  }
  for (const auto& offer :
       cpp20::span(zx_args->runtime_service_offers, zx_args->runtime_service_offer_count)) {
    offers.push_back(std::string(offer));
  }
  return offers;
}

}  // namespace

namespace compat {

std::vector<fuchsia_driver_framework::wire::NodeProperty> CreateProperties(
    fidl::AnyArena& arena, fdf::Logger& logger, device_add_args_t* zx_args) {
  std::vector<fuchsia_driver_framework::wire::NodeProperty> properties;
  properties.reserve(zx_args->prop_count + zx_args->str_prop_count +
                     zx_args->fidl_protocol_offer_count + 1);
  bool has_protocol = false;
  for (auto [id, _, value] : cpp20::span(zx_args->props, zx_args->prop_count)) {
    properties.emplace_back(fdf::MakeProperty(arena, id, value));
    if (id == BIND_PROTOCOL) {
      has_protocol = true;
    }
  }

  for (auto [key, value] : cpp20::span(zx_args->str_props, zx_args->str_prop_count)) {
    switch (value.data_type) {
      case ZX_DEVICE_PROPERTY_VALUE_BOOL:
        properties.emplace_back(fdf::MakeProperty(arena, key, value.data.bool_val));
        break;
      case ZX_DEVICE_PROPERTY_VALUE_STRING:
        properties.emplace_back(fdf::MakeProperty(arena, key, value.data.str_val));
        break;
      case ZX_DEVICE_PROPERTY_VALUE_INT:
        properties.emplace_back(fdf::MakeProperty(arena, key, value.data.int_val));
        break;
      case ZX_DEVICE_PROPERTY_VALUE_ENUM:
        properties.emplace_back(fdf::MakeProperty(arena, key, value.data.enum_val));
        break;
      default:
        FDF_LOGL(ERROR, logger, "Unsupported property type, key: %s", key);
        break;
    }
  }

  for (auto value :
       cpp20::span(zx_args->fidl_protocol_offers, zx_args->fidl_protocol_offer_count)) {
    properties.emplace_back(
        fdf::MakeProperty(arena, value, std::string(value) + ".ZirconTransport"));

    auto property = fidl_offer_to_device_prop(arena, value);
    if (property) {
      properties.push_back(*property);
    }
  }

  for (auto value : cpp20::span(zx_args->fidl_service_offers, zx_args->fidl_service_offer_count)) {
    properties.emplace_back(
        fdf::MakeProperty(arena, value, std::string(value) + ".ZirconTransport"));

    auto property = fidl_offer_to_device_prop(arena, value);
    if (property) {
      properties.push_back(*property);
    }
  }

  for (auto value :
       cpp20::span(zx_args->runtime_service_offers, zx_args->runtime_service_offer_count)) {
    properties.emplace_back(
        fdf::MakeProperty(arena, value, std::string(value) + ".DriverTransport"));

    auto property = fidl_offer_to_device_prop(arena, value);
    if (property) {
      properties.push_back(*property);
    }
  }

  // Some DFv1 devices expect to be able to set their own protocol, without specifying proto_id.
  // If we see a BIND_PROTOCOL property, don't add our own.
  if (!has_protocol) {
    // If we do not have a protocol id, set it to MISC to match DFv1 behavior.
    uint32_t proto_id = zx_args->proto_id == 0 ? ZX_PROTOCOL_MISC : zx_args->proto_id;
    properties.emplace_back(fdf::MakeProperty(arena, BIND_PROTOCOL, proto_id));
  }
  return properties;
}

Device::Device(device_t device, const zx_protocol_device_t* ops, Driver* driver,
               std::optional<Device*> parent, fdf::Logger* logger, async_dispatcher_t* dispatcher)
    : devfs_server_(*this, dispatcher),
      name_(device.name),
      logger_(logger),
      dispatcher_(dispatcher),
      driver_(driver),
      compat_symbol_(device),
      ops_(ops),
      parent_(parent),
      executor_(dispatcher) {}

Device::~Device() {
  // Free the dev node first so that nothing will call into the device as it's being destructed
  // further.
  if (devfs_server_auto_free_) {
    devfs_server_auto_free_.call();
  }

  // We only shut down the devices that have a parent, since that means that *this* compat driver
  // owns the device. If the device does not have a parent, then ops_ belongs to another driver, and
  // it's that driver's responsibility to be shut down.
  if (parent_) {
    // Call the parent's pre-release.
    if (HasOp((*parent_)->ops_, &zx_protocol_device_t::child_pre_release)) {
      (*parent_)->ops_->child_pre_release((*parent_)->compat_symbol_.context,
                                          compat_symbol_.context);
    }

    if (HasOp(ops_, &zx_protocol_device_t::release)) {
      ops_->release(compat_symbol_.context);
    }
  }

  for (auto& completer : remove_completers_) {
    completer.complete_ok();
  }
}

zx_device_t* Device::ZxDevice() { return static_cast<zx_device_t*>(this); }

void Device::Bind(fidl::WireSharedClient<fdf::Node> node) { node_ = std::move(node); }

void Device::Unbind() {
  // This closes the client-end of the node to signal to the driver framework
  // that node should be removed.
  //
  // `fidl::WireClient` does not provide a direct way to unbind a client, so we
  // assign a default client to unbind the existing client.
  node_ = {};
}

fpromise::promise<void> Device::UnbindOp() {
  ZX_ASSERT_MSG(!unbind_completer_, "Cannot call UnbindOp twice");
  fpromise::bridge<void> finished_bridge;
  unbind_completer_ = std::move(finished_bridge.completer);

  // If we are being unbound we have to remove all of our children first.
  return RemoveChildren().then(
      [this, bridge = std::move(finished_bridge)](fpromise::result<>& result) mutable {
        // We don't call unbind on the root parent device because it belongs to another driver.
        // We find the root parent device because it does not have parent_ set.
        if (parent_.has_value() && HasOp(ops_, &zx_protocol_device_t::unbind)) {
          // CompleteUnbind will be called from |device_unbind_reply|.
          ops_->unbind(compat_symbol_.context);
        } else {
          CompleteUnbind();
        }
        return bridge.consumer.promise();
      });
}

void Device::CompleteUnbind() {
  // Remove ourself from devfs.
  if (devfs_server_auto_free_) {
    devfs_server_auto_free_.call();
  }

  // Our unbind is finished, so close all outstanding connections to devfs clients.
  devfs_server_.CloseAllConnections([this]() {
    // Now call our unbind completer.
    ZX_ASSERT(unbind_completer_);
    unbind_completer_.complete_ok();
  });
}

const char* Device::Name() const { return name_.data(); }

bool Device::HasChildren() const { return !children_.empty(); }

zx_status_t Device::Add(device_add_args_t* zx_args, zx_device_t** out) {
  device_t compat_device = {
      .proto_ops =
          {
              .ops = zx_args->proto_ops,
              .id = zx_args->proto_id,
          },
      .name = zx_args->name,
      .context = zx_args->ctx,
  };

  auto device =
      std::make_shared<Device>(compat_device, zx_args->ops, driver_, this, logger_, dispatcher_);
  // Update the compat symbol name pointer with a pointer the device owns.
  device->compat_symbol_.name = device->name_.data();

  device->topological_path_ = topological_path_;
  if (!device->topological_path_.empty()) {
    device->topological_path_ += "/";
  }
  device->topological_path_ += device->name_;

  if (driver()) {
    device->device_id_ = driver()->GetNextDeviceId();
  }

  auto outgoing_name = device->OutgoingName();

  std::optional<ServiceOffersV1> service_offers;
  if (zx_args->outgoing_dir_channel != ZX_HANDLE_INVALID) {
    service_offers = ServiceOffersV1(
        outgoing_name,
        fidl::ClientEnd<fuchsia_io::Directory>(zx::channel(zx_args->outgoing_dir_channel)),
        MakeServiceOffers(zx_args));

  } else if (HasOp(device->ops_, &zx_protocol_device_t::service_connect)) {
    // To support driver runtime protocol discovery, we need to implement the |RuntimeConnector|
    // protocol which will call the device's |service_connect| op.
    auto client_end = device->ServeRuntimeConnectorProtocol();
    if (client_end.is_error()) {
      return client_end.status_value();
    }
    service_offers = ServiceOffersV1(outgoing_name, std::move(*client_end), {});
  }

  if (zx_args->inspect_vmo != ZX_HANDLE_INVALID) {
    zx_status_t status = device->ServeInspectVmo(zx::vmo(zx_args->inspect_vmo));
    if (status != ZX_OK) {
      return status;
    }
  }

  device->device_server_ = DeviceServer(outgoing_name, zx_args->proto_id, device->topological_path_,
                                        std::move(service_offers));

  // Add the metadata from add_args:
  for (size_t i = 0; i < zx_args->metadata_count; i++) {
    auto status =
        device->AddMetadata(zx_args->metadata_list[i].type, zx_args->metadata_list[i].data,
                            zx_args->metadata_list[i].length);
    if (status != ZX_OK) {
      return status;
    }
  }

  device->properties_ = CreateProperties(arena_, *logger_, zx_args);
  device->device_flags_ = zx_args->flags;

  bool has_init = HasOp(device->ops_, &zx_protocol_device_t::init);
  if (!has_init) {
    device->InitReply(ZX_OK);
  }

  if (out) {
    *out = device->ZxDevice();
  }
  children_.push_back(std::move(device));
  return ZX_OK;
}

fpromise::promise<void, zx_status_t> Device::Export() {
  auto dev_vnode_name = OutgoingName();

  zx_status_t status = device_server_.Serve(dispatcher_, &driver()->outgoing());
  if (status != ZX_OK) {
    FDF_LOG(INFO, "Device %s failed to add to outgoing directory: %s", topological_path_.c_str(),
            zx_status_get_string(status));
    return fpromise::make_error_promise(status);
  }

  bool has_init = HasOp(ops_, &zx_protocol_device_t::init);
  auto options = fuchsia_device_fs::wire::ExportOptions();
  if (has_init) {
    options |= fuchsia_device_fs::wire::ExportOptions::kInvisible;
  }

  auto devfs_status = driver()->ExportToDevfsSync(options, devfs_server_, dev_vnode_name,
                                                  topological_path_, device_server_.proto_id());
  if (devfs_status.is_error()) {
    FDF_LOG(INFO, "Device %s failed to add to devfs: %s", topological_path_.c_str(),
            devfs_status.status_string());
    return fpromise::make_error_promise(devfs_status.status_value());
  }
  devfs_server_auto_free_ = std::move(*devfs_status);

  // TODO(fxdebug.dev/90735): When DriverDevelopment works in DFv2, don't print
  // this.
  FDF_LOG(DEBUG, "Created /dev/%s", topological_path().data());

  // If the device is non-bindable we want to create the node now. This lets the driver
  // immediately create more children once we return.
  if (device_flags_ & DEVICE_ADD_NON_BINDABLE) {
    status = CreateNode();
    if (status != ZX_OK) {
      FDF_LOG(INFO, "Device %s failed to create NON_BINDABLE node: %s", topological_path_.c_str(),
              zx_status_get_string(status));
      return fpromise::make_error_promise(status);
    }
  }

  // Wait for the device to initialize, then export to dev, then
  // create the device's Node.
  return fpromise::make_promise([has_init, this]() {
           // Emulate fuchsia.device.manager.DeviceController behaviour, and run the
           // init task after adding the device.
           if (has_init) {
             ops_->init(compat_symbol_.context);
           }
           return fpromise::make_result_promise<void, zx_status_t>(fpromise::ok());
         })
      .and_then(WaitForInitToComplete())
      .and_then([has_init, this]() {
        // Make the device visible if it has an init function.
        if (has_init) {
          auto status = driver()->devfs_exporter().exporter().sync()->MakeVisible(
              fidl::StringView::FromExternal(topological_path()));
          if (status->is_error()) {
            return fpromise::make_error_promise(status->error_value());
          }
        }

        // Create the node now that we are initialized.
        // If we were non bindable, we would've made the node earlier.
        if (!(device_flags_ & DEVICE_ADD_NON_BINDABLE)) {
          zx_status_t status = CreateNode();
          if (status != ZX_OK) {
            FDF_LOG(ERROR, "Failed to CreateNode for device: %s: %s", Name(),
                    zx_status_get_string(status));
            return fpromise::make_error_promise(status);
          }
        }

        return fpromise::make_result_promise(fpromise::result<void, zx_status_t>());
      })
      .or_else([this](const zx_status_t& status) {
        FDF_LOG(ERROR, "Failed to export /dev/%s to devfs: %s", topological_path().data(),
                zx_status_get_string(status));
        Remove();
        return fpromise::make_error_promise(status);
      })
      .wrap_with(scope());
}

zx_status_t Device::CreateNode() {
  // Create NodeAddArgs from `zx_args`.
  fidl::Arena arena;

  auto offers = device_server_.CreateOffers(arena);

  std::vector<fdf::wire::NodeSymbol> symbols;
  symbols.emplace_back(fdf::wire::NodeSymbol::Builder(arena)
                           .name(kDeviceSymbol)
                           .address(reinterpret_cast<uint64_t>(&compat_symbol_))
                           .Build());
  symbols.emplace_back(fdf::wire::NodeSymbol::Builder(arena)
                           .name(kOps)
                           .address(reinterpret_cast<uint64_t>(ops_))
                           .Build());

  auto valid_name = MakeValidName(name_);
  auto args =
      fdf::wire::NodeAddArgs::Builder(arena)
          .name(fidl::StringView::FromExternal(valid_name))
          .symbols(fidl::VectorView<fdf::wire::NodeSymbol>::FromExternal(symbols))
          .offers(fidl::VectorView<fcd::wire::Offer>::FromExternal(offers.data(), offers.size()))
          .properties(fidl::VectorView<fdf::wire::NodeProperty>::FromExternal(properties_))
          .Build();

  // Create NodeController, so we can control the device.
  auto controller_ends = fidl::CreateEndpoints<fdf::NodeController>();
  if (controller_ends.is_error()) {
    return controller_ends.status_value();
  }

  fpromise::bridge<> teardown_bridge;
  controller_teardown_finished_.emplace(teardown_bridge.consumer.promise());
  controller_.Bind(
      std::move(controller_ends->client), dispatcher_,
      fidl::ObserveTeardown([device = weak_from_this(),
                             completer = std::move(teardown_bridge.completer)]() mutable {
        // Because the dispatcher can be multi-threaded, we must use a
        // `fidl::WireSharedClient`. The `fidl::WireSharedClient` uses a
        // two-phase destruction to teardown the client.
        //
        // Because of this, the teardown might be happening after the
        // Device has already been erased. This is likely to occur if the
        // Driver is asked to shutdown. If that happens, the Driver will
        // free its Devices, the Device will release its NodeController,
        // and then this shutdown will occur later. In order to not have a
        // Use-After-Free here, only try to remove the Device if the
        // weak_ptr still exists.
        //
        // The weak pointer will be valid here if the NodeController
        // representing the Device exits on its own. This represents the
        // Device's child Driver exiting, and in that instance we want to
        // Remove the Device.
        if (auto ptr = device.lock()) {
          ptr->controller_ = {};
          if (ptr->pending_removal_) {
            // TODO(fxbug.dev/100470): We currently do not remove the DFv1 child
            // if the NodeController is removed but the driver didn't asked to be
            // removed. We need to investigate the correct behavior here.
            FDF_LOGL(INFO, ptr->logger(), "Device %s has its NodeController unexpectedly removed",
                     (ptr)->topological_path_.data());
          }
          // Only remove us if the driver requested it (normally via device_async_remove)
          if (ptr->pending_removal_ && !ptr->pending_rebind_) {
            ptr->UnbindAndRelease();
          }
        }
        completer.complete_ok();
      }));

  // If the node is not bindable, we own the node.
  fidl::ServerEnd<fdf::Node> node_server;
  if ((device_flags_ & DEVICE_ADD_NON_BINDABLE) != 0) {
    auto node_ends = fidl::CreateEndpoints<fdf::Node>();
    if (node_ends.is_error()) {
      return node_ends.status_value();
    }
    node_.Bind(std::move(node_ends->client), dispatcher_);
    node_server = std::move(node_ends->server);
  }

  // Add the device node.
  if (!(*parent_)->node_.is_valid()) {
    FDF_LOG(ERROR, "Cannot add device, as parent '%s' is not marked NON_BINDABLE.",
            (*parent_)->topological_path_.data());
    return ZX_ERR_NOT_SUPPORTED;
  }

  fpromise::bridge<void, std::variant<zx_status_t, fdf::NodeError>> bridge;
  auto callback = [completer = std::move(bridge.completer)](
                      fidl::WireUnownedResult<fdf::Node::AddChild>& result) mutable {
    if (!result.ok()) {
      completer.complete_error(result.error().status());
      return;
    }
    if (result->is_error()) {
      completer.complete_error(result->error_value());
      return;
    }
    completer.complete_ok();
  };
  (*parent_)
      ->node_->AddChild(args, std::move(controller_ends->server), std::move(node_server))
      .ThenExactlyOnce(std::move(callback));

  auto task =
      bridge.consumer.promise()
          .or_else([this](std::variant<zx_status_t, fdf::NodeError>& status) {
            if (auto error = std::get_if<zx_status_t>(&status); error) {
              if (*error == ZX_ERR_PEER_CLOSED) {
                // This is a warning because it can happen during shutdown.
                FDF_LOG(WARNING, "%s: Node channel closed while adding device", Name());
              } else {
                FDF_LOG(ERROR, "Failed to add device: %s: status: %s", Name(),
                        zx_status_get_string(*error));
              }
            } else if (auto error = std::get_if<fdf::NodeError>(&status); error) {
              if (*error == fdf::NodeError::kNodeRemoved) {
                // This is a warning because it can happen if the parent driver is unbound while we
                // are still setting up.
                FDF_LOG(WARNING, "Failed to add device '%s' while parent was removed", Name());
              } else {
                FDF_LOG(ERROR, "Failed to add device: NodeError: '%s': %u", Name(), *error);
              }
            }
          })
          .wrap_with(scope_);
  executor_.schedule_task(std::move(task));
  return ZX_OK;
}

fpromise::promise<void> Device::RemoveChildren() {
  std::vector<fpromise::promise<void>> promises;
  for (auto& child : children_) {
    promises.push_back(child->Remove());
  }
  return fpromise::join_promise_vector(std::move(promises))
      .then([](fpromise::result<std::vector<fpromise::result<void>>>& results) {
        if (results.is_error()) {
          return fpromise::make_error_promise();
        }
        for (auto& result : results.value()) {
          if (result.is_error()) {
            return fpromise::make_error_promise();
          }
        }
        return fpromise::make_ok_promise();
      });
}

fpromise::promise<void> Device::Remove() {
  fpromise::bridge<void> finished_bridge;
  remove_completers_.push_back(std::move(finished_bridge.completer));

  // If we don't have a controller, return early.
  // We are probably in a state where we are waiting for the controller to finish being removed.
  if (!controller_) {
    if (!pending_removal_) {
      // Our controller is already gone but we weren't in a removal, so manually remove ourself now.
      pending_removal_ = true;
      UnbindAndRelease();
    }
    return finished_bridge.consumer.promise();
  }

  pending_removal_ = true;
  auto result = controller_->Remove();
  // If we hit an error calling remove, we should log it.
  // We don't need to log if the error is that we cannot connect
  // to the protocol, because that means we are already in the process
  // of shutting down.
  if (!result.ok() && !result.is_peer_closed() && !result.is_canceled()) {
    FDF_LOG(ERROR, "Failed to remove device '%s': %s", Name(), result.FormatDescription().data());
  }
  return finished_bridge.consumer.promise();
}

void Device::UnbindAndRelease() {
  ZX_ASSERT_MSG(parent_.has_value(), "UnbindAndRelease called without a parent_: %s",
                topological_path_.c_str());

  // We schedule our removal on our parent's executor because we can't be removed
  // while being run in a promise on our own executor.
  parent_.value()->executor_.schedule_task(
      WaitForInitToComplete()
          .then([device = shared_from_this()](fpromise::result<void, zx_status_t>& init) {
            return device->UnbindOp();
          })
          .then([device = shared_from_this()](fpromise::result<void>& init) {
            // Our device should be destructed at the end of this callback when the reference to the
            // shared pointer is removed.
            device->parent_.value()->children_.remove(device);
          }));
}

void Device::InsertOrUpdateProperty(fuchsia_driver_framework::wire::NodePropertyKey key,
                                    fuchsia_driver_framework::wire::NodePropertyValue value) {
  bool found = false;
  for (auto& prop : properties_) {
    if (!prop.has_key()) {
      continue;
    }

    if (prop.key().Which() != key.Which()) {
      continue;
    }

    if (key.is_string_value()) {
      std::string_view prop_key_view(prop.key().string_value().data(),
                                     prop.key().string_value().size());
      std::string_view key_view(key.string_value().data(), key.string_value().size());
      if (key_view == prop_key_view) {
        found = true;
      }
    } else if (key.is_int_value()) {
      if (key.int_value() == prop.key().int_value()) {
        found = true;
      }
    }

    if (found) {
      prop.value() = value;
      break;
    }
  }
  if (!found) {
    properties_.emplace_back(
        fdf::wire::NodeProperty::Builder(arena_).key(key).value(value).Build());
  }
}

std::string Device::OutgoingName() { return name_ + "-" + std::to_string(device_id_); }

zx_status_t Device::GetProtocol(uint32_t proto_id, void* out) const {
  if (HasOp(ops_, &zx_protocol_device_t::get_protocol)) {
    return ops_->get_protocol(compat_symbol_.context, proto_id, out);
  }

  if ((compat_symbol_.proto_ops.id != proto_id) || (compat_symbol_.proto_ops.ops == nullptr)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (!out) {
    return ZX_OK;
  }

  struct GenericProtocol {
    const void* ops;
    void* ctx;
  };

  auto proto = static_cast<GenericProtocol*>(out);
  proto->ops = compat_symbol_.proto_ops.ops;
  proto->ctx = compat_symbol_.context;
  return ZX_OK;
}

zx_status_t Device::AddMetadata(uint32_t type, const void* data, size_t size) {
  return device_server_.AddMetadata(type, data, size);
}

zx_status_t Device::GetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual) {
  return device_server_.GetMetadata(type, buf, buflen, actual);
}

zx_status_t Device::GetMetadataSize(uint32_t type, size_t* out_size) {
  return device_server_.GetMetadataSize(type, out_size);
}

zx_status_t Device::MessageOp(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
  if (!HasOp(ops_, &zx_protocol_device_t::message)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ops_->message(compat_symbol_.context, msg, txn);
}

zx::result<uint32_t> Device::SetPerformanceStateOp(uint32_t state) {
  if (!HasOp(ops_, &zx_protocol_device_t::set_performance_state)) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  uint32_t out_state;
  zx_status_t status = ops_->set_performance_state(compat_symbol_.context, state, &out_state);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out_state);
}

void Device::InitReply(zx_status_t status) {
  std::scoped_lock lock(init_lock_);
  init_is_finished_ = true;
  init_status_ = status;
  for (auto& waiter : init_waiters_) {
    if (status == ZX_OK) {
      waiter.complete_ok();
    } else {
      waiter.complete_error(init_status_);
    }
  }
  init_waiters_.clear();
}

fpromise::promise<void, zx_status_t> Device::WaitForInitToComplete() {
  std::scoped_lock lock(init_lock_);
  if (init_is_finished_) {
    if (init_status_ == ZX_OK) {
      return fpromise::make_result_promise<void, zx_status_t>(fpromise::ok());
    }
    return fpromise::make_result_promise<void, zx_status_t>(fpromise::error(init_status_));
  }
  fpromise::bridge<void, zx_status_t> bridge;
  init_waiters_.push_back(std::move(bridge.completer));

  return bridge.consumer.promise_or(fpromise::error(ZX_ERR_UNAVAILABLE));
}

constexpr char kCompatKey[] = "fuchsia.compat.LIBNAME";
fpromise::promise<void, zx_status_t> Device::RebindToLibname(std::string_view libname) {
  if (controller_teardown_finished_ == std::nullopt) {
    FDF_LOG(ERROR, "Calling rebind before device is set up?");
    return fpromise::make_error_promise(ZX_ERR_BAD_STATE);
  }
  InsertOrUpdateProperty(
      fdf::wire::NodePropertyKey::WithStringValue(arena_,
                                                  fidl::StringView::FromExternal(kCompatKey)),
      fdf::wire::NodePropertyValue::WithStringValue(arena_, fidl::StringView(arena_, libname)));
  // Once the controller teardown is finished (and the device is safely deleted),
  // we re-create the device.
  pending_rebind_ = true;
  auto promise =
      std::move(controller_teardown_finished_.value())
          .or_else([]() -> fpromise::result<void, zx_status_t> {
            ZX_ASSERT_MSG(false, "Unbind should always succeed");
          })
          .and_then([weak = weak_from_this()]() mutable -> fpromise::result<void, zx_status_t> {
            auto ptr = weak.lock();
            if (!ptr) {
              return fpromise::error(ZX_ERR_CANCELED);
            }
            // Reset FIDL clients so they don't complain when rebound.
            ptr->controller_ = {};
            ptr->node_ = {};
            zx_status_t status = ptr->CreateNode();
            ptr->pending_rebind_ = false;
            if (status != ZX_OK) {
              FDF_LOGL(ERROR, ptr->logger(), "Failed to recreate node: %s",
                       zx_status_get_string(status));
              return fpromise::error(status);
            }

            return fpromise::ok();
          })
          .wrap_with(scope_);
  Remove();
  return promise;
}

zx_status_t Device::ConnectFragmentFidl(const char* fragment_name, const char* protocol_name,
                                        zx::channel request) {
  if (std::string_view(fragment_name) != "default") {
    bool fragment_exists = false;
    for (auto& fragment : fragments_) {
      if (fragment == fragment_name) {
        fragment_exists = true;
        break;
      }
    }
    if (!fragment_exists) {
      FDF_LOG(ERROR, "Tried to connect to fragment '%s' but it's not in the fragment list",
              fragment_name);
      return ZX_ERR_NOT_FOUND;
    }
  }

  auto connect_string = std::string(fuchsia_driver_compat::Service::Name)
                            .append("/")
                            .append(fragment_name)
                            .append("/device");

  auto device =
      driver_->driver_namespace().Connect<fuchsia_driver_compat::Device>(connect_string.c_str());
  if (device.status_value() != ZX_OK) {
    FDF_LOG(ERROR, "Error connecting: %s", device.status_string());
    return device.status_value();
  }
  auto result = fidl::WireCall(*device)->ConnectFidl(fidl::StringView::FromExternal(protocol_name),
                                                     std::move(request));
  if (result.status() != ZX_OK) {
    FDF_LOG(ERROR, "Error calling connect fidl: %s", result.status_string());
    return result.status();
  }

  return ZX_OK;
}

zx_status_t Device::ConnectFragmentFidl(const char* fragment_name, const char* service_name,
                                        const char* protocol_name, zx::channel request) {
  if (std::string_view(fragment_name) != "default") {
    bool fragment_exists = false;
    for (auto& fragment : fragments_) {
      if (fragment == fragment_name) {
        fragment_exists = true;
        break;
      }
    }
    if (!fragment_exists) {
      FDF_LOG(ERROR, "Tried to connect to fragment '%s' but it's not in the fragment list",
              fragment_name);
      return ZX_ERR_NOT_FOUND;
    }
  }

  auto protocol_path =
      std::string(service_name).append("/").append(fragment_name).append("/").append(protocol_name);

  auto result = component::internal::ConnectAtRaw(driver_->driver_namespace().svc_dir(),
                                                  std::move(request), protocol_path.c_str());
  if (result.is_error()) {
    FDF_LOG(ERROR, "Error connecting: %s", result.status_string());
    return result.status_value();
  }

  return ZX_OK;
}

zx_status_t Device::AddComposite(const char* name, const composite_device_desc_t* comp_desc) {
  auto creator =
      driver_->driver_namespace().Connect<fuchsia_device_composite::DeprecatedCompositeCreator>();
  if (creator.status_value() != ZX_OK) {
    FDF_LOG(ERROR, "Error connecting: %s", creator.status_string());
    return creator.status_value();
  }

  fidl::Arena allocator;
  auto composite = CreateComposite(allocator, comp_desc);
  if (composite.is_error()) {
    FDF_LOG(ERROR, "Error creating composite: %s", composite.status_string());
    return composite.error_value();
  }

  // TODO(fxb/111891): Support metadata for AddComposite().
  if (comp_desc->metadata_count > 0) {
    FDF_LOG(WARNING, "AddComposite() currently doesn't support metadata. See fxb/111891.");
  }

  auto result = fidl::WireCall(*creator)->AddCompositeDevice(fidl::StringView::FromExternal(name),
                                                             std::move(composite.value()));
  if (result.status() != ZX_OK) {
    FDF_LOG(ERROR, "Error calling connect fidl: %s", result.status_string());
    return result.status();
  }

  return ZX_OK;
}

zx_status_t Device::AddNodeGroup(const char* name, const node_group_desc_t* group_desc) {
  if (!name || !group_desc) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!group_desc->nodes || group_desc->nodes_count == 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!group_desc->metadata_list && group_desc->metadata_count > 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto node_group_manager =
      driver_->driver_namespace().Connect<fuchsia_driver_framework::NodeGroupManager>();
  if (node_group_manager.is_error()) {
    FDF_LOG(ERROR, "Error connecting: %s", node_group_manager.status_string());
    return node_group_manager.status_value();
  }

  fidl::Arena allocator;
  auto nodes = fidl::VectorView<fdf::wire::NodeRepresentation>(allocator, group_desc->nodes_count);
  for (size_t i = 0; i < group_desc->nodes_count; i++) {
    auto node_result = ConvertNodeRepresentation(allocator, group_desc->nodes[i]);
    if (!node_result.is_ok()) {
      return node_result.error_value();
    }
    nodes[i] = std::move(node_result.value());
  }

  // TODO(fxb/111891): Support metadata for AddComposite().
  if (group_desc->metadata_count > 0) {
    FDF_LOG(WARNING, "AddNodeGroup() currently doesn't support metadata. See fxb/111891.");
  }

  auto node_group = fdf::wire::NodeGroup::Builder(allocator)
                        .name(fidl::StringView(allocator, name))
                        .nodes(std::move(nodes))
                        .Build();

  auto result = fidl::WireCall(*node_group_manager)->AddNodeGroup(std::move(node_group));
  if (result.status() != ZX_OK) {
    FDF_LOG(ERROR, "Error calling connect fidl: %s", result.status_string());
    return result.status();
  }

  return ZX_OK;
}

zx_status_t Device::ConnectRuntime(const char* protocol_name, fdf::Channel request) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_driver_framework::RuntimeConnector>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }
  zx_status_t status = ConnectFragmentFidl(
      "default", fidl::DiscoverableProtocolName<fuchsia_driver_framework::RuntimeConnector>,
      endpoints->server.TakeChannel());
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Error connecting to RuntimeConnector protocol: %s",
            zx_status_get_string(status));
    return status;
  }
  auto result =
      fidl::WireCall(endpoints->client)
          ->Connect(fidl::StringView::FromExternal(protocol_name),
                    fuchsia_driver_framework::wire::RuntimeProtocolServerEnd{request.release()});
  if (result.status() != ZX_OK) {
    FDF_LOG(ERROR, "Error calling RuntimeConnector::Connect fidl: %s", result.status_string());
    return result.status();
  }
  return ZX_OK;
}

zx_status_t Device::ConnectRuntime(const char* service_name, const char* protocol_name,
                                   fdf::Channel request) {
  zx::channel client_token, server_token;
  auto status = zx::channel::create(0, &client_token, &server_token);
  if (status != ZX_OK) {
    return status;
  }
  status = fdf::ProtocolConnect(std::move(client_token), std::move(request));
  if (status != ZX_OK) {
    return status;
  }

  return ConnectFragmentFidl("default", service_name, protocol_name, std::move(server_token));
}

zx::result<fidl::ClientEnd<fuchsia_io::Directory>> Device::ServeRuntimeConnectorProtocol() {
  auto& outgoing = driver()->outgoing();
  zx::result<> status = outgoing.component().AddUnmanagedProtocol<fdf::RuntimeConnector>(
      bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure));
  if (status.is_error()) {
    return status.take_error();
  }
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto serve = outgoing.Serve(std::move(endpoints->server));
  if (serve.is_error()) {
    return serve.take_error();
  }
  return zx::ok(std::move(endpoints->client));
}

zx_status_t Device::ServeInspectVmo(zx::vmo inspect_vmo) {
  uint64_t size;
  zx_status_t status = inspect_vmo.get_size(&size);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to vmo size: %s", zx_status_get_string(status));
    return status;
  }
  inspect_vmo_file_.emplace(fbl::MakeRefCounted<fs::VmoFile>(std::move(inspect_vmo), size));

  auto inspect_filename = OutgoingName().append(".inspect");
  ZX_ASSERT(driver() != nullptr);
  status =
      driver()->diagnostics_dir().AddEntry(inspect_filename.c_str(), inspect_vmo_file_.value());
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to add inspect vmo: %s", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

void Device::Connect(ConnectRequestView request, ConnectCompleter::Sync& completer) {
  // We should only have served this protocol if the |service_connect| op existed.
  ZX_ASSERT(HasOp(ops_, &zx_protocol_device_t::service_connect));

  auto protocol_name = std::string(request->protocol_name.data(), request->protocol_name.size());
  zx_status_t status = ops_->service_connect(compat_symbol_.context, protocol_name.c_str(),
                                             request->runtime_protocol.handle);
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    completer.ReplySuccess();
  }
}

void Device::LogError(const char* error) {
  FDF_LOG(ERROR, "%s: %s", topological_path_.c_str(), error);
}
bool Device::IsUnbound() { return pending_removal_; }

void Device::ConnectToDeviceFidl(ConnectToDeviceFidlRequestView request,
                                 ConnectToDeviceFidlCompleter::Sync& completer) {
  devfs_server_.ConnectToDeviceFidl(std::move(request->server));
}

void Device::Bind(BindRequestView request, BindCompleter::Sync& completer) {
  if (HasChildren()) {
    // A DFv1 driver will add a child device once it's bound. If the device has any children, refuse
    // the Bind() call.
    completer.ReplyError(ZX_ERR_ALREADY_BOUND);
    return;
  }
  auto promise = RebindToLibname(request->driver.get())
                     .then([completer = completer.ToAsync()](
                               fpromise::result<void, zx_status_t>& result) mutable {
                       if (result.is_ok()) {
                         completer.ReplySuccess();
                       } else {
                         completer.ReplyError(result.take_error());
                       }
                     });

  executor().schedule_task(std::move(promise));
}

void Device::GetCurrentPerformanceState(GetCurrentPerformanceStateCompleter::Sync& completer) {
  completer.Reply(0);
}

void Device::Rebind(RebindRequestView request, RebindCompleter::Sync& completer) {
  auto promise = RebindToLibname(request->driver.get())
                     .then([completer = completer.ToAsync()](
                               fpromise::result<void, zx_status_t>& result) mutable {
                       if (result.is_ok()) {
                         completer.ReplySuccess();
                       } else {
                         completer.ReplyError(result.take_error());
                       }
                     });

  executor().schedule_task(std::move(promise));
}

void Device::UnbindChildren(UnbindChildrenCompleter::Sync& completer) {
  executor().schedule_task(
      RemoveChildren().then([completer = completer.ToAsync()](fpromise::result<>& result) mutable {
        completer.ReplySuccess();
      }));
}

void Device::ScheduleUnbind(ScheduleUnbindCompleter::Sync& completer) {
  Remove();
  completer.ReplySuccess();
}

void Device::GetTopologicalPath(GetTopologicalPathCompleter::Sync& completer) {
  completer.ReplySuccess(fidl::StringView::FromExternal("/dev/" + topological_path_));
}

void Device::GetMinDriverLogSeverity(GetMinDriverLogSeverityCompleter::Sync& completer) {
  uint8_t severity = logger().GetSeverity();
  completer.Reply(ZX_OK, fuchsia_logger::wire::LogLevelFilter(severity));
}

void Device::SetMinDriverLogSeverity(SetMinDriverLogSeverityRequestView request,
                                     SetMinDriverLogSeverityCompleter::Sync& completer) {
  FuchsiaLogSeverity severity = static_cast<FuchsiaLogSeverity>(request->severity);
  logger().SetSeverity(severity);
  completer.Reply(ZX_OK);
}

void Device::SetPerformanceState(SetPerformanceStateRequestView request,
                                 SetPerformanceStateCompleter::Sync& completer) {
  zx::result result = SetPerformanceStateOp(request->requested_state);
  completer.Reply(result.status_value(), result.is_ok() ? result.value() : 0);
}

}  // namespace compat
