// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/misc/goldfish_control/control_device.h"

#include <fidl/fuchsia.hardware.goldfish.pipe/cpp/markers.h>
#include <fidl/fuchsia.hardware.goldfish.pipe/cpp/wire.h>
#include <fidl/fuchsia.hardware.goldfish/cpp/wire_test_base.h>
#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <fuchsia/hardware/goldfish/control/cpp/banjo.h>
#include <lib/async-loop/loop.h>
#include <lib/async-loop/testing/cpp/real_loop.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fake-bti/bti.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/bti.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/errors.h>
#include <zircon/rights.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <gtest/gtest.h>

#include "src/devices/lib/goldfish/pipe_headers/include/base.h"
#include "src/devices/testing/mock-ddk/mock-device.h"
#include "src/graphics/drivers/misc/goldfish_control/render_control_commands.h"

#define ASSERT_OK(expr) ASSERT_EQ(ZX_OK, expr)
#define EXPECT_OK(expr) EXPECT_EQ(ZX_OK, expr)

namespace goldfish {
namespace {

// TODO(fxbug.dev/80642): Use //src/devices/lib/goldfish/fake_pipe instead.
class FakePipe : public fidl::WireServer<fuchsia_hardware_goldfish_pipe::GoldfishPipe> {
 public:
  struct HeapInfo {
    fidl::ClientEnd<fuchsia_sysmem2::Heap> heap_client_end;
    bool is_registered = false;
    bool cpu_supported = false;
    bool ram_supported = false;
    bool inaccessible_supported = false;
  };

  void Create(CreateCompleter::Sync& completer) override {
    zx::vmo vmo;
    zx_status_t status = zx::vmo::create(PAGE_SIZE, 0u, &vmo);
    if (status != ZX_OK) {
      completer.Close(status);
      return;
    }
    status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &pipe_cmd_buffer_);
    if (status != ZX_OK) {
      completer.Close(status);
      return;
    }

    pipe_created_ = true;
    completer.ReplySuccess(kPipeId, std::move(vmo));
  }

  void SetEvent(SetEventRequestView request, SetEventCompleter::Sync& completer) override {
    if (request->id != kPipeId) {
      completer.Close(ZX_ERR_INVALID_ARGS);
      return;
    }
    if (!request->pipe_event.is_valid()) {
      completer.Close(ZX_ERR_BAD_HANDLE);
      return;
    }
    pipe_event_ = std::move(request->pipe_event);
    completer.ReplySuccess();
  }

  void Destroy(DestroyRequestView request, DestroyCompleter::Sync& completer) override {
    pipe_cmd_buffer_.reset();
    completer.Reply();
  }

  void Open(OpenRequestView request, OpenCompleter::Sync& completer) override {
    auto mapping = MapCmdBuffer();
    reinterpret_cast<PipeCmdBuffer*>(mapping.start())->status = 0;

    pipe_opened_ = true;
    completer.Reply();
  }

  void Exec(ExecRequestView request, ExecCompleter::Sync& completer) override {
    auto mapping = MapCmdBuffer();
    PipeCmdBuffer* cmd_buffer = reinterpret_cast<PipeCmdBuffer*>(mapping.start());
    cmd_buffer->rw_params.consumed_size = cmd_buffer->rw_params.sizes[0];
    cmd_buffer->status = 0;

    if (cmd_buffer->cmd ==
        static_cast<int32_t>(fuchsia_hardware_goldfish_pipe::PipeCmdCode::kWrite)) {
      // Store io buffer contents.
      auto io_buffer = MapIoBuffer();
      io_buffer_contents_.emplace_back(std::vector<uint8_t>(io_buffer_size_, 0));
      memcpy(io_buffer_contents_.back().data(), io_buffer.start(), io_buffer_size_);
    }

    if (cmd_buffer->cmd ==
        static_cast<int32_t>(fuchsia_hardware_goldfish_pipe::PipeCmdCode::kRead)) {
      auto io_buffer = MapIoBuffer();
      uint32_t op = *reinterpret_cast<uint32_t*>(io_buffer.start());

      switch (op) {
        case kOP_rcCreateBuffer2:
        case kOP_rcCreateColorBuffer:
          *reinterpret_cast<uint32_t*>(io_buffer.start()) = ++buffer_id_;
          break;
        case kOP_rcMapGpaToBufferHandle2:
        case kOP_rcSetColorBufferVulkanMode2:
          *reinterpret_cast<int32_t*>(io_buffer.start()) = 0;
          break;
        default:
          ZX_ASSERT_MSG(false, "invalid renderControl command (op %u)", op);
      }
    }

    completer.Reply();
  }

