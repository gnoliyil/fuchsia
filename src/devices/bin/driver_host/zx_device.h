// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_HOST_ZX_DEVICE_H_
#define SRC_DEVICES_BIN_DRIVER_HOST_ZX_DEVICE_H_

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/dispatcher.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/driver/compat/cpp/symbols.h>
#include <lib/fit/function.h>
#include <lib/fpromise/result.h>
#include <lib/sync/cpp/completion.h>
#include <lib/trace/event.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/channel.h>
#include <lib/zx/result.h>
#include <zircon/compiler.h>

#include <array>
#include <atomic>
#include <optional>
#include <string>

#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>
#include <fbl/recycler.h>
#include <fbl/ref_counted_upgradeable.h>
#include <fbl/ref_ptr.h>
#include <fbl/string_buffer.h>
#include <fbl/vector.h>

#include "driver.h"
#include "inspect.h"
#include "src/devices/lib/fidl/device_server.h"

class CompositeDevice;
class FidlProxyDevice;
class DeviceControllerConnection;
class DriverHostContext;
class DeviceInspect;
struct ProxyIostate;

// RAII object around async trace entries
class AsyncTrace {
 public:
  AsyncTrace(const char* category, const char* name)
      : category_(category), async_id_(TRACE_NONCE()) {
    label_.Append(name);
    TRACE_ASYNC_BEGIN(category, label_.data(), async_id_);
  }
  ~AsyncTrace() { finish(); }

  AsyncTrace(const AsyncTrace&) = delete;
  AsyncTrace& operator=(const AsyncTrace&) = delete;

  AsyncTrace(AsyncTrace&& other) { *this = std::move(other); }
  AsyncTrace& operator=(AsyncTrace&& other) {
    if (this != &other) {
      category_ = other.category_;
      label_ = std::move(other.label_);
      async_id_ = other.async_id_;
      other.label_.Clear();
    }
    return *this;
  }

  trace_async_id_t async_id() const { return async_id_; }

  // End the async trace immediately
  void finish() {
    if (!label_.empty()) {
      TRACE_ASYNC_END(category_, label_.data(), async_id_);
      label_.Clear();
    }
  }

 private:
  const char* category_;
  fbl::StringBuffer<32> label_;
  trace_async_id_t async_id_;
};

// 'MDEV'
#define DEV_MAGIC 0x4D444556

// Maximum number of dead devices to hold on the dead device list
// before we start free'ing the oldest when adding a new one.
constexpr size_t DEAD_DEVICE_MAX = 7;

// Tags used to manage the different containers a zx_device may exist in
namespace internal {
struct ZxDeviceChildrenListTag {};
struct ZxDeviceDeferListTag {};
struct ZxDeviceLocalIdMapTag {};
}  // namespace internal

