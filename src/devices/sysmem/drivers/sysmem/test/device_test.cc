// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <fidl/fuchsia.sysmem/cpp/fidl.h>
#include <fidl/fuchsia.sysmem/cpp/natural_types.h>
#include <fidl/fuchsia.sysmem2/cpp/fidl.h>
#include <fuchsia/hardware/platform/device/cpp/banjo.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/sync/completion.h>
#include <lib/zx/bti.h>
#include <stdlib.h>
#include <zircon/errors.h>

#include <ddktl/device.h>
#include <ddktl/unbind-txn.h>
#include <zxtest/zxtest.h>

#include "../buffer_collection.h"
#include "../device.h"
#include "../driver.h"
#include "../logical_buffer_collection.h"
#include "src/devices/bus/testing/fake-pdev/fake-pdev.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace sysmem_driver {
namespace {

TEST(Device, OverrideCommandLine) {
  sysmem_driver::Driver sysmem_ctx;
  std::shared_ptr<MockDevice> fake_parent = MockDevice::FakeRootParent();
  sysmem_driver::Device sysmem{fake_parent.get(), &sysmem_ctx};

  const char* kCommandLine = "test.device.commandline";

  int64_t value;
  zx_status_t status;

  value = 10;
  fake_parent->SetVariable(kCommandLine, "5");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_OK(status);
  EXPECT_EQ(5, value);

  value = 11;
  fake_parent->SetVariable(kCommandLine, "65537");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_OK(status);
  EXPECT_EQ(65537, value);

  // Trailing characters should cause the entire value to be ignored.
  value = 12;
  fake_parent->SetVariable(kCommandLine, "65536a");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);
  EXPECT_EQ(12, value);

  // Empty values should be ignored.
  value = 13;
  fake_parent->SetVariable(kCommandLine, "");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_OK(status);
  EXPECT_EQ(13, value);

  // Negative values are allowed (these get interpreted as a percentage of physical RAM), but only
  // up to 99% is allowed.
  value = 14;
  fake_parent->SetVariable(kCommandLine, "-100");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);
  EXPECT_EQ(14, value);

  value = 15;
  fake_parent->SetVariable(kCommandLine, "-99");
  status = sysmem.OverrideSizeFromCommandLine(kCommandLine, &value);
  EXPECT_OK(status);
  EXPECT_EQ(-99, value);
}

TEST(Device, GuardPageCommandLine) {
  sysmem_driver::Driver sysmem_ctx;
  std::shared_ptr<MockDevice> fake_parent = MockDevice::FakeRootParent();
  sysmem_driver::Device sysmem{fake_parent.get(), &sysmem_ctx};

  uint64_t guard_bytes = 1;
  bool unused_pages_guarded = true;
  zx::duration unused_page_check_cycle_period;
  bool internal_guard_pages = true;
  bool crash_on_fail = true;
  const char* kName = "driver.sysmem.contiguous_guard_page_count";
  const char* kInternalName = "driver.sysmem.contiguous_guard_pages_internal";

  fake_parent->SetVariable(kInternalName, "");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size(), guard_bytes);
  EXPECT_TRUE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_TRUE(internal_guard_pages);
  EXPECT_FALSE(crash_on_fail);
  fake_parent->SetVariable(kInternalName, nullptr);

  fake_parent->SetVariable(kName, "fasfas");
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                &unused_page_check_cycle_period,
                                                &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size(), guard_bytes);
  EXPECT_TRUE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_FALSE(internal_guard_pages);
  EXPECT_FALSE(crash_on_fail);

  fake_parent->SetVariable(kName, "");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size(), guard_bytes);
  EXPECT_TRUE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_FALSE(internal_guard_pages);
  EXPECT_FALSE(crash_on_fail);

  fake_parent->SetVariable(kName, "2");
  fake_parent->SetVariable(kInternalName, "");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size() * 2, guard_bytes);
  EXPECT_TRUE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_TRUE(internal_guard_pages);
  EXPECT_FALSE(crash_on_fail);

  const char* kFatalName = "driver.sysmem.contiguous_guard_pages_fatal";
  fake_parent->SetVariable(kFatalName, "");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size() * 2, guard_bytes);
  EXPECT_TRUE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_TRUE(internal_guard_pages);
  EXPECT_TRUE(crash_on_fail);

  const char* kUnusedDisabledName = "driver.sysmem.contiguous_guard_pages_unused_disabled";
  fake_parent->SetVariable(kUnusedDisabledName, "");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size() * 2, guard_bytes);
  EXPECT_FALSE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(600), unused_page_check_cycle_period);
  EXPECT_TRUE(internal_guard_pages);
  EXPECT_TRUE(crash_on_fail);

  const char* kUnusedCycleSecondsName = "driver.sysmem.contiguous_guard_pages_unused_cycle_seconds";
  fake_parent->SetVariable(kUnusedCycleSecondsName, "42");
  EXPECT_EQ(ZX_OK, sysmem.GetContiguousGuardParameters(&guard_bytes, &unused_pages_guarded,
                                                       &unused_page_check_cycle_period,
                                                       &internal_guard_pages, &crash_on_fail));
  EXPECT_EQ(zx_system_get_page_size() * 2, guard_bytes);
  EXPECT_FALSE(unused_pages_guarded);
  EXPECT_EQ(zx::sec(42), unused_page_check_cycle_period);
  EXPECT_TRUE(internal_guard_pages);
  EXPECT_TRUE(crash_on_fail);
}

