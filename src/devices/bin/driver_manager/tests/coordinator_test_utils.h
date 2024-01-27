// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_TESTS_COORDINATOR_TEST_UTILS_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_TESTS_COORDINATOR_TEST_UTILS_H_

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <fidl/fuchsia.hardware.power.statecontrol/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/driver.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/cpp/message.h>
#include <lib/fidl/cpp/wire/string_view.h>
#include <lib/fidl/cpp/wire/vector_view.h>
#include <lib/fidl/txn_header.h>
#include <string.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <fbl/algorithm.h>
#include <mock-boot-arguments/server.h>

#include "src/devices/bin/driver_manager/coordinator.h"
#include "src/devices/bin/driver_manager/devfs/devfs.h"
#include "src/devices/bin/driver_manager/driver_host.h"
#include "src/devices/bin/driver_manager/fdio.h"

constexpr char kSystemDriverPath[] = "#driver/platform-bus.so";

class DummyFsProvider : public FsProvider {
 public:
  ~DummyFsProvider() {}
  fidl::ClientEnd<fuchsia_io::Directory> CloneFs(const char* path) override {
    return fidl::ClientEnd<fuchsia_io::Directory>();
  }
};

CoordinatorConfig DefaultConfig(async_dispatcher_t* bootargs_dispatcher,
                                mock_boot_arguments::Server* boot_args,
                                fidl::WireSyncClient<fuchsia_boot::Arguments>* client);
void InitializeCoordinator(Coordinator* coordinator);

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_TESTS_COORDINATOR_TEST_UTILS_H_
