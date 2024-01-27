// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.interop.test/cpp/wire.h>
#include <lib/driver/component/cpp/driver_cpp.h>

namespace ft = fuchsia_interop_test;

namespace {

class LeafDriver : public fdf::DriverBase {
 public:
  LeafDriver(fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : fdf::DriverBase("leaf", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    auto waiter = incoming()->Connect<ft::Waiter>();
    if (waiter.is_error()) {
      node().reset();
      return waiter.take_error();
    }
    const fidl::WireSharedClient<ft::Waiter> client{std::move(waiter.value()), dispatcher()};
    auto result = client.sync()->Ack();
    if (!result.ok()) {
      node().reset();
      return zx::error(result.error().status());
    }

    return zx::ok();
  }
};

}  // namespace

FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(fdf::Lifecycle<LeafDriver>);
