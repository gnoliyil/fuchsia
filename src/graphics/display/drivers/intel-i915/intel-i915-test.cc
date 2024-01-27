// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915/intel-i915.h"

#include <fidl/fuchsia.hardware.sysmem/cpp/wire.h>
#include <fidl/fuchsia.hardware.sysmem/cpp/wire_test_base.h>
#include <fidl/fuchsia.sysmem/cpp/wire.h>
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
#include "src/graphics/display/drivers/intel-i915/pci-ids.h"
#include "src/lib/fsl/handles/object_info.h"

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

namespace i915 {

namespace {

// TODO(fxbug.dev/121924): Consider creating and using a unified set of sysmem
// testing doubles instead of writing mocks for each display driver test.
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

  void CheckBuffersAllocated(CheckBuffersAllocatedCompleter::Sync& completer) override {
    completer.Reply(ZX_OK);
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

class MockAllocator : public fidl::testing::WireTestBase<fuchsia_sysmem::Allocator> {
 public:
  explicit MockAllocator(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {
    EXPECT_TRUE(dispatcher_);
  }

  void BindSharedCollection(BindSharedCollectionRequestView request,
                            BindSharedCollectionCompleter::Sync& completer) override {
    const std::vector<sysmem::wire::PixelFormatType> kPixelFormatTypes = {
        sysmem::wire::PixelFormatType::kBgra32, sysmem::wire::PixelFormatType::kR8G8B8A8};

    auto buffer_collection_id = next_buffer_collection_id_++;
    active_buffer_collections_[buffer_collection_id] = {
        .token_client = std::move(request->token),
        .mock_buffer_collection = std::make_unique<MockNoCpuBufferCollection>(),
    };

    fidl::BindServer(
        dispatcher_, std::move(request->buffer_collection_request),
        active_buffer_collections_[buffer_collection_id].mock_buffer_collection.get(),
        [this, buffer_collection_id](MockNoCpuBufferCollection*, fidl::UnbindInfo,
                                     fidl::ServerEnd<fuchsia_sysmem::BufferCollection>) {
          inactive_buffer_collection_tokens_.push_back(
              std::move(active_buffer_collections_[buffer_collection_id].token_client));
          active_buffer_collections_.erase(buffer_collection_id);
        });
  }

  void SetDebugClientInfo(SetDebugClientInfoRequestView request,
                          SetDebugClientInfoCompleter::Sync& completer) override {
    EXPECT_EQ(request->name.get().find("intel-i915"), 0u);
  }

  std::vector<fidl::UnownedClientEnd<fuchsia_sysmem::BufferCollectionToken>>
  GetActiveBufferCollectionTokenClients() const {
    std::vector<fidl::UnownedClientEnd<fuchsia_sysmem::BufferCollectionToken>>
        unowned_token_clients;
    unowned_token_clients.reserve(active_buffer_collections_.size());

    for (const auto& kv : active_buffer_collections_) {
      unowned_token_clients.push_back(kv.second.token_client);
    }
    return unowned_token_clients;
  }

  std::vector<fidl::UnownedClientEnd<fuchsia_sysmem::BufferCollectionToken>>
  GetInactiveBufferCollectionTokenClients() const {
    std::vector<fidl::UnownedClientEnd<fuchsia_sysmem::BufferCollectionToken>>
        unowned_token_clients;
    unowned_token_clients.reserve(inactive_buffer_collection_tokens_.size());

    for (const auto& token : inactive_buffer_collection_tokens_) {
      unowned_token_clients.push_back(token);
    }
    return unowned_token_clients;
  }

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    EXPECT_TRUE(false);
  }

 private:
  struct BufferCollection {
    fidl::ClientEnd<fuchsia_sysmem::BufferCollectionToken> token_client;
    std::unique_ptr<MockNoCpuBufferCollection> mock_buffer_collection;
  };

  using BufferCollectionId = int;

  std::unordered_map<BufferCollectionId, BufferCollection> active_buffer_collections_;
  std::vector<fidl::ClientEnd<fuchsia_sysmem::BufferCollectionToken>>
      inactive_buffer_collection_tokens_;

  BufferCollectionId next_buffer_collection_id_ = 0;

  async_dispatcher_t* dispatcher_ = nullptr;
};

class FakeSysmem : public fidl::testing::WireTestBase<fuchsia_hardware_sysmem::Sysmem> {
 public:
  explicit FakeSysmem(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {
    EXPECT_TRUE(dispatcher_);
  }

  void ConnectServer(ConnectServerRequestView request,
                     ConnectServerCompleter::Sync& completer) override {
    mock_allocators_.emplace_front(dispatcher_);
    auto it = mock_allocators_.begin();
    fidl::BindServer(dispatcher_, std::move(request->allocator_request), &*it);
  }

  std::list<MockAllocator>& mock_allocators() { return mock_allocators_; }

  // FIDL methods
  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) final {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

 private:
  std::list<MockAllocator> mock_allocators_;
  async_dispatcher_t* dispatcher_ = nullptr;
};

class IntegrationTest : public ::testing::Test {
 protected:
  IntegrationTest() : loop_(&kAsyncLoopConfigNeverAttachToThread), sysmem_(loop_.dispatcher()) {}

  void SetUp() final {
    SetFramebuffer({});

    pci_.CreateBar(0u, std::numeric_limits<uint32_t>::max(), /*is_mmio=*/true);
    pci_.AddLegacyInterrupt();

    // This configures the "GMCH Graphics Control" register to report 2MB for the available GTT
    // Graphics Memory. All other bits of this register are set to zero and should get populated as
    // required for the tests below.
    pci_.PciWriteConfig16(registers::GmchGfxControl::kAddr, 0x40);

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

  void TearDown() override {
    loop_.Shutdown();
    parent_ = nullptr;
  }

  MockDevice* parent() const { return parent_.get(); }

 private:
  async::Loop loop_;
  // Emulated parent protocols.
  pci::FakePciProtocol pci_;
  FakeSysmem sysmem_;

  // mock-ddk parent device of the Controller under test.
  std::shared_ptr<MockDevice> parent_;
};

TEST(IntelI915Display, ImportBufferCollection) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  FakeSysmem fake_sysmem(loop.dispatcher());

  auto sysmem_endpoints = fidl::CreateEndpoints<fuchsia_hardware_sysmem::Sysmem>();
  ASSERT_TRUE(sysmem_endpoints.is_ok());
  auto& [sysmem_client, sysmem_server] = sysmem_endpoints.value();
  fidl::BindServer(loop.dispatcher(), std::move(sysmem_server), &fake_sysmem);

  // Initialize display controller and sysmem allocator.
  Controller display(nullptr);
  ASSERT_OK(display.SetAndInitSysmemForTesting(fidl::WireSyncClient(std::move(sysmem_client))));
  EXPECT_OK(loop.RunUntilIdle());

  EXPECT_EQ(fake_sysmem.mock_allocators().size(), 1u);
  const MockAllocator& allocator = fake_sysmem.mock_allocators().front();

  zx::result token1_endpoints = fidl::CreateEndpoints<sysmem::BufferCollectionToken>();
  ASSERT_TRUE(token1_endpoints.is_ok());
  zx::result token2_endpoints = fidl::CreateEndpoints<sysmem::BufferCollectionToken>();
  ASSERT_TRUE(token2_endpoints.is_ok());

  // Test ImportBufferCollection().
  constexpr uint64_t kValidBufferCollectionId = 1u;
  EXPECT_OK(display.DisplayControllerImplImportBufferCollection(
      kValidBufferCollectionId, token1_endpoints->client.TakeChannel()));

  // `collection_id` must be unused.
  EXPECT_EQ(display.DisplayControllerImplImportBufferCollection(
                kValidBufferCollectionId, token2_endpoints->client.TakeChannel()),
            ZX_ERR_ALREADY_EXISTS);

  loop.RunUntilIdle();

  // Verify that the current buffer collection token is used.
  {
    auto active_buffer_token_clients = allocator.GetActiveBufferCollectionTokenClients();
    EXPECT_EQ(active_buffer_token_clients.size(), 1u);

    auto inactive_buffer_token_clients = allocator.GetInactiveBufferCollectionTokenClients();
    EXPECT_EQ(inactive_buffer_token_clients.size(), 0u);

    auto [client_koid, client_related_koid] =
        fsl::GetKoids(active_buffer_token_clients[0].channel()->get());
    auto [server_koid, server_related_koid] =
        fsl::GetKoids(token1_endpoints->server.channel().get());

    EXPECT_NE(client_koid, ZX_KOID_INVALID);
    EXPECT_NE(client_related_koid, ZX_KOID_INVALID);
    EXPECT_NE(server_koid, ZX_KOID_INVALID);
    EXPECT_NE(server_related_koid, ZX_KOID_INVALID);

    EXPECT_EQ(client_koid, server_related_koid);
    EXPECT_EQ(server_koid, client_related_koid);
  }

  // Test ReleaseBufferCollection().
  constexpr uint64_t kInvalidBufferCollectionId = 2u;
  EXPECT_EQ(display.DisplayControllerImplReleaseBufferCollection(kInvalidBufferCollectionId),
            ZX_ERR_NOT_FOUND);
  EXPECT_OK(display.DisplayControllerImplReleaseBufferCollection(kValidBufferCollectionId));

  loop.RunUntilIdle();

  // Verify that the current buffer collection token is released.
  {
    auto active_buffer_token_clients = allocator.GetActiveBufferCollectionTokenClients();
    EXPECT_EQ(active_buffer_token_clients.size(), 0u);

    auto inactive_buffer_token_clients = allocator.GetInactiveBufferCollectionTokenClients();
    EXPECT_EQ(inactive_buffer_token_clients.size(), 1u);

    auto [client_koid, client_related_koid] =
        fsl::GetKoids(inactive_buffer_token_clients[0].channel()->get());
    auto [server_koid, server_related_koid] =
        fsl::GetKoids(token1_endpoints->server.channel().get());

    EXPECT_NE(client_koid, ZX_KOID_INVALID);
    EXPECT_NE(client_related_koid, ZX_KOID_INVALID);
    EXPECT_NE(server_koid, ZX_KOID_INVALID);
    EXPECT_NE(server_related_koid, ZX_KOID_INVALID);

    EXPECT_EQ(client_koid, server_related_koid);
    EXPECT_EQ(server_koid, client_related_koid);
  }

  // Shutdown the loop before destroying the FakeSysmem and MockAllocator which
  // may still have pending callbacks.
  loop.Shutdown();
}

TEST(IntelI915Display, SysmemRequirements) {
  Controller display(nullptr);
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(display.DisplayControllerImplSetBufferCollectionConstraints(
      &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
}

TEST(IntelI915Display, SysmemNoneFormat) {
  Controller display(nullptr);
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_NONE;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(display.DisplayControllerImplSetBufferCollectionConstraints(
      &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
}

TEST(IntelI915Display, SysmemInvalidFormat) {
  Controller display(nullptr);
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = UINT32_MAX;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_EQ(ZX_ERR_INVALID_ARGS, display.DisplayControllerImplSetBufferCollectionConstraints(
                                     &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_FALSE(collection.set_constraints_called());
}

TEST(IntelI915Display, SysmemInvalidType) {
  Controller display(nullptr);
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  image.type = 1000000;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_EQ(ZX_ERR_INVALID_ARGS, display.DisplayControllerImplSetBufferCollectionConstraints(
                                     &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_FALSE(collection.set_constraints_called());
}

// Tests that DDK basic DDK lifecycle hooks function as expected.
TEST_F(IntegrationTest, BindAndInit) {
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
TEST_F(IntegrationTest, InitFailsIfBootloaderGetInfoFails) {
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

TEST_F(IntegrationTest, GttAllocationDoesNotOverlapBootloaderFramebuffer) {
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

TEST_F(IntegrationTest, SysmemImport) {
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at `parent()`, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

  MockNoCpuBufferCollection collection;
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  image_t image = {};
  image.pixel_format = ZX_PIXEL_FORMAT_ARGB_8888;
  image.width = 128;
  image.height = kImageHeight;
  ASSERT_OK(
      fidl::BindSingleInFlightOnly(loop.dispatcher(), std::move(server_channel), &collection));

  EXPECT_OK(ctx->DisplayControllerImplSetBufferCollectionConstraints(
      &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
  loop.StartThread();
  EXPECT_OK(ctx->DisplayControllerImplImportImage(&image, client_channel.channel().get(), 0));

  const GttRegion& region = ctx->SetupGttImage(&image, FRAME_TRANSFORM_IDENTITY);
  EXPECT_LT(image.width * 4, kBytesPerRowDivisor);
  EXPECT_EQ(kBytesPerRowDivisor, region.bytes_per_row());
  ctx->DisplayControllerImplReleaseImage(&image);
}

TEST_F(IntegrationTest, SysmemRotated) {
  ASSERT_OK(Controller::Create(parent()));

  // There should be two published devices: one "intel_i915" device rooted at `parent()`, and a
  // grandchild "intel-gpu-core" device.
  ASSERT_EQ(1u, parent()->child_count());
  auto dev = parent()->GetLatestChild();
  Controller* ctx = dev->GetDeviceContext<Controller>();

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  ASSERT_OK(endpoints.status_value());
  auto& [client_channel, server_channel] = endpoints.value();

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

  EXPECT_OK(ctx->DisplayControllerImplSetBufferCollectionConstraints(
      &image, client_channel.channel().get()));

  loop.RunUntilIdle();
  EXPECT_TRUE(collection.set_constraints_called());
  loop.StartThread();
  image.type = IMAGE_TYPE_Y_LEGACY_TILED;
  EXPECT_OK(ctx->DisplayControllerImplImportImage(&image, client_channel.channel().get(), 0));

  // Check that rotating the image doesn't hang.
  const GttRegion& region = ctx->SetupGttImage(&image, FRAME_TRANSFORM_ROT_90);
  EXPECT_LT(image.width * 4, kBytesPerRowDivisor);
  EXPECT_EQ(kBytesPerRowDivisor, region.bytes_per_row());
  ctx->DisplayControllerImplReleaseImage(&image);
}

}  // namespace

}  // namespace i915
