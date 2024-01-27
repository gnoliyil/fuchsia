// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/device/device_impl.h"

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/camera3/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/sys/cpp/component_context.h>
#include <zircon/errors.h>

#include <limits>

#include "src/camera/bin/device/stream_impl.h"
#include "src/camera/bin/device/testing/fake_device_listener_registry.h"
#include "src/camera/lib/fake_controller/fake_controller.h"
#include "src/camera/lib/fake_legacy_stream/fake_legacy_stream.h"
#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace camera {

constexpr uint32_t kNamePriority = 30;  // Higher than Scenic but below the maximum.

// No-op function.
static void nop() {}

static void nop_stream_requested(fidl::InterfaceRequest<fuchsia::camera2::Stream> request,
                                 uint32_t format_index) {
  request.Close(ZX_ERR_NOT_SUPPORTED);
}

static void nop_buffers_requested(fuchsia::sysmem::BufferCollectionTokenHandle token,
                                  fit::function<void(uint32_t)> callback) {
  token.Bind()->Close();
  callback(0);
}

// Constraints provided by the driver alone may resolve to zero-sized or empty collections. A set of
// placeholder constrains can be used in cases where a test requires allocation to occur but does
// not care about its properties.
static constexpr fuchsia::sysmem::BufferCollectionConstraints kMinimalConstraintsForAllocation{
    .usage{.cpu = fuchsia::sysmem::cpuUsageRead},
    .min_buffer_count_for_camping = 1,
    .has_buffer_memory_constraints = true,
    .buffer_memory_constraints{.min_size_bytes = 4096}};

class DeviceImplTest : public gtest::RealLoopFixture {
 protected:
  DeviceImplTest()
      : context_(sys::ComponentContext::CreateAndServeOutgoingDirectory()),
        fake_listener_registry_(async_get_default_dispatcher()) {
    fake_properties_.set_image_format(
        {.pixel_format{.type = fuchsia::sysmem::PixelFormatType::NV12},
         .coded_width = 1920,
         .coded_height = 1080,
         .bytes_per_row = 1920,
         .color_space{.type = fuchsia::sysmem::ColorSpaceType::REC601_NTSC}});
    fake_properties_.set_supported_resolutions(
        {{.width = 1920, .height = 1080}, {.width = 1280, .height = 720}});
    fake_properties_.set_frame_rate({});
    fake_properties_.set_supports_crop_region(true);
    fake_legacy_config_.image_formats.resize(2, fake_properties_.image_format());
    fake_legacy_config_.image_formats[1].coded_width = 1280;
    fake_legacy_config_.image_formats[1].coded_height = 720;
  }

  void SetUp() override {
    context_->svc()->Connect(allocator_.NewRequest());
    allocator_.set_error_handler(MakeErrorHandler("Sysmem Allocator"));
    allocator_->SetDebugClientInfo(fsl::GetCurrentProcessName(), fsl::GetCurrentProcessKoid());

    fuchsia::sysmem::AllocatorSyncPtr allocator;
    context_->svc()->Connect(allocator.NewRequest());
    allocator->SetDebugClientInfo(fsl::GetCurrentProcessName(), fsl::GetCurrentProcessKoid());

    fuchsia::camera2::hal::ControllerHandle controller;
    auto controller_result = FakeController::Create(controller.NewRequest(), allocator.Unbind());
    ASSERT_TRUE(controller_result.is_ok());
    controller_ = controller_result.take_value();

    context_->svc()->Connect(allocator.NewRequest());
    allocator->SetDebugClientInfo(fsl::GetCurrentProcessName(), fsl::GetCurrentProcessKoid());

    fuchsia::ui::policy::DeviceListenerRegistryHandle registry;
    fake_listener_registry_.GetHandler()(registry.NewRequest());

    MetricsReporter::Initialize(*context_, false);

    zx::event bad_state_event;
    ASSERT_EQ(zx::event::create(0, &bad_state_event), ZX_OK);
    auto device_promise =
        DeviceImpl::Create(dispatcher(), executor_, std::move(controller), allocator.Unbind(),
                           std::move(registry), std::move(bad_state_event));
    bool device_created = false;
    executor_.schedule_task(device_promise.then(
        [this, &device_created](
            fpromise::result<std::unique_ptr<DeviceImpl>, zx_status_t>& device_result) mutable {
          device_created = true;
          ASSERT_TRUE(device_result.is_ok());
          device_ = device_result.take_value();
        }));
    RunLoopUntil([&device_created] { return device_created; });
    ASSERT_NE(device_, nullptr);
  }

  void TearDown() override {
    device_ = nullptr;
    controller_ = nullptr;
    allocator_ = nullptr;
    RunLoopUntilIdle();
  }

  static fit::function<void(zx_status_t status)> MakeErrorHandler(std::string server) {
    return [server](zx_status_t status) {
      ADD_FAILURE() << server << " server disconnected - " << status;
    };
  }

  template <class T>
  static void SetFailOnError(fidl::InterfacePtr<T>& ptr, std::string name = T::Name_) {
    ptr.set_error_handler([=](zx_status_t status) {
      ADD_FAILURE() << name << " server disconnected: " << zx_status_get_string(status);
    });
  }

  void RunLoopUntilFailureOr(bool& condition) {
    RunLoopUntil([&]() { return HasFailure() || condition; });
  }

  // Synchronizes messages to a device. This method returns when an error occurs or all messages
  // sent to |device| have been received by the server.
  void Sync(fuchsia::camera3::DevicePtr& device) {
    bool identifier_returned = false;
    device->GetIdentifier([&](fidl::StringPtr identifier) { identifier_returned = true; });
    RunLoopUntilFailureOr(identifier_returned);
  }

  // Synchronizes messages to a stream. This method returns when an error occurs or all messages
  // sent to |stream| have been received by the server.
  void Sync(fuchsia::camera3::StreamPtr& stream) {
    fuchsia::camera3::StreamPtr stream2;
    SetFailOnError(stream2, "Rebound Stream for DeviceImplTest::Sync");
    stream->Rebind(stream2.NewRequest());
    bool resolution_returned = false;
    stream2->WatchResolution([&](fuchsia::math::Size resolution) { resolution_returned = true; });
    RunLoopUntilFailureOr(resolution_returned);
  }

  async::Executor executor_{dispatcher()};
  std::unique_ptr<sys::ComponentContext> context_;
  std::unique_ptr<DeviceImpl> device_;
  std::unique_ptr<FakeController> controller_;
  fuchsia::sysmem::AllocatorPtr allocator_;
  fuchsia::camera3::StreamProperties2 fake_properties_;
  fuchsia::camera2::hal::StreamConfig fake_legacy_config_;
  FakeDeviceListenerRegistry fake_listener_registry_;
};

