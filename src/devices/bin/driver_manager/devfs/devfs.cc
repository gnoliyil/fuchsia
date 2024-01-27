// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devfs.h"

#include <fcntl.h>
#include <fidl/fuchsia.device.fs/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async/cpp/wait.h>
#include <lib/ddk/driver.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/cpp/message_part.h>
#include <lib/fidl/txn_header.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <string.h>
#include <zircon/types.h>

#include <functional>
#include <memory>
#include <random>
#include <unordered_set>
#include <variant>

#include <fbl/ref_ptr.h>

#include "src/devices/bin/driver_manager/devfs/builtin_devices.h"
#include "src/devices/lib/log/log.h"
#include "src/lib/fxl/strings/split_string.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/storage/vfs/cpp/fuchsia_vfs.h"
#include "src/lib/storage/vfs/cpp/pseudo_dir.h"
#include "src/lib/storage/vfs/cpp/remote_dir.h"
#include "src/lib/storage/vfs/cpp/service.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"

namespace {

// Helpers from the reference documentation for std::visit<>, to allow
// visit-by-overload of the std::variant<> returned by GetLastReference():
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// explicit deduction guide (not needed as of C++20)
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

namespace fio = fuchsia_io;

ProtoNode* Devfs::proto_node(uint32_t protocol_id) {
  auto it = proto_info_nodes.find(protocol_id);
  if (it == proto_info_nodes.end()) {
    return nullptr;
  }
  auto& [key, value] = *it;
  return &value;
}

std::string_view Devnode::name() const {
  if (name_.has_value()) {
    return name_.value();
  }
  return {};
}

Devnode::ExportOptions Devnode::export_options() const {
  return std::visit(overloaded{[](const NoRemote& no_remote) { return no_remote.export_options; },
                               [](const Service& service) { return service.export_options; },
                               [](const Remote&) { return ExportOptions{}; }},
                    target());
}

Devnode::ExportOptions* Devnode::export_options() {
  return std::visit(overloaded{[](const NoRemote& no_remote) { return &no_remote.export_options; },
                               [](const Service& service) { return &service.export_options; },
                               [](const Remote&) -> ExportOptions* { return nullptr; }},
                    target());
}

void Devnode::advertise_modified() {
  ZX_ASSERT(parent_ != nullptr);
  parent_->Notify(name(), fio::wire::WatchEvent::kRemoved);
  parent_->Notify(name(), fio::wire::WatchEvent::kAdded);
}

Devnode::VnodeImpl::VnodeImpl(Devnode& holder, Target target)
    : holder_(holder), target_(std::move(target)) {}

bool Devnode::VnodeImpl::IsDirectory() const {
  return std::visit(
      overloaded{[&](const NoRemote&) { return true; }, [&](const Service&) { return false; },
                 [&](const Remote& remote) { return !remote.connector.is_valid(); }},
      target_);
}

fs::VnodeProtocolSet Devnode::VnodeImpl::GetProtocols() const {
  fs::VnodeProtocolSet protocols = fs::VnodeProtocol::kDirectory;
  if (!IsDirectory()) {
    protocols = protocols | fs::VnodeProtocol::kConnector;
  }
  return protocols;
}

zx_status_t Devnode::VnodeImpl::GetNodeInfoForProtocol(fs::VnodeProtocol protocol,
                                                       fs::Rights rights,
                                                       fs::VnodeRepresentation* info) {
  switch (protocol) {
    case fs::VnodeProtocol::kConnector:
      if (IsDirectory()) {
        return ZX_ERR_NOT_SUPPORTED;
      }
      *info = fs::VnodeRepresentation::Connector{};
      return ZX_OK;
    case fs::VnodeProtocol::kFile:
      return ZX_ERR_NOT_SUPPORTED;
    case fs::VnodeProtocol::kDirectory:
      *info = fs::VnodeRepresentation::Directory{};
      return ZX_OK;
  }
}

zx_status_t Devnode::VnodeImpl::ConnectService(zx::channel channel) {
  return std::visit(
      overloaded{[&](const NoRemote&) { return ZX_ERR_NOT_SUPPORTED; },
                 [&](const Service&) { return ZX_ERR_NOT_SUPPORTED; },
                 [&](const Remote& remote) {
                   if (remote.connector.is_valid()) {
                     return remote.connector->ConnectMultiplexed(std::move(channel)).status();
                   }
                   return ZX_ERR_NOT_SUPPORTED;
                 }},
      target_);
}

bool Devnode::VnodeImpl::IsService() const {
  return std::visit(
      overloaded{[&](const NoRemote&) { return false; }, [&](const Service&) { return false; },
                 [&](const Remote& remote) { return remote.connector.is_valid(); }},
      target_);
}

zx_status_t Devnode::VnodeImpl::OpenNode(ValidatedOptions options,
                                         fbl::RefPtr<Vnode>* out_redirect) {
  if (options->flags.directory) {
    return ZX_OK;
  }
  if (IsDirectory()) {
    return ZX_OK;
  }
  if (IsService()) {
    return ZX_OK;
  }
  *out_redirect = remote_;
  return ZX_OK;
}

fs::VnodeProtocolSet Devnode::VnodeImpl::RemoteNode::GetProtocols() const {
  return parent_.GetProtocols();
}

zx_status_t Devnode::VnodeImpl::RemoteNode::GetNodeInfoForProtocol(fs::VnodeProtocol protocol,
                                                                   fs::Rights rights,
                                                                   fs::VnodeRepresentation* info) {
  return parent_.GetNodeInfoForProtocol(protocol, rights, info);
}

bool Devnode::VnodeImpl::RemoteNode::IsRemote() const { return true; }

zx_status_t Devnode::VnodeImpl::RemoteNode::OpenRemote(fio::OpenFlags flags, uint32_t mode,
                                                       fidl::StringView path,
                                                       fidl::ServerEnd<fio::Node> object) const {
  ZX_ASSERT_MSG(path.get() == ".", "unexpected path to remote '%.*s'",
                static_cast<int>(path.size()), path.data());
  return std::visit(
      overloaded{[&](const NoRemote&) { return ZX_ERR_NOT_SUPPORTED; },
                 [&](const Service& service) {
                   return fidl::WireCall(service.remote)
                       ->Open(flags, mode, fidl::StringView::FromExternal(service.path),
                              std::move(object))
                       .status();
                 },
                 [&](const Remote& remote) {
                   return remote.connector->ConnectMultiplexed(object.TakeChannel()).status();
                 }},
      parent_.target_);
}

zx_status_t Devnode::VnodeImpl::GetAttributes(fs::VnodeAttributes* a) {
  return children().GetAttributes(a);
}

zx_status_t Devnode::VnodeImpl::Lookup(std::string_view name, fbl::RefPtr<fs::Vnode>* out) {
  return children().Lookup(name, out);
}

zx_status_t Devnode::VnodeImpl::WatchDir(fs::Vfs* vfs, fio::wire::WatchMask mask, uint32_t options,
                                         fidl::ServerEnd<fio::DirectoryWatcher> watcher) {
  return children().WatchDir(vfs, mask, options, std::move(watcher));
}

zx_status_t Devnode::VnodeImpl::Readdir(fs::VdirCookie* cookie, void* dirents, size_t len,
                                        size_t* out_actual) {
  return children().Readdir(cookie, dirents, len, out_actual);
}

namespace {

void MustAddEntry(PseudoDir& parent, const std::string_view name,
                  const fbl::RefPtr<fs::Vnode>& dn) {
  const zx_status_t status = parent.AddEntry(name, dn);
  ZX_ASSERT_MSG(status == ZX_OK, "AddEntry(%.*s): %s", static_cast<int>(name.size()), name.data(),
                zx_status_get_string(status));
}

}  // namespace

Devnode::Devnode(Devfs& devfs)
    : devfs_(devfs), parent_(nullptr), node_(fbl::MakeRefCounted<VnodeImpl>(*this, NoRemote())) {}

Devnode::Devnode(Devfs& devfs, PseudoDir& parent, Target target, fbl::String name)
    : devfs_(devfs),
      parent_(&parent),
      node_(fbl::MakeRefCounted<VnodeImpl>(*this, std::move(target))),
      name_([this, &parent, name = std::move(name)]() {
        auto [it, inserted] = parent.unpublished.emplace(name, *this);
        ZX_ASSERT(inserted);
        return it->first;
      }()) {
  auto [device_controller, device_protocol] = std::visit(
      overloaded{
          [](const NoRemote&) {
            return std::make_tuple(fbl::RefPtr<fs::Service>(), fbl::RefPtr<fs::Service>());
          },
          [](const Service& service) {
            auto device_controller =
                fbl::MakeRefCounted<fs::Service>([&service](zx::channel channel) {
                  return fidl::WireCall(service.remote)
                      ->Open(fio::OpenFlags::kRightReadable | fio::OpenFlags::kRightWritable, 0,
                             fidl::StringView::FromExternal(service.path),
                             fidl::ServerEnd<fuchsia_io::Node>(std::move(channel)))
                      .status();
                });
            auto device_protocol =
                fbl::MakeRefCounted<fs::Service>([&service](zx::channel channel) {
                  return fidl::WireCall(service.remote)
                      ->Open(fio::OpenFlags::kRightReadable | fio::OpenFlags::kRightWritable, 0,
                             fidl::StringView::FromExternal(service.path),
                             fidl::ServerEnd<fuchsia_io::Node>(std::move(channel)))
                      .status();
                });
            return std::make_tuple(std::move(device_controller), std::move(device_protocol));
          },
          [](const Remote& remote) {
            auto device_controller =
                fbl::MakeRefCounted<fs::Service>([&remote](zx::channel channel) {
                  return remote.connector
                      ->ConnectToController(
                          fidl::ServerEnd<fuchsia_device::Controller>(std::move(channel)))
                      .status();
                });
            auto device_protocol = fbl::MakeRefCounted<fs::Service>([&remote](zx::channel channel) {
              return remote.connector->ConnectToDeviceProtocol(std::move(channel)).status();
            });
            return std::make_tuple(std::move(device_controller), std::move(device_protocol));
          }},
      this->target());
  if (device_controller) {
    children().AddEntry(fuchsia_device_fs::wire::kDeviceControllerName,
                        std::move(device_controller));
  }

  if (device_protocol) {
    children().AddEntry(fuchsia_device_fs::wire::kDeviceProtocolName, std::move(device_protocol));
  }
}

std::optional<std::reference_wrapper<fs::Vnode>> Devfs::Lookup(PseudoDir& parent,
                                                               std::string_view name) {
  {
    fbl::RefPtr<fs::Vnode> out;
    switch (const zx_status_t status = parent.Lookup(name, &out); status) {
      case ZX_OK:
        return *out;
      case ZX_ERR_NOT_FOUND:
        break;
      default:
        ZX_PANIC("%s", zx_status_get_string(status));
    }
  }
  const auto it = parent.unpublished.find(name);
  if (it != parent.unpublished.end()) {
    return it->second.get().node();
  }
  return {};
}

Devnode::~Devnode() {
  for (auto [key, child] : children().unpublished) {
    child.get().parent_ = nullptr;
  }
  children().unpublished.clear();

  children().RemoveAllEntries();

  if (parent_ == nullptr) {
    return;
  }
  PseudoDir& parent = *parent_;
  const std::string_view name = this->name();
  parent.unpublished.erase(name);
  switch (const zx_status_t status = parent.RemoveEntry(name, node_.get()); status) {
    case ZX_OK:
    case ZX_ERR_NOT_FOUND:
      // Our parent may have been removed before us.
      break;
    default:
      ZX_PANIC("RemoveEntry(%.*s): %s", static_cast<int>(name.size()), name.data(),
               zx_status_get_string(status));
  }
}

void Devnode::publish() {
  ZX_ASSERT(parent_ != nullptr);
  PseudoDir& parent = *parent_;

  const std::string_view name = this->name();
  const auto it = parent.unpublished.find(name);
  ZX_ASSERT(it != parent.unpublished.end());
  ZX_ASSERT(&it->second.get() == this);
  parent.unpublished.erase(it);

  MustAddEntry(parent, name, node_);
}

void DevfsDevice::advertise_modified() {
  if (topological_.has_value()) {
    topological_.value().advertise_modified();
  }
  if (protocol_.has_value()) {
    protocol_.value().advertise_modified();
  }
}

void DevfsDevice::publish() {
  if (topological_.has_value()) {
    topological_.value().publish();
  }
  if (protocol_.has_value()) {
    protocol_.value().publish();
  }
}

void DevfsDevice::unpublish() {
  topological_.reset();
  protocol_.reset();
}

ProtoNode::ProtoNode(fbl::String name, uint32_t initial_device_number)
    : name_(std::move(name)), next_device_number_(initial_device_number) {}

zx::result<fbl::String> ProtoNode::seq_name() {
  std::string dest;
  for (uint32_t i = 0; i < 1000; ++i) {
    dest.clear();
    fxl::StringAppendf(&dest, "%03u", (next_device_number_++) % 1000);
    {
      fbl::RefPtr<fs::Vnode> out;
      switch (const zx_status_t status = children().Lookup(dest, &out); status) {
        case ZX_OK:
          continue;
        case ZX_ERR_NOT_FOUND:
          break;
        default:
          return zx::error(status);
      }
    }
    if (children().unpublished.find(dest) != children().unpublished.end()) {
      continue;
    }
    return zx::ok(dest);
  }
  return zx::error(ZX_ERR_ALREADY_EXISTS);
}

zx_status_t Devnode::add_child(std::string_view name, uint32_t protocol, Remote remote,
                               DevfsDevice& out_child) {
  // Check that the child does not have a duplicate name.
  const std::optional other = devfs_.Lookup(children(), name);
  if (other.has_value()) {
    LOGF(WARNING, "rejecting duplicate device name '%.*s'", static_cast<int>(name.size()),
         name.data());
    return ZX_ERR_ALREADY_EXISTS;
  }

  // Find the protocol directory.
  ProtoNode* proto_dir = nullptr;
  switch (const uint32_t id = protocol; id) {
    case ZX_PROTOCOL_TEST_PARENT:
    case ZX_PROTOCOL_MISC:
      // misc devices are singletons, not a class in the sense of other device
      // classes.  They do not get aliases in /dev/class/misc/...  instead they
      // exist only under their parent device.
      break;
    default:
      proto_dir = devfs_.proto_node(id);
  }

  // Get the name of our new device in the protocol directory.
  fbl::String class_name;
  if (proto_dir) {
    class_name = name;
    if (protocol != ZX_PROTOCOL_CONSOLE) {
      zx::result seq_name = proto_dir->seq_name();
      if (seq_name.is_error()) {
        return seq_name.status_value();
      }
      class_name = seq_name.value();
    }
  }

  // Setup the DevfsDevice.
  if (proto_dir) {
    out_child.protocol_node().emplace(devfs_, proto_dir->children(), remote.Clone(), class_name);
  }
  out_child.topological_node().emplace(devfs_, children(), std::move(remote), name);

  return ZX_OK;
}

zx::result<fidl::ClientEnd<fio::Directory>> Devfs::Connect(fs::FuchsiaVfs& vfs) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto& [client, server] = endpoints.value();
  // NB: Serve the `PseudoDir` rather than the root `Devnode` because
  // otherwise we'd end up in the connector code path. Clients that want to open
  // the root node as a device can do so using `"."` and appropriate flags.
  return zx::make_result(vfs.ServeDirectory(root_.node_, std::move(server)), std::move(client));
}

