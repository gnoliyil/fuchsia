// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.lifecycle.test/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <zircon/errors.h>

#include <vector>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>

#include "src/devices/tests/ddk-lifecycle/test-driver-child.h"

namespace {

using fuchsia_device_lifecycle_test::Lifecycle;
using fuchsia_device_lifecycle_test::TestDevice;

class TestLifecycleDriver;
using DeviceType =
    ddk::Device<TestLifecycleDriver, ddk::Messageable<TestDevice>::Mixin, ddk::ChildPreReleaseable>;

class TestLifecycleDriver : public DeviceType {
 public:
  explicit TestLifecycleDriver(zx_device_t* parent) : DeviceType(parent) {}
  ~TestLifecycleDriver() {}

  zx_status_t Bind() {
    return DdkAdd(ddk::DeviceAddArgs("ddk-lifecycle-test").set_flags(DEVICE_ADD_NON_BINDABLE));
  }

  // Device protocol implementation.
  void DdkChildPreRelease(void* child_ctx);
  void DdkRelease() { delete this; }

  // Device message ops implementation.
  void SubscribeToLifecycle(SubscribeToLifecycleRequestView request,
                            SubscribeToLifecycleCompleter::Sync& completer) override;
  void AddChild(AddChildRequestView request, AddChildCompleter::Sync& completer) override;
  void RemoveChild(RemoveChildRequestView request, RemoveChildCompleter::Sync& completer) override;
  void AsyncRemoveChild(AsyncRemoveChildRequestView request,
                        AsyncRemoveChildCompleter::Sync& completer) override;
  void CompleteUnbind(CompleteUnbindRequestView request,
                      CompleteUnbindCompleter::Sync& completer) override;
  void CompleteChildInit(CompleteChildInitRequestView request,
                         CompleteChildInitCompleter::Sync& completer) override;

 private:
  // Converts the device pointer into an id we can use as a unique identifier.
  uint64_t zxdev_to_id(zx_device_t* dev) { return reinterpret_cast<uint64_t>(dev); }

  fidl::ServerEnd<Lifecycle> lifecycle_server_end_;
  // Child devices added via |AddChild|.
  std::vector<fbl::RefPtr<TestLifecycleDriverChild>> children_;
};

void TestLifecycleDriver::DdkChildPreRelease(void* child_ctx) {
  auto child = reinterpret_cast<TestLifecycleDriverChild*>(child_ctx);
  ZX_ASSERT(child != nullptr);
  auto id = zxdev_to_id(child->zxdev());

  if (lifecycle_server_end_.is_valid()) {
    fidl::Status result = fidl::WireSendEvent(lifecycle_server_end_)->OnChildPreRelease(id);
    ZX_ASSERT_MSG(result.ok(), "%s", result.FormatDescription().c_str());
  }
  // Remove the child from our |children_| vector.
  auto child_matcher = [&](fbl::RefPtr<TestLifecycleDriverChild> child_to_remove) {
    return child_to_remove.get() == child;
  };
  children_.erase(std::remove_if(children_.begin(), children_.end(), child_matcher),
                  children_.end());
}

void TestLifecycleDriver::AddChild(AddChildRequestView request,
                                   AddChildCompleter::Sync& completer) {
  fbl::RefPtr<TestLifecycleDriverChild> child;
  zx_status_t status = TestLifecycleDriverChild::Create(zxdev(), request->init_complete,
                                                        request->init_status, &child);
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    children_.push_back(child);
    completer.ReplySuccess(zxdev_to_id(child->zxdev()));
  }
}

void TestLifecycleDriver::RemoveChild(RemoveChildRequestView request,
                                      RemoveChildCompleter::Sync& completer) {
  bool found = false;
  for (auto& child : children_) {
    if (zxdev_to_id(child->zxdev()) == request->child_id) {
      // We will remove it from our |children_| vector when we get the child pre-release callback.
      child->DdkAsyncRemove();
      found = true;
      break;
    }
  }
  if (!found) {
    zxlogf(ERROR, "Could not find child: id %lu", request->child_id);
    completer.ReplyError(ZX_ERR_NOT_FOUND);
    return;
  }
  completer.ReplySuccess();
}

void TestLifecycleDriver::AsyncRemoveChild(AsyncRemoveChildRequestView request,
                                           AsyncRemoveChildCompleter::Sync& completer) {
  bool found = false;
  for (auto& child : children_) {
    if (zxdev_to_id(child->zxdev()) == request->child_id) {
      // We will remove it from our |children_| vector when we get the child pre-release callback.
      child->AsyncRemove(
          [completion = completer.ToAsync()]() mutable { completion.ReplySuccess(); });
      found = true;
      break;
    }
  }
  if (!found) {
    zxlogf(ERROR, "Could not find child: id %lu", request->child_id);
    completer.ReplyError(ZX_ERR_NOT_FOUND);
    return;
  }
}

void TestLifecycleDriver::CompleteUnbind(CompleteUnbindRequestView request,
                                         CompleteUnbindCompleter::Sync& completer) {
  for (auto& child : children_) {
    if (zxdev_to_id(child->zxdev()) == request->child_id) {
      // We will remove it from our |children_| vector when we get the child pre-release callback.
      child->CompleteUnbind();
      completer.ReplySuccess();
      return;
    }
  }
  zxlogf(ERROR, "Could not find child: id %lu", request->child_id);
  completer.ReplyError(ZX_ERR_NOT_FOUND);
  return;
}

void TestLifecycleDriver::CompleteChildInit(CompleteChildInitRequestView request,
                                            CompleteChildInitCompleter::Sync& completer) {
  zx_status_t status = ZX_ERR_NOT_FOUND;
  for (auto& child : children_) {
    if (zxdev_to_id(child->zxdev()) == request->child_id) {
      status = child->CompleteInit();
      break;
    }
  }
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to complete child init: id %lu", request->child_id);
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess();
}

void TestLifecycleDriver::SubscribeToLifecycle(SubscribeToLifecycleRequestView request,
                                               SubscribeToLifecycleCompleter::Sync& completer) {
  // Currently we only care about supporting one client.
  if (lifecycle_server_end_.is_valid()) {
    completer.ReplyError(ZX_ERR_ALREADY_BOUND);
  } else {
    lifecycle_server_end_ = std::move(request->client);
    completer.ReplySuccess();
  }
}

zx_status_t TestLifecycleBind(void* ctx, zx_device_t* device) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<TestLifecycleDriver>(&ac, device);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto status = dev->Bind();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    [[maybe_unused]] auto ptr = dev.release();
  }
  return status;
}

zx_driver_ops_t driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestLifecycleBind;
  return ops;
}();

}  // namespace

ZIRCON_DRIVER(TestLifecycle, driver_ops, "zircon", "0.1");