TEST_F(DeviceImplTest, CreateStreamNullConnection) {
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  StreamImpl stream(dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_,
                    fake_legacy_config_, nullptr, nop_stream_requested, nop_buffers_requested, nop);
}

TEST_F(DeviceImplTest, CreateStreamFakeLegacyStream) {
  fidl::InterfaceHandle<fuchsia::camera3::Stream> stream;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  StreamImpl stream_impl(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      stream.NewRequest(),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result = FakeLegacyStream::Create(std::move(request), allocator_);
        ASSERT_TRUE(result.is_ok());
      },
      nop_buffers_requested, nop);
  RunLoopUntilIdle();
}

TEST_F(DeviceImplTest, GetFrames) {
  fuchsia::camera3::StreamPtr stream;
  stream.set_error_handler(MakeErrorHandler("Stream"));
  constexpr uint32_t kBufferId1 = 42;
  constexpr uint32_t kBufferId2 = 17;
  constexpr int64_t kCaptureTimestamp1 = 600;
  constexpr int64_t kCaptureTimestamp2 = 700;
  constexpr int64_t kTimestamp1 = 650;
  constexpr int64_t kTimestamp2 = 745;
  constexpr uint32_t kMaxCampingBuffers = 1;
  std::unique_ptr<FakeLegacyStream> legacy_stream_fake;
  bool legacy_stream_created = false;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  auto stream_impl = std::make_unique<StreamImpl>(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      stream.NewRequest(),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result = FakeLegacyStream::Create(std::move(request), allocator_);
        ASSERT_TRUE(result.is_ok());
        legacy_stream_fake = result.take_value();
        legacy_stream_created = true;
      },
      [&](fuchsia::sysmem::BufferCollectionTokenHandle token,
          fit::function<void(uint32_t)> callback) {
        auto bound_token = token.BindSync();
        bound_token->SetName(1, "DeviceImplTestFakeStream");
        bound_token->Close();
        callback(kMaxCampingBuffers);
      },
      nop);

  fuchsia::sysmem::BufferCollectionTokenPtr token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> received_token;
  bool buffer_collection_returned = false;
  stream->WatchBufferCollection(
      [&](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
        received_token = std::move(token);
        buffer_collection_returned = true;
      });

  RunLoopUntil(
      [&]() { return HasFailure() || (legacy_stream_created && buffer_collection_returned); });
  ASSERT_FALSE(HasFailure());

  fuchsia::sysmem::BufferCollectionPtr collection;
  collection.set_error_handler(MakeErrorHandler("Buffer Collection"));
  allocator_->BindSharedCollection(std::move(received_token), collection.NewRequest());
  constexpr fuchsia::sysmem::BufferCollectionConstraints constraints{
      .usage{.cpu = fuchsia::sysmem::cpuUsageRead},
      .min_buffer_count_for_camping = kMaxCampingBuffers,
      .image_format_constraints_count = 1,
      .image_format_constraints{
          {{.pixel_format{.type = fuchsia::sysmem::PixelFormatType::NV12},
            .color_spaces_count = 1,
            .color_space{{{.type = fuchsia::sysmem::ColorSpaceType::REC601_NTSC}}},
            .min_coded_width = 1,
            .min_coded_height = 1}}}};
  collection->SetConstraints(true, constraints);
  bool buffers_allocated_returned = false;
  collection->WaitForBuffersAllocated(
      [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
        EXPECT_EQ(status, ZX_OK);
        buffers_allocated_returned = true;
      });
  RunLoopUntil([&]() { return HasFailure() || buffers_allocated_returned; });
  ASSERT_FALSE(HasFailure());

  RunLoopUntil([&] { return HasFailure() || legacy_stream_fake->IsStreaming(); });
  bool frame1_received = false;
  bool frame2_received = false;
  auto callback2 = [&](fuchsia::camera3::FrameInfo2 info) {
    ASSERT_EQ(info.buffer_index(), kBufferId2);
    EXPECT_EQ(info.frame_counter(), 2u);
    EXPECT_EQ(info.timestamp(), kTimestamp2);
    EXPECT_EQ(info.capture_timestamp(), kCaptureTimestamp2);
    frame2_received = true;
  };
  auto callback1 = [&](fuchsia::camera3::FrameInfo2 info) {
    ASSERT_EQ(info.buffer_index(), kBufferId1);
    EXPECT_EQ(info.frame_counter(), 1u);
    EXPECT_EQ(info.timestamp(), kTimestamp1);
    EXPECT_EQ(info.capture_timestamp(), kCaptureTimestamp1);
    frame1_received = true;
    info.mutable_release_fence()->reset();
    fuchsia::camera2::FrameAvailableInfo frame2_info;
    frame2_info.frame_status = fuchsia::camera2::FrameStatus::OK;
    frame2_info.buffer_id = kBufferId2;
    frame2_info.metadata.set_timestamp(kTimestamp2);
    frame2_info.metadata.set_capture_timestamp(kCaptureTimestamp2);
    ASSERT_EQ(legacy_stream_fake->SendFrameAvailable(std::move(frame2_info)), ZX_OK);
    stream->GetNextFrame2(std::move(callback2));
  };
  stream->GetNextFrame2(std::move(callback1));
  fuchsia::camera2::FrameAvailableInfo frame1_info;
  frame1_info.frame_status = fuchsia::camera2::FrameStatus::OK;
  frame1_info.buffer_id = kBufferId1;
  frame1_info.metadata.set_timestamp(kTimestamp1);
  frame1_info.metadata.set_capture_timestamp(kCaptureTimestamp1);
  ASSERT_EQ(legacy_stream_fake->SendFrameAvailable(std::move(frame1_info)), ZX_OK);
  while (!HasFailure() && (!frame1_received || !frame2_received)) {
    RunLoopUntilIdle();
  }

  // Make sure the stream recycles frames once its camping allocation is exhausted.
  // Also emulate a stuck client that does not return any frames.
  std::set<zx::eventpair> fences;
  uint32_t last_received_frame = -1;
  fit::function<void(fuchsia::camera3::FrameInfo)> on_next_frame;
  on_next_frame = [&](fuchsia::camera3::FrameInfo info) {
    last_received_frame = info.buffer_index;
    fences.insert(std::move(info.release_fence));
    stream->GetNextFrame(on_next_frame.share());
  };
  stream->GetNextFrame(on_next_frame.share());
  constexpr uint32_t kNumFrames = 17;
  for (uint32_t i = 0; i < kNumFrames; ++i) {
    fuchsia::camera2::FrameAvailableInfo frame_info{.buffer_id = i};
    frame_info.metadata.set_timestamp(0);
    frame_info.metadata.set_capture_timestamp(0);
    ASSERT_EQ(legacy_stream_fake->SendFrameAvailable(std::move(frame_info)), ZX_OK);
    if (i < constraints.min_buffer_count_for_camping) {
      // Up to the camping limit, wait until the frames are received.
      RunLoopUntil([&] { return HasFailure() || last_received_frame == i; });
    } else {
      // After the camping limit is reached due to the emulated stuck client, verify that the Stream
      // recycles the oldest buffers first.
      RunLoopUntil([&] { return HasFailure() || !legacy_stream_fake->IsOutstanding(i); });
    }
  }
  fences.clear();

  auto client_result = legacy_stream_fake->StreamClientStatus();
  EXPECT_TRUE(client_result.is_ok()) << client_result.error();
  stream = nullptr;
  stream_impl = nullptr;
}