  void GetBti(GetBtiCompleter::Sync& completer) override {
    zx::bti bti;
    zx_status_t status = fake_bti_create(bti.reset_and_get_address());
    if (status != ZX_OK) {
      completer.Close(status);
      return;
    }
    bti_ = bti.borrow();
    completer.ReplySuccess(std::move(bti));
  }

  void ConnectSysmem(ConnectSysmemRequestView request,
                     ConnectSysmemCompleter::Sync& completer) override {
    completer.ReplySuccess();
  }

  void RegisterSysmemHeap(RegisterSysmemHeapRequestView request,
                          RegisterSysmemHeapCompleter::Sync& completer) override {
    heap_info_[request->heap] = {};
    heap_info_[request->heap].heap_client_end =
        fidl::ClientEnd<fuchsia_sysmem2::Heap>(std::move(request->connection));
    completer.ReplySuccess();
  }

  zx_status_t SetUpPipeDevice() {
    zx_status_t status = HandleSysmemEvents();
    if (status != ZX_OK) {
      return status;
    }

    if (!pipe_io_buffer_.is_valid()) {
      status = PrepareIoBuffer();
      if (status != ZX_OK) {
        return status;
      }
    }
    return ZX_OK;
  }

  fzl::VmoMapper MapCmdBuffer() const {
    fzl::VmoMapper mapping;
    mapping.Map(pipe_cmd_buffer_, 0, sizeof(PipeCmdBuffer), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    return mapping;
  }

  fzl::VmoMapper MapIoBuffer() {
    if (!pipe_io_buffer_.is_valid()) {
      PrepareIoBuffer();
    }
    fzl::VmoMapper mapping;
    mapping.Map(pipe_io_buffer_, 0, io_buffer_size_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    return mapping;
  }

  bool IsPipeReady() const { return pipe_created_ && pipe_opened_; }

  uint32_t CurrentBufferHandle() { return buffer_id_; }

  const std::unordered_map<uint64_t, HeapInfo>& heap_info() const { return heap_info_; }

  const std::vector<std::vector<uint8_t>>& io_buffer_contents() const {
    return io_buffer_contents_;
  }

 private:
  class SysmemHeapEventHandler : public fidl::WireSyncEventHandler<fuchsia_sysmem2::Heap> {
   public:
    SysmemHeapEventHandler() = default;
    void OnRegister(fidl::WireEvent<fuchsia_sysmem2::Heap::OnRegister>* message) override {
      if (handler != nullptr) {
        handler(message);
      }
    }
    void SetOnRegisterHandler(
        fit::function<void(fidl::WireEvent<fuchsia_sysmem2::Heap::OnRegister>*)> new_handler) {
      handler = std::move(new_handler);
    }

   private:
    fit::function<void(fidl::WireEvent<fuchsia_sysmem2::Heap::OnRegister>*)> handler;
  };

  zx_status_t HandleSysmemEvents() {
    zx_status_t status = ZX_OK;
    for (auto& kv : heap_info_) {
      SysmemHeapEventHandler handler;
      handler.SetOnRegisterHandler(
          [this, heap = kv.first](fidl::WireEvent<fuchsia_sysmem2::Heap::OnRegister>* message) {
            auto& heap_info = heap_info_[heap];
            heap_info.is_registered = true;
            heap_info.cpu_supported =
                message->properties.coherency_domain_support().cpu_supported();
            heap_info.ram_supported =
                message->properties.coherency_domain_support().ram_supported();
            heap_info.inaccessible_supported =
                message->properties.coherency_domain_support().inaccessible_supported();
          });
      status = handler.HandleOneEvent(kv.second.heap_client_end).status();
      if (status != ZX_OK) {
        break;
      }
    }
    return status;
  }

  zx_status_t PrepareIoBuffer() {
    uint64_t num_pinned_vmos = 0u;
    std::vector<fake_bti_pinned_vmo_info_t> pinned_vmos;
    zx_status_t status = fake_bti_get_pinned_vmos(bti_->get(), nullptr, 0, &num_pinned_vmos);
    if (status != ZX_OK) {
      return status;
    }
    if (num_pinned_vmos == 0u) {
      return ZX_ERR_NOT_FOUND;
    }

    pinned_vmos.resize(num_pinned_vmos);
    status = fake_bti_get_pinned_vmos(bti_->get(), pinned_vmos.data(), num_pinned_vmos, nullptr);
    if (status != ZX_OK) {
      return status;
    }

    pipe_io_buffer_ = zx::vmo(pinned_vmos.back().vmo);
    pinned_vmos.pop_back();
    // close all the unused handles
    for (auto vmo_info : pinned_vmos) {
      zx_handle_close(vmo_info.vmo);
    }

    status = pipe_io_buffer_.get_size(&io_buffer_size_);
    return status;
  }

  zx::unowned_bti bti_;

  static constexpr int32_t kPipeId = 1;
  zx::vmo pipe_cmd_buffer_ = zx::vmo();
  zx::vmo pipe_io_buffer_ = zx::vmo();
  size_t io_buffer_size_;

  zx::event pipe_event_;

  bool pipe_created_ = false;
  bool pipe_opened_ = false;

  int32_t buffer_id_ = 0;

  std::unordered_map<uint64_t, HeapInfo> heap_info_;
  std::vector<std::vector<uint8_t>> io_buffer_contents_;
};

class FakeAddressSpace : public fidl::WireServer<fuchsia_hardware_goldfish::AddressSpaceDevice> {
  void OpenChildDriver(OpenChildDriverRequestView request,
                       OpenChildDriverCompleter::Sync& completer) override {
    request->req.Close(ZX_ERR_NOT_SUPPORTED);
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }
};

class FakeAddressSpaceChild
    : public fidl::testing::WireTestBase<fuchsia_hardware_goldfish::AddressSpaceChildDriver> {
  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    ADD_FAILURE() << "unexpected call to " << name;
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }
};

class FakeSync : public fidl::WireServer<fuchsia_hardware_goldfish::SyncDevice> {
 public:
  void CreateTimeline(CreateTimelineRequestView request,
                      CreateTimelineCompleter::Sync& completer) override {
    completer.Reply();
  }
};

class ControlDeviceTest : public testing::Test, public loop_fixture::RealLoop {
 public:
  ControlDeviceTest()
      : loop_(&kAsyncLoopConfigNeverAttachToThread),
        pipe_server_loop_(&kAsyncLoopConfigNeverAttachToThread),
        address_space_server_loop_(&kAsyncLoopConfigNeverAttachToThread),
        sync_server_loop_(&kAsyncLoopConfigNeverAttachToThread),
        outgoing_(dispatcher()) {}

