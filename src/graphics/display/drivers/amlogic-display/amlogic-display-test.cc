// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/amlogic-display/amlogic-display.h"

#include <fidl/fuchsia.sysmem/cpp/wire_test_base.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/inspect/cpp/inspect.h>
#include <zircon/pixelformat.h>
#include <zircon/syscalls/object.h>

#include <zxtest/zxtest.h>

#include "src/graphics/display/drivers/amlogic-display/osd.h"
#include "src/lib/fsl/handles/object_info.h"

namespace amlogic_display {

namespace {

namespace sysmem = fuchsia_sysmem;

// TODO(fxbug.dev/121924): Consider creating and using a unified set of sysmem
// testing doubles instead of writing mocks for each display driver test.
class MockBufferCollectionBase
    : public fidl::testing::WireTestBase<fuchsia_sysmem::BufferCollection> {
 public:
  MockBufferCollectionBase() = default;
  ~MockBufferCollectionBase() override = default;

  virtual void VerifyBufferCollectionConstraints(
      const sysmem::wire::BufferCollectionConstraints& constraints) = 0;
  virtual void VerifyName(const std::string& name) = 0;

  void SetConstraints(SetConstraintsRequestView request,
                      SetConstraintsCompleter::Sync& completer) override {
    if (!request->has_constraints) {
      return;
    }
    VerifyBufferCollectionConstraints(request->constraints);
    set_constraints_called_ = true;
  }

  void SetName(SetNameRequestView request, SetNameCompleter::Sync& completer) override {
    EXPECT_EQ(10u, request->priority);
    VerifyName(std::string(request->name.data(), request->name.size()));
    set_name_called_ = true;
  }

  void CheckBuffersAllocated(CheckBuffersAllocatedCompleter::Sync& completer) override {
    completer.Reply(ZX_OK);
  }

  void WaitForBuffersAllocated(WaitForBuffersAllocatedCompleter::Sync& completer) override {
    sysmem::wire::BufferCollectionInfo2 collection;
    collection.buffer_count = 1;
    collection.settings.has_image_format_constraints = true;
    collection.settings.image_format_constraints = image_format_constraints_;
    EXPECT_EQ(ZX_OK, zx::vmo::create(ZX_PAGE_SIZE, 0u, &collection.buffers[0].vmo));
    completer.Reply(ZX_OK, std::move(collection));
  }

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    EXPECT_TRUE(false);
  }

  void set_allocated_image_format_constraints(
      const sysmem::wire::ImageFormatConstraints& image_format_constraints) {
    image_format_constraints_ = image_format_constraints;
  }
  bool set_constraints_called() const { return set_constraints_called_; }
  bool set_name_called() const { return set_name_called_; }

 private:
  bool set_constraints_called_ = false;
  bool set_name_called_ = false;
  sysmem::wire::ImageFormatConstraints image_format_constraints_ = {};
};

class MockBufferCollection : public MockBufferCollectionBase {
 public:
  explicit MockBufferCollection(const std::vector<sysmem::wire::PixelFormatType>&
                                    pixel_format_types = {sysmem::wire::PixelFormatType::kBgra32,
                                                          sysmem::wire::PixelFormatType::kR8G8B8A8})
      : supported_pixel_format_types_(pixel_format_types) {
    set_allocated_image_format_constraints(kDefaultImageFormatConstraints);
  }
  ~MockBufferCollection() override = default;

  void VerifyBufferCollectionConstraints(
      const sysmem::wire::BufferCollectionConstraints& constraints) override {
    EXPECT_TRUE(constraints.buffer_memory_constraints.inaccessible_domain_supported);
    EXPECT_FALSE(constraints.buffer_memory_constraints.cpu_domain_supported);
    EXPECT_EQ(64u, constraints.image_format_constraints[0].bytes_per_row_divisor);

    bool has_rgba =
        std::find(supported_pixel_format_types_.begin(), supported_pixel_format_types_.end(),
                  fuchsia_sysmem::wire::PixelFormatType::kR8G8B8A8) !=
        supported_pixel_format_types_.end();
    bool has_bgra =
        std::find(supported_pixel_format_types_.begin(), supported_pixel_format_types_.end(),
                  fuchsia_sysmem::wire::PixelFormatType::kBgra32) !=
        supported_pixel_format_types_.end();

    size_t expected_format_constraints_count = 0u;
    const auto& image_format_constraints = constraints.image_format_constraints;
    if (has_bgra) {
      expected_format_constraints_count += 2;
      EXPECT_TRUE(std::find_if(
                      image_format_constraints.begin(),
                      image_format_constraints.begin() + constraints.image_format_constraints_count,
                      [](const auto& format) {
                        return format.pixel_format.format_modifier.value ==
                               sysmem::wire::kFormatModifierArmLinearTe;
                      }) != image_format_constraints.end());
    }
    if (has_rgba) {
      expected_format_constraints_count += 2;
    }

    EXPECT_EQ(expected_format_constraints_count, constraints.image_format_constraints_count);
  }