TEST_F(DeviceImplTest, GetFramesInvalidCall) {
  bool stream_errored = false;
  fuchsia::camera3::StreamPtr stream;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_BAD_STATE);
    stream_errored = true;
  });
  std::unique_ptr<FakeLegacyStream> fake_legacy_stream;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  auto stream_impl = std::make_unique<StreamImpl>(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      stream.NewRequest(),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result = FakeLegacyStream::Create(std::move(request), allocator_);
        ASSERT_TRUE(result.is_ok());
        fake_legacy_stream = result.take_value();
      },
      nop_buffers_requested, nop);
  stream->GetNextFrame([](fuchsia::camera3::FrameInfo info) {});
  stream->GetNextFrame([](fuchsia::camera3::FrameInfo info) {});
  while (!HasFailure() && !stream_errored) {
    RunLoopUntilIdle();
  }
}

TEST_F(DeviceImplTest, Configurations) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());

  uint32_t callback_count = 0;
  constexpr uint32_t kExpectedCallbackCount = 3;
  bool all_callbacks_received = false;
  device->GetConfigurations([&](std::vector<fuchsia::camera3::Configuration> configurations) {
    EXPECT_GE(configurations.size(), 2u);
    all_callbacks_received = ++callback_count == kExpectedCallbackCount;
  });
  device->SetCurrentConfiguration(0);
  RunLoopUntilIdle();
  device->WatchCurrentConfiguration([&](uint32_t index) {
    EXPECT_EQ(index, 0u);
    all_callbacks_received = ++callback_count == kExpectedCallbackCount;
    device->WatchCurrentConfiguration([&](uint32_t index) {
      EXPECT_EQ(index, 1u);
      all_callbacks_received = ++callback_count == kExpectedCallbackCount;
    });
    RunLoopUntilIdle();
    device->SetCurrentConfiguration(1);
  });
  RunLoopUntilFailureOr(all_callbacks_received);
}

TEST_F(DeviceImplTest, ConfigurationSwitchingWhileAllocated) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());

  uint32_t buffers_allocated = 0;
  constexpr uint32_t kExpectedBuffersAllocated = 2;
  bool all_buffers_allocated = false;
  bool first_stream_gone = false;
  device->GetConfigurations([&](std::vector<fuchsia::camera3::Configuration> configurations) {
    EXPECT_GE(configurations.size(), 2u);
  });
  device->SetCurrentConfiguration(0);

  // Connect and allocate stream
  fuchsia::camera3::StreamPtr stream;
  // Don't SetFailOnError as we expect stream to close when switching config
  device->ConnectToStream(0, stream.NewRequest());
  fuchsia::sysmem::BufferCollectionTokenPtr token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  fuchsia::sysmem::BufferCollectionPtr buffers;
  bool buffers_allocated_returned = false;
  zx::vmo vmo;
  stream->WatchBufferCollection(
      [&](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
        allocator_->BindSharedCollection(std::move(token), buffers.NewRequest());
        buffers->SetConstraints(true, kMinimalConstraintsForAllocation);
        buffers->SetName(kNamePriority, "Testc0s0Buffers");
        buffers->WaitForBuffersAllocated(
            [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
              EXPECT_EQ(status, ZX_OK);
              vmo = std::move(buffers.buffers[0].vmo);
              all_buffers_allocated = ++buffers_allocated == kExpectedBuffersAllocated;
              buffers_allocated_returned = true;
            });
      });

  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_OK);
    first_stream_gone = true;
    buffers.Unbind();
    vmo.reset();
  });

  RunLoopUntilFailureOr(buffers_allocated_returned);

  fuchsia::camera3::StreamPtr stream2;
  fuchsia::sysmem::BufferCollectionPtr buffers2;
  fuchsia::sysmem::BufferCollectionTokenPtr token2;
  zx::vmo vmo2;

  device->WatchCurrentConfiguration([&](uint32_t index) {
    EXPECT_EQ(index, 0u);

    device->SetCurrentConfiguration(1);
    device->WatchCurrentConfiguration([&](uint32_t index) { EXPECT_EQ(index, 1u); });

    RunLoopUntilFailureOr(first_stream_gone);

    // Connect and allocate stream
    device->ConnectToStream(0, stream2.NewRequest());
    allocator_->AllocateSharedCollection(token2.NewRequest());
    stream2->SetBufferCollection(std::move(token2));
    stream2->WatchBufferCollection(
        [&](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
          allocator_->BindSharedCollection(std::move(token), buffers2.NewRequest());
          buffers2->SetConstraints(true, kMinimalConstraintsForAllocation);
          buffers2->SetName(kNamePriority, "Testc1s0Buffers");
          buffers2->WaitForBuffersAllocated(
              [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
                EXPECT_EQ(status, ZX_OK);
                vmo2 = std::move(buffers.buffers[0].vmo);
                all_buffers_allocated = ++buffers_allocated == kExpectedBuffersAllocated;
              });
        });
  });

  RunLoopUntilFailureOr(all_buffers_allocated);
}