Devfs::Devfs(std::optional<Devnode>& root,
             std::optional<fidl::ClientEnd<fio::Directory>> diagnostics)
    : root_(root.emplace(*this)) {
  PseudoDir& pd = root_.children();
  if (diagnostics.has_value()) {
    MustAddEntry(pd, "diagnostics",
                 fbl::MakeRefCounted<fs::RemoteDir>(std::move(diagnostics.value())));
  }
  MustAddEntry(pd, "class", class_);
  MustAddEntry(pd, kNullDevName, fbl::MakeRefCounted<BuiltinDevVnode>(true));
  MustAddEntry(pd, kZeroDevName, fbl::MakeRefCounted<BuiltinDevVnode>(false));

  // Pre-populate the class directories.
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distrib(0, 1000);
  // TODO(https://fxbug.dev/113679): shrink this list to zero.
  //
  // Do not add to this list.
  //
  // These classes have clients that rely on the numbering scheme starting at
  // 000 and increasing sequentially. This list was generated using:
  //
  // rg -IoN --no-ignore -g '!out/' -g '!*.md' '\bclass/[^/]+/[0-9]{3}\b' | \
  // sed -E 's|class/(.*)/[0-9]{3}|"\1",|g' | sort | uniq
  const std::unordered_set<std::string_view> classes_that_assume_ordering({
      // TODO(https://fxbug.dev/113716): Remove.
      "adc",

      // TODO(https://fxbug.dev/113717): Remove.
      "aml-ram",

      // TODO(https://fxbug.dev/113680): Remove these.
      "audio-input",
      "audio-output",

      // TODO(https://fxbug.dev/113718): Remove.
      // TODO(https://fxbug.dev/113842): Remove.
      "backlight",

      // TODO(https://fxbug.dev/117160): Remove.
      "block",

      // TODO(https://fxbug.dev/113719): Remove.
      "bt-hci",

      // TODO(https://fxbug.dev/113720): Remove.
      "camera",

      // TODO(https://fxbug.dev/113828): Remove.
      "cpu-ctrl",

      // TODO(https://fxbug.dev/113829): Remove.
      "display-controller",

      // TODO(https://fxbug.dev/113830): Remove.
      "goldfish-address-space",
      "goldfish-control",
      "goldfish-pipe",

      // TODO(https://fxbug.dev/113832): Remove.
      "input",
      "input-report",

      // TODO(https://fxbug.dev/113833): Remove.
      "isp",

      // TODO(https://fxbug.dev/113834): Remove.
      "light",

      // TODO(https://fxbug.dev/113835): Remove.
      "ot-radio",

      // TODO(https://fxbug.dev/113842): Remove.
      "power-sensor",

      // TODO(https://fxbug.dev/113836): Remove.
      "pwm",

      // TODO(https://fxbug.dev/113838): Remove.
      "radar",

      // TODO(https://fxbug.dev/113839): Remove.
      "securemem",

      // TODO(https://fxbug.dev/113840): Remove.
      "tee",

      // TODO(https://fxbug.dev/113713): Remove.
      // TODO(https://fxbug.dev/113842): Remove.
      "temperature",

      // TODO(https://fxbug.dev/113841): Remove.
      "test",

      // TODO(https://fxbug.dev/113842): Remove.
      "thermal",

      // TODO(https://fxbug.dev/113845): Remove.
      "zxcrypt",
  });
  for (const auto& info : proto_infos) {
    if (!(info.flags & PF_NOPUB)) {
      uint32_t seq = distrib(gen);
      if (classes_that_assume_ordering.find(info.name) != classes_that_assume_ordering.end()) {
        seq = 0;
      }
      const auto [it, inserted] = proto_info_nodes.try_emplace(info.id, info.name, seq);
      const auto& [key, value] = *it;
      ZX_ASSERT_MSG(inserted, "duplicate protocol with id %d", key);
      MustAddEntry(*class_, info.name, value.children_);
    }
  }
}