  void SetUp() override {
    fake_parent_ = MockDevice::FakeRootParent();

    zx::result service_result = outgoing_.AddService<fuchsia_hardware_goldfish_pipe::Service>(
        fuchsia_hardware_goldfish_pipe::Service::InstanceHandler({
            .device = pipe_.bind_handler(pipe_server_loop_.dispatcher()),
        }));
    ASSERT_EQ(service_result.status_value(), ZX_OK);

    zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_OK(endpoints.status_value());
    ASSERT_OK(outgoing_.Serve(std::move(endpoints->server)).status_value());

    fake_parent_->AddFidlService(fuchsia_hardware_goldfish_pipe::Service::Name,
                                 std::move(endpoints->client), "goldfish-pipe");

    service_result = outgoing_.AddService<fuchsia_hardware_goldfish::AddressSpaceService>(
        fuchsia_hardware_goldfish::AddressSpaceService::InstanceHandler({
            .device = address_space_.bind_handler(address_space_server_loop_.dispatcher()),
        }));
    ASSERT_EQ(service_result.status_value(), ZX_OK);

    endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_OK(endpoints.status_value());
    ASSERT_OK(outgoing_.Serve(std::move(endpoints->server)).status_value());

    fake_parent_->AddFidlService(fuchsia_hardware_goldfish::AddressSpaceService::Name,
                                 std::move(endpoints->client), "goldfish-address-space");

    service_result = outgoing_.AddService<fuchsia_hardware_goldfish::SyncService>(
        fuchsia_hardware_goldfish::SyncService::InstanceHandler({
            .device = sync_.bind_handler(sync_server_loop_.dispatcher()),
        }));
    ASSERT_EQ(service_result.status_value(), ZX_OK);

    endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_OK(endpoints.status_value());
    ASSERT_OK(outgoing_.Serve(std::move(endpoints->server)).status_value());

    fake_parent_->AddFidlService(fuchsia_hardware_goldfish::SyncService::Name,
                                 std::move(endpoints->client), "goldfish-sync");

    pipe_server_loop_.StartThread("goldfish-pipe-fidl-server");
    address_space_server_loop_.StartThread("goldfish-address-space-fidl-server");
    sync_server_loop_.StartThread("goldfish-sync-fidl-server");

    auto dut = std::make_unique<Control>(fake_parent_.get());
    PerformBlockingWork([&]() { ASSERT_EQ(dut->Bind(), ZX_OK); });
    // The device will be deleted by MockDevice when the test ends.
    dut.release();

    ASSERT_EQ(fake_parent_->child_count(), 1u);
    auto fake_dut = fake_parent_->GetLatestChild();
    dut_ = fake_dut->GetDeviceContext<Control>();

    ASSERT_OK(pipe_.SetUpPipeDevice());
    ASSERT_TRUE(pipe_.IsPipeReady());

    // Bind control device FIDL server.
    auto control_endpoints = fidl::CreateEndpoints<fuchsia_hardware_goldfish::ControlDevice>();
    ASSERT_TRUE(control_endpoints.is_ok());

    control_fidl_server_ =
        fidl::BindServer(loop_.dispatcher(), std::move(control_endpoints->server),
                         fake_dut->GetDeviceContext<Control>());

    loop_.StartThread("goldfish-control-device-fidl-server");

    fidl_client_ = fidl::WireSyncClient(std::move(control_endpoints->client));
  }