class FakeDdkSysmem : public zxtest::Test {
 public:
  FakeDdkSysmem() : outgoing_(pdev_loop_.dispatcher()) {}

  void SetUp() override {
    pdev_.SetConfig({
        .use_fake_bti = true,
    });
    EXPECT_OK(pdev_loop_.StartThread());
    auto device_handler =
        [this](fidl::ServerEnd<fuchsia_hardware_platform_device::Device> request) {
          fidl::BindServer(pdev_loop_.dispatcher(), std::move(request), &pdev_);
        };
    fuchsia_hardware_platform_device::Service::InstanceHandler handler(
        {.device = std::move(device_handler)});

    auto service_result =
        outgoing_.AddService<fuchsia_hardware_platform_device::Service>(std::move(handler));
    ZX_ASSERT(service_result.is_ok());

    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ZX_ASSERT(endpoints.is_ok());
    ZX_ASSERT(outgoing_.Serve(std::move(endpoints->server)).is_ok());

    fake_parent_->AddFidlService(fuchsia_hardware_platform_device::Service::Name,
                                 std::move(endpoints->client));
    EXPECT_EQ(sysmem_->Bind(), ZX_OK);
  }

  void TearDown() override {
    ddk::UnbindTxn txn{sysmem_->zxdev()};
    sysmem_->DdkUnbind(std::move(txn));
    EXPECT_OK(sysmem_->zxdev()->WaitUntilUnbindReplyCalled());
    std::ignore = sysmem_.release();
    loop_.Shutdown();
  }

  fidl::ClientEnd<fuchsia_sysmem::Allocator> Connect() {
    zx::result allocator_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::Allocator>();
    EXPECT_OK(allocator_endpoints);
    auto [allocator_client_end, allocator_server_end] = std::move(*allocator_endpoints);

    zx::result connector_endpoints = fidl::CreateEndpoints<fuchsia_sysmem2::DriverConnector>();
    EXPECT_OK(connector_endpoints);
    auto [connector_client_end, connector_server_end] = std::move(*connector_endpoints);

    fidl::BindServer(loop_.dispatcher(), std::move(connector_server_end), sysmem_.get());
    EXPECT_OK(loop_.StartThread());

    auto result = fidl::WireCall(connector_client_end)->ConnectV1(std::move(allocator_server_end));
    EXPECT_OK(result);

    return std::move(allocator_client_end);
  }

  fidl::ClientEnd<fuchsia_sysmem::BufferCollection> AllocateNonSharedCollection() {
    fidl::WireSyncClient<fuchsia_sysmem::Allocator> allocator(Connect());

    zx::result collection_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
    EXPECT_OK(collection_endpoints);
    auto [collection_client_end, collection_server_end] = std::move(*collection_endpoints);

    EXPECT_OK(allocator->AllocateNonSharedCollection(std::move(collection_server_end)));
    return std::move(collection_client_end);
  }

 protected:
  sysmem_driver::Driver sysmem_ctx_;
  std::shared_ptr<MockDevice> fake_parent_ = MockDevice::FakeRootParent();
  std::unique_ptr<sysmem_driver::Device> sysmem_{new Device{fake_parent_.get(), &sysmem_ctx_}};

  fake_pdev::FakePDevFidl pdev_;

  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
  // Separate loop so we can make sync FIDL calls from loop_ to pdev_loop_.
  async::Loop pdev_loop_{&kAsyncLoopConfigNeverAttachToThread};
  component::OutgoingDirectory outgoing_;
};

TEST_F(FakeDdkSysmem, TearDownLoop) {
  // Queue up something that would be processed on the FIDL thread, so we can try to detect a
  // use-after-free if the FidlServer outlives the sysmem device.
  AllocateNonSharedCollection();
}

