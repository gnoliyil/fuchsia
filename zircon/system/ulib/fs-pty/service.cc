// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fidl/cpp/wire/transaction.h>
#include <lib/fs-pty/service.h>
#include <lib/fs-pty/tty-connection-internal.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <thread>

#include <fbl/algorithm.h>

namespace fs_pty::internal {

void DispatchPtyDeviceMessage(fidl::WireServer<fuchsia_hardware_pty::Device>* interface,
                              fidl::IncomingHeaderAndMessage& msg, fidl::Transaction* txn) {
  fidl::WireDispatch(interface, std::move(msg), txn);
}

// Return ZX_ERR_NOT_SUPPORTED for all of the PTY things we don't actually support
void NullPtyDeviceImpl::OpenClient(OpenClientRequestView request,
                                   OpenClientCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::OpenClient> buf;
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED);
}

void NullPtyDeviceImpl::ClrSetFeature(ClrSetFeatureRequestView request,
                                      ClrSetFeatureCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::ClrSetFeature> buf;
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED, 0);
}

void NullPtyDeviceImpl::GetWindowSize(GetWindowSizeCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::GetWindowSize> buf;
  fuchsia_hardware_pty::wire::WindowSize wsz = {.width = 0, .height = 0};
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED, wsz);
}

void NullPtyDeviceImpl::MakeActive(MakeActiveRequestView request,
                                   MakeActiveCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::MakeActive> buf;
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED);
}

void NullPtyDeviceImpl::ReadEvents(ReadEventsCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::ReadEvents> buf;
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED, 0);
}

void NullPtyDeviceImpl::SetWindowSize(SetWindowSizeRequestView request,
                                      SetWindowSizeCompleter::Sync& completer) {
  fidl::ServerBuffer<fuchsia_hardware_pty::Device::SetWindowSize> buf;
  completer.buffer(buf.view()).Reply(ZX_ERR_NOT_SUPPORTED);
}

// We need to provide these methods because |fuchsia.hardware.pty.Device|
// composes |fuchsia.io.File2|. Assert in all of these, since these should be
// handled by fs::Connection before our HandleFsSpecificMessage() is called.

void NullPtyDeviceImpl::Read(ReadRequestView request, ReadCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Write(WriteRequestView request, WriteCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Reopen(ReopenRequestView request, ReopenCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Clone(CloneRequestView request, CloneCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Close(CloseCompleter::Sync& completer) { ZX_ASSERT(false); }

void NullPtyDeviceImpl::GetConnectionInfo(GetConnectionInfoCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Describe(DescribeCompleter::Sync& completer) { ZX_ASSERT(false); }

void NullPtyDeviceImpl::GetAttr(GetAttrCompleter::Sync& completer) { ZX_ASSERT(false); }

void NullPtyDeviceImpl::ReadAt(ReadAtRequestView request, ReadAtCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::WriteAt(WriteAtRequestView request, WriteAtCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Seek(SeekRequestView request, SeekCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Resize(ResizeRequestView request, ResizeCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::GetBackingMemory(GetBackingMemoryRequestView request,
                                         GetBackingMemoryCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::Sync(SyncCompleter::Sync& completer) { ZX_ASSERT(false); }

void NullPtyDeviceImpl::SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::GetFlags(GetFlagsCompleter::Sync& completer) { ZX_ASSERT(false); }

void NullPtyDeviceImpl::SetFlags(SetFlagsRequestView request, SetFlagsCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

void NullPtyDeviceImpl::QueryFilesystem(QueryFilesystemCompleter::Sync& completer) {
  ZX_ASSERT(false);
}

}  // namespace fs_pty::internal
