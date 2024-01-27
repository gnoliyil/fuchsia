// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_DDK_INCLUDE_LIB_DDK_DEVICE_H_
#define SRC_LIB_DDK_INCLUDE_LIB_DDK_DEVICE_H_

#include <lib/fdf/types.h>
#include <zircon/compiler.h>
#include <zircon/fidl.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

__BEGIN_CDECLS

typedef struct zx_device zx_device_t;
typedef struct zx_driver zx_driver_t;
typedef struct zx_device_prop zx_device_prop_t;

typedef struct zx_protocol_device zx_protocol_device_t;

// Max device name length, not including a null-terminator
#define ZX_DEVICE_NAME_MAX 31

// echo -n "zx_device_ops_v0.52" | sha256sum | cut -c1-16
#define DEVICE_OPS_VERSION_0_52 0xb834fdab33623bb4

// Current Version
#define DEVICE_OPS_VERSION DEVICE_OPS_VERSION_0_52

// TODO: temporary flags used by devcoord to communicate
// with the system bus device.
#define DEVICE_SUSPEND_FLAG_REBOOT 0xdcdc0100
#define DEVICE_SUSPEND_FLAG_POWEROFF 0xdcdc0200
#define DEVICE_SUSPEND_FLAG_MEXEC 0xdcdc0300
#define DEVICE_SUSPEND_FLAG_SUSPEND_RAM 0xdcdc0400
#define DEVICE_SUSPEND_REASON_MASK 0xffffff00

// These values should be same as the enum fuchsia.device.DevicePowerState
// generated from FIDL. The system wide power manager will be using the
// power states from FIDL generated file.
#define DEV_POWER_STATE_D0 UINT8_C(0)
#define DEV_POWER_STATE_D1 UINT8_C(1)
#define DEV_POWER_STATE_D2 UINT8_C(2)
#define DEV_POWER_STATE_D3HOT UINT8_C(3)
#define DEV_POWER_STATE_D3COLD UINT8_C(4)

// Performance state
#define DEV_PERFORMANCE_STATE_P0 UINT32_C(0)

// reboot modifiers
#define DEVICE_SUSPEND_FLAG_REBOOT_BOOTLOADER (DEVICE_SUSPEND_FLAG_REBOOT | 0x01)
#define DEVICE_SUSPEND_FLAG_REBOOT_RECOVERY (DEVICE_SUSPEND_FLAG_REBOOT | 0x02)
#define DEVICE_SUSPEND_FLAG_REBOOT_KERNEL_INITIATED (DEVICE_SUSPEND_FLAG_REBOOT | 0x03)

#define DEVICE_SUSPEND_REASON_POWEROFF UINT8_C(0x10)
#define DEVICE_SUSPEND_REASON_SUSPEND_RAM UINT8_C(0x20)
#define DEVICE_SUSPEND_REASON_MEXEC UINT8_C(0x30)
#define DEVICE_SUSPEND_REASON_REBOOT UINT8_C(0x40)
#define DEVICE_SUSPEND_REASON_REBOOT_RECOVERY (UINT8_C(DEVICE_SUSPEND_REASON_REBOOT | 0x01))
#define DEVICE_SUSPEND_REASON_REBOOT_BOOTLOADER (UINT8_C(DEVICE_SUSPEND_REASON_REBOOT | 0x02))
#define DEVICE_SUSPEND_REASON_REBOOT_KERNEL_INITIATED (UINT8_C(DEVICE_SUSPEND_REASON_REBOOT | 0x03))
#define DEVICE_SUSPEND_REASON_SELECTIVE_SUSPEND UINT8_C(0x50)
#define DEVICE_MASK_SUSPEND_REASON UINT8_C(0xf0)

//@doc(docs/ddk/device-ops.md)

