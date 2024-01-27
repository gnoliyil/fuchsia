// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <inttypes.h>
#include <lib/component/incoming/cpp/clone.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/io.h>
#include <lib/fdio/watcher.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/fit/defer.h>
#include <lib/inspect/cpp/reader.h>
#include <zircon/errors.h>

#include <condition_variable>
#include <ostream>

#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramdisk.h>
#include <zxtest/zxtest.h>

#include "src/security/lib/fcrypto/digest.h"
#include "src/security/lib/fcrypto/secret.h"
#include "src/security/lib/zxcrypt/client.h"

namespace {
constexpr zx::duration kTimeout = zx::sec(3);
constexpr uint32_t kBlockSz = 512;
constexpr uint32_t kBlockCnt = 20;

std::string GetInspectInstanceGuid(const zx::vmo& inspect_vmo) {
  auto base_hierarchy = inspect::ReadFromVmo(inspect_vmo).take_value();
  auto* hierarchy = base_hierarchy.GetByPath({"zxcrypt0x0"});
  if (hierarchy == nullptr) {
    return "";
  }
  auto* property = hierarchy->node().get_property<inspect::StringPropertyValue>("instance_guid");
  if (property == nullptr) {
    return "";
  }
  return property->value();
}

void GetInspectVMOHandle(const fbl::unique_fd& devfs_root, zx::vmo& out_vmo) {
  constexpr char path[] = "diagnostics/class/zxcrypt/000.inspect";
  ASSERT_OK(device_watcher::RecursiveWaitForFile(devfs_root.get(), path));
  fbl::unique_fd fd;
  ASSERT_TRUE(fd.reset(openat(devfs_root.get(), path, O_RDONLY)), "%s", strerror(errno));
  ASSERT_OK(fdio_get_vmo_clone(fd.get(), out_vmo.reset_and_get_address()));
}

TEST(ZxcryptInspect, ExportsGuid) {
  // Zxcrypt volume manager requires this.
  driver_integration_test::IsolatedDevmgr devmgr;
  driver_integration_test::IsolatedDevmgr::Args args;
  ASSERT_EQ(driver_integration_test::IsolatedDevmgr::Create(&args, &devmgr), ZX_OK);
  ASSERT_EQ(device_watcher::RecursiveWaitForFile(devmgr.devfs_root().get(),
                                                 "sys/platform/00:00:2d/ramctl")
                .status_value(),
            ZX_OK);

  fbl::unique_fd devfs_root_fd = devmgr.devfs_root().duplicate();

  // Create a new ramdisk to stick our zxcrypt instance on.
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_OK(ramdisk_create_at(devmgr.devfs_root().get(), kBlockSz, kBlockCnt, &ramdisk));
  auto cleanup = fit::defer([ramdisk]() { ASSERT_OK(ramdisk_destroy(ramdisk)); });
  zx::result channel =
      device_watcher::RecursiveWaitForFile(devfs_root_fd.get(), ramdisk_get_path(ramdisk));
  ASSERT_OK(channel.status_value());
  fbl::unique_fd ramdisk_fd;
  {
    // TODO(https://fxbug.dev/112484): this relies on multiplexing.
    fidl::UnownedClientEnd<fuchsia_io::Node> client(ramdisk_get_block_interface(ramdisk));
    zx::result owned = component::Clone(client);
    ASSERT_OK(owned.status_value());
    ASSERT_OK(
        fdio_fd_create(owned.value().TakeChannel().release(), ramdisk_fd.reset_and_get_address()));
  }

  // Create a new zxcrypt volume manager using the ramdisk.
  auto vol_mgr =
      std::make_unique<zxcrypt::VolumeManager>(std::move(ramdisk_fd), std::move(devfs_root_fd));
  zx::channel zxc_client_chan;
  ASSERT_OK(vol_mgr->OpenClient(kTimeout, zxc_client_chan));

  // Create a new crypto key.
  crypto::Secret key;
  size_t digest_len;
  ASSERT_OK(crypto::digest::GetDigestLen(crypto::digest::kSHA256, &digest_len));
  ASSERT_OK(key.Generate(digest_len));

  // Unsealing should fail right now until we format. It'll look like a bad key error, but really we
  // haven't even got a formatted device yet.
  zxcrypt::EncryptedVolumeClient volume_client(std::move(zxc_client_chan));
  ASSERT_EQ(volume_client.Unseal(key.get(), key.len(), 0), ZX_ERR_ACCESS_DENIED);
  {
    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURE(GetInspectVMOHandle(devmgr.devfs_root(), vmo));
    ASSERT_TRUE(GetInspectInstanceGuid(vmo).empty());
  }

  // After formatting, we should be able to unseal a device and see its GUID in inspect.
  ASSERT_OK(volume_client.Format(key.get(), key.len(), 0));

  ASSERT_OK(volume_client.Unseal(key.get(), key.len(), 0));
  {
    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURE(GetInspectVMOHandle(devmgr.devfs_root(), vmo));
    std::string guid = GetInspectInstanceGuid(vmo);
    ASSERT_FALSE(guid.empty());
  }

  ASSERT_OK(volume_client.Seal());
}

}  // namespace