// Test that creating and tearing down a SecureMem connection works correctly.
TEST_F(FakeDdkSysmem, DummySecureMem) {
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_sysmem::SecureMem>();
  ASSERT_OK(endpoints);
  auto& [client, server] = endpoints.value();
  ASSERT_OK(sysmem_->CommonSysmemRegisterSecureMem(std::move(client)));

  // This shouldn't deadlock waiting for a message on the channel.
  EXPECT_OK(sysmem_->CommonSysmemUnregisterSecureMem());

  // This shouldn't cause a panic due to receiving peer closed.
  client.reset();
}

TEST_F(FakeDdkSysmem, NamedToken) {
  fidl::WireSyncClient<fuchsia_sysmem::Allocator> allocator(Connect());

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  EXPECT_OK(token_endpoints);
  auto [token_client_end, token_server_end] = std::move(*token_endpoints);

  EXPECT_OK(allocator->AllocateSharedCollection(std::move(token_server_end)));

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollectionToken> token(std::move(token_client_end));

  // The buffer collection should end up with a name of "a" because that's the highest priority.
  EXPECT_OK(token->SetName(5u, "c"));
  EXPECT_OK(token->SetName(100u, "a"));
  EXPECT_OK(token->SetName(6u, "b"));

  zx::result collection_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  EXPECT_OK(collection_endpoints);
  auto [collection_client_end, collection_server_end] = std::move(*collection_endpoints);

  EXPECT_OK(
      allocator->BindSharedCollection(token.TakeClientEnd(), std::move(collection_server_end)));

  // Poll until a matching buffer collection is found.
  while (true) {
    bool found_collection = false;
    sync_completion_t completion;
    async::PostTask(sysmem_->dispatcher(), [&] {
      if (sysmem_->logical_buffer_collections().size() == 1) {
        const auto* logical_collection = *sysmem_->logical_buffer_collections().begin();
        auto collection_views = logical_collection->collection_views();
        if (collection_views.size() == 1) {
          auto name = logical_collection->name();
          EXPECT_TRUE(name);
          EXPECT_EQ("a", *name);
          found_collection = true;
        }
      }
      sync_completion_signal(&completion);
    });

    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    if (found_collection)
      break;
  }
}

TEST_F(FakeDdkSysmem, NamedClient) {
  auto collection_client_end = AllocateNonSharedCollection();

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollection> collection(
      std::move(collection_client_end));
  EXPECT_OK(collection->SetDebugClientInfo("a", 5));

  // Poll until a matching buffer collection is found.
  while (true) {
    bool found_collection = false;
    sync_completion_t completion;
    async::PostTask(sysmem_->dispatcher(), [&] {
      if (sysmem_->logical_buffer_collections().size() == 1) {
        const auto* logical_collection = *sysmem_->logical_buffer_collections().begin();
        if (logical_collection->collection_views().size() == 1) {
          const BufferCollection* collection = logical_collection->collection_views().front();
          if (collection->node_properties().client_debug_info().name == "a") {
            EXPECT_EQ(5u, collection->node_properties().client_debug_info().id);
            found_collection = true;
          }
        }
      }
      sync_completion_signal(&completion);
    });

    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    if (found_collection)
      break;
  }
}

// Check that the allocator name overrides the collection name.
TEST_F(FakeDdkSysmem, NamedAllocatorToken) {
  fidl::WireSyncClient<fuchsia_sysmem::Allocator> allocator(Connect());

  zx::result token_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollectionToken>();
  EXPECT_OK(token_endpoints);
  auto [token_client_end, token_server_end] = std::move(*token_endpoints);

  EXPECT_OK(allocator->AllocateSharedCollection(std::move(token_server_end)));

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollectionToken> token(std::move(token_client_end));

  EXPECT_OK(token->SetDebugClientInfo("bad", 6));
  EXPECT_OK(allocator->SetDebugClientInfo("a", 5));

  zx::result collection_endpoints = fidl::CreateEndpoints<fuchsia_sysmem::BufferCollection>();
  EXPECT_OK(collection_endpoints);
  auto [collection_client_end, collection_server_end] = std::move(*collection_endpoints);

  EXPECT_OK(
      allocator->BindSharedCollection(token.TakeClientEnd(), std::move(collection_server_end)));

  // Poll until a matching buffer collection is found.
  while (true) {
    bool found_collection = false;
    sync_completion_t completion;
    async::PostTask(sysmem_->dispatcher(), [&] {
      if (sysmem_->logical_buffer_collections().size() == 1) {
        const auto* logical_collection = *sysmem_->logical_buffer_collections().begin();
        auto collection_views = logical_collection->collection_views();
        if (collection_views.size() == 1) {
          const auto& collection = collection_views.front();
          if (collection->node_properties().client_debug_info().name == "a") {
            EXPECT_EQ(5u, collection->node_properties().client_debug_info().id);
            found_collection = true;
          }
        }
      }
      sync_completion_signal(&completion);
    });

    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    if (found_collection)
      break;
  }
}