typedef struct device_fidl_txn device_fidl_txn_t;
// An outstanding FIDL transaction used when the driver host is managing
// a FIDL channel.
struct device_fidl_txn {
  // Replies to the outstanding request and complete the FIDL transaction.
  //
  // Pass the |device_fidl_txn_t| object itself as the first parameter. The |msg|
  // should already be encoded. This function always consumes any handles
  // present in |msg|.
  //
  // Call |reply| only once for each |txn| object. After |reply| returns, the
  // |txn| object is considered invalid and might have been freed or reused
  // for another purpose.
  zx_status_t (*reply)(device_fidl_txn_t* txn, const fidl_outgoing_msg_t* msg);

  // Internal value used for driver host bookkeeping.  Must not be mutated.
  uintptr_t driver_host_context;
};

//@ # The Device Protocol
//
// Device drivers implement a set of hooks (methods) to support the
// operations that may be done on the devices that they publish.
//
// These are described below, including the action that is taken
// by the default implementation that is used for each hook if the
// driver does not provide its own implementation.

typedef struct zx_protocol_device {
  //@ ## version
  // This field must be set to `DEVICE_OPS_VERSION`
  uint64_t version;

  //@ ## get_protocol
  // The get_protocol hook is called when a driver invokes
  // **device_get_protocol()** on a device object.  The implementation must
  // populate *protocol* with a protocol structure determined by *proto_id*.
  // If the requested *proto_id* is not supported, the implementation must
  // return ZX_ERR_NOT_SUPPORTED.
  //
  // The default get_protocol hook returns with *protocol*=*proto_ops* if *proto_id*
  // matches the one given when **device_add()** created the device, and returns
  // ZX_ERR_NOT_SUPPORTED otherwise.
  //
  // See the **device_get_protocol()** docs for a description of the layout of
  // *protocol*.
  //
  // This hook is never called by the devhost runtime other than when
  // **device_get_protocol()** is invoked by some driver.  It is executed
  // synchronously in the same thread as the caller.
  zx_status_t (*get_protocol)(void* ctx, uint32_t proto_id, void* protocol);

  //@ ## open_protocol_session_multibindable
  // The open_protocol_session_multibindable hook is called when the component driver invokes
  // **device_open_protocol_session_multibindable()** on a device object that supports multibinding.
  // A driver that supports multibinding, may choose to implement this hook, if it needs to know
  // which composite device is making the protocol ops it requested.
  //
  // The implementation must populate *protocol* with
  // a protocol structure determined by *proto_id* and a protocol context that is unique to this
  // session. If the requested *proto_id* is not supported, the implementation must return
  // ZX_ERR_NOT_SUPPORTED.
  //
  //
  // See the **device_open_protocol_session_multibindable()** docs for a description of the layout
  // of *protocol*.
  //
  // This hook is never called by the devhost runtime other than when
  // **device_open_protocol_session_multibindable()** is invoked by component driver. It is executed
  // synchronously in the same thread as the caller.
  zx_status_t (*open_protocol_session_multibindable)(void* ctx, uint32_t proto_id, void* protocol);
  //@ ## close_protocol_session_multibindable
  // The close_protocol_session_multibindable hook is called when the component driver invokes
  // **device_close_protocol_session_multibindable()** on a device object that supports
  // multibinding. A driver that supports multibinding, may choose to implement this hook along with
  // open_protocol_session_multibindable, if it needs to know which composite device is making the
  // protocol ops it requested.
  //
  // The implementation must identify the context passed and close the corresponding
  // session.
  //
  // This hook is never called by the devhost runtime other than when
  // **device_close_protocol_session_multibindable()** is invoked by component driver. It is
  // executed synchronously in the same thread as the caller.
  zx_status_t (*close_protocol_session_multibindable)(void* ctx, void* protocol);
  //@ ## init
  // The init hook is called when a device is initially added.
  //
  // If implemented, the device is guaranteed to be invisible and not able to be unbound until the
  // driver calls **device_init_reply()** on itself. **device_init_reply()** can be called from
  // any thread - it does not necessarily need to be called before the |init| hook returns.
  //
  // This allows drivers to safely complete initialization without explicit synchronization with
  // the unbind hook, such as adding device metadata or completing blocking operations in a
  // worker thread. Once the initialization is completed, **device_init_reply()** should be
  // called to make the device visible and able to be unbound.
  //
  // The hook is always called from the devhost's main thread.
  void (*init)(void* ctx);

  //@ ## unbind
  // The unbind hook is called to begin removal of a device (due to hot unplug, fatal error, etc).
  //
  // The driver should avoid further method calls to its parent device or any
  // protocols obtained from that device, and expect that any further such calls
  // will return with an error.
  //
  // The driver should adjust its state to encourage its client connections to close
  // (cause IO to error out, etc), and call **device_unbind_reply()** on itself when ready.
  // See the docs for **device_unbind_reply()** for important semantics.
  //
  // The driver must continue to handle all device hooks except for message, open, read, and write
  // until the **release** hook is invoked.
  //
  // Prior to unbind being called, the DDK will suspend processing of all FIDL messages and new
  // connections will be disallowed at this point. A device driver is responsible for ensuring that
  // any pending FIDL transactions are replied to or closed prior to replying to unbind. A device
  // which handles asynchronous FIDL messages *must* implement this hook.
  //
  // **Note:** This hook will not be called for a **device instance**.
  //
  // This is an optional hook (except for drivers that implement message). The default
  // implementation will be a hook that replies immediately with **device_unbind_reply()**.
  //
  // This hook will be called from the devhost's main thread. It will be executed sometime
  // after any of the following events occuring: **device_async_remove()** is invoked on the
  // device, the device's parent has completed its unbind hook via **device_unbind_reply**,
  // or a fuchsia.device.Controller/ScheduleUnbind request is received.
  void (*unbind)(void* ctx);

  //@ ## release
  // The release hook is called after this device has finished unbinding, all open client
  // connections of the device have been closed, and all child devices have been unbound and
  // released.
  //
  // At the point release is invoked, the driver will not receive any further calls
  // and absolutely must not use the underlying **zx_device_t** or any protocols obtained
  // from that device once this method returns.
  //
  // The driver must free all memory and release all resources related to this device
  // before returning.
  //
  // This hook may be called from any thread including the devhost's main
  // thread.
  void (*release)(void* ctx);

  //@ ## suspend
  // The suspend hook is used for suspending a device from a working to
  // non-working low power state(sleep state), or from a non-working sleep state
  // to a deeper sleep state.
  //
  // requested_state is always a non-working sleep state.
  // enable_wake is whether to configure the device for wakeup from the requested non
  // working sleep state. If enable_wake is true and the device does not support
  // wake up, the hook fails without suspending the device.
  // suspend_reason provides information for the driver why the suspend hook is called.
  // Bus drivers and platform drivers like acpi will find this information useful to
  // issue any system calls or save the reboot reason.
  //
  // The driver should put the device into the requested_state and call **device_suspend_reply()**
  // on itself. device_suspend_reply() will take in two parameters: status of the suspend operation
  // and an out_state. If status is success, the out_state is same as requested_state.
  // If status is failure, out_state is the low power state the device is currently in.
  //
  // This hook assumes that the drivers are aware of their current state. This hook will only
  // be executed on the devhost's main thread.
  void (*suspend)(void* ctx, uint8_t requested_state, bool enable_wake, uint8_t suspend_reason);

  //@ ## resume
  // The resume hook is used for resuming a device from a non-working sleep
  // state to a working state. It requires reinitializing the device completely
  // or partially depending on the sleep state that device was in, when the
  // resume call was made.
  //
  // requested_state is the performance state that the device has to be in.
  //
  // The driver should put the device into the requested_state and call **device_resume_reply()**
  // on itself. device_resume_reply() will take in the following parameters:
  // (1)Status of the resume operation (2)out_power_state (3) out_perf_state
  // On success, the device has been resumed successfully to a working state,
  // out_perf_state is same as requested state.
  // If the device is not able to resume to a working state, the hook returns a
  // failure. out_power_state has the non working state the device is in.
  // if out_power_state is a working state, out_perf_state has the performance
  // state the device is in.
  // This hook assumes that the drivers are aware of their current state.
  //
  // This hook will only be executed on the devhost's main thread.
  void (*resume)(void* ctx, uint32_t requested_state);

  //@ ## set_performance_state
  // The set_performance_state hook is used for transitioning the performant state of
  // a device.
  //
  // requested_state is always a working performant state that is published during
  // device_add.
  //
  // On success, the out_state is same as the requested_state. If the device is in working state,
  // the transition is made immediately.
  // On failure, the out_state is the performance state the device is currently in.
  // If the device is not in working state, driver will return ZX_ERR_UNAVAILABLE and out_state will
  // be fully working performance state.
  //
  // This hook assumes that the drivers are aware of their current sleep state and current
  // performance state.
  //
  // This hook will only be executed on the devhost's main thread.
  //
  zx_status_t (*set_performance_state)(void* ctx, uint32_t requested_state, uint32_t* out_state);

  //@ ## configure_autosuspend
  // The configure_autosuspend hook is used for configuring whether a driver can
  // auto suspend the device depending on the activity and idleness of the device.
  //
  // If "enable" is true, auto suspend is configured. deepest_sleep_state is the deepest
  // sleep state the device is expected to go into when the device is suspended.
  //
  // On success, the device is configured to be autosuspended
  // On failure, the device not configured to be autosuspended. If the device does
  // not implement the autosuspend hook, it means the device does not support autosuspend.
  //
  // This hook will only be executed on the devhost's main thread.
  //

  zx_status_t (*configure_auto_suspend)(void* ctx, bool enable, uint8_t deepest_sleep_state);

  //@ ## rxrpc
  // Only called for bus devices.
  // When the "shadow" of a busdev sends an rpc message, the
  // device that is shadowing is notified by the rxrpc op and
  // should attempt to read and respond to a single message on
  // the provided channel.
  //
  // Any error return from this method will result in the channel
  // being closed and the remote "shadow" losing its connection.
  //
  // This method is called with ZX_HANDLE_INVALID for the channel
  // when a new client connects -- at which point any state from
  // the previous client should be torn down.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*rxrpc)(void* ctx, zx_handle_t channel);

  //@ ## message
  // Process a FIDL rpc message.  This is used to handle class or
  // device specific messaging.  fuchsia.io.{Node,File,Device} are
  // handles by the devhost itself.
  //
  // The entire message becomes the responsibility of the driver,
  // including the handles.
  //
  // The txn provided to respond to the message is only valid for
  // the duration of the message() call.  It must not be cached
  // and used later.
  //
  // If this method wishes to respond asynchronously, the txn
  // should be copied and the method should return ZX_ERR_ASYNC.
  //
  // If this method returns anything other than ZX_OK or
  // ZX_ERR_ASYNC, the underlying connection is closed.
  //
  // This hook will only be executed on the devhost's main thread.
  zx_status_t (*message)(void* ctx, fidl_incoming_msg_t* msg, device_fidl_txn_t* txn);

  //@ ## service_connect
  // Connect a runtime client to this device.
  //
  // Provides ownership of *channel*, which is a fdf runtime channel.
  //
  // This hook is never called by the devhost runtime other than when
  // **device_service_connect()** is invoked by some driver. It is executed
  // synchronously in the same thread as the caller.
  zx_status_t (*service_connect)(void* ctx, const char* service_name, fdf_handle_t channel);

  //@ ## child_pre_release
  // The child_pre_release hook is used to signal that a child device
  // will soon be released. This is after the child and all its descendents
  // have been unbound and removed from the device filesystem, and all client
  // connections to the child have been closed.
  //
  // The device may want to drop any references to the child context or child
  // **zx_device_t**.
  //
  // This hook may be called from any thread including the devhost's main
  // thread.
  void (*child_pre_release)(void* ctx, void* child_ctx);

} zx_protocol_device_t;

