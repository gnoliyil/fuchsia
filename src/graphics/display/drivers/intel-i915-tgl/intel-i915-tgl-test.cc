// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915-tgl/intel-i915-tgl.h"

#include <fidl/fuchsia.hardware.sysmem/cpp/wire.h>
#include <fidl/fuchsia.hardware.sysmem/cpp/wire_test_base.h>
#include <fidl/fuchsia.sysmem/cpp/wire_test_base.h>
#include <fuchsia/hardware/sysmem/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/loop.h>
#include <lib/ddk/driver.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/mmio-ptr/fake.h>
#include <lib/zircon-internal/align.h>
#include <lib/zx/vmar.h>
#include <zircon/pixelformat.h>

#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "src/devices/pci/testing/pci_protocol_fake.h"
#include "src/devices/testing/mock-ddk/mock-device.h"
#include "src/graphics/display/drivers/intel-i915-tgl/pci-ids.h"

#define ASSERT_OK(x) ASSERT_EQ(ZX_OK, (x))
#define EXPECT_OK(x) EXPECT_EQ(ZX_OK, (x))

namespace sysmem = fuchsia_sysmem;

namespace {
constexpr uint32_t kBytesPerRowDivisor = 1024;
constexpr uint32_t kImageHeight = 32;

// Module-scope global data structure that acts as the data source for the zx_framebuffer_get_info
// implementation below.
struct Framebuffer {
  zx_status_t status = ZX_OK;
  uint32_t format = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t stride = 0u;
};
thread_local Framebuffer g_framebuffer;

void SetFramebuffer(const Framebuffer& buffer) { g_framebuffer = buffer; }

}  // namespace

zx_status_t zx_framebuffer_get_info(zx_handle_t resource, uint32_t* format, uint32_t* width,
                                    uint32_t* height, uint32_t* stride) {
  *format = g_framebuffer.format;
  *width = g_framebuffer.width;
  *height = g_framebuffer.height;
  *stride = g_framebuffer.stride;
  return g_framebuffer.status;
}

namespace i915_tgl {

namespace {

class MockNoCpuBufferCollection
    : public fidl::testing::WireTestBase<fuchsia_sysmem::BufferCollection> {
 public:
  void set_format_modifier(uint64_t format_modifier) { format_modifier_ = format_modifier; }

  bool set_constraints_called() const { return set_constraints_called_; }
  void SetConstraints(SetConstraintsRequestView request,
                      SetConstraintsCompleter::Sync& _completer) override {
    set_constraints_called_ = true;
    EXPECT_FALSE(request->constraints.buffer_memory_constraints.inaccessible_domain_supported);
    EXPECT_FALSE(request->constraints.buffer_memory_constraints.cpu_domain_supported);
    constraints_ = request->constraints;
  }

  void WaitForBuffersAllocated(WaitForBuffersAllocatedCompleter::Sync& completer) override {
    fuchsia_sysmem::wire::BufferCollectionInfo2 info;
    info.settings.has_image_format_constraints = true;
    auto& constraints = info.settings.image_format_constraints;
    for (size_t i = 0; i < constraints_.image_format_constraints_count; i++) {
      if (constraints_.image_format_constraints[i].pixel_format.format_modifier.value ==
          format_modifier_) {
        constraints = constraints_.image_format_constraints[i];
        break;
      }
    }
    constraints.bytes_per_row_divisor = kBytesPerRowDivisor;
    info.buffer_count = 1;
    EXPECT_OK(zx::vmo::create(kBytesPerRowDivisor * kImageHeight, 0, &info.buffers[0].vmo));
    completer.Reply(ZX_OK, std::move(info));
  }

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    EXPECT_TRUE(false);
  }

 private:
  bool set_constraints_called_ = false;
  uint64_t format_modifier_ = fuchsia_sysmem::wire::kFormatModifierLinear;
  fuchsia_sysmem::wire::BufferCollectionConstraints constraints_;
};

class FakeSysmem : public fidl::testing::WireTestBase<fuchsia_hardware_sysmem::Sysmem> {
 public:
  FakeSysmem() = default;

  // FIDL methods
  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) final {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }
};

class TglIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() final {
    SetFramebuffer({});

    pci_.CreateBar(0u, std::numeric_limits<uint32_t>::max(), /*is_mmio=*/true);
    pci_.AddLegacyInterrupt();

    // This configures the "GMCH Graphics Control" register to report 2MB for the available GTT
    // Graphics Memory. All other bits of this register are set to zero and should get populated as
    // required for the tests below.
    pci_.PciWriteConfig16(tgl_registers::GmchGfxControl::kAddr, 0x40);

    constexpr uint16_t kIntelVendorId = 0x8086;
    pci_.SetDeviceInfo({
        .vendor_id = kIntelVendorId,
        .device_id = kTestDeviceDid,
    });

    parent_ = MockDevice::FakeRootParent();
    parent_->AddFidlProtocol(
        fidl::DiscoverableProtocolName<fuchsia_hardware_sysmem::Sysmem>,
        [this](zx::channel channel) {
          fidl::BindServer(loop_.dispatcher(),
                           fidl::ServerEnd<fuchsia_hardware_sysmem::Sysmem>(std::move(channel)),
                           &sysmem_);
          return ZX_OK;
        },
        "sysmem-fidl");
    parent_->AddFidlProtocol(
        fidl::DiscoverableProtocolName<fuchsia_hardware_pci::Device>,
        [this](zx::channel channel) {
          fidl::BindServer(loop_.dispatcher(),
                           fidl::ServerEnd<fuchsia_hardware_pci::Device>(std::move(channel)),
                           &pci_);
          return ZX_OK;
        },
        "pci");
    loop_.StartThread("pci-fidl-server-thread");
  }

  void TearDown() override { parent_ = nullptr; }

  MockDevice* parent() const { return parent_.get(); }

 private:
  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
  // Emulated parent protocols.
  pci::FakePciProtocol pci_;
  FakeSysmem sysmem_;

  // mock-ddk parent device of the Controller under test.
  std::shared_ptr<MockDevice> parent_;
};

