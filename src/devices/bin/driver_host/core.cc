// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fit/defer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/errors.h>
#include <zircon/listnode.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <array>
#include <atomic>
#include <new>
#include <utility>

#include <ddktl/resume-txn.h>
#include <fbl/auto_lock.h>

#include "src/devices/bin/driver_host/driver_host.h"
#include "src/devices/bin/driver_host/log.h"
#include "src/devices/lib/log/log.h"

namespace fio = fuchsia_io;

using fuchsia_device::wire::DevicePowerState;
using fuchsia_device_manager::wire::SystemPowerState;

namespace internal {

static thread_local internal::BindContext* g_bind_context;
static thread_local CreationContext* g_creation_context;

// The bind and creation contexts is setup before the bind() or
// create() ops are invoked to provide the ability to sanity check the
// required DeviceAdd() operations these hooks should be making.
void set_bind_context(internal::BindContext* ctx) { g_bind_context = ctx; }

void set_creation_context(CreationContext* ctx) {
  ZX_DEBUG_ASSERT(!ctx || ctx->coordinator_client);
  g_creation_context = ctx;
}

}  // namespace internal

static void default_unbind(void* ctx) {}
static void default_suspend(void* ctx, uint8_t requested_state, bool enable_wake,
                            uint8_t suspend_reason) {}
static void default_resume(void* ctx, uint32_t requested_state) {}
static void default_release(void* ctx) {}

static zx_status_t default_set_performance_state(void* ctx, uint32_t requested_state,
                                                 uint32_t* out_state) {
  return ZX_ERR_NOT_SUPPORTED;
}
static zx_status_t default_rxrpc(void* ctx, zx_handle_t channel) { return ZX_ERR_NOT_SUPPORTED; }

static zx_status_t default_message(void* ctx, fidl_incoming_msg_t* msg, device_fidl_txn_t* txn) {
  fidl_message_header_t* hdr = (fidl_message_header_t*)msg->bytes;
  LOGF(WARNING, "Unsupported FIDL protocol (ordinal %#16lx)", hdr->ordinal);
  FidlHandleCloseMany(msg->handles, msg->num_handles);
  return ZX_ERR_NOT_SUPPORTED;
}

static void default_child_pre_release(void* ctx, void* child_ctx) {}

static zx_status_t default_service_connect(void* ctx, const char* service_name,
                                           fdf_handle_t channel) {
  return ZX_ERR_NOT_SUPPORTED;
}

const zx_protocol_device_t internal::kDeviceDefaultOps = []() {
  zx_protocol_device_t ops = {};
  ops.unbind = default_unbind;
  ops.release = default_release;
  ops.suspend = default_suspend;
  ops.resume = default_resume;
  ops.rxrpc = default_rxrpc;
  ops.message = default_message;
  ops.set_performance_state = default_set_performance_state;
  ops.child_pre_release = default_child_pre_release;
  ops.service_connect = default_service_connect;
  return ops;
}();

[[noreturn]] static void device_invalid_fatal(void* ctx) {
  LOGF(FATAL, "Device used after destruction");
  __builtin_trap();
}

static zx_protocol_device_t device_invalid_ops = []() {
  zx_protocol_device_t ops = {};
  ops.unbind = +[](void* ctx) { device_invalid_fatal(ctx); };
  ops.suspend = +[](void* ctx, uint8_t requested_state, bool enable_wake, uint8_t suspend_reason) {
    device_invalid_fatal(ctx);
  };
  ops.resume = +[](void* ctx, uint32_t) { device_invalid_fatal(ctx); };
  ops.release = +[](void* ctx) { device_invalid_fatal(ctx); };
  ops.rxrpc = +[](void* ctx, zx_handle_t) -> zx_status_t { device_invalid_fatal(ctx); };
  ops.message = +[](void* ctx, fidl_incoming_msg_t*, device_fidl_txn_t*) -> zx_status_t {
    device_invalid_fatal(ctx);
  };
  ops.set_performance_state =
      +[](void* ctx, uint32_t requested_state, uint32_t* out_state) -> zx_status_t {
    device_invalid_fatal(ctx);
  };
  ops.service_connect =
      +[](void* ctx, const char*, fdf_handle_t) -> zx_status_t { device_invalid_fatal(ctx); };
  return ops;
}();