  void VerifyName(const std::string& name) override { EXPECT_STREQ(name, "Display"); }

 private:
  std::vector<sysmem::wire::PixelFormatType> supported_pixel_format_types_;

  static constexpr sysmem::wire::ImageFormatConstraints kDefaultImageFormatConstraints = {
      .pixel_format =
          fuchsia_sysmem::wire::PixelFormat{
              .type = sysmem::wire::PixelFormatType::kBgra32,
              .has_format_modifier = true,
              .format_modifier = {sysmem::wire::kFormatModifierLinear},
          },
      .min_coded_height = 4,
      .min_bytes_per_row = 4096,
  };
};

class MockBufferCollectionForCapture : public MockBufferCollectionBase {
 public:
  MockBufferCollectionForCapture() {
    set_allocated_image_format_constraints(kDefaultImageFormatConstraints);
  }
  ~MockBufferCollectionForCapture() override = default;

  void VerifyBufferCollectionConstraints(
      const sysmem::wire::BufferCollectionConstraints& constraints) override {
    EXPECT_TRUE(constraints.buffer_memory_constraints.inaccessible_domain_supported);
    EXPECT_FALSE(constraints.buffer_memory_constraints.cpu_domain_supported);
    EXPECT_EQ(64u, constraints.image_format_constraints[0].bytes_per_row_divisor);

    size_t expected_format_constraints_count = 1u;
    EXPECT_EQ(expected_format_constraints_count, constraints.image_format_constraints_count);

    const auto& image_format_constraints = constraints.image_format_constraints;
    EXPECT_EQ(image_format_constraints[0].pixel_format.type,
              fuchsia_sysmem::wire::PixelFormatType::kBgr24);
    EXPECT_TRUE(image_format_constraints[0].pixel_format.has_format_modifier);
    EXPECT_EQ(image_format_constraints[0].pixel_format.format_modifier.value,
              fuchsia_sysmem::wire::kFormatModifierLinear);
  }

  void VerifyName(const std::string& name) override { EXPECT_STREQ(name, "Display capture"); }

 private:
  static constexpr sysmem::wire::ImageFormatConstraints kDefaultImageFormatConstraints = {
      .pixel_format =
          fuchsia_sysmem::wire::PixelFormat{
              .type = sysmem::wire::PixelFormatType::kBgr24,
              .has_format_modifier = true,
              .format_modifier = {sysmem::wire::kFormatModifierLinear},
          },
      .min_coded_height = 4,
      .min_bytes_per_row = 4096,
  };
};

class MockAllocator : public fidl::testing::WireTestBase<fuchsia_sysmem::Allocator> {
 public:
  using BufferCollectionId = int;
  using MockBufferCollectionBuilder =
      fit::function<std::unique_ptr<MockBufferCollectionBase>(void)>;

