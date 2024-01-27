// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_TESTING_DEVMGR_INTEGRATION_TEST_SHIM_INCLUDE_LIB_DEVMGR_INTEGRATION_TEST_FIXTURE_H_
#define SRC_DEVICES_TESTING_DEVMGR_INTEGRATION_TEST_SHIM_INCLUDE_LIB_DEVMGR_INTEGRATION_TEST_FIXTURE_H_

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/driver/test/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/dispatcher.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>

#include <fbl/unique_fd.h>

namespace devmgr_launcher {

struct Args {
  const char* root_device_driver = nullptr;
  bool driver_tests_enable_all = false;
  std::vector<std::string> driver_tests_enable;
  std::vector<std::string> driver_tests_disable;
};
}  // namespace devmgr_launcher

namespace devmgr_integration_test {

class IsolatedDevmgr {
 public:
  IsolatedDevmgr();
  ~IsolatedDevmgr();
  IsolatedDevmgr(const IsolatedDevmgr&) = delete;
  IsolatedDevmgr& operator=(const IsolatedDevmgr&) = delete;
  IsolatedDevmgr(IsolatedDevmgr&& other) noexcept;
  IsolatedDevmgr& operator=(IsolatedDevmgr&& other) noexcept;

  void reset() { *this = IsolatedDevmgr(); }

  // Get an args structure pre-populated with the test sysdev driver, the
  // test control driver, and the test driver directory.
  static devmgr_launcher::Args DefaultArgs();

  // Launch a new isolated devmgr.
  //
  // TODO(https://fxbug.dev/114254): Remove |dispatcher| once RealmBuilder::Build no longer requires
  // it.
  static zx::result<IsolatedDevmgr> Create(devmgr_launcher::Args args,
                                           async_dispatcher_t* dispatcher);

  // Get a fd to the root of the isolate devmgr's devfs.  This fd
  // may be used with openat() and fdio_watch_directory().
  const fbl::unique_fd& devfs_root() const { return devfs_root_; }

 private:
  std::unique_ptr<component_testing::RealmRoot> realm_;

  // FD to the root of devmgr's devfs
  fbl::unique_fd devfs_root_;
};

}  // namespace devmgr_integration_test

#endif  // SRC_DEVICES_TESTING_DEVMGR_INTEGRATION_TEST_SHIM_INCLUDE_LIB_DEVMGR_INTEGRATION_TEST_FIXTURE_H_
