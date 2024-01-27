// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYS_SERVICE_CPP_SERVICE_HANDLER_H_
#define LIB_SYS_SERVICE_CPP_SERVICE_HANDLER_H_

#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/service_handler_base.h>

namespace vfs {
class Service;
class PseudoDir;
}  // namespace vfs

namespace sys {

// A handler for an instance of a service.
class ServiceHandler : public fidl::ServiceHandlerBase {
 public:
  ServiceHandler() noexcept;
  ServiceHandler(ServiceHandler&&);

  ~ServiceHandler() override;

  // Add a |member| to the instance, which will is handled by |handler|.
  zx_status_t AddMember(std::string member, MemberHandler handler) const override;

  // Take the underlying pseudo-directory from the service handler.
  //
  // Once taken, the service handler is no longer safe to use.
  std::unique_ptr<vfs::PseudoDir> TakeDirectory() { return std::move(dir_); }

 private:
  std::unique_ptr<vfs::PseudoDir> dir_;
};

}  // namespace sys

#endif  // LIB_SYS_SERVICE_CPP_SERVICE_HANDLER_H_