void DriverHostContext::DeviceDestroy(zx_device_t* dev) {
  inspect_.DeviceDestroyStats().Update();

  // ensure any ops will be fatal
  dev->set_ops(&device_invalid_ops);

  dev->magic = 0xdeaddeaddeaddead;

  // ensure all pointers are invalid
  dev->set_ctx(nullptr);
  dev->set_parent(nullptr);
  dev->FreeInspect();
  dev->driver_ref_.Destroy();
  dev->driver = nullptr;
  {
    fbl::AutoLock guard(&dev->proxy_ios_lock);
    dev->proxy_ios = nullptr;
  }

  // Defer destruction to help catch use-after-free and also
  // so the compiler can't (easily) optimize away the poisoning
  // we do above.
  ZX_DEBUG_ASSERT(!fbl::InContainer<zx_device::ChildrenListTag>(*dev));
  dead_devices_.push_back(dev);

  if (dead_devices_count_ == DEAD_DEVICE_MAX) {
    zx_device_t* to_delete = dead_devices_.pop_front();
    delete to_delete;
  } else {
    dead_devices_count_++;
  }
}

void DriverHostContext::FinalizeDyingDevices() {
  // Early exit if there's no work
  if (defer_device_list_.is_empty()) {
    return;
  }

  // Otherwise we snapshot the list
  auto list = std::move(defer_device_list_);

  // We detach all the devices from their parents list-of-children
  // while under the DM lock to avoid an enumerator starting to mutate
  // things before we're done detaching them.
  for (auto& dev : list) {
    if (dev.parent()) {
      dev.parent()->remove_child(dev);
    }
  }

  // Then we can get to the actual final teardown where we have
  // to drop the lock to call the callback
  zx_device* dev;
  while ((dev = list.pop_front()) != nullptr) {
    // invoke release op
    if (dev->flags() & DEV_FLAG_ADDED) {
      if (dev->parent()) {
        api_lock_.Release();
        dev->parent()->ChildPreReleaseOp(dev->ctx());
        api_lock_.Acquire();
      }

      // Shut down the dispatcher if this is the last device.
      if (dev->driver->device_count() == 1) {
        // Drop the api_lock_ *before* stopping the dispatcher since otherwise we can get deadlocks
        // because anything currently running on the dispatcher might need api_lock_.
        api_lock_.Release();
        dev->driver->StopDispatcher();
        dev->ReleaseSyncOp();
        api_lock_.Acquire();
      } else {
        api_lock_.Release();
        dev->ReleaseOp();
        api_lock_.Acquire();
      }
    }

    if (dev->parent()) {
      zx_device_t& parent = *dev->parent();
      // When all the children are gone, complete the pending unbind request.
      if ((!(parent.flags() & DEV_FLAG_DEAD)) && parent.children().is_empty()) {
        if (auto unbind_children = parent.take_unbind_children_conn(); unbind_children) {
          unbind_children(ZX_OK);
        }
      }
      // If the parent wants rebinding when its children are gone,
      // And the parent is not dead, And this was the last child...
      if ((parent.flags() & DEV_FLAG_WANTS_REBIND) && (!(parent.flags() & DEV_FLAG_DEAD)) &&
          parent.children().is_empty()) {
        // Clear the wants rebind flag and request the rebind
        parent.unset_flag(DEV_FLAG_WANTS_REBIND);
        zx_status_t status = DeviceBind(dev->parent(), parent.get_rebind_drv_name().c_str());
        if (status != ZX_OK) {
          parent.call_rebind_conn_if_exists(status);
        }
      }

      dev->set_parent(nullptr);
    }

    // destroy/deallocate the device
    DeviceDestroy(dev);
  }
}