TEST_F(DeviceImplTest, Identifier) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  bool callback_received = false;
  device->GetIdentifier([&](fidl::StringPtr identifier) {
    ASSERT_TRUE(identifier.has_value());
    constexpr auto kExpectedDeviceIdentifier = "FFFF0ABC";
    EXPECT_EQ(identifier.value(), kExpectedDeviceIdentifier);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
}

TEST_F(DeviceImplTest, RequestStreamFromController) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());
  fuchsia::sysmem::BufferCollectionTokenPtr token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  fuchsia::sysmem::BufferCollectionPtr buffers;
  SetFailOnError(buffers, "BufferCollection");
  bool buffers_allocated_returned = false;
  zx::vmo vmo;
  stream->WatchBufferCollection(
      [&](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
        allocator_->BindSharedCollection(std::move(token), buffers.NewRequest());
        buffers->SetConstraints(true, kMinimalConstraintsForAllocation);
        buffers->WaitForBuffersAllocated(
            [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
              EXPECT_EQ(status, ZX_OK);
              vmo = std::move(buffers.buffers[0].vmo);
              buffers_allocated_returned = true;
            });
      });
  RunLoopUntilFailureOr(buffers_allocated_returned);

  std::string vmo_name;
  RunLoopUntil([&] {
    vmo_name = fsl::GetObjectName(vmo.get());
    return vmo_name != "Sysmem-core";
  });
  EXPECT_EQ(vmo_name, "camera_c0s0:0");

  constexpr uint32_t kBufferId = 42;
  bool callback_received = false;
  stream->GetNextFrame([&](fuchsia::camera3::FrameInfo info) {
    EXPECT_EQ(info.buffer_index, kBufferId);
    callback_received = true;
  });
  bool frame_sent = false;
  while (!HasFailure() && !frame_sent) {
    RunLoopUntilIdle();
    fuchsia::camera2::FrameAvailableInfo info;
    info.frame_status = fuchsia::camera2::FrameStatus::OK;
    info.buffer_id = kBufferId;
    info.metadata.set_timestamp(0);
    info.metadata.set_capture_timestamp(0);
    zx_status_t status = controller_->SendFrameViaLegacyStream(std::move(info));
    if (status == ZX_OK) {
      frame_sent = true;
    } else {
      EXPECT_EQ(status, ZX_ERR_SHOULD_WAIT);
    }
  }
  RunLoopUntilFailureOr(callback_received);
  buffers->Close();
  RunLoopUntilIdle();
}

TEST_F(DeviceImplTest, MultipleDeviceClients) {
  // Create the first client.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  Sync(device);

  // Try to connect a second client, which should succeed.
  fuchsia::camera3::DevicePtr device2;
  SetFailOnError(device2, "Device");
  device_->GetHandler()(device2.NewRequest());
  Sync(device2);

  // Make sure new clients can get configurations and see the current configuration.
  bool get_configurations_returned = false;
  device2->GetConfigurations([&](std::vector<fuchsia::camera3::Configuration> configurations) {
    ASSERT_GE(configurations.size(), 1u);
    get_configurations_returned = true;
  });
  RunLoopUntilFailureOr(get_configurations_returned);
  bool watch_returned = false;
  device2->WatchCurrentConfiguration([&](uint32_t index) {
    EXPECT_EQ(index, 0u);
    watch_returned = true;
  });
  RunLoopUntilFailureOr(watch_returned);
}

TEST_F(DeviceImplTest, StreamClientDisconnect) {
  // Create the first client.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  device->ConnectToStream(0, stream.NewRequest());
  SetFailOnError(stream, "Stream");

  // Try to connect a second client, which should fail.
  fuchsia::camera3::StreamPtr stream2;
  bool error_received = false;
  stream2.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_ALREADY_BOUND);
    error_received = true;
  });
  device->ConnectToStream(0, stream2.NewRequest());
  RunLoopUntilFailureOr(error_received);

  // Disconnect the first client, then try to connect the second again.
  stream = nullptr;
  bool callback_received = false;
  while (!HasFailure() && !callback_received) {
    error_received = false;
    device->ConnectToStream(0, stream2.NewRequest());
    fuchsia::sysmem::BufferCollectionTokenPtr token;
    SetFailOnError(token, "Token");
    allocator_->AllocateSharedCollection(token.NewRequest());
    stream2->SetBufferCollection(std::move(token));
    // Call a returning API to verify the connection status.
    stream2->WatchBufferCollection([&](fuchsia::sysmem::BufferCollectionTokenHandle token) {
      EXPECT_EQ(token.BindSync()->Close(), ZX_OK);
      callback_received = true;
    });
    RunLoopUntil([&] { return error_received || callback_received; });
  }
}

TEST_F(DeviceImplTest, SetResolution) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  device->ConnectToStream(0, stream.NewRequest());
  SetFailOnError(stream, "Stream");
  constexpr fuchsia::math::Size kExpectedDefaultSize{.width = 1920, .height = 1080};
  constexpr fuchsia::math::Size kRequestedSize{.width = 1025, .height = 32};
  constexpr fuchsia::math::Size kExpectedSize{.width = 1280, .height = 720};
  constexpr fuchsia::math::Size kRequestedSize2{.width = 1, .height = 1};
  constexpr fuchsia::math::Size kExpectedSize2{.width = 1024, .height = 576};
  constexpr fuchsia::math::Size kRequestedSize3{.width = 1280, .height = 720};
  constexpr fuchsia::math::Size kExpectedSize3{.width = 1280, .height = 720};
  bool callback_received = false;
  stream->WatchResolution([&](fuchsia::math::Size coded_size) {
    EXPECT_GE(coded_size.width, kExpectedDefaultSize.width);
    EXPECT_GE(coded_size.height, kExpectedDefaultSize.height);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
  stream->SetResolution(kRequestedSize);
  callback_received = false;
  stream->WatchResolution([&](fuchsia::math::Size coded_size) {
    EXPECT_GE(coded_size.width, kExpectedSize.width);
    EXPECT_GE(coded_size.height, kExpectedSize.height);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
  callback_received = false;
  stream->SetResolution(kRequestedSize2);
  stream->WatchResolution([&](fuchsia::math::Size coded_size) {
    EXPECT_GE(coded_size.width, kExpectedSize2.width);
    EXPECT_GE(coded_size.height, kExpectedSize2.height);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
  callback_received = false;
  stream->SetResolution(kRequestedSize3);
  stream->WatchResolution([&](fuchsia::math::Size coded_size) {
    EXPECT_GE(coded_size.width, kExpectedSize3.width);
    EXPECT_GE(coded_size.height, kExpectedSize3.height);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
}

TEST_F(DeviceImplTest, SetResolutionInvalid) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  device->ConnectToStream(0, stream.NewRequest());
  constexpr fuchsia::math::Size kSize{.width = std::numeric_limits<int32_t>::max(), .height = 42};
  stream->SetResolution(kSize);
  bool error_received = false;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
    error_received = true;
  });
  RunLoopUntilFailureOr(error_received);
}

TEST_F(DeviceImplTest, SetConfigurationDisconnectsStreams) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  bool error_received = false;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_OK);
    error_received = true;
  });
  device->ConnectToStream(0, stream.NewRequest());
  Sync(stream);
  device->SetCurrentConfiguration(0);
  RunLoopUntilFailureOr(error_received);
}

