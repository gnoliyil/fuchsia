// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.protocolbase banjo file

#pragma once

#include <banjo/examples/protocolbase/c/banjo.h>
#include <ddktl/device-internal.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fdf/env.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include "banjo-internal.h"

// DDK protocolbase-protocol support
//
// :: Proxies ::
//
// ddk::SynchronousBaseProtocolClient is a simple wrapper around
// synchronous_base_protocol_t. It does not own the pointers passed to it.
//
// :: Mixins ::
//
// ddk::SynchronousBaseProtocol is a mixin class that simplifies writing DDK drivers
// that implement the synchronous-base protocol. It doesn't set the base protocol.
//
// :: Examples ::
//
// // A driver that implements a ZX_PROTOCOL_SYNCHRONOUS_BASE device.
// class SynchronousBaseDevice;
// using SynchronousBaseDeviceType = ddk::Device<SynchronousBaseDevice, /* ddk mixins */>;
//
// class SynchronousBaseDevice : public SynchronousBaseDeviceType,
//                      public ddk::SynchronousBaseProtocol<SynchronousBaseDevice> {
//   public:
//     SynchronousBaseDevice(zx_device_t* parent)
//         : SynchronousBaseDeviceType(parent) {}
//
//     zx_status_t SynchronousBaseStatus(zx_status_t status, zx_status_t* out_status_2);
//
//     zx_time_t SynchronousBaseTime(zx_time_t time, zx_time_t* out_time_2);
//
//     zx_duration_t SynchronousBaseDuration(zx_duration_t duration, zx_duration_t* out_duration_2);
//
//     zx_koid_t SynchronousBaseKoid(zx_koid_t koid, zx_koid_t* out_koid_2);
//
//     zx_off_t SynchronousBaseOff(zx_off_t off, zx_off_t* out_off_2);
//
//     ...
// };
// :: Proxies ::
//
// ddk::DriverTransportProtocolClient is a simple wrapper around
// driver_transport_protocol_t. It does not own the pointers passed to it.
//
// :: Mixins ::
//
// ddk::DriverTransportProtocol is a mixin class that simplifies writing DDK drivers
// that implement the driver-transport protocol. It doesn't set the base protocol.
//
// :: Examples ::
//
// // A driver that implements a ZX_PROTOCOL_DRIVER_TRANSPORT device.
// class DriverTransportDevice;
// using DriverTransportDeviceType = ddk::Device<DriverTransportDevice, /* ddk mixins */>;
//
// class DriverTransportDevice : public DriverTransportDeviceType,
//                      public ddk::DriverTransportProtocol<DriverTransportDevice> {
//   public:
//     DriverTransportDevice(zx_device_t* parent)
//         : DriverTransportDeviceType(parent) {}
//
//     zx_status_t DriverTransportStatus(zx_status_t status);
//
//     ...
// };
// :: Proxies ::
//
// ddk::AsyncBaseProtocolClient is a simple wrapper around
// async_base_protocol_t. It does not own the pointers passed to it.
//
// :: Mixins ::
//
// ddk::AsyncBaseProtocol is a mixin class that simplifies writing DDK drivers
// that implement the async-base protocol. It doesn't set the base protocol.
//
// :: Examples ::
//
// // A driver that implements a ZX_PROTOCOL_ASYNC_BASE device.
// class AsyncBaseDevice;
// using AsyncBaseDeviceType = ddk::Device<AsyncBaseDevice, /* ddk mixins */>;
//
// class AsyncBaseDevice : public AsyncBaseDeviceType,
//                      public ddk::AsyncBaseProtocol<AsyncBaseDevice> {
//   public:
//     AsyncBaseDevice(zx_device_t* parent)
//         : AsyncBaseDeviceType(parent) {}
//
//     void AsyncBaseStatus(zx_status_t status, async_base_status_callback callback, void* cookie);
//
//     void AsyncBaseTime(zx_time_t time, async_base_time_callback callback, void* cookie);
//
//     void AsyncBaseDuration(zx_duration_t duration, async_base_duration_callback callback, void* cookie);
//
//     void AsyncBaseKoid(zx_koid_t koid, async_base_koid_callback callback, void* cookie);
//
//     void AsyncBaseOff(zx_off_t off, async_base_off_callback callback, void* cookie);
//
//     ...
// };