  explicit MockAllocator(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {
    ASSERT_TRUE(dispatcher_);
  }

  void set_mock_buffer_collection_builder(MockBufferCollectionBuilder builder) {
    mock_buffer_collection_builder_ = std::move(builder);
  }

  void BindSharedCollection(BindSharedCollectionRequestView request,
                            BindSharedCollectionCompleter::Sync& completer) override {
    ASSERT_TRUE(mock_buffer_collection_builder_ != nullptr);
    auto buffer_collection_id = next_buffer_collection_id_++;
    active_buffer_collections_[buffer_collection_id] = {
        .token_client = std::move(request->token),
        .mock_buffer_collection = mock_buffer_collection_builder_(),
    };

    fidl::BindServer(
        dispatcher_, std::move(request->buffer_collection_request),
        active_buffer_collections_[buffer_collection_id].mock_buffer_collection.get(),
        [this, buffer_collection_id](MockBufferCollectionBase*, fidl::UnbindInfo,
                                     fidl::ServerEnd<fuchsia_sysmem::BufferCollection>) {
          inactive_buffer_collection_tokens_.push_back(
              std::move(active_buffer_collections_[buffer_collection_id].token_client));
          active_buffer_collections_.erase(buffer_collection_id);
        });
  }

  MockBufferCollectionBase* GetMostRecentBufferCollection() {
    const BufferCollectionId most_recent_collection_id = next_buffer_collection_id_ - 1;
    if (active_buffer_collections_.find(most_recent_collection_id) ==
        active_buffer_collections_.end()) {
      return nullptr;
    }
    return active_buffer_collections_.at(most_recent_collection_id).mock_buffer_collection.get();
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
    std::unique_ptr<MockBufferCollectionBase> mock_buffer_collection;
  };

  std::unordered_map<BufferCollectionId, BufferCollection> active_buffer_collections_;
  std::vector<fidl::ClientEnd<fuchsia_sysmem::BufferCollectionToken>>
      inactive_buffer_collection_tokens_;

  BufferCollectionId next_buffer_collection_id_ = 0;
  MockBufferCollectionBuilder mock_buffer_collection_builder_ = nullptr;

  async_dispatcher_t* dispatcher_ = nullptr;
};

class FakeCanvasProtocol : ddk::AmlogicCanvasProtocol<FakeCanvasProtocol> {
 public:
  zx_status_t AmlogicCanvasConfig(zx::vmo vmo, size_t offset, const canvas_info_t* info,
                                  uint8_t* canvas_idx) {
    for (size_t i = 0; i < std::size(in_use_); i++) {
      ZX_DEBUG_ASSERT_MSG(i <= std::numeric_limits<uint8_t>::max(), "%zu", i);
      if (!in_use_[i]) {
        in_use_[i] = true;
        *canvas_idx = static_cast<uint8_t>(i);
        return ZX_OK;
      }
    }
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t AmlogicCanvasFree(uint8_t canvas_idx) {
    EXPECT_TRUE(in_use_[canvas_idx]);
    in_use_[canvas_idx] = false;
    return ZX_OK;
  }

  void CheckThatNoEntriesInUse() {
    for (uint32_t i = 0; i < std::size(in_use_); i++) {
      EXPECT_FALSE(in_use_[i]);
    }
  }

  const amlogic_canvas_protocol_t& get_protocol() { return protocol_; }

 private:
  static constexpr uint32_t kCanvasEntries = 256;
  bool in_use_[kCanvasEntries] = {};
  amlogic_canvas_protocol_t protocol_ = {.ops = &amlogic_canvas_protocol_ops_, .ctx = this};
};

class FakeSysmemTest : public zxtest::Test {
 public:
  FakeSysmemTest() : loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override {
    display_ = std::make_unique<AmlogicDisplay>(/*parent=*/nullptr);
    display_->SetFormatSupportCheck([](auto) { return true; });
    display_->SetCanvasForTesting(ddk::AmlogicCanvasProtocolClient(&canvas_.get_protocol()));

    auto vout = std::make_unique<Vout>();
    vout->InitDsiForTesting(/*panel_type=*/PANEL_TV070WSM_FT, /*width=*/1024, /*height=*/600);
    display_->SetVoutForTesting(std::move(vout));

    allocator_ = std::make_unique<MockAllocator>(loop_.dispatcher());
    allocator_->set_mock_buffer_collection_builder([] {
      // Allocate importable primary Image by default.
      const std::vector<sysmem::wire::PixelFormatType> kPixelFormatTypes = {
          sysmem::wire::PixelFormatType::kBgra32, sysmem::wire::PixelFormatType::kR8G8B8A8};
      return std::make_unique<MockBufferCollection>(kPixelFormatTypes);
    });

    {
      zx::result endpoints = fidl::CreateEndpoints<sysmem::Allocator>();
      ASSERT_OK(endpoints);
      fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server), allocator_.get());
      display_->SetSysmemAllocatorForTesting(fidl::WireSyncClient(std::move(endpoints->client)));
    }

    loop_.StartThread("sysmem-handler-loop");
  }

  void TearDown() override {
    // Shutdown the loop before destroying the MockAllocator which may still have
    // pending callbacks.
    loop_.Shutdown();
    loop_.JoinThreads();
  }

 protected:
  async::Loop loop_;

