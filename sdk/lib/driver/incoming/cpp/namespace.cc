// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/driver/incoming/cpp/namespace.h>

namespace fdf {

zx::result<Namespace> Namespace::Create(
    fidl::VectorView<fuchsia_component_runner::wire::ComponentNamespaceEntry>& entries) {
  fdio_ns_t* incoming;
  if (zx_status_t status = fdio_ns_create(&incoming); status != ZX_OK) {
    return zx::error(status);
  }

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }

  Namespace self(incoming, std::move(endpoints->client));
  for (auto& entry : entries) {
    std::string path(entry.path().data(), entry.path().size());
    if (zx_status_t status =
            fdio_ns_bind(incoming, path.data(), entry.directory().TakeChannel().release());
        status != ZX_OK) {
      return zx::error(status);
    }
  }
  if (zx_status_t status = fdio_ns_service_connect(self.incoming_, "/svc",
                                                   endpoints->server.TakeChannel().release());
      status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(self));
}

zx::result<Namespace> Namespace::Create(
    std::vector<fuchsia_component_runner::ComponentNamespaceEntry>& entries) {
  fdio_ns_t* ns;
  if (zx_status_t status = fdio_ns_create(&ns); status != ZX_OK) {
    return zx::error(status);
  }

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }

  Namespace self(ns, std::move(endpoints->client));
  for (auto& entry : entries) {
    auto path = entry.path();
    auto directory = std::move(entry.directory());
    ZX_ASSERT(path.has_value());
    ZX_ASSERT(directory.has_value());
    if (zx_status_t status =
            fdio_ns_bind(ns, path.value().data(), directory.value().TakeChannel().release());
        status != ZX_OK) {
      return zx::error(status);
    }
  }
  if (zx_status_t status = fdio_ns_service_connect(self.incoming_, "/svc",
                                                   endpoints->server.TakeChannel().release());
      status != ZX_OK) {
    return zx::error(status);
  }

  return zx::ok(std::move(self));
}

Namespace::Namespace(fdio_ns_t* incoming, fidl::ClientEnd<fuchsia_io::Directory> svc_dir)
    : incoming_(incoming), svc_dir_(std::move(svc_dir)) {}

Namespace::~Namespace() {
  if (incoming_) {
    fdio_ns_destroy(incoming_);
  }
}

Namespace::Namespace(Namespace&& other) noexcept
    : incoming_(other.incoming_), svc_dir_(std::move(other.svc_dir_)) {
  other.incoming_ = nullptr;
}

Namespace& Namespace::operator=(Namespace&& other) noexcept {
  this->~Namespace();
  incoming_ = other.incoming_;
  other.incoming_ = nullptr;
  svc_dir_ = std::move(other.svc_dir_);
  return *this;
}

zx::result<> Namespace::Open(const char* path, fuchsia_io::wire::OpenFlags flags,
                             zx::channel server_end) const {
  zx_status_t status =
      fdio_ns_open(incoming_, path, static_cast<uint32_t>(flags), server_end.release());
  return zx::make_result(status);
}

}  // namespace fdf