zx_status_t DriverHostContext::DeviceValidate(const fbl::RefPtr<zx_device_t>& dev) {
  if (dev == nullptr) {
    LOGF(ERROR, "Invalid device");
    return ZX_ERR_INVALID_ARGS;
  }
  if (dev->flags() & DEV_FLAG_ADDED) {
    LOGD(ERROR, *dev, "Already added device %p", dev.get());
    return ZX_ERR_BAD_STATE;
  }
  if (dev->magic != DEV_MAGIC) {
    LOGD(ERROR, *dev, "Invalid signature for device %p: %#lx", dev.get(), dev->magic);
    return ZX_ERR_BAD_STATE;
  }
  if (dev->ops() == nullptr) {
    LOGD(ERROR, *dev, "Invalid ops for device %p", dev.get());
    return ZX_ERR_INVALID_ARGS;
  }
  if ((dev->protocol_id() == ZX_PROTOCOL_ROOT)) {
    LOGD(ERROR, *dev, "Invalid protocol for device %p: %#x", dev.get(), dev->protocol_id());
    // These protocols is only allowed for the special
    // singleton misc or root parent devices.
    return ZX_ERR_INVALID_ARGS;
  }
  // devices which do not declare a primary protocol
  // are implied to be misc devices
  if (dev->protocol_id() == 0) {
    dev->set_protocol_id(ZX_PROTOCOL_MISC);
  }

  return ZX_OK;
}

namespace internal {

namespace {

#define REMOVAL_BAD_FLAGS (DEV_FLAG_DEAD | DEV_FLAG_BUSY | DEV_FLAG_MULTI_BIND)

const char* removal_problem(uint32_t flags) {
  if (flags & DEV_FLAG_DEAD) {
    return "already dead";
  }
  if (flags & DEV_FLAG_BUSY) {
    return "being created";
  }
  if (flags & DEV_FLAG_MULTI_BIND) {
    return "multi-bind-able device";
  }
  return "?";
}

}  // namespace

uint32_t get_perf_state(const fbl::RefPtr<zx_device>& dev, uint32_t requested_perf_state) {
  // Give preference to the performance state that is explicitly for this device.
  if (dev->current_performance_state() != fuchsia_device::wire::kDevicePerformanceStateP0) {
    return dev->current_performance_state();
  }
  return requested_perf_state;
}

}  // namespace internal

zx_status_t DriverHostContext::DeviceCreate(fbl::RefPtr<Driver> drv, const char* name, void* ctx,
                                            const zx_protocol_device_t* ops,
                                            fbl::RefPtr<zx_device_t>* out) {
  inspect_.DeviceCreateStats().Update();
  if (!drv) {
    LOGF(ERROR, "Cannot find driver");
    return ZX_ERR_INVALID_ARGS;
  }
  std::string device_name;
  if (name == nullptr) {
    LOGF(WARNING, "Invalid name for device");
    device_name = "invalid";
  } else {
    device_name = std::string(name);
  }

  fbl::RefPtr<zx_device> dev;
  zx_status_t status = zx_device::Create(this, device_name, drv, &dev);
  if (status != ZX_OK) {
    return status;
  }

  if (name == nullptr) {
    dev->magic = 0;
  }

  dev->set_ops(ops);

  // TODO(teisenbe): Why do we default to dev.get() here?  Why not just
  // nullptr
  dev->set_ctx(ctx ? ctx : dev.get());
  *out = std::move(dev);
  return ZX_OK;
}

