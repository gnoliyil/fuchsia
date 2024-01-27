// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/vfs.h>
#include <zircon/status.h>

#include <string>
#include <string_view>

#include <runtests-utils/service-proxy-dir.h>

namespace fio = fuchsia_io;

namespace runtests {

ServiceProxyDir::ServiceProxyDir(fidl::ClientEnd<fio::Directory> proxy_dir)
    : proxy_dir_(std::move(proxy_dir)) {}

void ServiceProxyDir::AddEntry(std::string name, fbl::RefPtr<fs::Vnode> node) {
  std::lock_guard lock(lock_);
  entries_[std::move(name)] = std::move(node);
}

zx_status_t ServiceProxyDir::GetAttributes(fs::VnodeAttributes* attr) {
  *attr = fs::VnodeAttributes();
  attr->mode = V_TYPE_DIR | V_IRUSR;
  attr->inode = fio::wire::kInoUnknown;
  attr->link_count = 1;
  return ZX_OK;
}

zx_status_t ServiceProxyDir::GetNodeInfoForProtocol([[maybe_unused]] fs::VnodeProtocol protocol,
                                                    [[maybe_unused]] fs::Rights rights,
                                                    fs::VnodeRepresentation* info) {
  *info = fs::VnodeRepresentation::Directory();
  return ZX_OK;
}

fs::VnodeProtocolSet ServiceProxyDir::GetProtocols() const { return fs::VnodeProtocol::kDirectory; }

zx_status_t ServiceProxyDir::Lookup(std::string_view name, fbl::RefPtr<fs::Vnode>* out) {
  auto entry_name = std::string(name.data(), name.length());

  std::lock_guard lock(lock_);
  auto entry = entries_.find(entry_name);
  if (entry != entries_.end()) {
    *out = entry->second;
    return ZX_OK;
  }

  entries_.emplace(
      entry_name,
      *out =
          fbl::MakeRefCounted<fs::Service>([this, entry_name](fidl::ServerEnd<fio::Node> request) {
            return fidl::WireCall(proxy_dir_)
                ->Open(fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kRightWritable,
                       {}, fidl::StringView::FromExternal(entry_name), std::move(request))
                .status();
          }));

  return ZX_OK;
}

}  // namespace runtests
