// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/directory_connection.h"

#include <fcntl.h>
#include <fidl/fuchsia.io.admin/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.io2/cpp/wire.h>
#include <lib/fdio/io.h>
#include <lib/fdio/vfs.h>
#include <lib/zircon-internal/debug.h>
#include <lib/zx/handle.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zircon/assert.h>

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fbl/string_buffer.h>

#include "src/lib/storage/vfs/cpp/advisory_lock.h"
#include "src/lib/storage/vfs/cpp/debug.h"
#include "src/lib/storage/vfs/cpp/fidl_transaction.h"
#include "src/lib/storage/vfs/cpp/mount_channel.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fio = fuchsia_io;
namespace fio2 = fuchsia_io2;

namespace fs {

namespace {

// Performs a path walk and opens a connection to another node.
void OpenAt(FuchsiaVfs* vfs, const fbl::RefPtr<Vnode>& parent, fidl::ServerEnd<fio::Node> channel,
            std::string_view path, VnodeConnectionOptions options, Rights parent_rights,
            uint32_t mode) {
  bool describe = options.flags.describe;
  vfs->Open(parent, path, options, parent_rights, mode).visit([&](auto&& result) {
    using ResultT = std::decay_t<decltype(result)>;
    using OpenResult = fs::Vfs::OpenResult;
    if constexpr (std::is_same_v<ResultT, OpenResult::Error>) {
      if (describe) {
        fidl::WireEventSender<fio::Node>(std::move(channel)).OnOpen(result, fio::wire::NodeInfo());
      }
    } else if constexpr (std::is_same_v<ResultT, OpenResult::Remote>) {
      // Remote handoff to a remote filesystem node.
      vfs->ForwardOpenRemote(std::move(result.vnode), std::move(channel), result.path, options,
                             mode);
    } else if constexpr (std::is_same_v<ResultT, OpenResult::RemoteRoot>) {
      // Remote handoff to a remote filesystem node.
      vfs->ForwardOpenRemote(std::move(result.vnode), std::move(channel), ".", options, mode);
    } else if constexpr (std::is_same_v<ResultT, OpenResult::Ok>) {
      // |Vfs::Open| already performs option validation for us.
      vfs->Serve(result.vnode, std::move(channel), result.validated_options);
    }
  });
}

// Performs a path walk and adds inotify filter to the obtained vnode.
void AddInotifyFilterAt(FuchsiaVfs* vfs, const fbl::RefPtr<Vnode>& parent, std::string_view path,
                        fio2::wire::InotifyWatchMask filter, uint32_t watch_descriptor,
                        zx::socket socket) {
  // TODO Not handling remote handoff currently.
  vfs->TraversePathFetchVnode(parent, path).visit([&](auto&& result) {
    using ResultT = std::decay_t<decltype(result)>;
    using TraversePathResult = fs::Vfs::TraversePathResult;
    if constexpr (std::is_same_v<ResultT, TraversePathResult::Error>) {
      return;
    } else if constexpr (std::is_same_v<ResultT, TraversePathResult::Remote>) {
      // Remote handoff to a remote filesystem node.
      // TODO remote handoffs not supported for inotify currently.
      return;
    } else if constexpr (std::is_same_v<ResultT, TraversePathResult::RemoteRoot>) {
      // Remote handoff to a remote filesystem node.
      // TODO remote handoffs not supported for inotify currently.
      return;
    } else if constexpr (std::is_same_v<ResultT, TraversePathResult::Ok>) {
      // We have got the vnode to add the filter to.
      vfs->AddInotifyFilterToVnode(result.vnode, parent, filter, watch_descriptor,
                                   std::move(socket));
    }
  });
}
}  // namespace

namespace internal {

DirectoryConnection::DirectoryConnection(fs::FuchsiaVfs* vfs, fbl::RefPtr<fs::Vnode> vnode,
                                         VnodeProtocol protocol, VnodeConnectionOptions options)
    : Connection(vfs, std::move(vnode), protocol, options,
                 FidlProtocol::Create<fuchsia_io_admin::DirectoryAdmin>(this)) {}

void DirectoryConnection::Clone(CloneRequestView request, CloneCompleter::Sync& completer) {
  Connection::NodeClone(request->flags, std::move(request->object));
}

void DirectoryConnection::Close(CloseRequestView request, CloseCompleter::Sync& completer) {
  auto result = Connection::NodeClose();
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::Close2(Close2RequestView request, Close2Completer::Sync& completer) {
  auto result = Connection::NodeClose();
  if (result.is_error()) {
    // TODO(fxbug.dev/64992) this should just be:
    //    completer.ReplyError(result.error());
    // when the bug is fixed. This applies to other error replies in this file.
    zx_status_t error = result.error();
    completer.Reply(::fuchsia_io::wire::NodeClose2Result::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&error)));
  } else {
    completer.Reply({});
  }
}

void DirectoryConnection::Describe(DescribeRequestView request,
                                   DescribeCompleter::Sync& completer) {
  auto result = Connection::NodeDescribe();
  if (result.is_error()) {
    completer.Close(result.error());
    return;
  }
  ConvertToIoV1NodeInfo(result.take_value(),
                        [&](fio::wire::NodeInfo&& info) { completer.Reply(std::move(info)); });
}

void DirectoryConnection::Sync(SyncRequestView request, SyncCompleter::Sync& completer) {
  Connection::NodeSync([completer = completer.ToAsync()](zx_status_t sync_status) mutable {
    completer.Reply(sync_status);
  });
}

void DirectoryConnection::GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeGetAttr();
  if (result.is_error()) {
    completer.Reply(result.error(), fio::wire::NodeAttributes());
  } else {
    completer.Reply(ZX_OK, result.value().ToIoV1NodeAttributes());
  }
}

