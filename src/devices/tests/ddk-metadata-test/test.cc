// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/devmgr-integration-test/fixture.h>

#include <ddktl/device.h>
#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

using devmgr_integration_test::IsolatedDevmgr;

TEST(MetadataTest, RunTests) {
  const char kDriver[] = "/boot/meta/ddk-metadata-test-driver.cm";
  auto args = IsolatedDevmgr::DefaultArgs();

  args.root_device_driver = "/boot/meta/test-parent-sys.cm";

  // NB: this loop is never run. RealmBuilder::Build is in the call stack, and insists on a non-null
  // dispatcher.
  //
  // TODO(https://fxbug.dev/114254): Remove this.
  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  zx::result devmgr = IsolatedDevmgr::Create(std::move(args), loop.dispatcher());
  ASSERT_OK(devmgr.status_value());

  fidl::ClientEnd<fuchsia_device::Controller> client_end;
  {
    zx::result channel =
        device_watcher::RecursiveWaitForFile(devmgr.value().devfs_root().get(), "sys/test/test");
    ASSERT_OK(channel);
    client_end.channel() = std::move(channel.value());
  }
  fidl::WireSyncClient controller(std::move(client_end));

  const fidl::WireResult result = controller->Bind(fidl::StringView{kDriver});
  ASSERT_OK(result.status());
  const fit::result response = result.value();
  // The driver will run its tests in its bind routine, and return ZX_OK on success.
  ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));
}

// Test the Metadata struct helper:
TEST(MetadataTest, GetMetadataStructTest) {
  auto parent = MockDevice::FakeRootParent();  // Hold on to the parent during the test.
  constexpr size_t kMetadataType = 5;
  struct MetadataType {
    int data[4];
    float data1;
  };
  MetadataType metadata_source;
  parent->SetMetadata(kMetadataType, &metadata_source, sizeof(metadata_source));

  auto metadata_result = ddk::GetMetadata<MetadataType>(parent.get(), kMetadataType);

  ASSERT_TRUE(metadata_result.is_ok());
  ASSERT_BYTES_EQ(metadata_result.value().get(), &metadata_source, sizeof(MetadataType));
}

// Test the Metadata Array helper:
TEST(MetadataTest, MetadataArrayTests) {
  auto parent = MockDevice::FakeRootParent();  // Hold on to the parent during the test.
  constexpr size_t kMetadataType = 5;
  struct MetadataType {
    int data[4];
    float data1;
  };

  constexpr size_t kMetadataArrayLen = 5;
  MetadataType metadata_source[kMetadataArrayLen];
  parent->SetMetadata(kMetadataType, &metadata_source, sizeof(metadata_source));

  auto metadata_result = ddk::GetMetadataArray<MetadataType>(parent.get(), kMetadataType);

  ASSERT_TRUE(metadata_result.is_ok());
  ASSERT_EQ(metadata_result->size(), kMetadataArrayLen);
  for (size_t i = 0; i < metadata_result->size(); ++i) {
    ASSERT_BYTES_EQ(&metadata_result.value()[i], &metadata_source[i], sizeof(MetadataType));
  }
}

// Simple device to test DeviceAddArgs
class TestDevice : public ddk::Device<TestDevice> {
 public:
  explicit TestDevice(zx_device_t* parent) : ddk::Device<TestDevice>(parent) {}
  void DdkRelease() {
    // DdkRelease must delete this before it returns.
    delete this;
  }
};

TEST(MetadataTest, DeviceAddArgTests) {
  // We use two parents here because if we added the metadata to the normal parent,
  // it would be accessible through the child by recursion.
  auto parent = MockDevice::FakeRootParent();           // Hold on to the parent during the test.
  auto metadata_parent = MockDevice::FakeRootParent();  // This will actually have the metadata
  constexpr size_t kMetadataType = 5;
  struct MetadataType {
    int data[4];
    float data1;
  };

  constexpr size_t kMetadataArrayLen = 5;
  MetadataType metadata_source[kMetadataArrayLen];
  metadata_parent->SetMetadata(kMetadataType, &metadata_source, sizeof(metadata_source));

  // Bind a new device:
  auto dev = std::make_unique<TestDevice>(parent.get());
  // Now, add the child, but allow it to get the metadata from the metadata_parent:
  auto args = ddk::DeviceAddArgs("dut").forward_metadata(metadata_parent.get(), kMetadataType);
  ASSERT_EQ(args.get().metadata_count, 1);
  ASSERT_EQ(args.get().metadata_list[0].type, kMetadataType);

  ASSERT_OK(dev->DdkAdd(args));
  auto dev_ptr = dev.release();

  // Now the metadata should be available in the device:
  auto metadata_result = ddk::GetMetadataArray<MetadataType>(dev_ptr->zxdev(), kMetadataType);

  ASSERT_TRUE(metadata_result.is_ok());
  ASSERT_EQ(metadata_result->size(), kMetadataArrayLen);
  for (size_t i = 0; i < metadata_result->size(); ++i) {
    ASSERT_BYTES_EQ(&metadata_result.value()[i], &metadata_source[i], sizeof(MetadataType));
  }
}

}  // namespace