namespace ddk {

template <typename D, typename Base = internal::base_mixin, bool runtime_enforce_no_reentrancy = false>
class SynchronousBaseProtocol : public Base {
public:
    SynchronousBaseProtocol() {
        synchronous_base_protocol_server_driver_ = fdf_env_get_current_driver();
        internal::CheckSynchronousBaseProtocolSubclass<D>();
        synchronous_base_protocol_ops_.status = SynchronousBaseStatus;
        synchronous_base_protocol_ops_.time = SynchronousBaseTime;
        synchronous_base_protocol_ops_.duration = SynchronousBaseDuration;
        synchronous_base_protocol_ops_.koid = SynchronousBaseKoid;
        synchronous_base_protocol_ops_.off = SynchronousBaseOff;

        if constexpr (internal::is_base_proto<Base>::value) {
            auto dev = static_cast<D*>(this);
            // Can only inherit from one base_protocol implementation.
            ZX_ASSERT(dev->ddk_proto_id_ == 0);
            dev->ddk_proto_id_ = ZX_PROTOCOL_SYNCHRONOUS_BASE;
            dev->ddk_proto_ops_ = &synchronous_base_protocol_ops_;
        }
    }

    const void* synchronous_base_protocol_server_driver() const {
        return synchronous_base_protocol_server_driver_;
    }

protected:
    synchronous_base_protocol_ops_t synchronous_base_protocol_ops_ = {};
    const void* synchronous_base_protocol_server_driver_;

private:
    static const void* GetServerDriver(void* ctx) {
        return static_cast<D*>(ctx)->synchronous_base_protocol_server_driver();
    }

    static zx_status_t SynchronousBaseStatus(void* ctx, zx_status_t status, zx_status_t* out_status_2) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->SynchronousBaseStatus(status, out_status_2);
        fdf_env_register_driver_exit();
        return ret;
    }
    static zx_time_t SynchronousBaseTime(void* ctx, zx_time_t time, zx_time_t* out_time_2) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->SynchronousBaseTime(time, out_time_2);
        fdf_env_register_driver_exit();
        return ret;
    }
    static zx_duration_t SynchronousBaseDuration(void* ctx, zx_duration_t duration, zx_duration_t* out_duration_2) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->SynchronousBaseDuration(duration, out_duration_2);
        fdf_env_register_driver_exit();
        return ret;
    }
    static zx_koid_t SynchronousBaseKoid(void* ctx, zx_koid_t koid, zx_koid_t* out_koid_2) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->SynchronousBaseKoid(koid, out_koid_2);
        fdf_env_register_driver_exit();
        return ret;
    }
    static zx_off_t SynchronousBaseOff(void* ctx, zx_off_t off, zx_off_t* out_off_2) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->SynchronousBaseOff(off, out_off_2);
        fdf_env_register_driver_exit();
        return ret;
    }
};

class SynchronousBaseProtocolClient {
public:
    SynchronousBaseProtocolClient()
        : ops_(nullptr), ctx_(nullptr) {}
    SynchronousBaseProtocolClient(const synchronous_base_protocol_t* proto)
        : ops_(proto->ops), ctx_(proto->ctx) {}

