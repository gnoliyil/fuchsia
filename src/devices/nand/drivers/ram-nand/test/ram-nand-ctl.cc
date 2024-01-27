// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.driver.test/cpp/wire.h>
#include <fidl/fuchsia.hardware.nand/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/fdio/cpp/caller.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <utility>

#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramnand.h>
#include <zxtest/zxtest.h>

namespace {

fuchsia_hardware_nand::wire::RamNandInfo BuildConfig() {
  return {
      .nand_info = {4096, 4, 5, 6, 0, fuchsia_hardware_nand::wire::Class::kTest, {}},
  };
}

class NandDevice {
 public:
  static zx::result<NandDevice> Create(
      fuchsia_hardware_nand::wire::RamNandInfo config = BuildConfig()) {
    std::optional<ramdevice_client::RamNand> ram_nand;
    if (zx_status_t status = ramdevice_client::RamNand::Create(std::move(config), &ram_nand);
        status != ZX_OK) {
      return zx::error(status);
    }
    return zx::ok(NandDevice(std::move(ram_nand.value())));
  }

  NandDevice(const NandDevice&) = delete;
  NandDevice& operator=(const NandDevice&) = delete;

  NandDevice(NandDevice&&) = default;
  NandDevice& operator=(NandDevice&&) = default;

  ~NandDevice() = default;

  const char* path() { return ram_nand_.path(); }
  const char* filename() { return ram_nand_.filename(); }

 private:
  explicit NandDevice(ramdevice_client::RamNand ram_nand) : ram_nand_(std::move(ram_nand)) {}

  ramdevice_client::RamNand ram_nand_;
};

TEST(RamNandCtlTest, TrivialLifetime) {
  std::unique_ptr<device_watcher::DirWatcher> watcher;
  fbl::unique_fd dir_fd(open(ramdevice_client::RamNand::kBasePath, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(dir_fd);
  ASSERT_EQ(device_watcher::DirWatcher::Create(dir_fd.get(), &watcher), ZX_OK);

  fbl::String path;
  fbl::String filename;
  {
    zx::result result = NandDevice::Create();
    ASSERT_OK(result.status_value());
    NandDevice& device = result.value();
    path = fbl::String(device.path());
    filename = fbl::String(device.filename());
  }
  ASSERT_EQ(watcher->WaitForRemoval(filename, zx::sec(5)), ZX_OK);

  fbl::unique_fd found(open(path.c_str(), O_RDWR));
  ASSERT_FALSE(found);
}

TEST(RamNandCtlTest, ExportConfig) {
  fuchsia_hardware_nand::wire::RamNandInfo config = BuildConfig();
  config.export_nand_config = true;

  zx::result device = NandDevice::Create(std::move(config));
  ASSERT_OK(device.status_value());
}

TEST(RamNandCtlTest, ExportPartitions) {
  fuchsia_hardware_nand::wire::RamNandInfo config = BuildConfig();
  config.export_partition_map = true;

  zx::result device = NandDevice::Create(std::move(config));
  ASSERT_OK(device.status_value());
}

TEST(RamNandCtlTest, CreateFailure) {
  fuchsia_hardware_nand::wire::RamNandInfo config = BuildConfig();
  config.nand_info.num_blocks = 0;

  zx::result device = NandDevice::Create(std::move(config));
  ASSERT_STATUS(device.status_value(), ZX_ERR_INVALID_ARGS);
}

}  // namespace

int main(int argc, char** argv) {
  // Connect to DriverTestRealm.
  auto client_end = component::Connect<fuchsia_driver_test::Realm>();
  if (!client_end.is_ok()) {
    fprintf(stderr, "Failed to connect to Realm FIDL: %d\n", client_end.error_value());
    return 1;
  }
  fidl::WireSyncClient client{std::move(*client_end)};

  // Start the DriverTestRealm with correct arguments.
  fidl::Arena arena;
  fuchsia_driver_test::wire::RealmArgs realm_args(arena);
  realm_args.set_root_driver(arena, "fuchsia-boot:///#driver/platform-bus.so");
  auto wire_result = client->Start(realm_args);
  if (wire_result.status() != ZX_OK) {
    fprintf(stderr, "Failed to call to Realm:Start: %d\n", wire_result.status());
    return 1;
  }
  if (wire_result->is_error()) {
    fprintf(stderr, "Realm:Start failed: %d\n", wire_result->error_value());
    return 1;
  }

  zx::result channel = device_watcher::RecursiveWaitForFile(ramdevice_client::RamNand::kBasePath);
  if (channel.is_error()) {
    fprintf(stderr, "Failed to open device file: %s\n", channel.status_string());
    return 1;
  }
  setlinebuf(stdout);
  return RUN_ALL_TESTS(argc, argv);
}
