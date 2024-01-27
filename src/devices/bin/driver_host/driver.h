// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_H_
#define SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_H_

#include <lib/ddk/driver.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/zx/result.h>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "lib/sync/cpp/completion.h"
#include "src/devices/bin/driver_host/zx_driver.h"

// Per driver instance unique context. Primarily use for tracking the default driver runtime
// dispatcher.
class Driver : public fbl::RefCounted<Driver> {
 public:
  // |zx_driver| must outlive |Driver|.
  // |default_dispatcher_scheduler_role| is the scheduler role to set for the default dispatcher
  // created for the driver. It may be an empty string.
  static zx::result<fbl::RefPtr<Driver>> Create(
      zx_driver_t* zx_driver, std::string_view default_dispatcher_scheduler_role = "");

  explicit Driver(zx_driver_t* zx_driver) : zx_driver_(zx_driver) {}

  // No copy, no move.
  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;
  Driver(Driver&&) = delete;
  Driver& operator=(Driver&&) = delete;

  ~Driver();

  zx_driver_t* zx_driver() const { return zx_driver_; }

  fdf::UnownedDispatcher dispatcher() const { return dispatcher_.borrow(); }

  // api_lock_ should *not* be held when calling this since anything running on the dispatcher might
  // need it and we'd end up in a deadlock.
  void StopDispatcher() {
    dispatcher_.ShutdownAsync();
    released_.Wait();
  }

  bool IsDispatcherShutdown() { return released_.Wait(zx::time::infinite_past()) == ZX_OK; }

  void IncrementDeviceCount() { device_count_++; }
  void DecrementDeviceCount() { device_count_--; }
  size_t device_count() const { return device_count_; }

 private:
  zx_driver_t* zx_driver_;

  // Signalled once dispatcher has been shut down.
  libsync::Completion released_;

  fdf::Dispatcher dispatcher_;
  size_t device_count_ = 0;
};

// Reference to a driver. Used so we know how many devices exist for a particular driver.
class DriverRef {
 public:
  explicit DriverRef(Driver* driver) : driver_(driver) { driver_->IncrementDeviceCount(); }
  ~DriverRef() {
    if (driver_) {
      driver_->DecrementDeviceCount();
    }
  }

  void Destroy() {
    ZX_ASSERT(driver_ != nullptr);
    driver_->DecrementDeviceCount();
    driver_ = nullptr;
  }

 private:
  Driver* driver_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_H_
