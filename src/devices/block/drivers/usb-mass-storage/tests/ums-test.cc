// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <fidl/fuchsia.hardware.usb.peripheral.block/cpp/wire.h>
#include <fidl/fuchsia.hardware.usb.peripheral/cpp/wire.h>
#include <fidl/fuchsia.hardware.usb.virtual.bus/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/usb-virtual-bus-launcher/usb-virtual-bus-launcher.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <memory>

#include <fbl/string.h>
#include <usb/usb.h>
#include <zxtest/zxtest.h>

#include "src/lib/storage/block_client/cpp/remote_block_device.h"

namespace usb_virtual_bus {
namespace {

using usb_virtual::BusLauncher;

zx_status_t BRead(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                  size_t buffer_size, size_t offset) {
  return block_client::SingleReadBytes(device, buffer, buffer_size, offset);
}

zx_status_t BWrite(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                   size_t buffer_size, size_t offset) {
  return block_client::SingleWriteBytes(device, buffer, buffer_size, offset);
}

namespace usb_peripheral = fuchsia_hardware_usb_peripheral;
namespace usb_peripheral_block = fuchsia_hardware_usb_peripheral_block;

constexpr const char kManufacturer[] = "Google";
constexpr const char kProduct[] = "USB test drive";
constexpr const char kSerial[] = "ebfd5ad49d2a";

template <typename T>
void ValidateResult(const T& result) {
  ASSERT_OK(result.status());
  ASSERT_OK(result.value().status);
}

usb_peripheral::wire::DeviceDescriptor GetDeviceDescriptor() {
  usb_peripheral::wire::DeviceDescriptor device_desc = {};
  device_desc.bcd_usb = htole16(0x0200);
  device_desc.b_device_class = 0;
  device_desc.b_device_sub_class = 0;
  device_desc.b_device_protocol = 0;
  device_desc.b_max_packet_size0 = 64;
  // idVendor and idProduct are filled in later
  device_desc.bcd_device = htole16(0x0100);
  // iManufacturer; iProduct and iSerialNumber are filled in later
  device_desc.b_num_configurations = 1;

  device_desc.manufacturer = fidl::StringView(kManufacturer);
  device_desc.product = fidl::StringView(kProduct);
  device_desc.serial = fidl::StringView(kSerial);

  device_desc.id_vendor = htole16(0x18D1);
  device_desc.id_product = htole16(0xA021);
  return device_desc;
}

class BlockDeviceController {
 public:
  explicit BlockDeviceController(BusLauncher* bus) : bus_(bus) {}

  void Disconnect() {
    cachecontrol_ = {};
    ASSERT_OK(bus_->ClearPeripheralDeviceFunctions());
    ASSERT_OK(bus_->Disconnect());
  }

  void Connect() {
    using ConfigurationDescriptor =
        ::fidl::VectorView<fuchsia_hardware_usb_peripheral::wire::FunctionDescriptor>;
    usb_peripheral::wire::FunctionDescriptor ums_function_desc = {
        .interface_class = USB_CLASS_MSC,
        .interface_subclass = USB_SUBCLASS_MSC_SCSI,
        .interface_protocol = USB_PROTOCOL_MSC_BULK_ONLY,
    };

    std::vector<usb_peripheral::wire::FunctionDescriptor> function_descs;
    function_descs.push_back(ums_function_desc);
    std::vector<ConfigurationDescriptor> config_descs;
    config_descs.emplace_back(
        fidl::VectorView<usb_peripheral::wire::FunctionDescriptor>::FromExternal(function_descs));
    ASSERT_OK(bus_->SetupPeripheralDevice(GetDeviceDescriptor(), std::move(config_descs)));

    fbl::unique_fd fd{openat(bus_->GetRootFd(), "class/usb-cache-test", O_RDONLY)};

    fbl::String devpath;
    while (fdio_watch_directory(fd.get(), WaitForAnyFile, ZX_TIME_INFINITE, &devpath) !=
           ZX_ERR_STOP) {
    }
    fdio_cpp::UnownedFdioCaller caller(fd);

    zx::result client_end =
        component::ConnectAt<usb_peripheral_block::Device>(caller.directory(), devpath.c_str());
    ASSERT_OK(client_end);

    cachecontrol_.Bind(std::move(client_end.value()));
  }

  void EnableWritebackCache() {
    auto result = cachecontrol_->EnableWritebackCache();
    ASSERT_NO_FATAL_FAILURE(ValidateResult(result));
  }

  void DisableWritebackCache() {
    auto result = cachecontrol_->DisableWritebackCache();
    ASSERT_NO_FATAL_FAILURE(ValidateResult(result));
  }

  void SetWritebackCacheReported(bool report) {
    auto result = cachecontrol_->SetWritebackCacheReported(report);
    ASSERT_NO_FATAL_FAILURE(ValidateResult(result));
  }

