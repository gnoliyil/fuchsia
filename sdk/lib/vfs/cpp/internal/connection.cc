// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fdio/vfs.h>
#include <lib/vfs/cpp/flags.h>
#include <lib/vfs/cpp/internal/connection.h>
#include <lib/vfs/cpp/internal/node.h>

namespace vfs {
namespace internal {

Connection::Connection(fuchsia::io::OpenFlags flags) : flags_(flags) {}

Connection::~Connection() = default;

void Connection::Clone(Node* vn, fuchsia::io::OpenFlags flags, zx::channel request,
                       async_dispatcher_t* dispatcher) {
  vn->Clone(flags, flags_, std::move(request), dispatcher);
}

void Connection::Close(Node* vn, fuchsia::io::Node::CloseCallback callback) {
  zx_status_t status = vn->PreClose(this);
  if (status == ZX_OK) {
    callback(fpromise::ok());
  } else {
    callback(fpromise::error(status));
  }
  vn->Close(this);
  // |this| is destroyed at this point.
}

void Connection::Describe(Node* vn, fit::function<void(fuchsia::io::NodeInfoDeprecated)> callback) {
  fuchsia::io::NodeInfoDeprecated info{};
  vn->Describe(&info);
  if (info.has_invalid_tag()) {
    vn->Close(this);
  } else {
    callback(std::move(info));
  }
}

zx_status_t Connection::Bind(zx::channel request, async_dispatcher_t* dispatcher) {
  auto status = BindInternal(std::move(request), dispatcher);
  if (status == ZX_OK && Flags::ShouldDescribe(flags_)) {
    SendOnOpenEvent(status);
  }  // can't send status as binding failed and request object is gone.
  return status;
}

void Connection::Sync(Node* vn, fuchsia::io::Node::SyncCallback callback) {
  // TODO: Check flags.
  zx_status_t status = vn->Sync();
  if (status == ZX_OK) {
    callback(fpromise::ok());
  } else {
    callback(fpromise::error(status));
  }
}

void Connection::GetAttr(Node* vn, fuchsia::io::Node::GetAttrCallback callback) {
  // TODO: Check flags.
  fuchsia::io::NodeAttributes attributes{};
  zx_status_t status = vn->GetAttr(&attributes);
  callback(status, attributes);
}

void Connection::SetAttr(Node* vn, fuchsia::io::NodeAttributeFlags flags,
                         fuchsia::io::NodeAttributes attributes,
                         fuchsia::io::Node::SetAttrCallback callback) {
  if (Flags::IsNodeReference(flags_)) {
    callback(ZX_ERR_BAD_HANDLE);
  } else {
    // TODO: Check flags.
    callback(vn->SetAttr(flags, attributes));
  }
}

std::unique_ptr<fuchsia::io::NodeInfoDeprecated> Connection::NodeInfoIfStatusOk(
    Node* vn, zx_status_t status) {
  std::unique_ptr<fuchsia::io::NodeInfoDeprecated> node_info;
  if (status == ZX_OK) {
    node_info = std::make_unique<fuchsia::io::NodeInfoDeprecated>();
    vn->Describe(node_info.get());
  }
  return node_info;
}

}  // namespace internal
}  // namespace vfs
