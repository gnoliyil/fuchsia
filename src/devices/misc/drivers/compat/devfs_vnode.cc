// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devfs_vnode.h"

#include <lib/ddk/device.h>

#include <string_view>

#include <fbl/string_buffer.h>

#include "fbl/ref_ptr.h"
#include "src/devices/bin/driver_host/simple_binding.h"
#include "src/devices/lib/fidl/transaction.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"

namespace {

// Utility class for dispatching messages to a device.
class FidlDispatcher : public fidl::internal::IncomingMessageDispatcher {
 public:
  explicit FidlDispatcher(fbl::RefPtr<DevfsVnode> node) : node_(std::move(node)) {}

  static void CreateAndBind(fbl::RefPtr<DevfsVnode> node, async_dispatcher_t* dispatcher,
                            zx::channel channel);

 private:
  void dispatch_message(fidl::IncomingHeaderAndMessage&& msg, ::fidl::Transaction* txn,
                        fidl::internal::MessageStorageViewBase* storage_view) final;
  fbl::RefPtr<DevfsVnode> node_;
};

void FidlDispatcher::CreateAndBind(fbl::RefPtr<DevfsVnode> node, async_dispatcher_t* dispatcher,
                                   zx::channel channel) {
  auto fidl = std::make_unique<FidlDispatcher>(std::move(node));
  auto fidl_ptr = fidl.get();

  // Create the binding. We pass the FidlDispatcher's pointer into the unbound
  // function so it stays alive as long as the binding.
  auto binding = std::make_unique<devfs::SimpleBinding>(dispatcher, std::move(channel), fidl_ptr,
                                                        [fidl = std::move(fidl)](void*) {});
  devfs::BeginWait(&binding);
}

void FidlDispatcher::dispatch_message(fidl::IncomingHeaderAndMessage&& msg,
                                      ::fidl::Transaction* txn,
                                      fidl::internal::MessageStorageViewBase* storage_view) {
  // If the device is unbound it shouldn't receive messages so close the channel.
  if (!node_->dev()) {
    txn->Close(ZX_ERR_IO_NOT_PRESENT);
    return;
  }

  fidl_incoming_msg_t c_msg = std::move(msg).ReleaseToEncodedCMessage();
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  zx_status_t status = node_->dev()->MessageOp(&c_msg, ddk_txn.Txn());
  if (status != ZX_OK && status != ZX_ERR_ASYNC) {
    // Close the connection on any error
    txn->Close(status);
  }
}

}  // namespace

void DevfsVnode::ConnectToDeviceFidl(zx::channel server) {
  FidlDispatcher::CreateAndBind(fbl::RefPtr(this), dispatcher_, std::move(server));
}

zx_status_t DevfsVnode::GetAttributes(fs::VnodeAttributes* a) {
  a->mode = V_TYPE_CDEV | V_IRUSR | V_IWUSR;
  a->content_size = 0;
  a->link_count = 1;
  return ZX_OK;
}

fs::VnodeProtocolSet DevfsVnode::GetProtocols() const { return fs::VnodeProtocol::kFile; }

zx_status_t DevfsVnode::GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                               fs::VnodeRepresentation* info) {
  if (protocol == fs::VnodeProtocol::kFile) {
    *info = fs::VnodeRepresentation::File{};
    return ZX_OK;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

void DevfsVnode::HandleFsSpecificMessage(fidl::IncomingHeaderAndMessage& msg,
                                         fidl::Transaction* txn) {
  ::fidl::DispatchResult dispatch_result =
      fidl::WireTryDispatch<fuchsia_device::Controller>(dev_, msg, txn);
  if (dispatch_result == ::fidl::DispatchResult::kFound) {
    return;
  }

  fidl_incoming_msg_t c_msg = std::move(msg).ReleaseToEncodedCMessage();
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  zx_status_t status = dev_->MessageOp(&c_msg, ddk_txn.Txn());
  if (status != ZX_OK && status != ZX_ERR_ASYNC) {
    // Close the connection on any error
    txn->Close(status);
  }
}
