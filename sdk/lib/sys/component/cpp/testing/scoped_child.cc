
// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/component/decl/cpp/fidl.h>
#include <fuchsia/io/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <lib/sys/component/cpp/testing/internal/errors.h>
#include <lib/sys/component/cpp/testing/internal/realm.h>
#include <lib/sys/component/cpp/testing/scoped_child.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/cpp/service_directory.h>
#include <zircon/assert.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>
#include <optional>
#include <random>

namespace component_testing {

namespace {

std::size_t random_unsigned() {
  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<std::size_t> distribution;
  return distribution(generator);
}
}  // namespace

ScopedChild ScopedChild::New(fuchsia::component::RealmSyncPtr realm_proxy, std::string collection,
                             std::string url) {
  std::string name = "auto-" + std::to_string(random_unsigned());
  return New(std::move(realm_proxy), std::move(collection), std::move(name), std::move(url));
}

ScopedChild ScopedChild::New(fuchsia::component::RealmSyncPtr realm_proxy, std::string collection,
                             std::string name, std::string url) {
  internal::CreateChild(realm_proxy.get(), collection, name, std::move(url));
  fuchsia::io::DirectorySyncPtr exposed_dir;
  exposed_dir.Bind(internal::OpenExposedDir(
      realm_proxy.get(),
      fuchsia::component::decl::ChildRef{.name = name, .collection = collection}));
  return ScopedChild(sys::ServiceDirectory::CreateFromNamespace(),
                     fuchsia::component::decl::ChildRef{.name = name, .collection = collection},
                     std::move(exposed_dir));
}

ScopedChild ScopedChild::New(std::string collection, std::string name, std::string url,
                             std::shared_ptr<sys::ServiceDirectory> svc) {
  fuchsia::component::RealmSyncPtr realm_proxy;
  svc->Connect(realm_proxy.NewRequest());
  internal::CreateChild(realm_proxy.get(), collection, name, std::move(url));
  fuchsia::io::DirectorySyncPtr exposed_dir;
  exposed_dir.Bind(internal::OpenExposedDir(
      realm_proxy.get(),
      fuchsia::component::decl::ChildRef{.name = name, .collection = collection}));
  return ScopedChild(svc,
                     fuchsia::component::decl::ChildRef{.name = name, .collection = collection},
                     std::move(exposed_dir));
}

ScopedChild ScopedChild::New(std::string collection, std::string url,
                             std::shared_ptr<sys::ServiceDirectory> svc) {
  std::string name = "auto-" + std::to_string(random_unsigned());
  return New(std::move(collection), std::move(name), std::move(url), std::move(svc));
}

ScopedChild::ScopedChild(std::shared_ptr<sys::ServiceDirectory> svc,
                         fuchsia::component::decl::ChildRef child_ref,
                         fuchsia::io::DirectorySyncPtr exposed_dir)
    : svc_(std::move(svc)),
      child_ref_(std::move(child_ref)),
      exposed_dir_(std::move(exposed_dir)) {}

ScopedChild::~ScopedChild() {
  if (has_moved_) {
    return;
  }

  if (dispatcher_) {
    fuchsia::component::RealmPtr async_realm_proxy;
    svc_->Connect(async_realm_proxy.NewRequest(dispatcher_));
    internal::DestroyChild(async_realm_proxy.get(), child_ref_,
                           [callback = std::move(teardown_callback_),
                            // We have to move the proxy into the callback so that the channel
                            // connection doesn't prematurely close.
                            _proxy = std::move(async_realm_proxy)](
                               fuchsia::component::Realm_DestroyChild_Result result) {
                             if (callback == nullptr) {
                               return;
                             }

                             if (result.is_err()) {
                               callback(result.err());
                             } else {
                               callback(cpp17::nullopt);
                             }
                           });
  } else {
    fuchsia::component::RealmSyncPtr sync_realm_proxy;
    svc_->Connect(sync_realm_proxy.NewRequest());
    internal::DestroyChild(sync_realm_proxy.get(), child_ref_);
  }
}

ScopedChild::ScopedChild(ScopedChild&& other) noexcept
    : svc_(std::move(other.svc_)),
      child_ref_(std::move(other.child_ref_)),
      exposed_dir_(std::move(other.exposed_dir_)),
      dispatcher_(other.dispatcher_),
      teardown_callback_(std::move(other.teardown_callback_)) {
  other.has_moved_ = true;
}

ScopedChild& ScopedChild::operator=(ScopedChild&& other) noexcept {
  this->svc_ = std::move(other.svc_);
  this->child_ref_ = std::move(other.child_ref_);
  this->exposed_dir_ = std::move(other.exposed_dir_);
  this->dispatcher_ = other.dispatcher_;
  this->teardown_callback_ = std::move(other.teardown_callback_);
  other.has_moved_ = true;
  return *this;
}

void ScopedChild::MakeTeardownAsync(async_dispatcher_t* dispatcher, TeardownCallback callback) {
  if (dispatcher == nullptr) {
    dispatcher = async_get_default_dispatcher();
  }

  dispatcher_ = dispatcher;
  teardown_callback_ = std::move(callback);
}

zx_status_t ScopedChild::Connect(const std::string& interface_name, zx::channel request) const {
  return fdio_service_connect_at(exposed_dir_.unowned_channel()->get(), interface_name.c_str(),
                                 request.release());
}

std::string ScopedChild::GetChildName() const { return child_ref_.name; }

const fuchsia::io::DirectorySyncPtr& ScopedChild::exposed() const { return exposed_dir_; }

}  // namespace component_testing