  std::unique_ptr<AmlogicDisplay> display_;
  std::unique_ptr<MockAllocator> allocator_;
  FakeCanvasProtocol canvas_;
};

template <typename Lambda>
bool PollUntil(Lambda predicate, zx::duration poll_interval, int max_intervals) {
  ZX_ASSERT(max_intervals >= 0);

  for (int sleeps_left = max_intervals; sleeps_left > 0; --sleeps_left) {
    if (predicate())
      return true;
    zx::nanosleep(zx::deadline_after(poll_interval));
  }

  return predicate();
}

TEST_F(FakeSysmemTest, ImportBufferCollection) {
  zx::result token1_endpoints = fidl::CreateEndpoints<sysmem::BufferCollectionToken>();
  ASSERT_OK(token1_endpoints);
  zx::result token2_endpoints = fidl::CreateEndpoints<sysmem::BufferCollectionToken>();
  ASSERT_OK(token2_endpoints);

  // Test ImportBufferCollection().
  constexpr uint64_t kValidBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(
      kValidBufferCollectionId, token1_endpoints->client.TakeChannel()));

  // `collection_id` must be unused.
  EXPECT_EQ(display_->DisplayControllerImplImportBufferCollection(
                kValidBufferCollectionId, token2_endpoints->client.TakeChannel()),
            ZX_ERR_ALREADY_EXISTS);

  EXPECT_TRUE(
      PollUntil([&]() { return !allocator_->GetActiveBufferCollectionTokenClients().empty(); },
                zx::msec(5), 1000));

  // Verify that the current buffer collection token is used (active).
  {
    auto active_buffer_token_clients = allocator_->GetActiveBufferCollectionTokenClients();
    EXPECT_EQ(active_buffer_token_clients.size(), 1u);

    auto inactive_buffer_token_clients = allocator_->GetInactiveBufferCollectionTokenClients();
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
  EXPECT_EQ(display_->DisplayControllerImplReleaseBufferCollection(kInvalidBufferCollectionId),
            ZX_ERR_NOT_FOUND);
  EXPECT_OK(display_->DisplayControllerImplReleaseBufferCollection(kValidBufferCollectionId));

  EXPECT_TRUE(
      PollUntil([&]() { return allocator_->GetActiveBufferCollectionTokenClients().empty(); },
                zx::msec(5), 1000));

  // Verify that the current buffer collection token is released (inactive).
  {
    auto active_buffer_token_clients = allocator_->GetActiveBufferCollectionTokenClients();
    EXPECT_EQ(active_buffer_token_clients.size(), 0u);

    auto inactive_buffer_token_clients = allocator_->GetInactiveBufferCollectionTokenClients();
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
}

TEST_F(FakeSysmemTest, ImportImage) {
  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  ASSERT_OK(token_endpoints);
  auto& [token_client, token_server] = token_endpoints.value();

  constexpr uint64_t kBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(kBufferCollectionId,
                                                                  token_client.TakeChannel()));

  // Driver sets BufferCollection buffer memory constraints.
  const image_t kDefaultConfig = {
      .width = 1024,
      .height = 768,
      .pixel_format = ZX_PIXEL_FORMAT_ARGB_8888,
      .type = IMAGE_TYPE_SIMPLE,
      .handle = 0,
  };
  EXPECT_OK(display_->DisplayControllerImplSetBufferCollectionConstraints2(&kDefaultConfig,
                                                                           kBufferCollectionId));

  constexpr uint64_t kInvalidBufferCollectionId = 100u;
  EXPECT_EQ(display_->DisplayControllerImplSetBufferCollectionConstraints2(
                &kDefaultConfig, kInvalidBufferCollectionId),
            ZX_ERR_NOT_FOUND);

  // Invalid import: Bad image type.
  image_t invalid_config = kDefaultConfig;
  invalid_config.type = IMAGE_TYPE_CAPTURE;
  EXPECT_EQ(display_->DisplayControllerImplImportImage2(&invalid_config, kBufferCollectionId,
                                                        /*index=*/0),
            ZX_ERR_INVALID_ARGS);

  // Invalid import: Invalid collection ID.
  invalid_config = kDefaultConfig;
  EXPECT_EQ(display_->DisplayControllerImplImportImage2(&invalid_config, kInvalidBufferCollectionId,
                                                        /*index=*/0),
            ZX_ERR_NOT_FOUND);

  // Invalid import: Invalid buffer collection index.
  invalid_config = kDefaultConfig;
  constexpr uint64_t kInvalidBufferCollectionIndex = 100u;
  EXPECT_EQ(display_->DisplayControllerImplImportImage2(&invalid_config, kBufferCollectionId,
                                                        kInvalidBufferCollectionIndex),
            ZX_ERR_OUT_OF_RANGE);

  // Invalid import: Unsupported format.
  constexpr zx_pixel_format_t kUnsupportedPixelFormat = ZX_PIXEL_FORMAT_I420;
  display_->SetFormatSupportCheck([](zx_pixel_format_t pixel_format) -> bool {
    return pixel_format != kUnsupportedPixelFormat;
  });
  invalid_config = kDefaultConfig;
  invalid_config.pixel_format = kUnsupportedPixelFormat;
  EXPECT_EQ(display_->DisplayControllerImplImportImage2(&invalid_config, kBufferCollectionId,
                                                        /*index=*/0),
            ZX_ERR_INVALID_ARGS);
  display_->SetFormatSupportCheck([](auto) { return true; });

  // Valid import.
  image_t valid_config = kDefaultConfig;
  EXPECT_EQ(valid_config.handle, 0u);
  EXPECT_OK(display_->DisplayControllerImplImportImage2(&valid_config, kBufferCollectionId,
                                                        /*index=*/0));
  EXPECT_NE(valid_config.handle, 0u);

  // Release the image.
  display_->DisplayControllerImplReleaseImage(&valid_config);

  EXPECT_OK(display_->DisplayControllerImplReleaseBufferCollection(kBufferCollectionId));
}

TEST_F(FakeSysmemTest, ImportImageForCapture) {
  allocator_->set_mock_buffer_collection_builder(
      [] { return std::make_unique<MockBufferCollectionForCapture>(); });

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  ASSERT_OK(token_endpoints);
  auto& [token_client, token_server] = token_endpoints.value();

  constexpr uint64_t kBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(kBufferCollectionId,
                                                                  token_client.TakeChannel()));