void DirectoryConnection::SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeSetAttr(request->flags, request->attributes);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::NodeGetFlags(NodeGetFlagsRequestView request,
                                       NodeGetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeGetFlags();
  if (result.is_error()) {
    completer.Reply(result.error(), 0);
  } else {
    completer.Reply(ZX_OK, result.value());
  }
}

void DirectoryConnection::NodeSetFlags(NodeSetFlagsRequestView request,
                                       NodeSetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeSetFlags(request->flags);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::AddInotifyFilter(AddInotifyFilterRequestView request,
                                           AddInotifyFilterCompleter::Sync& completer) {
  AddInotifyFilterAt(vfs(), vnode(), std::string_view(request->path.data(), request->path.size()),
                     request->filter, request->watch_descriptor, std::move(request->socket));
  completer.Reply();
}

void DirectoryConnection::Open(OpenRequestView request, OpenCompleter::Sync& completer) {
  bool describe = request->flags & fio::wire::kOpenFlagDescribe;
  auto write_error = [describe](fidl::ServerEnd<fio::Node> channel, zx_status_t error) {
    if (describe) {
      fidl::WireEventSender<fio::Node>(std::move(channel)).OnOpen(error, fio::wire::NodeInfo());
    }
  };

  std::string_view path(request->path.data(), request->path.size());
  if (path.size() > PATH_MAX) {
    return write_error(std::move(request->object), ZX_ERR_BAD_PATH);
  }

  if (path.empty() ||
      ((path == "." || path == "/") && (request->flags & fio::wire::kOpenFlagNotDirectory))) {
    return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
  }

  uint32_t flags = request->flags;
  if (path.back() == '/') {
    flags |= fio::wire::kOpenFlagDirectory;
  }

  auto open_options = VnodeConnectionOptions::FromIoV1Flags(flags);

  if (!PrevalidateFlags(flags)) {
    FS_PRETTY_TRACE_DEBUG("[DirectoryOpen] prevalidate failed",
                          ", incoming flags: ", ZxFlags(request->flags),
                          ", path: ", request->path.data());
    return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
  }

  uint32_t mode = request->mode;
  const uint32_t mode_type = mode & fio::wire::kModeTypeMask;
  if (mode_type == 0) {
    if (open_options.flags.directory) {
      mode |= fio::wire::kModeTypeDirectory;
    }
  } else {
    if (open_options.flags.directory && mode_type != fio::wire::kModeTypeDirectory) {
      return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
    }

    if (open_options.flags.not_directory && mode_type == fio::wire::kModeTypeDirectory) {
      return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
    }
  }

  FS_PRETTY_TRACE_DEBUG("[DirectoryOpen] our options: ", options(),
                        ", incoming options: ", open_options, ", path: ", request->path.data());
  if (options().flags.node_reference) {
    return write_error(std::move(request->object), ZX_ERR_BAD_HANDLE);
  }
  if (open_options.flags.clone_same_rights) {
    return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
  }
  // Reject the Open() call if we haven't gotten OPEN_FLAG_NODE_REFERENCE,
  // nor have any OPEN_RIGHT_* or OPEN_FLAG_POSIX.
  if (!open_options.flags.node_reference && !open_options.rights.any() &&
      !open_options.flags.posix_write && !open_options.flags.posix_execute) {
    return write_error(std::move(request->object), ZX_ERR_INVALID_ARGS);
  }

  // Check for directory rights inheritance
  zx_status_t status = EnforceHierarchicalRights(options().rights, open_options, &open_options);
  if (status != ZX_OK) {
    FS_PRETTY_TRACE_DEBUG("Rights violation during DirectoryOpen");
    return write_error(std::move(request->object), status);
  }
  OpenAt(vfs(), vnode(), std::move(request->object), path, open_options, options().rights, mode);
}

void DirectoryConnection::Unlink(UnlinkRequestView request, UnlinkCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryUnlink] our options: ", options(),
                        ", name: ", request->name.data());

  zx_status_t status;
  if (options().flags.node_reference) {
    status = ZX_ERR_BAD_HANDLE;
    completer.Reply(::fuchsia_io::wire::DirectoryUnlinkResult::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  if (!options().rights.write) {
    status = ZX_ERR_BAD_HANDLE;
    completer.Reply(::fuchsia_io::wire::DirectoryUnlinkResult::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  std::string_view name_str(request->name.data(), request->name.size());
  if (!IsValidName(name_str)) {
    status = ZX_ERR_INVALID_ARGS;
    completer.Reply(::fuchsia_io::wire::DirectoryUnlinkResult::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  status = vfs()->Unlink(vnode(), name_str,
                         request->options.has_flags()
                             ? static_cast<bool>((request->options.flags() &
                                                  fuchsia_io2::wire::UnlinkFlags::kMustBeDirectory))
                             : false);
  if (status == ZX_OK) {
    // TODO(fxbug.dev/64992) this should just be:
    //    completer.ReplySuccess();
    // when the bug is fixed. This applies to other success replies in this file.
    ::fuchsia_io::wire::DirectoryUnlinkResponse response;
    completer.Reply(::fuchsia_io::wire::DirectoryUnlinkResult::WithResponse(
        fidl::ObjectView<::fuchsia_io::wire::DirectoryUnlinkResponse>::FromExternal(&response)));
  } else {
    completer.Reply(::fuchsia_io::wire::DirectoryUnlinkResult::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
  }
}

void DirectoryConnection::ReadDirents(ReadDirentsRequestView request,
                                      ReadDirentsCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryReadDirents] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE, fidl::VectorView<uint8_t>());
    return;
  }
  if (request->max_bytes > fio::wire::kMaxBuf) {
    completer.Reply(ZX_ERR_BAD_HANDLE, fidl::VectorView<uint8_t>());
    return;
  }
  uint8_t data[request->max_bytes];
  size_t actual = 0;
  zx_status_t status =
      vfs()->Readdir(vnode().get(), &dircookie_, data, request->max_bytes, &actual);
  completer.Reply(status, fidl::VectorView<uint8_t>::FromExternal(data, actual));
}

void DirectoryConnection::Rewind(RewindRequestView request, RewindCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryRewind] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  dircookie_ = VdirCookie();
  completer.Reply(ZX_OK);
}

void DirectoryConnection::GetToken(GetTokenRequestView request,
                                   GetTokenCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryGetToken] our options: ", options());

  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE, zx::handle());
    return;
  }
  zx::event returned_token;
  zx_status_t status = vfs()->VnodeToToken(vnode(), &token(), &returned_token);
  completer.Reply(status, std::move(returned_token));
}