// protocols look like:
// typedef struct {
//     protocol_xyz_ops_t* ops;
//     void* ctx;
// } protocol_xyz_t;
zx_status_t device_get_protocol(const zx_device_t* dev, uint32_t proto_id, void* protocol);

// Direct Device Ops Functions

zx_status_t device_service_connect(zx_device_t* device, const char* service_name,
                                   fdf_handle_t channel);

// Opens a connection to the specified runtime service offered by |device|.
//
// |device| is typically the parent of the device invoking this function.
// |service_name| can be constructed with `my_service_name::Name`.
// |request| must be the server end of a zircon channel.
//
// If you are inside a C++ device class, it may be more convenient to use the
// DdkConnectRuntimeProtocol wrapper method from ddktl, which supplies |device| and
// |service_name| automatically.
zx_status_t device_connect_runtime_protocol(zx_device_t* device, const char* service_name,
                                            const char* protocol_name, fdf_handle_t request);

// Device Metadata Support

// retrieves metadata for a specific device
// searches parent devices to find a match
zx_status_t device_get_metadata(zx_device_t* dev, uint32_t type, void* buf, size_t buflen,
                                size_t* actual);

// retrieves metadata size for a specific device
// searches parent devices to find a match
zx_status_t device_get_metadata_size(zx_device_t* dev, uint32_t type, size_t* out_size);