  void TearDown() override {
    device_async_remove(dut_->zxdev());
    mock_ddk::ReleaseFlaggedDevices(fake_parent_.get());
  }

 protected:
  Control* dut_ = nullptr;

  FakePipe pipe_;
  FakeAddressSpace address_space_;
  FakeAddressSpaceChild address_space_child_;
  FakeSync sync_;

  std::shared_ptr<MockDevice> fake_parent_;

  async::Loop loop_;
  async::Loop pipe_server_loop_;
  async::Loop address_space_server_loop_;
  async::Loop sync_server_loop_;
  component::OutgoingDirectory outgoing_;
  std::optional<fidl::ServerBindingRef<fuchsia_hardware_goldfish::ControlDevice>>
      control_fidl_server_ = std::nullopt;
  fidl::WireSyncClient<fuchsia_hardware_goldfish::ControlDevice> fidl_client_ = {};
};

TEST_F(ControlDeviceTest, Bind) {
  const auto& heaps = pipe_.heap_info();
  ASSERT_EQ(heaps.size(), 2u);
  ASSERT_TRUE(heaps.find(static_cast<uint64_t>(
                  fuchsia_sysmem2::wire::HeapType::kGoldfishDeviceLocal)) != heaps.end());
  ASSERT_TRUE(heaps.find(static_cast<uint64_t>(
                  fuchsia_sysmem2::wire::HeapType::kGoldfishHostVisible)) != heaps.end());

  const auto& device_local_heap_info =
      heaps.at(static_cast<uint64_t>(fuchsia_sysmem2::wire::HeapType::kGoldfishDeviceLocal));
  EXPECT_TRUE(device_local_heap_info.heap_client_end.is_valid());
  EXPECT_TRUE(device_local_heap_info.is_registered);
  EXPECT_TRUE(device_local_heap_info.inaccessible_supported);

  const auto& host_visible_heap_info =
      heaps.at(static_cast<uint64_t>(fuchsia_sysmem2::wire::HeapType::kGoldfishHostVisible));
  EXPECT_TRUE(host_visible_heap_info.heap_client_end.is_valid());
  EXPECT_TRUE(host_visible_heap_info.is_registered);
  EXPECT_TRUE(host_visible_heap_info.cpu_supported);
}

// Test |fuchsia.hardware.goldfish.Control.CreateBuffer2| method.
class BufferTest : public ControlDeviceTest, public testing::WithParamInterface<uint32_t> {};

TEST_P(BufferTest, TestCreate2) {
  constexpr size_t kSize = 65536u;
  constexpr uint64_t kPhysicalAddress = 0x12345678abcd0000;
  const auto memory_property = GetParam();
  const bool is_host_visible =
      memory_property == fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible;

  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  dut_->RegisterBufferHandle(buffer_vmo);
  fidl::Arena allocator;
  fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params(allocator);
  create_params.set_size(allocator, kSize).set_memory_property(memory_property);
  if (is_host_visible) {
    create_params.set_physical_address(allocator, kPhysicalAddress);
  }
  auto create_buffer_result =
      fidl_client_->CreateBuffer2(std::move(buffer_vmo), std::move(create_params));

  ASSERT_TRUE(create_buffer_result.ok());
  ASSERT_TRUE(create_buffer_result->is_ok());

  CreateBuffer2Cmd create_buffer_cmd{
      .op = kOP_rcCreateBuffer2,
      .size = kSize_rcCreateBuffer2,
      .buffer_size = kSize,
      .memory_property = memory_property,
  };

  MapGpaToBufferHandle2Cmd map_gpa_cmd{
      .op = kOP_rcMapGpaToBufferHandle2,
      .size = kSize_rcMapGpaToBufferHandle2,
      .id = pipe_.CurrentBufferHandle(),
      .gpa = kPhysicalAddress,
      .map_size = kSize,
  };

  const auto& io_buffer_contents = pipe_.io_buffer_contents();
  size_t create_buffer_cmd_idx = 0;
  if (is_host_visible) {
    ASSERT_GE(io_buffer_contents.size(), 2u);
    create_buffer_cmd_idx = io_buffer_contents.size() - 2;
  } else {
    ASSERT_GE(io_buffer_contents.size(), 1u);
    create_buffer_cmd_idx = io_buffer_contents.size() - 1;
  }

  EXPECT_EQ(memcmp(&create_buffer_cmd, io_buffer_contents[create_buffer_cmd_idx].data(),
                   sizeof(CreateBuffer2Cmd)),
            0);
  if (is_host_visible) {
    EXPECT_EQ(memcmp(&map_gpa_cmd, io_buffer_contents[create_buffer_cmd_idx + 1].data(),
                     sizeof(MapGpaToBufferHandle2Cmd)),
              0);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ControlDeviceTest, BufferTest,
    testing::Values(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal,
                    fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible),
    [](const testing::TestParamInfo<BufferTest::ParamType>& info) {
      std::string memory_property;
      switch (info.param) {
        case fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal:
          memory_property = "DEVICE_LOCAL";
          break;
        case fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible:
          memory_property = "HOST_VISIBLE";
          break;
        default:
          memory_property = "UNSUPPORTED_MEMORY_PROPERTY";
      }
      return memory_property;
    });

TEST_F(ControlDeviceTest, CreateBuffer2_AlreadyExists) {
  constexpr size_t kSize = 65536u;
  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  zx::vmo copy_vmo;
  ASSERT_OK(buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy_vmo));

  dut_->RegisterBufferHandle(buffer_vmo);
  fidl::Arena allocator;
  fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params(allocator);
  create_params.set_size(allocator, kSize)
      .set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);
  auto create_buffer_result =
      fidl_client_->CreateBuffer2(std::move(buffer_vmo), std::move(create_params));