zx_status_t DriverHostContext::DeviceAdd(const fbl::RefPtr<zx_device_t>& dev,
                                         const fbl::RefPtr<zx_device_t>& parent,
                                         device_add_args_t* add_args, zx::vmo inspect,
                                         fidl::ClientEnd<fio::Directory> outgoing_dir) {
  inspect_.DeviceAddStats().Update();
  auto mark_dead = fit::defer([&dev]() {
    if (dev) {
      dev->set_flag(DEV_FLAG_DEAD);
    }
  });

  zx_status_t status;
  if ((status = DeviceValidate(dev)) < 0) {
    return status;
  }
  if (parent == nullptr) {
    LOGD(ERROR, *dev, "Cannot add device %p to invalid parent", dev.get());
    return ZX_ERR_NOT_SUPPORTED;
  }
  if (parent->flags() & DEV_FLAG_DEAD) {
    LOGD(ERROR, *dev, "Cannot add device %p to dead parent %p", dev.get(), parent.get());
    return ZX_ERR_BAD_STATE;
  }

  internal::BindContext* bind_ctx = nullptr;
  internal::CreationContext* creation_ctx = nullptr;

  // If the bind or creation ctx (thread locals) are set, we are in
  // a thread that is handling a bind() or create() callback and if
  // that ctx's parent matches the one provided to add we need to do
  // some additional checking...
  if ((internal::g_bind_context != nullptr) && (internal::g_bind_context->parent == parent)) {
    bind_ctx = internal::g_bind_context;
  }
  if ((internal::g_creation_context != nullptr) &&
      (internal::g_creation_context->parent == parent)) {
    creation_ctx = internal::g_creation_context;
    // create() must create only one child
    if (creation_ctx->child != nullptr) {
      LOGD(ERROR, *dev, "Driver attempted to create multiple proxy devices");
      return ZX_ERR_BAD_STATE;
    }
  }
  VLOGD(1, *dev, "Adding device %p (parent %p)", dev.get(), parent.get());

  dev->set_flag(DEV_FLAG_BUSY);

  // proxy devices are created through this handshake process
  if (creation_ctx) {
    if (dev->flags() & DEV_FLAG_INVISIBLE) {
      LOGD(ERROR, *dev, "Driver attempted to create invisible device in create()");
      return ZX_ERR_INVALID_ARGS;
    }
    dev->set_flag(DEV_FLAG_ADDED);
    dev->unset_flag(DEV_FLAG_BUSY);
    dev->coordinator_client = creation_ctx->coordinator_client.Clone();
    creation_ctx->child = dev;
    mark_dead.cancel();
    return ZX_OK;
  }

  dev->set_parent(parent);

  // attach to our parent
  parent->add_child(dev.get());

  // Add always consumes the handle
  status = DriverManagerAdd(parent, dev, add_args, std::move(inspect), std::move(outgoing_dir));
  if (status < 0) {
    constexpr char kLogFormat[] = "Failed to add device %p to driver_manager: %s";
    if (status == ZX_ERR_PEER_CLOSED) {
      // TODO(https://fxbug.dev/52627): change to an ERROR log once driver
      // manager can shut down gracefully.
      LOGD(WARNING, *dev, kLogFormat, dev.get(), zx_status_get_string(status));
    } else {
      LOGD(ERROR, *dev, kLogFormat, dev.get(), zx_status_get_string(status));
    }

    dev->parent()->remove_child(*dev);
    dev->set_parent(nullptr);

    // since we are under the lock the whole time, we added the node
    // to the tail and then we peeled it back off the tail when we
    // failed, we don't need to interact with the enum lock mechanism
    dev->unset_flag(DEV_FLAG_BUSY);
    return status;
  }
  dev->set_flag(DEV_FLAG_ADDED);
  dev->unset_flag(DEV_FLAG_BUSY);

  // record this device in the bind context if there is one
  if (bind_ctx && (bind_ctx->child == nullptr)) {
    bind_ctx->child = dev;
  }
  mark_dead.cancel();
  return ZX_OK;
}

zx_status_t DriverHostContext::DeviceInit(const fbl::RefPtr<zx_device_t>& dev) {
  if (dev->flags() & DEV_FLAG_INITIALIZING) {
    return ZX_ERR_BAD_STATE;
  }
  // Call dev's init op.
  if (dev->ops()->init) {
    dev->set_flag(DEV_FLAG_INITIALIZING);
    api_lock_.Release();
    dev->InitOp();
    api_lock_.Acquire();
  } else {
    dev->init_cb(ZX_OK);
  }
  return ZX_OK;
}

void DriverHostContext::DeviceInitReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                                        const device_init_reply_args_t* args) {
  if (!(dev->flags() & DEV_FLAG_INITIALIZING)) {
    LOGD(FATAL, *dev, "Device %p cannot reply to init (flags %#x)", dev.get(), dev->flags());
  }
  if (status == ZX_OK) {
    if (args && args->power_states && args->power_state_count != 0) {
      dev->SetPowerStates(args->power_states, args->power_state_count);
    }
    if (args && args->performance_states && (args->performance_state_count != 0)) {
      dev->SetPerformanceStates(args->performance_states, args->performance_state_count);
    }
  }

  if (dev->init_cb == nullptr) {
    LOGD(FATAL, *dev, "Device %p cannot reply to init, no callback set (flags %#x)", dev.get(),
         dev->flags());
  }

  dev->init_cb(status);
  // Device is no longer invisible.
  dev->unset_flag(DEV_FLAG_INVISIBLE);
  // If all children completed intializing,
  // complete pending bind and rebind connections.
  bool complete_bind_rebind = true;
  for (auto& child : dev->parent()->children()) {
    if (child.flags() & DEV_FLAG_INVISIBLE) {
      complete_bind_rebind = false;
    }
  }
  if (complete_bind_rebind && dev->parent()->complete_bind_rebind_after_init()) {
    if (auto bind_conn = dev->parent()->take_bind_conn(); bind_conn) {
      bind_conn(status);
    }
    dev->parent()->call_rebind_conn_if_exists(status);
  }
}

