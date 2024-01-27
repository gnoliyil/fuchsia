// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu.h"

#include <fidl/fuchsia.sysmem/cpp/wire_test_base.h>
#include <fuchsia/hardware/display/controller/c/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fake-bti/bti.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/virtio/backends/fake.h>

#include <zxtest/zxtest.h>

namespace sysmem = fuchsia_sysmem;

namespace {
// Use a stub buffer collection instead of the real sysmem since some tests may
// require things that aren't available on the current system.
class StubBufferCollection : public fidl::testing::WireTestBase<fuchsia_sysmem::BufferCollection> {
 public:
  void SetConstraints(SetConstraintsRequestView request,
                      SetConstraintsCompleter::Sync& _completer) override {
    auto& image_constraints = request->constraints.image_format_constraints[0];
    EXPECT_EQ(sysmem::wire::PixelFormatType::kBgra32, image_constraints.pixel_format.type);
    EXPECT_EQ(4u, image_constraints.bytes_per_row_divisor);
  }

  void WaitForBuffersAllocated(WaitForBuffersAllocatedCompleter::Sync& _completer) override {
    sysmem::wire::BufferCollectionInfo2 info;
    info.settings.has_image_format_constraints = true;
    info.buffer_count = 1;
    ASSERT_OK(zx::vmo::create(4096, 0, &info.buffers[0].vmo));
    sysmem::wire::ImageFormatConstraints& constraints = info.settings.image_format_constraints;
    constraints.pixel_format.type = sysmem::wire::PixelFormatType::kBgra32;
    constraints.pixel_format.has_format_modifier = true;
    constraints.pixel_format.format_modifier.value = sysmem::wire::kFormatModifierLinear;
    constraints.max_coded_width = 1000;
    constraints.max_bytes_per_row = 4000;
    constraints.bytes_per_row_divisor = 1;
    _completer.Reply(ZX_OK, std::move(info));
  }

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    EXPECT_TRUE(false);
  }
};

class FakeGpuBackend : public virtio::FakeBackend {
 public:
  FakeGpuBackend() : FakeBackend({{0, 1024}}) {}
};

class VirtioGpuTest : public zxtest::Test {
 public:
  VirtioGpuTest() : loop_(&kAsyncLoopConfigAttachToCurrentThread) {}
  void SetUp() override {
    zx::bti bti;
    fake_bti_create(bti.reset_and_get_address());
    device_ = std::make_unique<virtio::GpuDevice>(nullptr, std::move(bti),
                                                  std::make_unique<FakeGpuBackend>());

    zx::result server_channel = fidl::CreateEndpoints(&client_channel_);
    ASSERT_OK(server_channel);

    ASSERT_OK(fidl::BindSingleInFlightOnly(loop_.dispatcher(), std::move(server_channel.value()),
                                           &collection_));

    loop_.StartThread();
  }
  void TearDown() override {
    // Ensure the loop processes all queued FIDL messages.
    loop_.Quit();
    loop_.JoinThreads();
    loop_.ResetQuit();
    loop_.RunUntilIdle();
  }

 protected:
  std::unique_ptr<virtio::GpuDevice> device_;
  StubBufferCollection collection_;
  async::Loop loop_;
  fidl::ClientEnd<fuchsia_sysmem::BufferCollection> client_channel_;
};

TEST_F(VirtioGpuTest, ImportVmo) {
  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_RGB_x888;
  image.width = 4;
  image.height = 4;

  zx::vmo vmo;
  size_t offset;
  uint32_t pixel_size;
  uint32_t row_bytes;
  EXPECT_OK(
      device_->GetVmoAndStride(&image, client_channel_, 0, &vmo, &offset, &pixel_size, &row_bytes));
  EXPECT_EQ(4, pixel_size);
  EXPECT_EQ(16, row_bytes);
}

TEST_F(VirtioGpuTest, SetConstraints) {
  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_RGB_x888;
  image.width = 4;
  image.height = 4;
  display_controller_impl_protocol_t proto;
  EXPECT_OK(device_->DdkGetProtocol(ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL,
                                    reinterpret_cast<void*>(&proto)));
  EXPECT_OK(proto.ops->set_buffer_collection_constraints(device_.get(), &image,
                                                         client_channel_.channel().get()));
}

}  // namespace