  ASSERT_TRUE(create_buffer_result.ok());
  ASSERT_TRUE(create_buffer_result->is_ok());

  fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params2(allocator);
  create_params2.set_size(allocator, kSize)
      .set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);
  auto create_copy_buffer_result =
      fidl_client_->CreateBuffer2(std::move(copy_vmo), std::move(create_params2));

  ASSERT_TRUE(create_copy_buffer_result.ok());
  ASSERT_TRUE(create_copy_buffer_result->is_error());
  ASSERT_EQ(create_copy_buffer_result->error_value(), ZX_ERR_ALREADY_EXISTS);
}

TEST_F(ControlDeviceTest, CreateBuffer2_InvalidArgs) {
  constexpr size_t kSize = 65536u;
  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params(allocator);
    // missing size
    create_params.set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

    auto result = fidl_client_->CreateBuffer2(std::move(buffer_vmo), std::move(create_params));
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params2(allocator);
    // missing memory property
    create_params2.set_size(allocator, kSize);

    auto result = fidl_client_->CreateBuffer2(std::move(buffer_vmo), std::move(create_params2));
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }
}

TEST_F(ControlDeviceTest, CreateBuffer2_InvalidVmo) {
  constexpr size_t kSize = 65536u;
  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  fidl::Arena allocator;
  fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params(allocator);
  create_params.set_size(allocator, kSize)
      .set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

  auto create_unregistered_buffer_result =
      fidl_client_->CreateBuffer2(std::move(buffer_vmo), std::move(create_params));

  ASSERT_TRUE(create_unregistered_buffer_result.ok());
  ASSERT_TRUE(create_unregistered_buffer_result->is_error());
  ASSERT_EQ(create_unregistered_buffer_result->error_value(), ZX_ERR_INVALID_ARGS);

  fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params2(allocator);
  create_params2.set_size(allocator, kSize)
      .set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

  auto create_invalid_buffer_result =
      fidl_client_->CreateBuffer2(zx::vmo(), std::move(create_params2));

  ASSERT_EQ(create_invalid_buffer_result.status(), ZX_ERR_INVALID_ARGS);
}

// Test |fuchsia.hardware.goldfish.Control.CreateColorBuffer2| method.
class ColorBufferTest
    : public ControlDeviceTest,
      public testing::WithParamInterface<
          std::tuple<fuchsia_hardware_goldfish::wire::ColorBufferFormatType, uint32_t>> {};