zx_status_t DriverHostContext::DeviceRemove(const fbl::RefPtr<zx_device_t>& dev, bool unbind_self) {
  if (dev->flags() & REMOVAL_BAD_FLAGS) {
    LOGD(ERROR, *dev, "Cannot remove device %p: %s", dev.get(),
         internal::removal_problem(dev->flags()));
    return ZX_ERR_INVALID_ARGS;
  }
  if (dev->flags() & DEV_FLAG_INVISIBLE) {
    // We failed during init and the device is being removed. Complete the pending
    // bind/rebind conn of parent if any.
    if (auto bind_conn = dev->parent()->take_bind_conn(); bind_conn) {
      bind_conn(ZX_ERR_IO);
    }
    dev->parent()->call_rebind_conn_if_exists(ZX_ERR_IO);
  }
  VLOGD(1, *dev, "Device %p is being scheduled for removal", dev.get());
  // Ask the devcoordinator to schedule the removal of this device and its children.
  ScheduleRemove(dev, unbind_self);
  return ZX_OK;
}

zx_status_t DriverHostContext::DeviceCompleteRemoval(const fbl::RefPtr<zx_device_t>& dev) {
  VLOGD(1, *dev, "Device %p is being removed (removal requested)", dev.get());

  // This recovers the leaked reference that happened in device_add_from_driver().
  auto dev_add_ref = fbl::ImportFromRawPtr(dev.get());
  DriverManagerRemove(std::move(dev_add_ref));

  dev->set_flag(DEV_FLAG_DEAD);

  return ZX_OK;
}

zx_status_t DriverHostContext::DeviceUnbind(const fbl::RefPtr<zx_device_t>& dev) {
  enum_lock_acquire();

  if (!(dev->flags() & DEV_FLAG_UNBINDING)) {
    dev->set_flag(DEV_FLAG_UNBINDING);
    // Call dev's unbind op.
    if (dev->ops()->unbind) {
      VLOGD(1, *dev, "Device %p is being unbound", dev.get());
      api_lock_.Release();
      dev->UnbindOp();
      api_lock_.Acquire();
    } else {
      // We should reply to the unbind hook so we don't get stuck.
      dev->unbind_cb(ZX_OK);
    }
  }
  enum_lock_release();
  return ZX_OK;
}

void DriverHostContext::DeviceUnbindReply(const fbl::RefPtr<zx_device_t>& dev) {
  if (dev->flags() & REMOVAL_BAD_FLAGS) {
    LOGD(FATAL, *dev, "Device %p cannot reply to unbind, bad flags: %s", dev.get(),
         internal::removal_problem(dev->flags()));
  }
  if (!(dev->flags() & DEV_FLAG_UNBINDING)) {
    LOGD(FATAL, *dev, "Device %p cannot reply to unbind, not in unbinding state (flags %#x)",
         dev.get(), dev->flags());
  }
  VLOGD(1, *dev, "Device %p unbind completed", dev.get());
  if (dev->unbind_cb == nullptr) {
    LOGD(FATAL, *dev, "Device %p cannot reply to unbind, no callback set (flags %#x)", dev.get(),
         dev->flags());
  }
  if (std::optional<devfs_fidl::DeviceServer>& vnode = dev->vnode; vnode.has_value()) {
    vnode.value().CloseAllConnections(/*callback=*/nullptr);
  }
  dev->unbind_cb(ZX_OK);
}

void DriverHostContext::DeviceSuspendReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                                           uint8_t out_state) {
  if (dev->suspend_cb) {
    dev->suspend_cb(status, out_state);
  } else {
    LOGD(FATAL, *dev, "Device %p cannot reply to suspend, no callback set", dev.get());
  }
}