TEST_F(DeviceImplTest, Rebind) {
  // First device connection.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());

  // First stream connection.
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());
  Sync(stream);

  // Rebind second device connection.
  fuchsia::camera3::DevicePtr device2;
  SetFailOnError(device2, "Device");
  device->Rebind(device2.NewRequest());

  // Attempt to bind second stream independently.
  fuchsia::camera3::StreamPtr stream2;
  bool error_received = false;
  stream2.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_ALREADY_BOUND);
    error_received = true;
  });
  device->ConnectToStream(0, stream2.NewRequest());
  RunLoopUntilFailureOr(error_received);

  // Attempt to bind second stream via rebind.
  SetFailOnError(stream2, "Stream");
  stream->Rebind(stream2.NewRequest());
  Sync(stream2);
}

TEST_F(DeviceImplTest, OrphanStream) {
  // Connect to the device.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  Sync(device);

  // Connect to the stream.
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());
  Sync(stream);

  // Disconnect from the device.
  device = nullptr;

  // Reset the error handler to expect peer-closed.
  bool stream_error_received = false;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_OK);
    stream_error_received = true;
  });

  // Connect to the device as a new client and set the configuration.
  fuchsia::camera3::DevicePtr device2;
  SetFailOnError(device2, "Device2");
  device_->GetHandler()(device2.NewRequest());
  device2->SetCurrentConfiguration(0);

  // Make sure the first stream is closed when the new device connects.
  RunLoopUntilFailureOr(stream_error_received);

  // The second client should be able to connect to the stream now.
  fuchsia::camera3::StreamPtr stream2;
  SetFailOnError(stream, "Stream2");
  device2->ConnectToStream(0, stream2.NewRequest());
  Sync(stream2);
}

TEST_F(DeviceImplTest, SetCropRegion) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  device->ConnectToStream(0, stream.NewRequest());
  SetFailOnError(stream, "Stream");
  bool callback_received = false;
  stream->WatchCropRegion([&](std::unique_ptr<fuchsia::math::RectF> region) {
    EXPECT_EQ(region, nullptr);
    callback_received = true;
  });
  RunLoopUntilFailureOr(callback_received);
  constexpr fuchsia::math::RectF kCropRegion{.x = 0.1f, .y = 0.4f, .width = 0.7f, .height = 0.2f};
  callback_received = false;
  stream->WatchCropRegion([&](std::unique_ptr<fuchsia::math::RectF> region) {
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->x, kCropRegion.x);
    EXPECT_EQ(region->y, kCropRegion.y);
    EXPECT_EQ(region->width, kCropRegion.width);
    EXPECT_EQ(region->height, kCropRegion.height);
    callback_received = true;
  });
  stream->SetCropRegion(std::make_unique<fuchsia::math::RectF>(kCropRegion));
  RunLoopUntilFailureOr(callback_received);
  bool error_received = false;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
    error_received = true;
  });
  constexpr fuchsia::math::RectF kInvalidCropRegion{
      .x = 0.1f, .y = 0.4f, .width = 0.7f, .height = 0.7f};
  stream->SetCropRegion(std::make_unique<fuchsia::math::RectF>(kInvalidCropRegion));
  RunLoopUntilFailureOr(error_received);
}

TEST_F(DeviceImplTest, SoftwareMuteState) {
  // Connect to the device.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  bool watch_returned = false;
  device->WatchMuteState([&](bool software_muted, bool hardware_muted) {
    EXPECT_FALSE(software_muted);
    EXPECT_FALSE(hardware_muted);
    watch_returned = true;
  });
  RunLoopUntilFailureOr(watch_returned);

  // Connect to the stream.
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());

  fuchsia::sysmem::BufferCollectionTokenPtr token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> received_token;
  watch_returned = false;
  stream->WatchBufferCollection(
      [&](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
        received_token = std::move(token);
        watch_returned = true;
      });
  RunLoopUntilFailureOr(watch_returned);

  fuchsia::sysmem::BufferCollectionPtr collection;
  collection.set_error_handler(MakeErrorHandler("Buffer Collection"));
  allocator_->BindSharedCollection(std::move(received_token), collection.NewRequest());
  collection->SetConstraints(
      true, {.usage{.cpu = fuchsia::sysmem::cpuUsageRead},
             .min_buffer_count_for_camping = 5,
             .image_format_constraints_count = 1,
             .image_format_constraints{
                 {{.pixel_format{.type = fuchsia::sysmem::PixelFormatType::NV12},
                   .color_spaces_count = 1,
                   .color_space{{{.type = fuchsia::sysmem::ColorSpaceType::REC601_NTSC}}},
                   .min_coded_width = 1,
                   .min_coded_height = 1}}}});
  bool buffers_allocated_returned = false;
  collection->WaitForBuffersAllocated(
      [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
        EXPECT_EQ(status, ZX_OK);
        buffers_allocated_returned = true;
      });
  RunLoopUntil([&]() { return HasFailure() || buffers_allocated_returned; });
  ASSERT_FALSE(HasFailure());

  uint32_t next_buffer_id = 0;
  fit::closure send_frame = [&] {
    fuchsia::camera2::FrameAvailableInfo frame_info{
        .frame_status = fuchsia::camera2::FrameStatus::OK, .buffer_id = next_buffer_id};
    frame_info.metadata.set_timestamp(0);
    frame_info.metadata.set_capture_timestamp(0);
    zx_status_t status = controller_->SendFrameViaLegacyStream(std::move(frame_info));
    if (status == ZX_ERR_SHOULD_WAIT || status == ZX_ERR_BAD_STATE) {
      // Keep trying until the device starts streaming.
      async::PostTask(async_get_default_dispatcher(), send_frame.share());
    } else {
      ++next_buffer_id;
      ASSERT_EQ(status, ZX_OK);
    }
  };

  // Because the device and stream protocols are asynchronous, mute requests may be handled by
  // streams while in a number of different states. Without deep hooks into the implementation, it
  // is impossible to force the stream into a particular state. Instead, this test repeatedly
  // toggles mute state in an attempt to exercise all cases.
  constexpr uint32_t kToggleCount = 50;
  for (uint32_t i = 0; i < kToggleCount; ++i) {
    // Get a frame (unmuted).
    bool frame_received = false;
    stream->GetNextFrame([&](fuchsia::camera3::FrameInfo info) { frame_received = true; });
    send_frame();
    RunLoopUntilFailureOr(frame_received);

    // Get a frame then immediately try to mute the device.
    bool mute_completed = false;
    bool muted_frame_requested = false;
    bool unmute_requested = false;
    bool unmuted_frame_received = false;
    fuchsia::camera3::Stream::GetNextFrameCallback callback =
        [&](fuchsia::camera3::FrameInfo info) {
          if (muted_frame_requested) {
            ASSERT_TRUE(unmute_requested)
                << "Frame requested after receiving mute callback returned anyway.";
          }
          if (unmute_requested) {
            unmuted_frame_received = true;
          } else {
            if (mute_completed) {
              muted_frame_requested = true;
            }
            stream->GetNextFrame(callback.share());
            send_frame();
          }
        };
    callback({});
    uint32_t mute_buffer_id_begin = next_buffer_id;
    uint32_t mute_buffer_id_end = mute_buffer_id_begin;
    device->SetSoftwareMuteState(true, [&] {
      mute_completed = true;
      mute_buffer_id_end = next_buffer_id;
    });
    RunLoopUntilFailureOr(mute_completed);

    // Make sure all buffers were returned.
    for (uint32_t j = mute_buffer_id_begin; j < mute_buffer_id_end; ++j) {
      RunLoopUntil(
          [&] { return HasFailure() || !controller_->LegacyStreamBufferIsOutstanding(j); });
    }

    // Unmute the device to get the last frame. Note that frames received while internally muted are
    // discarded, so repeated sending of frames is necessary.
    unmute_requested = true;
    device->SetSoftwareMuteState(false, [] {});
    while (!HasFailure() && !unmuted_frame_received) {
      send_frame();
      // Delay each attempt to avoid flooding the channel.
      RunLoopWithTimeout(zx::msec(10));
    }

    ASSERT_FALSE(HasFailure());
  }

  collection->Close();
}