TEST_P(ColorBufferTest, TestCreate) {
  constexpr uint32_t kWidth = 1024u;
  constexpr uint32_t kHeight = 768u;
  constexpr uint32_t kSize = kWidth * kHeight * 4;
  constexpr uint64_t kPhysicalAddress = 0x12345678abcd0000;
  const auto format = std::get<0>(GetParam());
  const auto memory_property = std::get<1>(GetParam());
  const bool is_host_visible =
      memory_property == fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible;

  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  dut_->RegisterBufferHandle(buffer_vmo);

  fidl::Arena allocator;
  fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
  create_params.set_width(kWidth).set_height(kHeight).set_format(format).set_memory_property(
      memory_property);
  if (is_host_visible) {
    create_params.set_physical_address(allocator, kPhysicalAddress);
  }

  auto create_color_buffer_result =
      fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

  ASSERT_TRUE(create_color_buffer_result.ok());
  EXPECT_OK(create_color_buffer_result.value().res);
  const int32_t expected_page_offset = is_host_visible ? 0 : -1;
  EXPECT_EQ(create_color_buffer_result.value().hw_address_page_offset, expected_page_offset);

  CreateColorBufferCmd create_color_buffer_cmd{
      .op = kOP_rcCreateColorBuffer,
      .size = kSize_rcCreateColorBuffer,
      .width = kWidth,
      .height = kHeight,
      .internalformat = static_cast<uint32_t>(format),
  };

  SetColorBufferVulkanMode2Cmd set_vulkan_mode_cmd{
      .op = kOP_rcSetColorBufferVulkanMode2,
      .size = kSize_rcSetColorBufferVulkanMode2,
      .id = pipe_.CurrentBufferHandle(),
      .mode = 1u,  // VULKAN_ONLY
      .memory_property = memory_property,
  };

  MapGpaToBufferHandle2Cmd map_gpa_cmd{
      .op = kOP_rcMapGpaToBufferHandle2,
      .size = kSize_rcMapGpaToBufferHandle2,
      .id = pipe_.CurrentBufferHandle(),
      .gpa = kPhysicalAddress,
      .map_size = kSize,
  };

  const auto& io_buffer_contents = pipe_.io_buffer_contents();
  size_t create_color_buffer_cmd_idx = 0;
  if (is_host_visible) {
    ASSERT_GE(io_buffer_contents.size(), 3u);
    create_color_buffer_cmd_idx = io_buffer_contents.size() - 3;
  } else {
    ASSERT_GE(io_buffer_contents.size(), 2u);
    create_color_buffer_cmd_idx = io_buffer_contents.size() - 2;
  }

  EXPECT_EQ(memcmp(&create_color_buffer_cmd, io_buffer_contents[create_color_buffer_cmd_idx].data(),
                   sizeof(CreateColorBufferCmd)),
            0);
  EXPECT_EQ(memcmp(&set_vulkan_mode_cmd, io_buffer_contents[create_color_buffer_cmd_idx + 1].data(),
                   sizeof(set_vulkan_mode_cmd)),
            0);
  if (is_host_visible) {
    EXPECT_EQ(memcmp(&map_gpa_cmd, io_buffer_contents[create_color_buffer_cmd_idx + 2].data(),
                     sizeof(MapGpaToBufferHandle2Cmd)),
              0);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ControlDeviceTest, ColorBufferTest,
    testing::Combine(
        testing::Values(fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRg,
                        fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba,
                        fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kBgra,
                        fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kLuminance),
        testing::Values(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal,
                        fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible)),
    [](const testing::TestParamInfo<ColorBufferTest::ParamType>& info) {
      std::string format;
      switch (std::get<0>(info.param)) {
        case fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRg:
          format = "RG";
          break;
        case fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba:
          format = "RGBA";
          break;
        case fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kBgra:
          format = "BGRA";
          break;
        case fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kLuminance:
          format = "LUMINANCE";
          break;
        default:
          format = "UNSUPPORTED_FORMAT";
      }

      std::string memory_property;
      switch (std::get<1>(info.param)) {
        case fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal:
          memory_property = "DEVICE_LOCAL";
          break;
        case fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible:
          memory_property = "HOST_VISIBLE";
          break;
        default:
          memory_property = "UNSUPPORTED_MEMORY_PROPERTY";
      }

      return format + "_" + memory_property;
    });

TEST_F(ControlDeviceTest, CreateColorBuffer2_AlreadyExists) {
  constexpr uint32_t kWidth = 1024u;
  constexpr uint32_t kHeight = 768u;
  constexpr uint32_t kSize = kWidth * kHeight * 4;
  constexpr auto kFormat = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba;
  constexpr auto kMemoryProperty = fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal;

  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  zx::vmo copy_vmo;
  ASSERT_OK(buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy_vmo));

  dut_->RegisterBufferHandle(buffer_vmo);

  {
    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        kMemoryProperty);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_OK(create_color_buffer_result.value().res);
  }

  {
    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        kMemoryProperty);

    auto create_copy_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(copy_vmo), std::move(create_params));

    ASSERT_TRUE(create_copy_buffer_result.ok());
    ASSERT_EQ(create_copy_buffer_result.value().res, ZX_ERR_ALREADY_EXISTS);
  }
}