// This needs to be a struct, not a class, to match the public definition
struct zx_device
    : public devfs_fidl::DeviceInterface,
      public fbl::RefCountedUpgradeable<zx_device>,
      public fbl::Recyclable<zx_device>,
      public fbl::ContainableBaseClasses<
          fbl::TaggedDoublyLinkedListable<zx_device*, internal::ZxDeviceChildrenListTag>,
          fbl::TaggedDoublyLinkedListable<zx_device*, internal::ZxDeviceDeferListTag>,
          fbl::TaggedWAVLTreeContainable<fbl::RefPtr<zx_device>, internal::ZxDeviceLocalIdMapTag>> {
 private:
  using TraceLabelBuffer = fbl::StringBuffer<32>;

 public:
  using ChildrenListTag = internal::ZxDeviceChildrenListTag;
  using DeferListTag = internal::ZxDeviceDeferListTag;
  using LocalIdMapTag = internal::ZxDeviceLocalIdMapTag;

  ~zx_device();

  zx_device(const zx_device&) = delete;
  zx_device& operator=(const zx_device&) = delete;

  // |ctx| must outlive |*out_dev|.  This is managed in the full binary by creating
  // the DriverHostContext in main() (having essentially a static lifetime).
  static zx_status_t Create(DriverHostContext* ctx, std::string name, fbl::RefPtr<Driver> driver,
                            fbl::RefPtr<zx_device>* out_dev);

  void InitOp() {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks", get_trace_label("init", &trace_label));
      Dispatch(ops_.init);
      completion.Signal();
    });

    completion.Wait();
  }

  void UnbindOp() {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks", get_trace_label("unbind", &trace_label));
      Dispatch(ops_.unbind);
      completion.Signal();
    });

    completion.Wait();
  }

  zx_status_t ServiceConnectOp(const char* service_name, fdf_handle_t channel) {
    if (!ops_.service_connect) {
      return ZX_ERR_NOT_SUPPORTED;
    }

    libsync::Completion completion;
    zx_status_t status;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      status = ops_.service_connect(ctx(), service_name, channel);
      completion.Signal();
    });

    completion.Wait();
    return status;
  }

  void ReleaseOp() {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks", get_trace_label("release", &trace_label));
      Dispatch(ops_.release);
      completion.Signal();
    });

    completion.Wait();
  }

  // This is identical to |ReleaseOp|, except it runs the op on the current thread.
  // This is necessary when we're running ReleaseOp for the last device published by a driver, since
  // it is called after the dispatcher has been shut down.
  void ReleaseSyncOp() {
    TraceLabelBuffer trace_label;
    TRACE_DURATION("driver_host:driver-hooks", get_trace_label("release-sync", &trace_label));
    Dispatch(ops_.release);
  }

  void SuspendNewOp(uint8_t requested_state, bool enable_wake, uint8_t suspend_reason) {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks", get_trace_label("suspend", &trace_label));
      Dispatch(ops_.suspend, requested_state, enable_wake, suspend_reason);
      completion.Signal();
    });

    completion.Wait();
  }

  zx_status_t SetPerformanceStateOp(uint32_t requested_state, uint32_t* out_state) {
    if (!ops_.set_performance_state) {
      return ZX_ERR_NOT_SUPPORTED;
    }

    libsync::Completion completion;
    zx_status_t status;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks",
                     get_trace_label("set_performance_state", &trace_label));
      status = ops_.set_performance_state(ctx(), requested_state, out_state);
      completion.Signal();
    });

    completion.Wait();
    return status;
  }

  zx_status_t ConfigureAutoSuspendOp(bool enable, uint8_t requested_state) {
    if (!ops_.configure_auto_suspend) {
      return ZX_ERR_NOT_SUPPORTED;
    }

    libsync::Completion completion;
    zx_status_t status;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks",
                     get_trace_label("conf_auto_suspend", &trace_label));
      status = ops_.configure_auto_suspend(ctx(), enable, requested_state);
      completion.Signal();
    });

    completion.Wait();
    return status;
  }

  void ResumeNewOp(uint32_t requested_state) {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks", get_trace_label("resume", &trace_label));
      Dispatch(ops_.resume, requested_state);
      completion.Signal();
    });

    completion.Wait();
  }

  void ChildPreReleaseOp(void* child_ctx) {
    libsync::Completion completion;

    async::PostTask(driver->dispatcher()->async_dispatcher(), [&]() {
      TraceLabelBuffer trace_label;
      TRACE_DURATION("driver_host:driver-hooks",
                     get_trace_label("child_pre_release", &trace_label));
      Dispatch(ops_.child_pre_release, child_ctx);
      completion.Signal();
    });

    completion.Wait();
  }

  void set_bind_conn(fit::callback<void(zx_status_t)>);
  fit::callback<void(zx_status_t)> take_bind_conn();

  void set_rebind_conn(fit::callback<void(zx_status_t)>);
  void call_rebind_conn_if_exists(zx_status_t status);
  void set_rebind_drv_name(std::string drv_name);
  const std::string& get_rebind_drv_name() { return rebind_drv_name_; }

  void set_unbind_children_conn(fit::callback<void(zx_status_t)>);
  fit::callback<void(zx_status_t)> take_unbind_children_conn();

  // Check if this driver_host has a device with the given ID, and if so returns a
  // reference to it.
  static fbl::RefPtr<zx_device> GetDeviceFromLocalId(uint64_t local_id);

  uint64_t local_id() const { return local_id_; }
  void set_local_id(uint64_t id);

  uintptr_t magic = DEV_MAGIC;

  const zx_protocol_device_t& ops() const { return ops_; }
  void set_ops(zx_protocol_device_t ops) {
    ops_ = ops;
    inspect_->set_ops(ops);
  }

  compat::device_t* get_dfv2_symbol() { return &dfv2_symbol_; }

  uint32_t flags() const { return flags_; }
  void set_flag(uint32_t flag) {
    flags_ |= flag;
    inspect_->set_flags(flags_);
  }
  void unset_flag(uint32_t flag) {
    flags_ &= ~flag;
    inspect_->set_flags(flags_);
  }

  // The RPC channel is owned by |conn|
  // fuchsia.device.manager.Coordinator
  fidl::WireSharedClient<fuchsia_device_manager::Coordinator> coordinator_client;

  fit::callback<void(zx_status_t)> init_cb;

  fit::callback<void(zx_status_t)> removal_cb;

  fit::callback<void(zx_status_t)> unbind_cb;

  fit::callback<void(zx_status_t, uint8_t)> suspend_cb;
  fit::callback<void(zx_status_t, uint8_t, uint32_t)> resume_cb;

  // most devices implement a single
  // protocol beyond the base device protocol
  uint32_t protocol_id() const { return dfv2_symbol_.proto_ops.id; }

  void set_protocol_id(uint32_t protocol_id) {
    dfv2_symbol_.proto_ops.id = protocol_id;
    inspect_->set_protocol_id(protocol_id);
  }

  const void* protocol_ops() const { return dfv2_symbol_.proto_ops.ops; }
  void set_protocol_ops(const void* ops) { dfv2_symbol_.proto_ops.ops = ops; }

  inline void* ctx() const { return dfv2_symbol_.context; }
  void set_ctx(void* ctx) { dfv2_symbol_.context = ctx; }

  cpp20::span<const char*> fidl_service_offers() {
    return {fidl_service_offers_.data(), fidl_service_offers_.size()};
  }

  void set_fidl_service_offers(cpp20::span<const char*> fidl_service_offers) {
    fidl_service_offers_ = {fidl_service_offers.begin(), fidl_service_offers.end()};
    inspect_->set_fidl_service_offers(fidl_service_offers);
  }

  std::vector<const char*> fidl_service_offers_;

  cpp20::span<const char*> runtime_service_offers() {
    return {runtime_service_offers_.data(), runtime_service_offers_.size()};
  }

  void set_runtime_service_offers(cpp20::span<const char*> runtime_service_offers) {
    runtime_service_offers_ = {runtime_service_offers.begin(), runtime_service_offers.end()};
  }

  std::vector<const char*> runtime_service_offers_;

  // driver that has published this device
  fbl::RefPtr<Driver> driver;
  DriverRef driver_ref_;

  const fbl::RefPtr<zx_device_t>& parent() const { return parent_; }
  void set_parent(fbl::RefPtr<zx_device_t> parent) {
    parent_ = parent;
    inspect_->set_parent(parent);
  }

  void add_child(zx_device* child);
  void remove_child(zx_device& child);
  const fbl::TaggedDoublyLinkedList<zx_device*, ChildrenListTag>& children() { return children_; }

  // This is an atomic so that the connection's async loop can inspect this
  // value to determine if an expected shutdown is happening.  See comments in
  // DriverManagerRemove().
  fbl::Mutex controller_lock;
  std::optional<fidl::ServerBindingRef<fuchsia_device_manager::DeviceController>> controller_binding
      TA_GUARDED(controller_lock);
  std::optional<devfs_fidl::DeviceServer> vnode;

  fbl::Mutex proxy_ios_lock;
  ProxyIostate* proxy_ios TA_GUARDED(proxy_ios_lock) = nullptr;

  const char* name() const { return name_; }

  zx_driver_t* zx_driver() { return driver->zx_driver(); }

  // Trait structure for the local ID map
  struct LocalIdKeyTraits {
    static uint64_t GetKey(const zx_device& obj) { return obj.local_id_; }
    static bool LessThan(const uint64_t& key1, const uint64_t& key2) { return key1 < key2; }
    static bool EqualTo(const uint64_t& key1, const uint64_t& key2) { return key1 == key2; }
  };

  using DevicePowerStates = std::array<fuchsia_device::wire::DevicePowerStateInfo,
                                       fuchsia_device::wire::kMaxDevicePowerStates>;
  using SystemPowerStateMapping = std::array<fuchsia_device::wire::SystemPowerStateInfo,
                                             fuchsia_device_manager::wire::kMaxSystemPowerStates>;
  using PerformanceStates = std::array<fuchsia_device::wire::DevicePerformanceStateInfo,
                                       fuchsia_device::wire::kMaxDevicePerformanceStates>;

  bool has_composite() const;
  void set_composite(fbl::RefPtr<CompositeDevice> composite, bool fragment = true);
  fbl::RefPtr<CompositeDevice> take_composite();

  bool is_composite() const;
  fbl::RefPtr<CompositeDevice> composite();

  void set_fidl_proxy(fbl::RefPtr<FidlProxyDevice> fidl_proxy);

  bool is_fidl_proxy() const;
  fbl::RefPtr<FidlProxyDevice> fidl_proxy();

  const DevicePowerStates& GetPowerStates() const;
  const PerformanceStates& GetPerformanceStates() const;

  zx_status_t SetPowerStates(const device_power_state_info_t* power_states, uint8_t count);

  bool IsPowerStateSupported(fuchsia_device::wire::DevicePowerState requested_state) {
    // requested_state is bounded by the enum.
    return power_states_[static_cast<uint8_t>(requested_state)].is_supported;
  }

  bool IsPerformanceStateSupported(uint32_t requested_state);
  bool auto_suspend_configured() { return auto_suspend_configured_; }
  void set_auto_suspend_configured(bool value) {
    auto_suspend_configured_ = value;
    inspect_->set_auto_suspend(value);
  }

  zx_status_t SetSystemPowerStateMapping(const SystemPowerStateMapping& mapping);

  zx_status_t SetPerformanceStates(const device_performance_state_info_t* performance_states,
                                   uint8_t count);

  const SystemPowerStateMapping& GetSystemPowerStateMapping() const;

  uint32_t current_performance_state() { return current_performance_state_; }

  void set_current_performance_state(uint32_t state) {
    current_performance_state_ = state;
    inspect_->set_current_performance_state(state);
  }

  zx_status_t get_dev_power_state_from_mapping(uint32_t flags,
                                               fuchsia_device::wire::SystemPowerStateInfo* info,
                                               uint8_t* suspend_reason);

  // Begin an async tracing entry for this device.  It will have the given category, and the name
  // "<device_name>:<tag>".
  AsyncTrace BeginAsyncTrace(const char* category, const char* tag) {
    TraceLabelBuffer name;
    get_trace_label(tag, &name);
    return AsyncTrace(category, name.data());
  }

  fidl::ClientEnd<fuchsia_io::Directory>& runtime_outgoing_dir() { return runtime_outgoing_dir_; }

  void set_runtime_outgoing_dir(fidl::ClientEnd<fuchsia_io::Directory> outgoing_dir) {
    runtime_outgoing_dir_ = std::move(outgoing_dir);
  }

  DriverHostContext* driver_host_context() const { return driver_host_context_; }
  bool complete_bind_rebind_after_init() const { return complete_bind_rebind_after_init_; }
  void set_complete_bind_rebind_after_init(bool value) { complete_bind_rebind_after_init_ = value; }

  DeviceInspect& inspect() { return *inspect_; }
  void FreeInspect() { inspect_.reset(); }

  zx_status_t Rebind();

 private:
  zx::result<std::string> GetTopologicalPath();

  // Methods from the devfs_fidl::DeviceInterface class.
  void LogError(const char* error) override;
  bool IsUnbound() override;
  bool MessageOp(fidl::IncomingHeaderAndMessage msg, device_fidl_txn_t txn) override;
  void ConnectToDeviceFidl(ConnectToDeviceFidlRequestView request,
                           ConnectToDeviceFidlCompleter::Sync& completer) override;
  void Bind(BindRequestView request, BindCompleter::Sync& completer) override;
  void Rebind(RebindRequestView request, RebindCompleter::Sync& completer) override;
  void UnbindChildren(UnbindChildrenCompleter::Sync& completer) override;
  void ScheduleUnbind(ScheduleUnbindCompleter::Sync& completer) override;
  void GetTopologicalPath(GetTopologicalPathCompleter::Sync& completer) override;
  void GetMinDriverLogSeverity(GetMinDriverLogSeverityCompleter::Sync& completer) override;
  void GetCurrentPerformanceState(GetCurrentPerformanceStateCompleter::Sync& completer) override;
  void SetMinDriverLogSeverity(SetMinDriverLogSeverityRequestView request,
                               SetMinDriverLogSeverityCompleter::Sync& completer) override;
  void SetPerformanceState(SetPerformanceStateRequestView request,
                           SetPerformanceStateCompleter::Sync& completer) override;

  explicit zx_device(DriverHostContext* ctx, std::string name, fbl::RefPtr<Driver> driver);

  char name_[ZX_DEVICE_NAME_MAX + 1] = {};

  friend class fbl::Recyclable<zx_device_t>;
  void fbl_recycle();

  // Templates that dispatch the protocol operations if they were set.
  // If they were not set, the second paramater is returned to the caller
  // (usually ZX_ERR_NOT_SUPPORTED)
  template <typename... ArgTypes>
  void Dispatch(void (*op)(void* ctx, ArgTypes...), ArgTypes... args) {
    if (op) {
      (*op)(ctx(), args...);
    }
  }

  // Utility for getting a label for tracing that identifies this device
  template <size_t N>
  const char* get_trace_label(const char* label, fbl::StringBuffer<N>* out) const {
    out->Clear();
    out->AppendPrintf("%s:%s", this->name(), label);
    return out->data();
  }

  // True when this device is a composite device, distinguishing this device from a fragment.
  bool is_composite_ = false;

  // If this device is a fragment of a composite, or if this device is a composite device itself,
  // this points to the composite control structure.
  fbl::RefPtr<CompositeDevice> composite_;

  // True when this device is a FIDL proxy device.
  bool is_fidl_proxy_ = false;

  fbl::RefPtr<FidlProxyDevice> fidl_proxy_;

  // Identifier assigned by devmgr that can be used to assemble composite
  // devices.
  uint64_t local_id_ = 0;

  uint32_t flags_ = 0;

  zx_protocol_device_t ops_ = {};

  compat::device_t dfv2_symbol_ = compat::kDefaultDevice;

  // parent in the device tree
  fbl::RefPtr<zx_device_t> parent_;

  // list of this device's children in the device tree
  fbl::TaggedDoublyLinkedList<zx_device*, ChildrenListTag> children_;

  fbl::Mutex bind_conn_lock_;

  fit::callback<void(zx_status_t)> bind_conn_ TA_GUARDED(bind_conn_lock_);

  fbl::Mutex rebind_conn_lock_;

  fit::callback<void(zx_status_t)> rebind_conn_ TA_GUARDED(rebind_conn_lock_);
  bool complete_bind_rebind_after_init_ = false;

  fbl::Mutex unbind_children_conn_lock_;

  fit::callback<void(zx_status_t)> unbind_children_conn_ TA_GUARDED(unbind_children_conn_lock_);

  std::string rebind_drv_name_ = {};

  PerformanceStates performance_states_;
  DevicePowerStates power_states_;
  SystemPowerStateMapping system_power_states_mapping_;
  uint32_t current_performance_state_ = fuchsia_device::wire::kDevicePerformanceStateP0;
  bool auto_suspend_configured_ = false;

  // Runtime protocols served by the parent.
  fidl::ClientEnd<fuchsia_io::Directory> runtime_outgoing_dir_;

  DriverHostContext* const driver_host_context_;
  std::optional<DeviceInspect> inspect_;
};