  // Driver sets BufferCollection buffer memory constraints.
  const image_t kDefaultConfig = {
      .width = 1024,
      .height = 768,
      .pixel_format = ZX_PIXEL_FORMAT_RGB_888,
      .type = IMAGE_TYPE_CAPTURE,
      .handle = 0,
  };
  EXPECT_OK(display_->DisplayControllerImplSetBufferCollectionConstraints2(&kDefaultConfig,
                                                                           kBufferCollectionId));

  // Invalid import: invalid buffer collection ID.
  uint64_t capture_handle = 0;
  const uint64_t kInvalidBufferCollectionId = 100;
  EXPECT_EQ(display_->DisplayControllerImplImportImageForCapture2(kInvalidBufferCollectionId,
                                                                  /*index=*/0, &capture_handle),
            ZX_ERR_NOT_FOUND);

  // Invalid import: index out of range.
  const uint64_t kInvalidIndex = 100;
  EXPECT_EQ(display_->DisplayControllerImplImportImageForCapture2(kBufferCollectionId,
                                                                  kInvalidIndex, &capture_handle),
            ZX_ERR_OUT_OF_RANGE);

  // Valid import.
  capture_handle = 0;
  EXPECT_OK(display_->DisplayControllerImplImportImageForCapture2(kBufferCollectionId, /*index=*/0,
                                                                  &capture_handle));
  EXPECT_NE(capture_handle, 0u);

  // Release the image.
  display_->DisplayControllerImplReleaseCapture(capture_handle);

  EXPECT_OK(display_->DisplayControllerImplReleaseBufferCollection(kBufferCollectionId));
}

TEST_F(FakeSysmemTest, SysmemRequirements) {
  MockBufferCollectionBase* collection = nullptr;
  allocator_->set_mock_buffer_collection_builder([&collection] {
    const std::vector<sysmem::wire::PixelFormatType> kPixelFormatTypes = {
        sysmem::wire::PixelFormatType::kBgra32, sysmem::wire::PixelFormatType::kR8G8B8A8};
    auto new_buffer_collection = std::make_unique<MockBufferCollection>(kPixelFormatTypes);
    collection = new_buffer_collection.get();
    return new_buffer_collection;
  });

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  ASSERT_OK(token_endpoints);
  auto& [token_client, token_server] = token_endpoints.value();

  constexpr uint64_t kBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(kBufferCollectionId,
                                                                  token_client.TakeChannel()));

  EXPECT_TRUE(PollUntil([&] { return collection != nullptr; }, zx::msec(5), 1000));

  image_t image = {};
  EXPECT_OK(
      display_->DisplayControllerImplSetBufferCollectionConstraints2(&image, kBufferCollectionId));

  EXPECT_TRUE(PollUntil([&] { return collection->set_constraints_called(); }, zx::msec(5), 1000));
  EXPECT_TRUE(collection->set_name_called());
}