TEST_F(ControlDeviceTest, CreateColorBuffer2_InvalidArgs) {
  constexpr uint32_t kWidth = 1024u;
  constexpr uint32_t kHeight = 768u;
  constexpr uint32_t kSize = kWidth * kHeight * 4;
  constexpr auto kFormat = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba;
  constexpr auto kMemoryProperty = fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal;

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    // missing width
    create_params.set_height(kHeight).set_format(kFormat).set_memory_property(kMemoryProperty);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_EQ(create_color_buffer_result.value().res, ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    // missing height
    create_params.set_width(kWidth).set_format(kFormat).set_memory_property(kMemoryProperty);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_EQ(create_color_buffer_result.value().res, ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    // missing format
    create_params.set_width(kWidth).set_height(kHeight).set_memory_property(kMemoryProperty);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_EQ(create_color_buffer_result.value().res, ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    // missing memory property
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_EQ(create_color_buffer_result.value().res, ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }

  {
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    // missing physical address
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_EQ(create_color_buffer_result.value().res, ZX_ERR_INVALID_ARGS);

    dut_->FreeBufferHandle(info.koid);
  }
}

TEST_F(ControlDeviceTest, CreateColorBuffer2_InvalidVmo) {
  constexpr uint32_t kWidth = 1024u;
  constexpr uint32_t kHeight = 768u;
  constexpr uint32_t kSize = kWidth * kHeight * 4;
  constexpr auto kFormat = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba;
  constexpr auto kMemoryProperty = fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal;

  zx::vmo buffer_vmo;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

  {
    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        kMemoryProperty);

    auto create_unregistered_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(buffer_vmo), std::move(create_params));

    ASSERT_TRUE(create_unregistered_buffer_result.ok());
    EXPECT_EQ(create_unregistered_buffer_result.value().res, ZX_ERR_INVALID_ARGS);
  }

  {
    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        kMemoryProperty);

    auto create_invalid_buffer_result =
        fidl_client_->CreateColorBuffer2(zx::vmo(), std::move(create_params));

    ASSERT_EQ(create_invalid_buffer_result.status(), ZX_ERR_INVALID_ARGS);
  }
}

// Test |fuchsia.hardware.goldfish.Control.GetBufferHandle| method.
TEST_F(ControlDeviceTest, GetBufferHandle_Success) {
  zx::vmo buffer_vmo, buffer_vmo_dup;
  zx::vmo color_buffer_vmo, color_buffer_vmo_dup;

  // Create data buffer.
  {
    constexpr size_t kSize = 65536u;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));
    ASSERT_OK(buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &buffer_vmo_dup));

    zx::vmo copy_vmo;
    ASSERT_OK(buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy_vmo));

    dut_->RegisterBufferHandle(buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateBuffer2Params create_params(allocator);
    create_params.set_size(allocator, kSize)
        .set_memory_property(fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

    auto create_buffer_result =
        fidl_client_->CreateBuffer2(std::move(copy_vmo), std::move(create_params));

    ASSERT_TRUE(create_buffer_result.ok());
    EXPECT_TRUE(create_buffer_result->is_ok());
  }

  // Create color buffer.
  {
    constexpr uint32_t kWidth = 1024u;
    constexpr uint32_t kHeight = 768u;
    constexpr uint32_t kSize = kWidth * kHeight * 4;
    constexpr auto kFormat = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba;
    constexpr auto kMemoryProperty = fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal;

    ASSERT_OK(zx::vmo::create(kSize, 0u, &color_buffer_vmo));
    ASSERT_OK(color_buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &color_buffer_vmo_dup));

    zx::vmo copy_vmo;
    ASSERT_OK(color_buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy_vmo));

    dut_->RegisterBufferHandle(color_buffer_vmo);

    fidl::Arena allocator;
    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params create_params(allocator);
    create_params.set_width(kWidth).set_height(kHeight).set_format(kFormat).set_memory_property(
        kMemoryProperty);

    auto create_color_buffer_result =
        fidl_client_->CreateColorBuffer2(std::move(copy_vmo), std::move(create_params));

    ASSERT_TRUE(create_color_buffer_result.ok());
    EXPECT_OK(create_color_buffer_result.value().res);
  }

  // Test GetBufferHandle() method.
  auto get_buffer_handle_result = fidl_client_->GetBufferHandle(std::move(buffer_vmo));
  ASSERT_TRUE(get_buffer_handle_result.ok());
  EXPECT_OK(get_buffer_handle_result.value().res);
  EXPECT_NE(get_buffer_handle_result.value().id, 0u);
  EXPECT_EQ(get_buffer_handle_result.value().type,
            fuchsia_hardware_goldfish::wire::BufferHandleType::kBuffer);

  auto get_color_buffer_handle_result = fidl_client_->GetBufferHandle(std::move(color_buffer_vmo));
  ASSERT_TRUE(get_color_buffer_handle_result.ok());
  EXPECT_OK(get_color_buffer_handle_result.value().res);
  EXPECT_NE(get_color_buffer_handle_result.value().id, 0u);
  EXPECT_NE(get_color_buffer_handle_result.value().id, get_buffer_handle_result.value().id);
  EXPECT_EQ(get_color_buffer_handle_result.value().type,
            fuchsia_hardware_goldfish::wire::BufferHandleType::kColorBuffer);

  // Test GetBufferHandleInfo() method.
  auto get_buffer_handle_info_result = fidl_client_->GetBufferHandleInfo(std::move(buffer_vmo_dup));
  ASSERT_TRUE(get_buffer_handle_info_result.ok());
  ASSERT_TRUE(get_buffer_handle_info_result->is_ok());

  const auto& buffer_handle_info = get_buffer_handle_info_result->value()->info;
  EXPECT_NE(buffer_handle_info.id(), 0u);
  EXPECT_EQ(buffer_handle_info.type(), fuchsia_hardware_goldfish::wire::BufferHandleType::kBuffer);
  EXPECT_EQ(buffer_handle_info.memory_property(),
            fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

  auto get_color_buffer_handle_info_result =
      fidl_client_->GetBufferHandleInfo(std::move(color_buffer_vmo_dup));
  ASSERT_TRUE(get_color_buffer_handle_info_result.ok());
  ASSERT_TRUE(get_color_buffer_handle_info_result->is_ok());

  const auto& color_buffer_handle_info = get_color_buffer_handle_info_result->value()->info;
  EXPECT_NE(color_buffer_handle_info.id(), 0u);
  EXPECT_EQ(color_buffer_handle_info.type(),
            fuchsia_hardware_goldfish::wire::BufferHandleType::kColorBuffer);
  EXPECT_EQ(color_buffer_handle_info.memory_property(),
            fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);
}