void DirectoryConnection::Rename(RenameRequestView request, RenameCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryRename] our options: ", options(),
                        ", src: ", request->src.data(), ", dst: ", request->dst.data());

  // |fuchsia.io/Directory.Rename| only specified the token to be a generic handle; casting it here.
  zx::event token(request->dst_parent_token.release());

  if (request->src.empty() || request->dst.empty()) {
    completer.Reply(ZX_ERR_INVALID_ARGS);
    return;
  }
  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status = vfs()->Rename(std::move(token), vnode(),
                                     std::string_view(request->src.data(), request->src.size()),
                                     std::string_view(request->dst.data(), request->dst.size()));
  completer.Reply(status);
}

void DirectoryConnection::Rename2(Rename2RequestView request, Rename2Completer::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryRename] our options: ", options(),
                        ", src: ", request->src.data(), ", dst: ", request->dst.data());

  zx_status_t status;
  if (request->src.empty() || request->dst.empty()) {
    status = ZX_ERR_INVALID_ARGS;
    completer.Reply(::fuchsia_io::wire::DirectoryRename2Result::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  if (options().flags.node_reference) {
    status = ZX_ERR_BAD_HANDLE;
    completer.Reply(::fuchsia_io::wire::DirectoryRename2Result::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  if (!options().rights.write) {
    status = ZX_ERR_BAD_HANDLE;
    completer.Reply(::fuchsia_io::wire::DirectoryRename2Result::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
    return;
  }
  status = vfs()->Rename(std::move(request->dst_parent_token), vnode(),
                         std::string_view(request->src.data(), request->src.size()),
                         std::string_view(request->dst.data(), request->dst.size()));
  if (status == ZX_OK) {
    ::fuchsia_io::wire::DirectoryRename2Response response;
    completer.Reply(::fuchsia_io::wire::DirectoryRename2Result::WithResponse(
        fidl::ObjectView<::fuchsia_io::wire::DirectoryRename2Response>::FromExternal(&response)));
  } else {
    completer.Reply(::fuchsia_io::wire::DirectoryRename2Result::WithErr(
        fidl::ObjectView<zx_status_t>::FromExternal(&status)));
  }
}

void DirectoryConnection::Link(LinkRequestView request, LinkCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryLink] our options: ", options(), ", src: ", request->src.data(),
                        ", dst: ", request->dst.data());

  // |fuchsia.io/Directory.Rename| only specified the token to be a generic handle; casting it here.
  zx::event token(request->dst_parent_token.release());

  if (request->src.empty() || request->dst.empty()) {
    completer.Reply(ZX_ERR_INVALID_ARGS);
    return;
  }
  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status = vfs()->Link(std::move(token), vnode(),
                                   std::string_view(request->src.data(), request->src.size()),
                                   std::string_view(request->dst.data(), request->dst.size()));
  completer.Reply(status);
}

void DirectoryConnection::Watch(WatchRequestView request, WatchCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryWatch] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status =
      vnode()->WatchDir(vfs(), request->mask, request->options, std::move(request->watcher));
  completer.Reply(status);
}

void DirectoryConnection::Mount(MountRequestView request, MountCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminMount] our options: ", options());

  if (!options().rights.admin) {
    // Note: this is best-effort, and would fail if the remote endpoint does not speak the
    // |fuchsia.io/DirectoryAdmin| protocol.
    fidl::ClientEnd<fuchsia_io_admin::DirectoryAdmin> remote_admin(request->remote.TakeChannel());
    FuchsiaVfs::UnmountHandle(std::move(remote_admin), zx::time::infinite());
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  MountChannel c = MountChannel(std::move(request->remote));
  zx_status_t status = vfs()->InstallRemote(vnode(), std::move(c));
  completer.Reply(status);
}

void DirectoryConnection::MountAndCreate(MountAndCreateRequestView request,
                                         MountAndCreateCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminMountAndCreate] our options: ", options());

  if (!options().rights.admin) {
    // Note: this is best-effort, and would fail if the remote endpoint does not speak the
    // |fuchsia.io/DirectoryAdmin| protocol.
    fidl::ClientEnd<fuchsia_io_admin::DirectoryAdmin> remote_admin(request->remote.TakeChannel());
    FuchsiaVfs::UnmountHandle(std::move(remote_admin), zx::time::infinite());
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  zx_status_t status =
      vfs()->MountMkdir(vnode(), std::string_view(request->name.data(), request->name.size()),
                        MountChannel(std::move(request->remote)), request->flags);
  completer.Reply(status);
}

void DirectoryConnection::Unmount(UnmountRequestView request, UnmountCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminUnmount] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  Connection::UnmountAndShutdown(
      [completer = completer.ToAsync()](zx_status_t unmount_status) mutable {
        completer.Reply(unmount_status);
      });
}