TEST_F(FakeDdkSysmem, MaxSize) {
  sysmem_->set_settings(sysmem_driver::Settings{.max_allocation_size = zx_system_get_page_size()});

  auto collection_client = AllocateNonSharedCollection();

  fuchsia_sysmem::BufferCollectionConstraints constraints;
  constraints.min_buffer_count() = 1;
  constraints.has_buffer_memory_constraints() = true;
  constraints.buffer_memory_constraints().min_size_bytes() = zx_system_get_page_size() * 2;
  constraints.buffer_memory_constraints().cpu_domain_supported() = true;
  constraints.usage().cpu() = fuchsia_sysmem::kCpuUsageRead;

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollection> collection(std::move(collection_client));
  fidl::Arena arena;
  EXPECT_OK(collection->SetConstraints(true, fidl::ToWire(arena, std::move(constraints))));

  // Sysmem should fail the collection and return an error.
  fidl::WireResult result = collection->WaitForBuffersAllocated();
  EXPECT_NE(result.status(), ZX_OK);
}

// Check that teardown doesn't leak any memory (detected through LSAN).
TEST_F(FakeDdkSysmem, TeardownLeak) {
  auto collection_client = AllocateNonSharedCollection();

  fuchsia_sysmem::BufferCollectionConstraints constraints;
  constraints.min_buffer_count() = 1;
  constraints.has_buffer_memory_constraints() = true;
  constraints.buffer_memory_constraints().min_size_bytes() = zx_system_get_page_size();
  constraints.buffer_memory_constraints().cpu_domain_supported() = true;
  constraints.usage().cpu() = fuchsia_sysmem::kCpuUsageRead;

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollection> collection(std::move(collection_client));
  fidl::Arena arena;
  EXPECT_OK(collection->SetConstraints(true, fidl::ToWire(arena, std::move(constraints))));

  fidl::WireResult result = collection->WaitForBuffersAllocated();

  EXPECT_OK(result);
  EXPECT_OK(result.value().status);

  for (uint32_t i = 0; i < result.value().buffer_collection_info.buffer_count; i++) {
    result.value().buffer_collection_info.buffers[i].vmo.reset();
  }
  collection = {};
}

// Check that there are no circular references from a VMO to the logical buffer collection, even
// when aux buffers are checked for.
TEST_F(FakeDdkSysmem, AuxBufferLeak) {
  auto collection_client = AllocateNonSharedCollection();

  fuchsia_sysmem::BufferCollectionConstraints constraints;
  constraints.min_buffer_count() = 1;
  constraints.has_buffer_memory_constraints() = true;
  constraints.buffer_memory_constraints().min_size_bytes() = zx_system_get_page_size();
  constraints.buffer_memory_constraints().cpu_domain_supported() = true;
  constraints.usage().cpu() = fuchsia_sysmem::kCpuUsageRead;

  fidl::WireSyncClient<fuchsia_sysmem::BufferCollection> collection(std::move(collection_client));
  fidl::Arena arena;
  EXPECT_OK(collection->SetConstraints(true, fidl::ToWire(arena, std::move(constraints))));

  fidl::WireResult result = collection->WaitForBuffersAllocated();

  EXPECT_OK(result);
  EXPECT_OK(result.value().status);

  for (uint32_t i = 0; i < result.value().buffer_collection_info.buffer_count; i++) {
    result.value().buffer_collection_info.buffers[i].vmo.reset();
  }

  fidl::WireResult aux_result = collection->GetAuxBuffers();

  EXPECT_OK(aux_result);
  EXPECT_OK(aux_result.value().status);
  EXPECT_EQ(1u, aux_result.value().buffer_collection_info_aux_buffers.buffer_count);
  EXPECT_EQ(ZX_HANDLE_INVALID,
            aux_result.value().buffer_collection_info_aux_buffers.buffers[0].vmo);
  collection = {};

  // Poll until all buffer collections are deleted.
  while (true) {
    bool no_collections = false;
    sync_completion_t completion;
    async::PostTask(sysmem_->dispatcher(), [&] {
      no_collections = sysmem_->logical_buffer_collections().empty();
      sync_completion_signal(&completion);
    });

    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    if (no_collections)
      break;
  }
}

}  // namespace
}  // namespace sysmem_driver