TEST_F(ControlDeviceTest, GetBufferHandle_Invalid) {
  // Register data buffer, but don't create it.
  {
    constexpr size_t kSize = 65536u;
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    auto get_buffer_handle_result = fidl_client_->GetBufferHandle(std::move(buffer_vmo));
    ASSERT_TRUE(get_buffer_handle_result.ok());
    EXPECT_EQ(get_buffer_handle_result.value().res, ZX_ERR_NOT_FOUND);

    dut_->FreeBufferHandle(info.koid);
  }

  // Check non-registered buffer VMO.
  {
    constexpr size_t kSize = 65536u;
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    auto get_buffer_handle_result = fidl_client_->GetBufferHandle(std::move(buffer_vmo));
    ASSERT_TRUE(get_buffer_handle_result.ok());
    EXPECT_EQ(get_buffer_handle_result.value().res, ZX_ERR_INVALID_ARGS);
  }

  // Check invalid buffer VMO.
  {
    auto get_buffer_handle_result = fidl_client_->GetBufferHandle(zx::vmo());
    ASSERT_EQ(get_buffer_handle_result.status(), ZX_ERR_INVALID_ARGS);
  }
}

TEST_F(ControlDeviceTest, GetBufferHandleInfo_Invalid) {
  // Register data buffer, but don't create it.
  {
    constexpr size_t kSize = 65536u;
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    zx_info_handle_basic_t info;
    ASSERT_OK(buffer_vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));

    dut_->RegisterBufferHandle(buffer_vmo);

    auto get_buffer_handle_info_result = fidl_client_->GetBufferHandleInfo(std::move(buffer_vmo));
    ASSERT_TRUE(get_buffer_handle_info_result.ok());
    EXPECT_TRUE(get_buffer_handle_info_result->is_error());
    EXPECT_EQ(get_buffer_handle_info_result->error_value(), ZX_ERR_NOT_FOUND);

    dut_->FreeBufferHandle(info.koid);
  }

  // Check non-registered buffer VMO.
  {
    constexpr size_t kSize = 65536u;
    zx::vmo buffer_vmo;
    ASSERT_OK(zx::vmo::create(kSize, 0u, &buffer_vmo));

    auto get_buffer_handle_info_result = fidl_client_->GetBufferHandleInfo(std::move(buffer_vmo));
    ASSERT_TRUE(get_buffer_handle_info_result.ok());
    EXPECT_TRUE(get_buffer_handle_info_result->is_error());
    EXPECT_EQ(get_buffer_handle_info_result->error_value(), ZX_ERR_INVALID_ARGS);
  }

  // Check invalid buffer VMO.
  {
    auto get_buffer_handle_info_result = fidl_client_->GetBufferHandleInfo(zx::vmo());
    ASSERT_EQ(get_buffer_handle_info_result.status(), ZX_ERR_INVALID_ARGS);
  }
}

}  // namespace
}  // namespace goldfish