// zx_device_t objects must be created or initialized by the driver manager's
// device_create() function.  Drivers MAY NOT touch any
// fields in the zx_device_t, except for the protocol_id and protocol_ops
// fields which it may fill out after init and before device_add() is called,
// and the ctx field which may be used to store driver-specific data.

// clang-format off

#define DEV_FLAG_DEAD                  0x00000001  // this device has been removed and
                                                   // is safe for ref0 and release()
#define DEV_FLAG_INITIALIZING          0x00000002  // device is being initialized
#define DEV_FLAG_UNBINDABLE            0x00000004  // nobody may autobind to this device
#define DEV_FLAG_BUSY                  0x00000010  // device being created
#define DEV_FLAG_ADDED                 0x00000100  // device_add() has been called for this device
#define DEV_FLAG_INVISIBLE             0x00000200  // device not visible via devfs
#define DEV_FLAG_UNBINDING             0x00000400  // informed that it should self-delete asap
#define DEV_FLAG_WANTS_REBIND          0x00000800  // when last child goes, rebind this device
#define DEV_FLAG_ALLOW_MULTI_COMPOSITE 0x00001000  // can be part of multiple composite devices
#define DEV_FLAG_MUST_ISOLATE          0x00002000  // must be in separate host from child devices
// clang-format on

// Request to bind a driver with drv_libname to device. If device is already bound to a driver,
// ZX_ERR_ALREADY_BOUND is returned
zx_status_t device_bind(const fbl::RefPtr<zx_device_t>& dev, const char* drv_libname);
zx_status_t device_unbind(const fbl::RefPtr<zx_device_t>& dev);
// Request to unbind all children from the specified device. Returns true if the device has children
// and the unbind request was scheduled, otherwise false.
zx::result<bool> device_schedule_unbind_children(const fbl::RefPtr<zx_device_t>& dev);
zx_status_t device_schedule_remove(const fbl::RefPtr<zx_device_t>& dev, bool unbind_self);
zx_status_t device_run_compatibility_tests(const fbl::RefPtr<zx_device_t>& dev,
                                           int64_t hook_wait_time,
                                           fit::callback<void(zx_status_t)> cb);

#endif  // SRC_DEVICES_BIN_DRIVER_HOST_ZX_DEVICE_H_