    SynchronousBaseProtocolClient(zx_device_t* parent) {
        synchronous_base_protocol_t proto;
        if (device_get_protocol(parent, ZX_PROTOCOL_SYNCHRONOUS_BASE, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    SynchronousBaseProtocolClient(zx_device_t* parent, const char* fragment_name) {
        synchronous_base_protocol_t proto;
        if (device_get_fragment_protocol(parent, fragment_name, ZX_PROTOCOL_SYNCHRONOUS_BASE, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    // Create a SynchronousBaseProtocolClient from the given parent device + "fragment".
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent,
                                        SynchronousBaseProtocolClient* result) {
        synchronous_base_protocol_t proto;
        zx_status_t status = device_get_protocol(
                parent, ZX_PROTOCOL_SYNCHRONOUS_BASE, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = SynchronousBaseProtocolClient(&proto);
        return ZX_OK;
    }

    // Create a SynchronousBaseProtocolClient from the given parent device.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent, const char* fragment_name,
                                        SynchronousBaseProtocolClient* result) {
        synchronous_base_protocol_t proto;
        zx_status_t status = device_get_fragment_protocol(parent, fragment_name,
                                 ZX_PROTOCOL_SYNCHRONOUS_BASE, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = SynchronousBaseProtocolClient(&proto);
        return ZX_OK;
    }

    void GetProto(synchronous_base_protocol_t* proto) const {
        proto->ctx = ctx_;
        proto->ops = ops_;
    }
    bool is_valid() const {
        return ops_ != nullptr;
    }
    void clear() {
        ctx_ = nullptr;
        ops_ = nullptr;
    }

    zx_status_t Status(zx_status_t status, zx_status_t* out_status_2) const {
        return ops_->status(ctx_, status, out_status_2);
    }

    zx_time_t Time(zx_time_t time, zx_time_t* out_time_2) const {
        return ops_->time(ctx_, time, out_time_2);
    }

    zx_duration_t Duration(zx_duration_t duration, zx_duration_t* out_duration_2) const {
        return ops_->duration(ctx_, duration, out_duration_2);
    }

    zx_koid_t Koid(zx_koid_t koid, zx_koid_t* out_koid_2) const {
        return ops_->koid(ctx_, koid, out_koid_2);
    }

    zx_off_t Off(zx_off_t off, zx_off_t* out_off_2) const {
        return ops_->off(ctx_, off, out_off_2);
    }

private:
    const synchronous_base_protocol_ops_t* ops_;
    void* ctx_;
};

template <typename D, typename Base = internal::base_mixin, bool runtime_enforce_no_reentrancy = false>
class DriverTransportProtocol : public Base {
public:
    DriverTransportProtocol() {
        driver_transport_protocol_server_driver_ = fdf_env_get_current_driver();
        internal::CheckDriverTransportProtocolSubclass<D>();
        driver_transport_protocol_ops_.status = DriverTransportStatus;

        if constexpr (internal::is_base_proto<Base>::value) {
            auto dev = static_cast<D*>(this);
            // Can only inherit from one base_protocol implementation.
            ZX_ASSERT(dev->ddk_proto_id_ == 0);
            dev->ddk_proto_id_ = ZX_PROTOCOL_DRIVER_TRANSPORT;
            dev->ddk_proto_ops_ = &driver_transport_protocol_ops_;
        }
    }

    const void* driver_transport_protocol_server_driver() const {
        return driver_transport_protocol_server_driver_;
    }

protected:
    driver_transport_protocol_ops_t driver_transport_protocol_ops_ = {};
    const void* driver_transport_protocol_server_driver_;

private:
    static const void* GetServerDriver(void* ctx) {
        return static_cast<D*>(ctx)->driver_transport_protocol_server_driver();
    }

    static zx_status_t DriverTransportStatus(void* ctx, zx_status_t status) {
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        auto ret = static_cast<D*>(ctx)->DriverTransportStatus(status);
        fdf_env_register_driver_exit();
        return ret;
    }
};

class DriverTransportProtocolClient {
public:
    DriverTransportProtocolClient()
        : ops_(nullptr), ctx_(nullptr) {}
    DriverTransportProtocolClient(const driver_transport_protocol_t* proto)
        : ops_(proto->ops), ctx_(proto->ctx) {}

    DriverTransportProtocolClient(zx_device_t* parent) {
        driver_transport_protocol_t proto;
        if (device_get_protocol(parent, ZX_PROTOCOL_DRIVER_TRANSPORT, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    DriverTransportProtocolClient(zx_device_t* parent, const char* fragment_name) {
        driver_transport_protocol_t proto;
        if (device_get_fragment_protocol(parent, fragment_name, ZX_PROTOCOL_DRIVER_TRANSPORT, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    // Create a DriverTransportProtocolClient from the given parent device + "fragment".
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent,
                                        DriverTransportProtocolClient* result) {
        driver_transport_protocol_t proto;
        zx_status_t status = device_get_protocol(
                parent, ZX_PROTOCOL_DRIVER_TRANSPORT, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = DriverTransportProtocolClient(&proto);
        return ZX_OK;
    }

    // Create a DriverTransportProtocolClient from the given parent device.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent, const char* fragment_name,
                                        DriverTransportProtocolClient* result) {
        driver_transport_protocol_t proto;
        zx_status_t status = device_get_fragment_protocol(parent, fragment_name,
                                 ZX_PROTOCOL_DRIVER_TRANSPORT, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = DriverTransportProtocolClient(&proto);
        return ZX_OK;
    }

    void GetProto(driver_transport_protocol_t* proto) const {
        proto->ctx = ctx_;
        proto->ops = ops_;
    }
    bool is_valid() const {
        return ops_ != nullptr;
    }
    void clear() {
        ctx_ = nullptr;
        ops_ = nullptr;
    }

    zx_status_t Status(zx_status_t status) const {
        return ops_->status(ctx_, status);
    }

private:
    const driver_transport_protocol_ops_t* ops_;
    void* ctx_;
};

template <typename D, typename Base = internal::base_mixin, bool runtime_enforce_no_reentrancy = false>
class AsyncBaseProtocol : public Base {
public:
    AsyncBaseProtocol() {
        async_base_protocol_server_driver_ = fdf_env_get_current_driver();
        internal::CheckAsyncBaseProtocolSubclass<D>();
        async_base_protocol_ops_.status = AsyncBaseStatus;
        async_base_protocol_ops_.time = AsyncBaseTime;
        async_base_protocol_ops_.duration = AsyncBaseDuration;
        async_base_protocol_ops_.koid = AsyncBaseKoid;
        async_base_protocol_ops_.off = AsyncBaseOff;

        if constexpr (internal::is_base_proto<Base>::value) {
            auto dev = static_cast<D*>(this);
            // Can only inherit from one base_protocol implementation.
            ZX_ASSERT(dev->ddk_proto_id_ == 0);
            dev->ddk_proto_id_ = ZX_PROTOCOL_ASYNC_BASE;
            dev->ddk_proto_ops_ = &async_base_protocol_ops_;
        }
    }

    const void* async_base_protocol_server_driver() const {
        return async_base_protocol_server_driver_;
    }

protected:
    async_base_protocol_ops_t async_base_protocol_ops_ = {};
    const void* async_base_protocol_server_driver_;

private:
    static const void* GetServerDriver(void* ctx) {
        return static_cast<D*>(ctx)->async_base_protocol_server_driver();
    }

    static void AsyncBaseStatus(void* ctx, zx_status_t status, async_base_status_callback callback, void* cookie) {
        if (callback && cookie) {
            struct AsyncCallbackWrapper {
                const void* driver;
                async_base_status_callback callback;
                void* cookie;
            };

            AsyncCallbackWrapper* wrapper = new AsyncCallbackWrapper {
                fdf_env_get_current_driver(),
                callback,
                cookie,
            };

            cookie = wrapper;
            callback = [](void* ctx, zx_status_t status, zx_status_t status_2) {
                AsyncCallbackWrapper* wrapper = static_cast<AsyncCallbackWrapper*>(ctx);
                fdf_env_register_driver_entry(wrapper->driver, runtime_enforce_no_reentrancy);
                wrapper->callback(wrapper->cookie, status, status_2);
                fdf_env_register_driver_exit();
                delete wrapper;
            };
        }
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        static_cast<D*>(ctx)->AsyncBaseStatus(status, callback, cookie);
        fdf_env_register_driver_exit();
    }
    static void AsyncBaseTime(void* ctx, zx_time_t time, async_base_time_callback callback, void* cookie) {
        if (callback && cookie) {
            struct AsyncCallbackWrapper {
                const void* driver;
                async_base_time_callback callback;
                void* cookie;
            };

            AsyncCallbackWrapper* wrapper = new AsyncCallbackWrapper {
                fdf_env_get_current_driver(),
                callback,
                cookie,
            };

            cookie = wrapper;
            callback = [](void* ctx, zx_time_t time, zx_time_t time_2) {
                AsyncCallbackWrapper* wrapper = static_cast<AsyncCallbackWrapper*>(ctx);
                fdf_env_register_driver_entry(wrapper->driver, runtime_enforce_no_reentrancy);
                wrapper->callback(wrapper->cookie, time, time_2);
                fdf_env_register_driver_exit();
                delete wrapper;
            };
        }
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        static_cast<D*>(ctx)->AsyncBaseTime(time, callback, cookie);
        fdf_env_register_driver_exit();
    }
    static void AsyncBaseDuration(void* ctx, zx_duration_t duration, async_base_duration_callback callback, void* cookie) {
        if (callback && cookie) {
            struct AsyncCallbackWrapper {
                const void* driver;
                async_base_duration_callback callback;
                void* cookie;
            };

            AsyncCallbackWrapper* wrapper = new AsyncCallbackWrapper {
                fdf_env_get_current_driver(),
                callback,
                cookie,
            };

            cookie = wrapper;
            callback = [](void* ctx, zx_duration_t duration, zx_duration_t duration_2) {
                AsyncCallbackWrapper* wrapper = static_cast<AsyncCallbackWrapper*>(ctx);
                fdf_env_register_driver_entry(wrapper->driver, runtime_enforce_no_reentrancy);
                wrapper->callback(wrapper->cookie, duration, duration_2);
                fdf_env_register_driver_exit();
                delete wrapper;
            };
        }
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        static_cast<D*>(ctx)->AsyncBaseDuration(duration, callback, cookie);
        fdf_env_register_driver_exit();
    }
    static void AsyncBaseKoid(void* ctx, zx_koid_t koid, async_base_koid_callback callback, void* cookie) {
        if (callback && cookie) {
            struct AsyncCallbackWrapper {
                const void* driver;
                async_base_koid_callback callback;
                void* cookie;
            };

            AsyncCallbackWrapper* wrapper = new AsyncCallbackWrapper {
                fdf_env_get_current_driver(),
                callback,
                cookie,
            };

            cookie = wrapper;
            callback = [](void* ctx, zx_koid_t koid, zx_koid_t koid_2) {
                AsyncCallbackWrapper* wrapper = static_cast<AsyncCallbackWrapper*>(ctx);
                fdf_env_register_driver_entry(wrapper->driver, runtime_enforce_no_reentrancy);
                wrapper->callback(wrapper->cookie, koid, koid_2);
                fdf_env_register_driver_exit();
                delete wrapper;
            };
        }
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        static_cast<D*>(ctx)->AsyncBaseKoid(koid, callback, cookie);
        fdf_env_register_driver_exit();
    }
    static void AsyncBaseOff(void* ctx, zx_off_t off, async_base_off_callback callback, void* cookie) {
        if (callback && cookie) {
            struct AsyncCallbackWrapper {
                const void* driver;
                async_base_off_callback callback;
                void* cookie;
            };

            AsyncCallbackWrapper* wrapper = new AsyncCallbackWrapper {
                fdf_env_get_current_driver(),
                callback,
                cookie,
            };

            cookie = wrapper;
            callback = [](void* ctx, zx_off_t off, zx_off_t off_2) {
                AsyncCallbackWrapper* wrapper = static_cast<AsyncCallbackWrapper*>(ctx);
                fdf_env_register_driver_entry(wrapper->driver, runtime_enforce_no_reentrancy);
                wrapper->callback(wrapper->cookie, off, off_2);
                fdf_env_register_driver_exit();
                delete wrapper;
            };
        }
        fdf_env_register_driver_entry(GetServerDriver(ctx), runtime_enforce_no_reentrancy);
        static_cast<D*>(ctx)->AsyncBaseOff(off, callback, cookie);
        fdf_env_register_driver_exit();
    }
};

class AsyncBaseProtocolClient {
public:
    AsyncBaseProtocolClient()
        : ops_(nullptr), ctx_(nullptr) {}
    AsyncBaseProtocolClient(const async_base_protocol_t* proto)
        : ops_(proto->ops), ctx_(proto->ctx) {}

    AsyncBaseProtocolClient(zx_device_t* parent) {
        async_base_protocol_t proto;
        if (device_get_protocol(parent, ZX_PROTOCOL_ASYNC_BASE, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    AsyncBaseProtocolClient(zx_device_t* parent, const char* fragment_name) {
        async_base_protocol_t proto;
        if (device_get_fragment_protocol(parent, fragment_name, ZX_PROTOCOL_ASYNC_BASE, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    // Create a AsyncBaseProtocolClient from the given parent device + "fragment".
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent,
                                        AsyncBaseProtocolClient* result) {
        async_base_protocol_t proto;
        zx_status_t status = device_get_protocol(
                parent, ZX_PROTOCOL_ASYNC_BASE, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = AsyncBaseProtocolClient(&proto);
        return ZX_OK;
    }

    // Create a AsyncBaseProtocolClient from the given parent device.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent, const char* fragment_name,
                                        AsyncBaseProtocolClient* result) {
        async_base_protocol_t proto;
        zx_status_t status = device_get_fragment_protocol(parent, fragment_name,
                                 ZX_PROTOCOL_ASYNC_BASE, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = AsyncBaseProtocolClient(&proto);
        return ZX_OK;
    }

    void GetProto(async_base_protocol_t* proto) const {
        proto->ctx = ctx_;
        proto->ops = ops_;
    }
    bool is_valid() const {
        return ops_ != nullptr;
    }
    void clear() {
        ctx_ = nullptr;
        ops_ = nullptr;
    }

    void Status(zx_status_t status, async_base_status_callback callback, void* cookie) const {
        ops_->status(ctx_, status, callback, cookie);
    }

    void Time(zx_time_t time, async_base_time_callback callback, void* cookie) const {
        ops_->time(ctx_, time, callback, cookie);
    }

    void Duration(zx_duration_t duration, async_base_duration_callback callback, void* cookie) const {
        ops_->duration(ctx_, duration, callback, cookie);
    }

    void Koid(zx_koid_t koid, async_base_koid_callback callback, void* cookie) const {
        ops_->koid(ctx_, koid, callback, cookie);
    }

    void Off(zx_off_t off, async_base_off_callback callback, void* cookie) const {
        ops_->off(ctx_, off, callback, cookie);
    }

private:
    const async_base_protocol_ops_t* ops_;
    void* ctx_;
};

} // namespace ddk