 private:
  BusLauncher* bus_;
  fidl::WireSyncClient<usb_peripheral_block::Device> cachecontrol_;
};

class UmsTest : public zxtest::Test {
 public:
  void SetUp() override {
    auto bus = BusLauncher::Create();
    ASSERT_OK(bus.status_value());
    bus_ = std::move(bus.value());
    ASSERT_NO_FATAL_FAILURE(InitUMS(&devpath_));
  }

  void TearDown() override {
    ASSERT_OK(bus_->ClearPeripheralDeviceFunctions());

    ASSERT_OK(bus_->Disable());
  }

 protected:
  // Initialize UMS. Asserts on failure.
  void InitUMS(fbl::String* devpath) {
    using ConfigurationDescriptor =
        ::fidl::VectorView<fuchsia_hardware_usb_peripheral::wire::FunctionDescriptor>;
    usb_peripheral::wire::FunctionDescriptor ums_function_desc = {
        .interface_class = USB_CLASS_MSC,
        .interface_subclass = USB_SUBCLASS_MSC_SCSI,
        .interface_protocol = USB_PROTOCOL_MSC_BULK_ONLY,
    };

    std::vector<usb_peripheral::wire::FunctionDescriptor> function_descs;
    function_descs.push_back(ums_function_desc);
    std::vector<ConfigurationDescriptor> config_descs;
    config_descs.emplace_back(
        fidl::VectorView<usb_peripheral::wire::FunctionDescriptor>::FromExternal(function_descs));

    ASSERT_OK(bus_->SetupPeripheralDevice(GetDeviceDescriptor(), std::move(config_descs)));

    fbl::unique_fd fd(openat(bus_->GetRootFd(), "class/block", O_RDONLY));
    while (fdio_watch_directory(fd.get(), WaitForAnyFile, ZX_TIME_INFINITE, devpath) !=
           ZX_ERR_STOP) {
    }
    *devpath = fbl::String::Concat({fbl::String("class/block/"), *devpath});
  }

  std::optional<BusLauncher> bus_;
  fbl::String devpath_;
  zx::unowned_channel peripheral_;
  zx::unowned_channel virtual_bus_handle_;
  fbl::String GetTestdevPath() {
    // Open the block device
    // Special case for bad block mode. Need to enumerate the singleton block device.
    // NOTE: This MUST be a tight loop with NO sleeps in order to reproduce
    // the block-watcher deadlock. Changing the timing even slightly
    // makes this test invalid.
    while (true) {
      fbl::unique_fd fd(openat(bus_->GetRootFd(), "class/block", O_RDONLY));
      DIR* dir_handle = fdopendir(fd.get());
      auto release_dir = fit::defer([=]() { closedir(dir_handle); });
      for (dirent* ent = readdir(dir_handle); ent; ent = readdir(dir_handle)) {
        std::string_view name(ent->d_name);
        if (name == ".") {
          continue;
        }
        last_known_devpath_ = fbl::String::Concat({fbl::String("class/block/"), name});
        return last_known_devpath_;
      }
    }
  }

  // Waits for the block device to be removed
  // TODO (fxbug.dev/33183, fxbug.dev/33378) -- Use something better
  // than a busy loop.
  void WaitForRemove() {
    struct stat dirinfo;
    // NOTE: This MUST be a tight loop with NO sleeps in order to reproduce
    // the block-watcher deadlock. Changing the timing even slightly
    // makes this test invalid.
    while (!stat(last_known_devpath_.c_str(), &dirinfo))
      ;
  }

 private:
  fbl::String last_known_devpath_;
};

TEST_F(UmsTest, DISABLED_ReconnectTest) {
  // Disconnect and re-connect the block device 50 times as a sanity check
  // for race conditions and deadlocks.
  // If the test freezes; or something crashes at this point, it is likely
  // a regression in a driver (not a test flake).
  BlockDeviceController controller(&bus_.value());
  for (size_t i = 0; i < 50; i++) {
    ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
    WaitForRemove();
    ASSERT_NO_FATAL_FAILURE(controller.Connect());
    GetTestdevPath();
  }
  ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
}

TEST_F(UmsTest, DISABLED_CachedWriteWithNoFlushShouldBeDiscarded) {
  // Enable writeback caching on the block device
  BlockDeviceController controller(&bus_.value());
  ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
  ASSERT_NO_FATAL_FAILURE(controller.Connect());
  ASSERT_NO_FATAL_FAILURE(controller.SetWritebackCacheReported(true));
  ASSERT_NO_FATAL_FAILURE(controller.EnableWritebackCache());
  fdio_cpp::UnownedFdioCaller caller(bus_->GetRootFd());

  uint32_t blk_size;
  std::unique_ptr<uint8_t[]> read_buffer;
  {
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), GetTestdevPath());
    ASSERT_OK(client_end);