TEST_F(DeviceImplTest, HardwareMuteState) {
  // Connect to the device.
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());

  // Device should start unmuted.
  bool watch_returned = false;
  device->WatchMuteState([&](bool software_muted, bool hardware_muted) {
    EXPECT_FALSE(software_muted);
    EXPECT_FALSE(hardware_muted);
    watch_returned = true;
  });
  RunLoopUntilFailureOr(watch_returned);

  // Verify mute event.
  watch_returned = false;
  device->WatchMuteState([&](bool software_muted, bool hardware_muted) {
    EXPECT_FALSE(software_muted);
    EXPECT_TRUE(hardware_muted);
    watch_returned = true;
  });
  fuchsia::ui::input::MediaButtonsEvent mute_event;
  mute_event.set_mic_mute(true);
  fake_listener_registry_.SendMediaButtonsEvent(std::move(mute_event));
  RunLoopUntilFailureOr(watch_returned);

  // Verify unmute event.
  watch_returned = false;
  device->WatchMuteState([&](bool software_muted, bool hardware_muted) {
    EXPECT_FALSE(software_muted);
    EXPECT_FALSE(hardware_muted);
    watch_returned = true;
  });
  fuchsia::ui::input::MediaButtonsEvent unmute_event;
  unmute_event.set_mic_mute(false);
  fake_listener_registry_.SendMediaButtonsEvent(std::move(unmute_event));
  RunLoopUntilFailureOr(watch_returned);
}

TEST_F(DeviceImplTest, GetProperties) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  bool configs_returned = false;
  std::vector<fuchsia::camera3::Configuration> configs;
  device->GetConfigurations([&](std::vector<fuchsia::camera3::Configuration> configurations) {
    configs = std::move(configurations);
    configs_returned = true;
  });
  RunLoopUntilFailureOr(configs_returned);
  fuchsia::camera3::StreamPtr stream;
  device->ConnectToStream(0, stream.NewRequest());
  bool properties_returned = false;
  stream->GetProperties([&](fuchsia::camera3::StreamProperties properties) {
    EXPECT_EQ(properties.supports_crop_region, configs[0].streams[0].supports_crop_region);
    EXPECT_EQ(properties.frame_rate.numerator, configs[0].streams[0].frame_rate.numerator);
    EXPECT_EQ(properties.frame_rate.denominator, configs[0].streams[0].frame_rate.denominator);
    EXPECT_EQ(properties.image_format.coded_width, configs[0].streams[0].image_format.coded_width);
    EXPECT_EQ(properties.image_format.coded_height,
              configs[0].streams[0].image_format.coded_height);
    properties_returned = true;
  });
  RunLoopUntilFailureOr(properties_returned);
  properties_returned = false;
  stream->GetProperties2([&](fuchsia::camera3::StreamProperties2 properties) {
    ASSERT_FALSE(properties.supported_resolutions().empty());
    EXPECT_EQ(static_cast<uint32_t>(properties.supported_resolutions().at(0).width),
              configs[0].streams[0].image_format.coded_width);
    EXPECT_EQ(static_cast<uint32_t>(properties.supported_resolutions().at(0).height),
              configs[0].streams[0].image_format.coded_height);
    properties_returned = true;
  });
  RunLoopUntilFailureOr(properties_returned);
}

TEST_F(DeviceImplTest, DISABLED_SetBufferCollectionAgainWhileFramesHeld) {
  constexpr uint32_t kCycleCount = 10;
  uint32_t cycle = 0;

  fuchsia::camera3::StreamPtr stream;
  constexpr uint32_t kMaxCampingBuffers = 1;
  std::array<std::unique_ptr<FakeLegacyStream>, kCycleCount> legacy_stream_fakes;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  auto stream_impl = std::make_unique<StreamImpl>(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      stream.NewRequest(),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result = FakeLegacyStream::Create(std::move(request), allocator_);
        ASSERT_TRUE(result.is_ok());
        legacy_stream_fakes[cycle] = result.take_value();
      },
      [&](fuchsia::sysmem::BufferCollectionTokenHandle token,
          fit::function<void(uint32_t)> callback) {
        token.Bind()->Close();
        callback(kMaxCampingBuffers);
      },
      nop);

  std::vector<fuchsia::camera3::FrameInfo> frames(kCycleCount);
  for (cycle = 0; cycle < kCycleCount; ++cycle) {
    fuchsia::sysmem::BufferCollectionTokenPtr token;
    allocator_->AllocateSharedCollection(token.NewRequest());
    stream->SetBufferCollection(std::move(token));
    bool frame_received = false;
    stream->WatchBufferCollection([&](fuchsia::sysmem::BufferCollectionTokenHandle token) {
      fuchsia::sysmem::BufferCollectionSyncPtr collection;
      allocator_->BindSharedCollection(std::move(token), collection.NewRequest());
      constexpr fuchsia::sysmem::BufferCollectionConstraints constraints{
          .usage{.cpu = fuchsia::sysmem::cpuUsageRead},
          .min_buffer_count_for_camping = kMaxCampingBuffers,
          .image_format_constraints_count = 1,
          .image_format_constraints{
              {{.pixel_format{.type = fuchsia::sysmem::PixelFormatType::NV12},
                .color_spaces_count = 1,
                .color_space{{{.type = fuchsia::sysmem::ColorSpaceType::REC601_NTSC}}},
                .min_coded_width = 1,
                .min_coded_height = 1}}}};
      collection->SetConstraints(true, constraints);
      zx_status_t status = ZX_OK;
      fuchsia::sysmem::BufferCollectionInfo_2 buffers;
      collection->WaitForBuffersAllocated(&status, &buffers);
      EXPECT_EQ(status, ZX_OK);
      collection->Close();
      stream->GetNextFrame([&](fuchsia::camera3::FrameInfo info) {
        // Keep the frame; do not release it.
        frames[cycle] = std::move(info);
        frame_received = true;
      });
      fuchsia::camera2::FrameAvailableInfo frame_info;
      frame_info.frame_status = fuchsia::camera2::FrameStatus::OK;
      frame_info.buffer_id = cycle;
      frame_info.metadata.set_timestamp(0);
      frame_info.metadata.set_capture_timestamp(0);
      while (!HasFailure() && !legacy_stream_fakes[cycle]->IsStreaming()) {
        RunLoopUntilIdle();
      }
      ASSERT_EQ(legacy_stream_fakes[cycle]->SendFrameAvailable(std::move(frame_info)), ZX_OK);
    });
    RunLoopUntilFailureOr(frame_received);
  }
}