zx_status_t Devnode::export_dir(fidl::ClientEnd<fio::Directory> service_dir,
                                std::string_view service_path, std::string_view devfs_path,
                                uint32_t protocol_id, ExportOptions options,
                                std::vector<std::unique_ptr<Devnode>>& out) {
  {
    const std::vector segments =
        fxl::SplitString(service_path, "/", fxl::WhiteSpaceHandling::kKeepWhitespace,
                         fxl::SplitResult::kSplitWantAll);
    if (segments.empty() ||
        std::any_of(segments.begin(), segments.end(), std::mem_fn(&std::string_view::empty))) {
      return ZX_ERR_INVALID_ARGS;
    }
  }

  const std::vector segments = fxl::SplitString(
      devfs_path, "/", fxl::WhiteSpaceHandling::kKeepWhitespace, fxl::SplitResult::kSplitWantAll);
  if (segments.empty() ||
      std::any_of(segments.begin(), segments.end(), std::mem_fn(&std::string_view::empty))) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Walk the request export path segment-by-segment.
  Devnode* dn = this;
  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string_view name = segments.at(i);
    zx::result child = [name, &children = dn->children()]() -> zx::result<Devnode*> {
      fbl::RefPtr<fs::Vnode> out;
      switch (const zx_status_t status = children.Lookup(name, &out); status) {
        case ZX_OK:
          return zx::ok(&fbl::RefPtr<Devnode::VnodeImpl>::Downcast(out)->holder_);
        case ZX_ERR_NOT_FOUND:
          break;
        default:
          return zx::error(status);
      }
      const auto it = children.unpublished.find(name);
      if (it != children.unpublished.end()) {
        return zx::ok(&it->second.get());
      }
      return zx::ok(nullptr);
    }();
    if (child.is_error()) {
      return child.status_value();
    }
    if (i != segments.size() - 1) {
      // This is not the final path segment. Use the existing node or create one
      // if it doesn't exist.
      if (child.value() != nullptr) {
        dn = child.value();
        continue;
      }
      PseudoDir& parent = dn->node().children();
      Devnode& child = *out.emplace_back(std::make_unique<Devnode>(devfs_, parent,
                                                                   NoRemote{
                                                                       .export_options = options,
                                                                   },
                                                                   name));
      if (!(options & ExportOptions::kInvisible)) {
        child.publish();
      }
      dn = &child;
      continue;
    }

    // At this point `dn` is the second-last path segment.
    if (child != nullptr) {
      // The full path described by `devfs_path` already exists.
      return ZX_ERR_ALREADY_EXISTS;
    }

    // If a protocol directory exists for `protocol_id`, then create a Devnode
    // under the protocol directory too.
    if (ProtoNode* dir_ptr = devfs_.proto_node(protocol_id); dir_ptr != nullptr) {
      ProtoNode& dn = *dir_ptr;
      zx::result seq_name = dn.seq_name();
      if (seq_name.is_error()) {
        return seq_name.status_value();
      }
      const fbl::String name = seq_name.value();

      // Clone the service node for the entry in the protocol directory.
      zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
      if (endpoints.is_error()) {
        return endpoints.status_value();
      }
      auto& [client, server] = endpoints.value();
      const fidl::Status result = fidl::WireCall(service_dir)
                                      ->Clone(fio::wire::OpenFlags::kCloneSameRights,
                                              fidl::ServerEnd<fio::Node>{server.TakeChannel()});
      if (!result.ok()) {
        return result.status();
      }

      Devnode& child =
          *out.emplace_back(std::make_unique<Devnode>(devfs_, dn.children(),
                                                      Service{
                                                          .remote = std::move(client),
                                                          .path = std::string(service_path),
                                                          .export_options = options,
                                                      },
                                                      name));
      if (!(options & ExportOptions::kInvisible)) {
        child.publish();
      }
    }

    {
      Devnode& child =
          *out.emplace_back(std::make_unique<Devnode>(devfs_, dn->node().children(),
                                                      Service{
                                                          .remote = std::move(service_dir),
                                                          .path = std::string(service_path),
                                                          .export_options = options,
                                                      },
                                                      name));
      if (!(options & ExportOptions::kInvisible)) {
        child.publish();
      }
    }
  }
  return ZX_OK;
}
