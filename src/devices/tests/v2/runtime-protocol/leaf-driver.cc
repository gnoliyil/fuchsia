// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.driver.framework/cpp/fidl.h>
#include <fidl/fuchsia.runtime.test/cpp/driver/fidl.h>
#include <fidl/fuchsia.runtime.test/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/driver/component/cpp/driver_cpp.h>
#include <lib/driver/incoming/cpp/namespace.h>
#include <lib/driver/logging/cpp/logger.h>
#include <lib/fdf/cpp/arena.h>
#include <lib/fdf/cpp/channel.h>
#include <lib/fdf/cpp/protocol.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/scope.h>

#include "src/lib/fidl/cpp/contrib/fpromise/client.h"

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf

namespace ft = fuchsia_runtime_test;

using fpromise::error;
using fpromise::ok;
using fpromise::promise;
using fpromise::result;

namespace {

class LeafDriver : public fdf::DriverBase {
 public:
  LeafDriver(fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : DriverBase("leaf", std::move(start_args), std::move(driver_dispatcher)),
        executor_(dispatcher()),
        node_(fidl::WireSharedClient(std::move(node()), dispatcher())) {}

  zx::result<> Start() override {
    auto setter_client = context().incoming()->Connect<ft::Service::Setter>();
    if (setter_client.is_error()) {
      return setter_client.take_error();
    }
    setter_.Bind(*std::move(setter_client), driver_dispatcher()->get());

    auto getter_client = context().incoming()->Connect<ft::Service::Getter>();
    if (getter_client.is_error()) {
      return getter_client.take_error();
    }
    getter_.Bind(*std::move(getter_client), driver_dispatcher()->get());

    auto waiter_client = context().incoming()->Connect<ft::Waiter>();
    if (waiter_client.is_error()) {
      return waiter_client.take_error();
    }
    waiter_.Bind(*std::move(waiter_client), dispatcher());

    auto task = CallSetter()
                    .and_then(fit::bind_member(this, &LeafDriver::CallGetter))
                    .and_then(fit::bind_member(this, &LeafDriver::CallAck))
                    .or_else(fit::bind_member(this, &LeafDriver::UnbindNode))
                    .wrap_with(scope_);
    executor_.schedule_task(std::move(task));
    return zx::ok();
  }

 private:
  // Magic number that we will set and retrieve from our parent via driver runtime protocols.
  static constexpr const uint32_t kMagic = 123456;

  fpromise::promise<void, zx_status_t> CallSetter() {
    fdf::Arena arena('TEST');

    ft::WrappedValue wrapped_value;
    wrapped_value.value() = kMagic;
    auto arg = fidl::ToWire(arena, wrapped_value);

    fpromise::bridge<void, zx_status_t> bridge;
    auto callback = [completer = std::move(bridge.completer)](
                        fdf::WireUnownedResult<ft::Setter::Set>& result) mutable {
      if (!result.ok()) {
        completer.complete_error(result.status());
        return;
      }
      completer.complete_ok();
    };
    setter_.buffer(arena)->Set(arg).ThenExactlyOnce(std::move(callback));
    return bridge.consumer.promise();
  }

  fpromise::promise<void, zx_status_t> CallGetter() {
    return fidl_fpromise::as_promise(getter_->Get())
        .then([&](fpromise::result<ft::GetterGetResponse, fidl::ErrorsIn<ft::Getter::Get>>& result)
                  -> fpromise::promise<void, zx_status_t> {
          if (result.is_error()) {
            zx_status_t status = result.error().is_domain_error()
                                     ? result.error().domain_error()
                                     : result.error().framework_error().status();
            return fpromise::make_error_promise(status);
          }
          if (result.value() != kMagic) {
            return fpromise::make_error_promise(ZX_ERR_BAD_STATE);
          }
          return fpromise::make_result_promise<void, zx_status_t>(fpromise::ok());
        });
  }

  result<void, zx_status_t> CallAck() {
    [[maybe_unused]] auto result = waiter_->Ack();
    return ok();
  }

  result<> UnbindNode(const zx_status_t& status) {
    FDF_LOG(ERROR, "Failed to start leaf driver: %s", zx_status_get_string(status));
    node_.AsyncTeardown();
    return ok();
  }

  async::Executor executor_;
  fidl::WireSharedClient<fdf::Node> node_;

  // This is specifically a |WireClient| so we can test using |fdf::Arena|.
  fdf::WireClient<ft::Setter> setter_;
  fdf::Client<ft::Getter> getter_;
  fidl::Client<ft::Waiter> waiter_;

  // NOTE: Must be the last member.
  fpromise::scope scope_;
};

}  // namespace

FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(fdf::Lifecycle<LeafDriver>);