    {
      const fidl::WireResult result = fidl::WireCall(client_end.value())->GetInfo();
      ASSERT_OK(result.status());
      const fit::result response = result.value();
      ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));
      blk_size = response.value()->info.block_size;
    }
    read_buffer.reset(new uint8_t[blk_size]);

    ASSERT_EQ(ZX_OK, BRead(client_end.value(), read_buffer.get(), blk_size, 0));
  }
  std::unique_ptr<uint8_t[]> write_buffer(new uint8_t[blk_size]);
  {
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), GetTestdevPath());
    ASSERT_OK(client_end);
    // Create a pattern to write to the block device
    for (size_t i = 0; i < blk_size; i++) {
      write_buffer.get()[i] = static_cast<unsigned char>(i);
    }
    // Write the data to the block device
    ASSERT_EQ(ZX_OK, BWrite(client_end.value(), write_buffer.get(), blk_size, 0));
  }
  // Disconnect the block device without flushing the cache.
  // This will cause the data that was written to be discarded.
  ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
  ASSERT_NO_FATAL_FAILURE(controller.Connect());
  {
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), GetTestdevPath());
    ASSERT_OK(client_end);
    ASSERT_EQ(ZX_OK, BRead(client_end.value(), write_buffer.get(), blk_size, 0));
    ASSERT_NE(0, memcmp(read_buffer.get(), write_buffer.get(), blk_size));
  }
}

TEST_F(UmsTest, DISABLED_UncachedWriteShouldBePersistedToBlockDevice) {
  BlockDeviceController controller(&bus_.value());
  // Disable writeback caching on the device
  ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
  ASSERT_NO_FATAL_FAILURE(controller.Connect());
  ASSERT_NO_FATAL_FAILURE(controller.SetWritebackCacheReported(false));
  ASSERT_NO_FATAL_FAILURE(controller.DisableWritebackCache());
  fdio_cpp::UnownedFdioCaller caller(bus_->GetRootFd());

  uint32_t blk_size;
  std::unique_ptr<uint8_t[]> write_buffer;
  {
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), GetTestdevPath());
    ASSERT_OK(client_end);
    {
      const fidl::WireResult result = fidl::WireCall(client_end.value())->GetInfo();
      ASSERT_OK(result.status());
      const fit::result response = result.value();
      ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));
      blk_size = response.value()->info.block_size;
    }

    // Allocate our buffer
    write_buffer.reset(new uint8_t[blk_size]);
    // Generate and write a pattern to the block device
    for (size_t i = 0; i < blk_size; i++) {
      write_buffer.get()[i] = static_cast<unsigned char>(i);
    }
    ASSERT_EQ(ZX_OK, BWrite(client_end.value(), write_buffer.get(), blk_size, 0));
    memset(write_buffer.get(), 0, blk_size);
  }
  // Disconnect and re-connect the block device
  ASSERT_NO_FATAL_FAILURE(controller.Disconnect());
  ASSERT_NO_FATAL_FAILURE(controller.Connect());
  {
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), GetTestdevPath());
    ASSERT_OK(client_end);
    // Read back the pattern, which should match what was written
    // since writeback caching was disabled.
    ASSERT_EQ(ZX_OK, BRead(client_end.value(), write_buffer.get(), blk_size, 0));
    for (size_t i = 0; i < blk_size; i++) {
      ASSERT_EQ(write_buffer.get()[i], static_cast<unsigned char>(i));
    }
  }
}

TEST_F(UmsTest, BlkdevTest) {
  char errmsg[1024];
  fdio_spawn_action_t actions[1];
  actions[0] = {};
  actions[0].action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY;
  zx_handle_t fd_channel;
  ASSERT_OK(fdio_fd_clone(bus_->GetRootFd(), &fd_channel));
  actions[0].ns.handle = fd_channel;
  actions[0].ns.prefix = "/dev2";
  fbl::String path = fbl::String::Concat({fbl::String("/dev2/"), GetTestdevPath()});
  const char* argv[] = {"/pkg/bin/blktest", "-d", path.c_str(), nullptr};
  zx_handle_t process;
  ASSERT_OK(fdio_spawn_etc(zx_job_default(), FDIO_SPAWN_CLONE_ALL, "/pkg/bin/blktest", argv,
                           nullptr, 1, actions, &process, errmsg));
  uint32_t observed;
  zx_object_wait_one(process, ZX_PROCESS_TERMINATED, ZX_TIME_INFINITE, &observed);
  zx_info_process_t proc_info;
  EXPECT_OK(zx_object_get_info(process, ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr,
                               nullptr));
  EXPECT_EQ(proc_info.return_code, 0);
}

}  // namespace
}  // namespace usb_virtual_bus