TEST_F(DeviceImplTest, FrameWaiterTest) {
  {  // Test that destructor of a non-triggered waiter does not panic.
    zx::eventpair client;
    std::vector<zx::eventpair> server(1);
    ASSERT_EQ(zx::eventpair::create(0, &client, &server[0]), ZX_OK);
    bool signaled = false;
    {
      FrameWaiter waiter(dispatcher(), std::move(server), [&] { signaled = true; });
      RunLoopUntilIdle();
    }
    RunLoopUntilIdle();
    EXPECT_FALSE(signaled);
  }

  {  // Test that closing the client endpoint triggers the wait.
    zx::eventpair client;
    std::vector<zx::eventpair> server(1);
    ASSERT_EQ(zx::eventpair::create(0, &client, &server[0]), ZX_OK);
    bool signaled = false;
    FrameWaiter waiter(dispatcher(), std::move(server), [&] { signaled = true; });
    client.reset();
    RunLoopUntilFailureOr(signaled);
  }

  {  // Test that only closing all client endpoints triggers the wait.
    constexpr uint32_t kNumFences = 3;
    std::vector<zx::eventpair> client(kNumFences);
    std::vector<zx::eventpair> server(kNumFences);
    for (uint32_t i = 0; i < kNumFences; ++i) {
      ASSERT_EQ(zx::eventpair::create(0, &client[i], &server[i]), ZX_OK);
    }
    bool signaled = false;
    FrameWaiter waiter(dispatcher(), std::move(server), [&] { signaled = true; });

    // Release some out of order first.
    client[0].reset();
    RunLoopUntilIdle();
    EXPECT_FALSE(signaled);
    client[2].reset();
    RunLoopUntilIdle();
    EXPECT_FALSE(signaled);

    // Release all remaining fences, which should trigger the waiter.
    client.clear();
    RunLoopUntilFailureOr(signaled);
  }
}

TEST_F(DeviceImplTest, NullToken) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  bool complete = false;
  stream.set_error_handler([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_BAD_STATE);
    ADD_FAILURE() << "Stream shouldn't disconnect";
  });
  device->ConnectToStream(0, stream.NewRequest());
  // Pass invalid handle
  fuchsia::sysmem::BufferCollectionTokenPtr token;
  stream->SetBufferCollection(std::move(token));
  stream->WatchBufferCollection([&](fuchsia::sysmem::BufferCollectionTokenHandle token) {
    ADD_FAILURE() << "Watch should not return when given an invalid token.";
  });
  auto kDelay = zx::msec(250);
  async::PostDelayedTask(
      dispatcher(), [&] { complete = true; }, kDelay);
  RunLoopUntilFailureOr(complete);
}

TEST_F(DeviceImplTest, GoodToken) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());
  fuchsia::sysmem::BufferCollectionTokenHandle token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  bool watch_returned = false;
  stream->WatchBufferCollection([&](fuchsia::sysmem::BufferCollectionTokenHandle token) {
    token.BindSync()->Close();
    watch_returned = true;
  });
  RunLoopUntilFailureOr(watch_returned);
}