void DirectoryConnection::UnmountNode(UnmountNodeRequestView request,
                                      UnmountNodeCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminUnmountNode] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, zx::channel());
    return;
  }
  fidl::ClientEnd<fuchsia_io::Directory> c;
  zx_status_t status = vfs()->UninstallRemote(vnode(), &c);
  completer.Reply(status, std::move(c));
}

void DirectoryConnection::QueryFilesystem(QueryFilesystemRequestView request,
                                          QueryFilesystemCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminQueryFilesystem] our options: ", options());

  fuchsia_io_admin::wire::FilesystemInfo info;
  zx_status_t status = vnode()->QueryFilesystem(&info);
  completer.Reply(
      status, status == ZX_OK
                  ? fidl::ObjectView<fuchsia_io_admin::wire::FilesystemInfo>::FromExternal(&info)
                  : nullptr);
}

void DirectoryConnection::GetDevicePath(GetDevicePathRequestView request,
                                        GetDevicePathCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminGetDevicePath] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, fidl::StringView());
    return;
  }

  if (auto device_path_or = vnode()->GetDevicePath(); device_path_or.is_error()) {
    completer.Reply(device_path_or.error_value(), {});
  } else {
    completer.Reply(ZX_OK, fidl::StringView::FromExternal(device_path_or.value()));
  }
}

void DirectoryConnection::AdvisoryLock(AdvisoryLockRequestView request,
                                       AdvisoryLockCompleter::Sync& completer) {
  zx_koid_t owner = GetChannelOwnerKoid();
  // advisory_lock replies to the completer
  auto async_completer = completer.ToAsync();
  fit::callback<void(zx_status_t)> callback = file_lock::lock_completer_t(
      [lock_completer = std::move(async_completer)](zx_status_t status) mutable {
        auto reply = fidl::ObjectView<int32_t>::FromExternal(&status);
        auto result = fuchsia_io2::wire::AdvisoryLockingAdvisoryLockResult::WithErr(reply);
        lock_completer.Reply(std::move(result));
      });

  advisory_lock(owner, vnode(), false, request->request, std::move(callback));
}
void DirectoryConnection::OnTeardown() {
  auto owner = GetChannelOwnerKoid();
  vnode()->DeleteFileLockInTeardown(owner);
}

}  // namespace internal

}  // namespace fs