void DriverHostContext::DeviceResumeReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                                          uint8_t out_power_state, uint32_t out_perf_state) {
  if (dev->resume_cb) {
    if (out_power_state == static_cast<uint8_t>(DevicePowerState::kDevicePowerStateD0)) {
      // Update the current performance state.
      dev->set_current_performance_state(out_perf_state);
    }
    dev->resume_cb(status, out_power_state, out_perf_state);
  } else {
    LOGD(FATAL, *dev, "Device %p cannot reply to resume, no callback set", dev.get());
  }
}

void DriverHostContext::DeviceSystemSuspend(const fbl::RefPtr<zx_device>& dev, uint32_t flags) {
  if (dev->auto_suspend_configured()) {
    dev->ops()->configure_auto_suspend(dev->ctx(), false, DEV_POWER_STATE_D0);
    LOGF(INFO, "System suspend overriding auto suspend for device %p '%s'", dev.get(), dev->name());
  }
  zx_status_t status = ZX_ERR_NOT_SUPPORTED;
  // If new suspend hook is implemented, prefer that.
  if (dev->ops()->suspend) {
    fuchsia_device::wire::SystemPowerStateInfo new_state_info;
    uint8_t suspend_reason = DEVICE_SUSPEND_REASON_SELECTIVE_SUSPEND;

    status = dev->get_dev_power_state_from_mapping(flags, &new_state_info, &suspend_reason);
    if (status == ZX_OK) {
      enum_lock_acquire();
      {
        api_lock_.Release();
        dev->SuspendNewOp(static_cast<uint8_t>(new_state_info.dev_state),
                          new_state_info.wakeup_enable, suspend_reason);
        api_lock_.Acquire();
      }
      enum_lock_release();
      return;
    }
  }

  // If suspend hook is not implemented, do not throw error during system suspend.
  if (status == ZX_ERR_NOT_SUPPORTED) {
    status = ZX_OK;
  }

  dev->suspend_cb(status, 0);
}

void DriverHostContext::DeviceSystemResume(const fbl::RefPtr<zx_device>& dev,
                                           uint32_t target_system_state) {
  if (dev->auto_suspend_configured()) {
    dev->ops()->configure_auto_suspend(dev->ctx(), false, DEV_POWER_STATE_D0);
    LOGF(INFO, "System resume overriding auto suspend for device %p '%s'", dev.get(), dev->name());
  }

  zx_status_t status = ZX_ERR_NOT_SUPPORTED;

  // If new resume hook is implemented, prefer that.
  if (dev->ops()->resume) {
    enum_lock_acquire();
    {
      api_lock_.Release();
      auto& sys_power_states = dev->GetSystemPowerStateMapping();

      // Subtract 1 from `target_system_state` because it uses a 1-based index
      uint32_t performance_state = sys_power_states.at(target_system_state - 1).performance_state;

      uint32_t requested_perf_state = internal::get_perf_state(dev, performance_state);
      dev->ops()->resume(dev->ctx(), requested_perf_state);
      api_lock_.Acquire();
    }
    enum_lock_release();
    return;
  }

  // default_resume() returns ZX_ERR_NOT_SUPPORTED
  if (status == ZX_ERR_NOT_SUPPORTED) {
    status = ZX_OK;
  }
  dev->resume_cb(status, static_cast<uint8_t>(DevicePowerState::kDevicePowerStateD0),
                 fuchsia_device::wire::kDevicePerformanceStateP0);
}

zx_status_t DriverHostContext::DeviceSetPerformanceState(const fbl::RefPtr<zx_device>& dev,
                                                         uint32_t requested_state,
                                                         uint32_t* out_state) {
  if (!(dev->IsPerformanceStateSupported(requested_state))) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (dev->ops()->set_performance_state) {
    zx_status_t status = dev->ops()->set_performance_state(dev->ctx(), requested_state, out_state);
    if (!(dev->IsPerformanceStateSupported(*out_state))) {
      LOGD(FATAL, *dev,
           "Device %p 'set_performance_state' hook returned an unsupported performance state",
           dev.get());
    }
    dev->set_current_performance_state(*out_state);
    return status;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

void DriverHostContext::QueueDeviceForFinalization(zx_device_t* device) {
  // Put on the deferred work list for finalization
  defer_device_list_.push_back(device);

  // Immediately finalize if there's not an active enumerator
  if (enumerators_ == 0) {
    FinalizeDyingDevices();
  }
}