TEST_F(DeviceImplTest, GetFramesMultiClient) {
  constexpr uint32_t kNumClients = 2;
  constexpr uint32_t kBufferId1 = 42;
  constexpr uint32_t kBufferId2 = 17;
  static constexpr uint32_t kMaxCampingBuffers = 1;
  fuchsia::camera3::StreamPtr original_stream;
  original_stream.set_error_handler(MakeErrorHandler("Stream"));
  std::unique_ptr<FakeLegacyStream> legacy_stream_fake;
  bool legacy_stream_created = false;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  auto stream_impl = std::make_unique<StreamImpl>(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      original_stream.NewRequest(),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result = FakeLegacyStream::Create(std::move(request), allocator_);
        ASSERT_TRUE(result.is_ok());
        legacy_stream_fake = result.take_value();
        legacy_stream_created = true;
      },
      [&](fuchsia::sysmem::BufferCollectionTokenHandle token,
          fit::function<void(uint32_t)> callback) {
        auto bound_token = token.BindSync();
        bound_token->SetName(1, "DeviceImplTestFakeStream");
        bound_token->Close();
        callback(kMaxCampingBuffers * kNumClients);
      },
      nop);
  struct PerClient {
    explicit PerClient(fuchsia::sysmem::AllocatorPtr& allocator) : allocator_(allocator) {}
    fuchsia::camera3::StreamPtr stream;
    fuchsia::sysmem::BufferCollectionTokenPtr initial_token;
    fuchsia::sysmem::BufferCollectionPtr collection;
    void OnToken(fuchsia::sysmem::BufferCollectionTokenHandle token) {
      allocator_->BindSharedCollection(std::move(token), collection.NewRequest());
      constexpr fuchsia::sysmem::BufferCollectionConstraints constraints{
          .usage{.cpu = fuchsia::sysmem::cpuUsageRead},
          .min_buffer_count_for_camping = kMaxCampingBuffers,
          .image_format_constraints_count = 1,
          .image_format_constraints{
              {{.pixel_format{.type = fuchsia::sysmem::PixelFormatType::NV12},
                .color_spaces_count = 1,
                .color_space{{{.type = fuchsia::sysmem::ColorSpaceType::REC601_NTSC}}},
                .min_coded_width = 1,
                .min_coded_height = 1}}}};
      collection->SetConstraints(true, constraints);
      collection->WaitForBuffersAllocated(
          [&](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) {
            EXPECT_EQ(status, ZX_OK);
          });
      stream->WatchBufferCollection(fit::bind_member(this, &PerClient::OnToken));
    }
    fuchsia::sysmem::AllocatorPtr& allocator_;
  };
  std::array<PerClient, kNumClients> clients{PerClient(allocator_), PerClient(allocator_)};
  for (auto& client : clients) {
    client.stream.set_error_handler(MakeErrorHandler("Stream"));
    original_stream->Rebind(client.stream.NewRequest());
    allocator_->AllocateSharedCollection(client.initial_token.NewRequest());
    client.stream->SetBufferCollection(std::move(client.initial_token));
    client.stream->WatchBufferCollection(fit::bind_member(&client, &PerClient::OnToken));
  }
  original_stream = nullptr;

  RunLoopUntil([&]() {
    return HasFailure() || (legacy_stream_created && legacy_stream_fake->IsStreaming());
  });
  ASSERT_FALSE(HasFailure());

  // Send two frames from the driver.
  fuchsia::camera2::FrameAvailableInfo frame1_info;
  frame1_info.frame_status = fuchsia::camera2::FrameStatus::OK;
  frame1_info.buffer_id = kBufferId1;
  frame1_info.metadata.set_timestamp(0);
  frame1_info.metadata.set_capture_timestamp(0);
  ASSERT_EQ(legacy_stream_fake->SendFrameAvailable(std::move(frame1_info)), ZX_OK);
  fuchsia::camera2::FrameAvailableInfo frame2_info;
  frame2_info.frame_status = fuchsia::camera2::FrameStatus::OK;
  frame2_info.buffer_id = kBufferId2;
  frame2_info.metadata.set_timestamp(0);
  frame2_info.metadata.set_capture_timestamp(0);
  ASSERT_EQ(legacy_stream_fake->SendFrameAvailable(std::move(frame2_info)), ZX_OK);

  // Try to receive each frame independently via each client.
  for (auto& client : clients) {
    bool frame1_received = false;
    bool frame2_received = false;
    auto callback2 = [&](fuchsia::camera3::FrameInfo info) {
      ASSERT_EQ(info.buffer_index, kBufferId2);
      frame2_received = true;
    };
    auto callback1 = [&](fuchsia::camera3::FrameInfo info) {
      ASSERT_EQ(info.buffer_index, kBufferId1);
      frame1_received = true;
      info.release_fence.reset();
      client.stream->GetNextFrame(std::move(callback2));
    };
    client.stream->GetNextFrame(std::move(callback1));
    while (!HasFailure() && (!frame1_received || !frame2_received)) {
      RunLoopUntilIdle();
    }
  }

  auto client_result = legacy_stream_fake->StreamClientStatus();
  EXPECT_TRUE(client_result.is_ok()) << client_result.error();
  for (auto& client : clients) {
    client.stream = nullptr;
  }
  stream_impl = nullptr;
}

TEST_F(DeviceImplTest, LegacyStreamPropertiesRestored) {
  constexpr struct {
    fuchsia::math::Size resolution{.width = 1280, .height = 720};
    uint32_t format_index = 1;
  } kLegacyStreamFormatAssociation;
  constexpr fuchsia::math::RectF kCropRegion{.x = 0.1f, .y = 0.2f, .width = 0.6f, .height = 0.4f};
  fuchsia::camera3::StreamPtr stream;
  stream.set_error_handler(MakeErrorHandler("Stream"));
  auto request = stream.NewRequest();
  // Send these messages first on the channel, before buffers have been negotiated.
  stream->SetCropRegion(std::make_unique<fuchsia::math::RectF>(kCropRegion));
  stream->SetResolution(kLegacyStreamFormatAssociation.resolution);
  std::unique_ptr<FakeLegacyStream> legacy_stream_fake;
  bool legacy_stream_created = false;
  auto config_metrics = MetricsReporter::Get().CreateConfigurationRecord(0, 1);
  auto stream_impl = std::make_unique<StreamImpl>(
      dispatcher(), config_metrics->GetStreamRecord(0), fake_properties_, fake_legacy_config_,
      std::move(request),
      [&](fidl::InterfaceRequest<fuchsia::camera2::Stream> request, uint32_t format_index) {
        auto result =
            FakeLegacyStream::Create(std::move(request), allocator_, format_index, dispatcher());
        ASSERT_TRUE(result.is_ok());
        legacy_stream_fake = result.take_value();
        legacy_stream_created = true;
      },
      [&](fuchsia::sysmem::BufferCollectionTokenHandle token,
          fit::function<void(uint32_t)> callback) {
        token.BindSync()->Close();
        callback(1);
      },
      nop);
  fuchsia::sysmem::BufferCollectionTokenPtr token;
  allocator_->AllocateSharedCollection(token.NewRequest());
  stream->SetBufferCollection(std::move(token));
  stream->WatchBufferCollection(
      [](fuchsia::sysmem::BufferCollectionTokenHandle token) { token.BindSync()->Close(); });
  RunLoopUntil([&]() {
    return HasFailure() || (legacy_stream_created && legacy_stream_fake->IsStreaming());
  });
  ASSERT_FALSE(HasFailure());
  auto [x_min, y_min, x_max, y_max] = legacy_stream_fake->GetRegionOfInterest();
  EXPECT_EQ(x_min, kCropRegion.x);
  EXPECT_EQ(y_min, kCropRegion.y);
  constexpr float kEpsilon = 0.001f;
  EXPECT_NEAR(x_max - x_min, kCropRegion.width, kEpsilon);
  EXPECT_NEAR(y_max - y_min, kCropRegion.height, kEpsilon);
  auto image_format = legacy_stream_fake->GetImageFormat();
  EXPECT_EQ(image_format, kLegacyStreamFormatAssociation.format_index);
}

TEST_F(DeviceImplTest, WatchOrientation) {
  fuchsia::camera3::DevicePtr device;
  SetFailOnError(device, "Device");
  device_->GetHandler()(device.NewRequest());
  fuchsia::camera3::StreamPtr stream;
  SetFailOnError(stream, "Stream");
  device->ConnectToStream(0, stream.NewRequest());
  bool orientation_returned = false;
  stream->WatchOrientation([&](fuchsia::camera3::Orientation orientation) {
    EXPECT_EQ(orientation, fuchsia::camera3::Orientation::UP);
    orientation_returned = true;
  });
  RunLoopUntilFailureOr(orientation_returned);
  stream->WatchOrientation([&](fuchsia::camera3::Orientation orientation) {
    ADD_FAILURE() << "WatchOrientation should not return a second time.";
  });
  // Run the loop for long enough that we can be confident the callback won't be invoked.
  RunLoopWithTimeout(zx::sec(2));
}

}  // namespace camera
