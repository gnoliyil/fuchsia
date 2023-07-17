// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <fuchsia/boot/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/zx/object.h>
#include <unistd.h>
#include <zircon/status.h>

#include <array>
#include <memory>
#include <string>
#include <thread>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "src/developer/sshd-host/constants.h"
#include "src/developer/sshd-host/service.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace sshd_host {

namespace {

using fuchsia::boot::Items;

// Mock fuchsia.boot.Items server
class FakeItems : public Items {
 public:
  fidl::InterfaceRequestHandler<Items> GetHandler() { return bindings_.GetHandler(this); }

  void Get(uint32_t type, uint32_t extra, GetCallback callback) override { EXPECT_TRUE(false); }

  void GetBootloaderFile(::std::string filename, GetBootloaderFileCallback callback) override {
    if (!bootloader_file_set_) {
      callback(zx::vmo(ZX_HANDLE_INVALID));
      return;
    }
    bootloader_file_set_ = false;

    if (filename == filename_) {
      callback(std::move(vmo_));
    } else {
      callback(zx::vmo(ZX_HANDLE_INVALID));
    }
  }

  void SetFile(const char *filename, const char *payload, uint64_t payload_len) {
    filename_ = std::string(filename);

    ASSERT_EQ(zx::vmo::create(payload_len, 0, &vmo_), ZX_OK);
    ASSERT_EQ(vmo_.write((payload), 0, payload_len), ZX_OK);
    ASSERT_EQ(vmo_.set_property(ZX_PROP_VMO_CONTENT_SIZE, &payload_len, sizeof(payload_len)),
              ZX_OK);

    bootloader_file_set_ = true;
  }

 private:
  fidl::BindingSet<Items> bindings_;
  bool bootloader_file_set_ = false;
  std::string filename_;
  ::zx::vmo vmo_;
};

class SshdHostBootItemTest : public gtest::RealLoopFixture {
 public:
  void SetUp() override {
    EXPECT_EQ(loop_.StartThread(), ZX_OK);
    EXPECT_EQ(service_directory_provider_.AddService(fake_items_.GetHandler()), ZX_OK);
  }

  void TearDown() override {
    loop_.Quit();
    loop_.JoinThreads();
  }

  async::Loop loop_{&kAsyncLoopConfigNoAttachToCurrentThread};
  sys::testing::ServiceDirectoryProvider service_directory_provider_{loop_.dispatcher()};
  FakeItems fake_items_;
  thrd_t thread_;
};

void remove_authorized_keys() {
  if (unlink(kAuthorizedKeysPath)) {
    ASSERT_EQ(errno, ENOENT);
  }
  if (rmdir(kSshDirectory)) {
    ASSERT_EQ(errno, ENOENT);
  }
}

void write_authorized_keys(const char *payload, size_t len) {
  if (mkdir(kSshDirectory, 0700), 0) {
    ASSERT_EQ(errno, EEXIST);
  }
  fbl::unique_fd kfd(open(kAuthorizedKeysPath, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR));
  ASSERT_TRUE(kfd.is_valid());
  ASSERT_EQ(write(kfd.get(), payload, len), ssize_t(len));
  fsync(kfd.get());
  ASSERT_EQ(close(kfd.release()), 0);
}

void verify_authorized_keys(const char *payload, size_t len) {
  auto file_buf = std::make_unique<uint8_t[]>(len);

  fbl::unique_fd kfd(open(kAuthorizedKeysPath, O_RDONLY));
  ASSERT_TRUE(kfd.is_valid());
  ASSERT_EQ(read(kfd.get(), file_buf.get(), len), ssize_t(len));
  // verify entire file was read.
  uint8_t tmp;
  ASSERT_EQ(read(kfd.get(), &tmp, sizeof(uint8_t)), 0);
  ASSERT_EQ(close(kfd.release()), 0);

  ASSERT_EQ(memcmp(file_buf.get(), payload, len), 0);
}

// If key file does not exist and the bootloader file is not found, nothing should happen.
TEST_F(SshdHostBootItemTest, TestNoKeyFileNoBootloaderFile) {
  remove_authorized_keys();

  zx_status_t status = provision_authorized_keys_from_bootloader_file(
      service_directory_provider_.service_directory());
  ASSERT_EQ(status, ZX_ERR_NOT_FOUND);

  ASSERT_EQ(opendir(kSshDirectory), nullptr);
  ASSERT_EQ(errno, ENOENT);
}

// If key file exists and no bootloader file is found, file should be untouched.
TEST_F(SshdHostBootItemTest, TestKeyFileExistsNoBootloaderFile) {
  constexpr char kAuthorizedKeysPayload[] = "authorized_keys_file_data";
  write_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));

  zx_status_t status = provision_authorized_keys_from_bootloader_file(
      service_directory_provider_.service_directory());
  ASSERT_EQ(status, ZX_ERR_NOT_FOUND);

  verify_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));
}

// If key file does not exist and a bootloader file is found, file should be written.
TEST_F(SshdHostBootItemTest, TestBootloaderFileProvisioningNoKeyFile) {
  constexpr char kAuthorizedKeysPayload[] = "authorized_keys_file_data_new";

  fake_items_.SetFile(kAuthorizedKeysBootloaderFileName.data(), kAuthorizedKeysPayload,
                      strlen(kAuthorizedKeysPayload));

  remove_authorized_keys();

  zx_status_t status = provision_authorized_keys_from_bootloader_file(
      service_directory_provider_.service_directory());
  ASSERT_EQ(status, ZX_OK);

  verify_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));
}

// If key file does not exist, the ssh directory does exist and a bootloader file is found,
// key file should be written.
TEST_F(SshdHostBootItemTest, TestBootloaderFileProvisioningSshDirNoKeyFile) {
  constexpr char kAuthorizedKeysPayload[] = "authorized_keys_file_data_new";

  fake_items_.SetFile(kAuthorizedKeysBootloaderFileName.data(), kAuthorizedKeysPayload,
                      strlen(kAuthorizedKeysPayload));

  remove_authorized_keys();
  ASSERT_EQ(mkdir(kSshDirectory, 0700), 0);

  zx_status_t status = provision_authorized_keys_from_bootloader_file(
      service_directory_provider_.service_directory());
  ASSERT_EQ(status, ZX_OK);

  verify_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));
}

// If key file already exists and a bootloader file is found, key file should not be changed.
TEST_F(SshdHostBootItemTest, TestBootloaderFileNotProvisionedWithExistingKeyFile) {
  constexpr char kAuthorizedKeysPayload[] = "existing authorized_keys_file_data";
  write_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));

  constexpr char kAuthorizedKeysBootItemPayload[] = "new authorized_keys_file_data";
  fake_items_.SetFile(kAuthorizedKeysBootloaderFileName.data(), kAuthorizedKeysBootItemPayload,
                      strlen(kAuthorizedKeysBootItemPayload));

  zx_status_t status = provision_authorized_keys_from_bootloader_file(
      service_directory_provider_.service_directory());
  ASSERT_EQ(status, ZX_ERR_ALREADY_EXISTS);

  verify_authorized_keys(kAuthorizedKeysPayload, strlen(kAuthorizedKeysPayload));
}

}  // namespace

}  // namespace sshd_host