TEST(IntelI915TglDisplay, SysmemRequirements) {
  Controller display(nullptr);
  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(
      display.DisplayControllerImplSetBufferCollectionConstraints(&image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
}

TEST(IntelI915TglDisplay, SysmemNoneFormat) {
  Controller display(nullptr);
  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_NONE;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(
      display.DisplayControllerImplSetBufferCollectionConstraints(&image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
}

TEST(IntelI915TglDisplay, SysmemInvalidFormat) {
  Controller display(nullptr);
  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = UINT32_MAX;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_EQ(ZX_ERR_INVALID_ARGS, display.DisplayControllerImplSetBufferCollectionConstraints(
                                     &image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_FALSE(collection.set_constraints_called());
}

TEST(IntelI915TglDisplay, SysmemInvalidType) {
  Controller display(nullptr);
  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  image.type = 1000000;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_EQ(ZX_ERR_INVALID_ARGS, display.DisplayControllerImplSetBufferCollectionConstraints(
                                     &image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_FALSE(collection.set_constraints_called());
}

// Tests that DDK basic DDK lifecycle hooks function as expected.
TEST_F(TglIntegrationTest, BindAndInit) {
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at |parent()|, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  ASSERT_EQ(2u, dev->child_count());

  // Perform the async initialization and wait for a response.
  dev->InitOp();
  EXPECT_EQ(ZX_OK, dev->WaitUntilInitReplyCalled());

  // Unbind the device and ensure it completes synchronously.
  dev->UnbindOp();
  EXPECT_TRUE(dev->UnbindReplyCalled());

  mock_ddk::ReleaseFlaggedDevices(parent());
  EXPECT_EQ(0u, dev->child_count());
}

// Tests that the device can initialize even if bootloader framebuffer information is not available
// and global GTT allocations start at offset 0.
TEST_F(TglIntegrationTest, InitFailsIfBootloaderGetInfoFails) {
  SetFramebuffer({.status = ZX_ERR_INVALID_ARGS});

  ASSERT_EQ(ZX_OK, Controller::Create(parent()));
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  uint64_t addr;
  EXPECT_EQ(ZX_OK, ctx->IntelGpuCoreGttAlloc(1, &addr));
  EXPECT_EQ(0u, addr);
}

// TODO(fxbug.dev/85836): Add tests for DisplayPort display enumeration by InitOp, covering the
// following cases:
//   - Display found during start up but not already powered.
//   - Display found during start up but already powered up.
//   - Display added and removed in a hotplug event.
// TODO(fxbug.dev/86314): Add test for HDMI display enumeration by InitOp.
// TODO(fxbug.dev/86315): Add test for DVI display enumeration by InitOp.

TEST_F(TglIntegrationTest, GttAllocationDoesNotOverlapBootloaderFramebuffer) {
  constexpr uint32_t kStride = 1920;
  constexpr uint32_t kHeight = 1080;
  SetFramebuffer({
      .format = ZX_PIXEL_FORMAT_RGB_888,
      .width = kStride,
      .height = kHeight,
      .stride = kStride,
  });
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at |parent()|, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  uint64_t addr;
  EXPECT_EQ(ZX_OK, ctx->IntelGpuCoreGttAlloc(1, &addr));
  EXPECT_EQ(ZX_ROUNDUP(kHeight * kStride * 3, PAGE_SIZE), addr);
}

TEST_F(TglIntegrationTest, SysmemImport) {
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at `parent()`, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  image.width = 128;
  image.height = kImageHeight;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(ctx->DisplayControllerImplSetBufferCollectionConstraints(&image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
  loop.StartThread();
  EXPECT_OK(ctx->DisplayControllerImplImportImage(&image, client_channel.get(), 0));

  const GttRegion& region = ctx->SetupGttImage(&image, FRAME_TRANSFORM_IDENTITY);
  EXPECT_LT(image.width * 4, kBytesPerRowDivisor);
  EXPECT_EQ(kBytesPerRowDivisor, region.bytes_per_row());
  ctx->DisplayControllerImplReleaseImage(&image);
}

TEST_F(TglIntegrationTest, SysmemRotated) {
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at `parent()`, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  zx::channel server_channel, client_channel;
  ASSERT_OK(zx::channel::create(0u, &server_channel, &client_channel));

  MockNoCpuBufferCollection collection;
  collection.set_format_modifier(fuchsia_sysmem::wire::kFormatModifierIntelI915YTiled);
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  image.width = 128;
  image.height = kImageHeight;
  // Must match set_format_modifier above, and also be y or yf tiled so rotation is allowed.
  image.type = IMAGE_TYPE_Y_LEGACY_TILED;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(ctx->DisplayControllerImplSetBufferCollectionConstraints(&image, client_channel.get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
  loop.StartThread();
  image.type = IMAGE_TYPE_Y_LEGACY_TILED;
  EXPECT_OK(ctx->DisplayControllerImplImportImage(&image, client_channel.get(), 0));

  // Check that rotating the image doesn't hang.
  const GttRegion& region = ctx->SetupGttImage(&image, FRAME_TRANSFORM_ROT_90);
  EXPECT_LT(image.width * 4, kBytesPerRowDivisor);
  EXPECT_EQ(kBytesPerRowDivisor, region.bytes_per_row());
  ctx->DisplayControllerImplReleaseImage(&image);
}

}  // namespace

}  // namespace i915_tgl