TEST_F(FakeSysmemTest, SysmemRequirements_BgraOnly) {
  MockBufferCollectionBase* collection = nullptr;
  allocator_->set_mock_buffer_collection_builder([&collection] {
    const std::vector<sysmem::wire::PixelFormatType> kPixelFormatTypes = {
        sysmem::wire::PixelFormatType::kBgra32,
    };
    auto new_buffer_collection = std::make_unique<MockBufferCollection>(kPixelFormatTypes);
    collection = new_buffer_collection.get();
    return new_buffer_collection;
  });
  display_->SetFormatSupportCheck([](zx_pixel_format_t format) {
    return format == ZX_PIXEL_FORMAT_RGB_x888 || format == ZX_PIXEL_FORMAT_ARGB_8888;
  });

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  ASSERT_OK(token_endpoints);
  auto& [token_client, token_server] = token_endpoints.value();

  constexpr uint64_t kBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(kBufferCollectionId,
                                                                  token_client.TakeChannel()));

  EXPECT_TRUE(PollUntil([&] { return collection != nullptr; }, zx::msec(5), 1000));

  image_t image = {};
  EXPECT_OK(
      display_->DisplayControllerImplSetBufferCollectionConstraints2(&image, kBufferCollectionId));

  EXPECT_TRUE(PollUntil([&] { return collection->set_constraints_called(); }, zx::msec(5), 1000));
  EXPECT_TRUE(collection->set_name_called());
}

TEST(AmlogicDisplay, FloatToFix3_10) {
  inspect::Inspector inspector;
  EXPECT_EQ(0x0000, Osd::FloatToFixed3_10(0.0f));
  EXPECT_EQ(0x0066, Osd::FloatToFixed3_10(0.1f));
  EXPECT_EQ(0x1f9a, Osd::FloatToFixed3_10(-0.1f));
  // Test for maximum positive (<4)
  EXPECT_EQ(0x0FFF, Osd::FloatToFixed3_10(4.0f));
  EXPECT_EQ(0x0FFF, Osd::FloatToFixed3_10(40.0f));
  EXPECT_EQ(0x0FFF, Osd::FloatToFixed3_10(3.9999f));
  // Test for minimum negative (>= -4)
  EXPECT_EQ(0x1000, Osd::FloatToFixed3_10(-4.0f));
  EXPECT_EQ(0x1000, Osd::FloatToFixed3_10(-14.0f));
}

TEST(AmlogicDisplay, FloatToFixed2_10) {
  inspect::Inspector inspector;
  EXPECT_EQ(0x0000, Osd::FloatToFixed2_10(0.0f));
  EXPECT_EQ(0x0066, Osd::FloatToFixed2_10(0.1f));
  EXPECT_EQ(0x0f9a, Osd::FloatToFixed2_10(-0.1f));
  // Test for maximum positive (<2)
  EXPECT_EQ(0x07FF, Osd::FloatToFixed2_10(2.0f));
  EXPECT_EQ(0x07FF, Osd::FloatToFixed2_10(20.0f));
  EXPECT_EQ(0x07FF, Osd::FloatToFixed2_10(1.9999f));
  // Test for minimum negative (>= -2)
  EXPECT_EQ(0x0800, Osd::FloatToFixed2_10(-2.0f));
  EXPECT_EQ(0x0800, Osd::FloatToFixed2_10(-14.0f));
}

TEST_F(FakeSysmemTest, NoLeakCaptureCanvas) {
  allocator_->set_mock_buffer_collection_builder(
      [] { return std::make_unique<MockBufferCollectionForCapture>(); });

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  ASSERT_OK(token_endpoints);
  auto& [token_client, token_server] = token_endpoints.value();

  constexpr uint64_t kBufferCollectionId = 1u;
  EXPECT_OK(display_->DisplayControllerImplImportBufferCollection(kBufferCollectionId,
                                                                  token_client.TakeChannel()));

  uint64_t capture_handle;
  EXPECT_OK(display_->DisplayControllerImplImportImageForCapture2(kBufferCollectionId, /*index=*/0,
                                                                  &capture_handle));
  EXPECT_OK(display_->DisplayControllerImplReleaseCapture(capture_handle));

  canvas_.CheckThatNoEntriesInUse();
}

}  // namespace

}  // namespace amlogic_display