// Adds metadata to a specific device.
zx_status_t device_add_metadata(zx_device_t* dev, uint32_t type, const void* data, size_t length);

// Takes ownership of the given FIDL transaction.
//
// |txn| is expected to be a device_fidl_txn_t* given to a device's message callback.
// After calling this function, it is invalid to use |txn|.  On success, |*new_txn| will refer to a
// new device_fidl_txn_t which is safe to use until after its reply method is invoked or the owning
// device's unbind hook is completed, whichever is earlier.
void device_fidl_transaction_take_ownership(device_fidl_txn_t* txn, device_fidl_txn_t* new_txn);

// Returns the number of fragments that can be returned by `device_get_fragments`.
uint32_t device_get_fragment_count(zx_device_t* dev);

typedef struct composite_device_fragment {
  char name[32];
  zx_device_t* device;
} composite_device_fragment_t;

// Returns a list of all of the fragments of this device, in the order
// in which they were provided to device_add_composite().  The returned
// devices must not be used after the composite device is unbound.
void device_get_fragments(zx_device_t* dev, composite_device_fragment_t* comp_list,
                          size_t comp_count, size_t* comp_actual);

// Returns the specific protocol from the named fragment, identified by the name
// provided when it was created via`device_add_composite`. Returns ZX_ERR_NOT_FOUND if
// no fragment exists.
zx_status_t device_get_fragment_protocol(zx_device_t* dev, const char* name, uint32_t proto_id,
                                         void* protocol);

// retrieves metadata for a specific device
// searches parent devices to find a match
zx_status_t device_get_fragment_metadata(zx_device_t* dev, const char* name, uint32_t type,
                                         void* buf, size_t buflen, size_t* actual);

// Device State Change Functions.  These match up with the signals defined in
// the fuchsia.device.Controller interface.
//
//@ #### Device State Bits
//{
#define DEV_STATE_READABLE ZX_USER_SIGNAL_0
#define DEV_STATE_WRITABLE ZX_USER_SIGNAL_2
#define DEV_STATE_ERROR ZX_USER_SIGNAL_3
#define DEV_STATE_HANGUP ZX_USER_SIGNAL_4
#define DEV_STATE_OOB ZX_USER_SIGNAL_1
//}

__END_CDECLS

#endif  // SRC_LIB_DDK_INCLUDE_LIB_DDK_DEVICE_H_
