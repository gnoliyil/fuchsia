// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/camera-gym/buffer_collage.h"

#include <fuchsia/images/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/default.h>
#include <lib/async/dispatcher.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/promise.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/zx/eventpair.h>
#include <zircon/errors.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

#include <cmath>
#include <optional>

#include "src/camera/bin/camera-gym/frame_capture.h"
#include "src/lib/fsl/vmo/file.h"

namespace camera {

using Command = fuchsia::camera::gym::Command;

using SetDescriptionCommand = fuchsia::camera::gym::SetDescriptionCommand;
using CaptureFrameCommand = fuchsia::camera::gym::CaptureFrameCommand;

constexpr uint32_t kViewRequestTimeoutMs = 5000;

constexpr float kOffscreenDepth = 1.0f;
constexpr float kBackgroundDepth = 0.0f;
constexpr float kCollectionDepth = -0.1f;
constexpr float kHighlightDepth = -0.2f;
constexpr float kDescriptionDepth = -0.3f;
constexpr float kMuteDepth = -0.4f;
constexpr float kHeartbeatDepth = -1.0f;

// Returns an event such that when the event is signaled and the dispatcher executed, the provided
// eventpair is closed. This can be used to bridge event- and eventpair-based fence semantics. If
// this function returns an error, |eventpair| is closed immediately.
fpromise::result<zx::event, zx_status_t> MakeEventBridge(async_dispatcher_t* dispatcher,
                                                         zx::eventpair* eventpair) {
  zx::event caller_event;
  zx::event waiter_event;
  zx_status_t status = zx::event::create(0, &caller_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  status = caller_event.duplicate(ZX_RIGHT_SAME_RIGHTS, &waiter_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  // A shared_ptr is necessary in order to begin the wait after setting the wait handler.
  auto wait = std::make_shared<async::Wait>(waiter_event.get(), ZX_EVENT_SIGNALED);
  wait->set_handler([wait, waiter_event = std::move(waiter_event)](
                        async_dispatcher_t* /*unused*/, async::Wait* /*unused*/,
                        zx_status_t /*unused*/, const zx_packet_signal_t* /*unused*/) mutable {
    // Close the waiter along with its captures.
    wait = nullptr;
  });
  status = wait->Begin(dispatcher);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  return fpromise::ok(std::move(caller_event));
}

BufferCollage::BufferCollage()
    : loop_(&kAsyncLoopConfigNoAttachToCurrentThread), view_provider_binding_(this) {
  SetStopOnError(scenic_);
  SetStopOnError(allocator_);

#if CAMERA_GYM_ENABLE_ROOT_PRESENTER
  SetStopOnError(presenter_);
#endif

  view_provider_binding_.set_error_handler([this](zx_status_t status) {
    FX_PLOGS(DEBUG, status) << "ViewProvider client disconnected.";
    view_provider_binding_.Unbind();
  });
}

BufferCollage::~BufferCollage() {
  zx_status_t status =
      async::PostTask(loop_.dispatcher(), fit::bind_member(this, &BufferCollage::Stop));
  ZX_ASSERT(status == ZX_OK);
  loop_.JoinThreads();
}

fpromise::result<std::unique_ptr<BufferCollage>, zx_status_t> BufferCollage::Create(
    fuchsia::ui::scenic::ScenicHandle scenic, fuchsia::sysmem::AllocatorHandle allocator,
    fuchsia::ui::policy::PresenterHandle presenter, fit::closure stop_callback) {
  auto collage = std::unique_ptr<BufferCollage>(new BufferCollage);
  collage->start_time_ = zx::clock::get_monotonic();

  // Bind interface handles and save the stop callback.
  zx_status_t status = collage->scenic_.Bind(std::move(scenic), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  status = collage->allocator_.Bind(std::move(allocator), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
#if CAMERA_GYM_ENABLE_ROOT_PRESENTER
  status = collage->presenter_.Bind(std::move(presenter), collage->loop_.dispatcher());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
#endif
  collage->stop_callback_ = std::move(stop_callback);

  // Create a scenic session and set its event handlers.
  {
    fuchsia::ui::scenic::SessionEndpoints endpoints;
    endpoints.set_touch_source(collage->touch_source_.NewRequest());
    collage->session_ = std::make_unique<scenic::Session>(
        collage->scenic_.get(), std::move(endpoints), collage->loop_.dispatcher());
  }
  collage->session_->set_error_handler(
      fit::bind_member(collage.get(), &BufferCollage::OnScenicError));
  collage->session_->set_event_handler(
      fit::bind_member(collage.get(), &BufferCollage::OnScenicEvent));
  collage->touch_source_->Watch({}, fit::bind_member(collage.get(), &BufferCollage::OnTouchEvents));

  // Start a thread and begin processing messages.
  status = collage->loop_.StartThread("BufferCollage Loop");
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }

#if CAMERA_GYM_ENABLE_ROOT_PRESENTER
  async::PostDelayedTask(collage->loop_.dispatcher(),
                         fit::bind_member(collage.get(), &BufferCollage::MaybeTakeDisplay),
                         zx::msec(kViewRequestTimeoutMs));
#endif

  return fpromise::ok(std::move(collage));
}

fidl::InterfaceRequestHandler<fuchsia::ui::app::ViewProvider> BufferCollage::GetHandler() {
  return fit::bind_member(this, &BufferCollage::OnNewRequest);
}

fpromise::promise<uint32_t> BufferCollage::AddCollection(
    fuchsia::sysmem::BufferCollectionTokenHandle token, fuchsia::sysmem::ImageFormat_2 image_format,
    std::string description) {
  TRACE_DURATION("camera", "BufferCollage::AddCollection");
  ZX_ASSERT(image_format.coded_width > 0);
  ZX_ASSERT(image_format.coded_height > 0);
  ZX_ASSERT(image_format.bytes_per_row > 0);

  fpromise::bridge<uint32_t> task_bridge;

  fit::closure add_collection = [this, token = std::move(token), image_format, description,
                                 result = std::move(task_bridge.completer)]() mutable {
    auto collection_id = next_collection_id_++;
    FX_LOGS(DEBUG) << "Adding collection with ID " << collection_id << ".";
    ZX_ASSERT(collection_views_.find(collection_id) == collection_views_.end());
    auto& view = collection_views_[collection_id];
    std::ostringstream oss;
    oss << " (" << collection_id << ")";
    auto name = "Collection (" + std::to_string(collection_id) + ")";
    SetRemoveCollectionViewOnError(view.collection, collection_id, name);
    SetStopOnError(view.image_pipe, "Image Pipe" + oss.str());
    view.image_format = image_format;
    constexpr uint32_t kTitleWidth = 768;
    constexpr uint32_t kTitleHeight = 128;
    view.description_node =
        std::make_unique<BitmapImageNode>(session_.get(), description, kTitleWidth, kTitleHeight);

    // Bind and duplicate the token.
    fuchsia::sysmem::BufferCollectionTokenPtr token_ptr;
    SetStopOnError(token_ptr, "BufferCollectionToken");
    zx_status_t status = token_ptr.Bind(std::move(token), loop_.dispatcher());
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status);
      Stop();
      result.complete_error();
      return;
    }
    fuchsia::sysmem::BufferCollectionTokenHandle scenic_token;
    token_ptr->Duplicate(ZX_RIGHT_SAME_RIGHTS, scenic_token.NewRequest());
    allocator_->BindSharedCollection(std::move(token_ptr),
                                     view.collection.NewRequest(loop_.dispatcher()));

    // Sync the collection and create an image pipe using the scenic token.
    view.collection->Sync([this, collection_id, token = std::move(scenic_token),
                           result = std::move(result)]() mutable {
      auto& view = collection_views_[collection_id];
      view.image_pipe_id = session_->AllocResourceId();
      auto command = scenic::NewCreateImagePipe2Cmd(view.image_pipe_id,
                                                    view.image_pipe.NewRequest(loop_.dispatcher()));
      session_->Enqueue(std::move(command));
      view.image_pipe->AddBufferCollection(1, std::move(token));
      UpdateLayout();

      if (frame_capture_) {
        // Frame capture: Set constraints to support frame capture.
        view.collection->SetConstraints(true, {.usage{
                                                   .cpu = fuchsia::sysmem::cpuUsageRead,
                                               },
                                               .min_buffer_count_for_camping = 0,
                                               .has_buffer_memory_constraints = true,
                                               .buffer_memory_constraints{
                                                   .ram_domain_supported = true,
                                                   .cpu_domain_supported = true,
                                               }});
      } else {
        // Set minimal constraints then wait for buffer allocation.
        view.collection->SetConstraints(true, {.usage{.none = fuchsia::sysmem::noneUsage}});
      }

      // Wait for buffer allocation.
      view.collection->WaitForBuffersAllocated(
          [this, collection_id, result = std::move(result)](
              zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) mutable {
            auto& view = collection_views_[collection_id];
            if (status != ZX_OK) {
              FX_PLOGS(ERROR, status) << "Failed to allocate buffers.";
              Stop();
              result.complete_error();
              return;
            }

            // Frame capture: Map all VMO's.
            if (frame_capture_) {
              for (uint32_t buffer_index = 0; buffer_index < buffers.buffer_count; buffer_index++) {
                // TODO(b/204456599) - Use VmoMapper helper class instead.
                const zx::vmo& vmo = buffers.buffers[buffer_index].vmo;
                auto vmo_size = buffers.settings.buffer_settings.size_bytes;
                uintptr_t vmo_virt_addr = 0;
                auto status =
                    zx::vmar::root_self()->map(ZX_VM_PERM_READ, 0 /* vmar_offset */, vmo,
                                               0 /* vmo_offset */, vmo_size, &vmo_virt_addr);
                ZX_ASSERT(status == ZX_OK);
                collection_views_[collection_id].buffer_id_to_virt_addr[buffer_index] =
                    vmo_virt_addr;
              }
            }

            collection_views_[collection_id].buffers = std::move(buffers);

            // Add the negotiated images to the image pipe.
            for (uint32_t i = 0; i < view.buffers.buffer_count; ++i) {
              view.image_pipe->AddImage(i + 1, 1, i, view.image_format);
            }

            // Complete the promise.
            FX_LOGS(DEBUG) << "Successfully added collection " << collection_id << ".";
            result.complete_ok(collection_id);
          });
    });
  };
  async::PostTask(loop_.dispatcher(), std::move(add_collection));
  return task_bridge.consumer.promise();
}

void BufferCollage::RemoveCollection(uint32_t id) {
  TRACE_DURATION("camera", "BufferCollage::RemoveCollection");
  if (async_get_default_dispatcher() != loop_.dispatcher()) {
    // Marshal the task to our own thread if called from elsewhere.
    auto nonce = TRACE_NONCE();
    TRACE_FLOW_BEGIN("camera", "post_remove_collection", nonce);
    async::PostTask(loop_.dispatcher(), [this, id, nonce] {
      TRACE_DURATION("camera", "BufferCollage::RemoveCollection.task");
      TRACE_FLOW_END("camera", "post_remove_collection", nonce);
      RemoveCollection(id);
    });
    return;
  }

  auto it = collection_views_.find(id);
  if (it == collection_views_.end()) {
    FX_LOGS(INFO) << "Skipping RemoveCollection for already-removed collection ID " << id;
    return;
  }
  auto& collection_view = it->second;
  auto image_pipe_id = collection_view.image_pipe_id;
  view_->DetachChild(*collection_view.node);
  view_->DetachChild(*collection_view.highlight_node);
  view_->DetachChild(collection_view.description_node->node);
  session_->ReleaseResource(image_pipe_id);  // De-allocate ImagePipe2 scenic side
  if (collection_view.collection.is_bound()) {
    collection_view.collection->Close();
  }

  // Frame capture: Unmap all buffers.
  if (frame_capture_) {
    for (uint32_t buffer_index = 0; buffer_index < collection_view.buffers.buffer_count;
         buffer_index++) {
      auto vmo_size = collection_view.buffers.settings.buffer_settings.size_bytes;

      // TODO(b/204456599) - Use VmoMapper helper class instead.
      auto it = collection_view.buffer_id_to_virt_addr.find(buffer_index);
      ZX_ASSERT(it != collection_view.buffer_id_to_virt_addr.end());
      auto vmo_virt_addr = collection_view.buffer_id_to_virt_addr[buffer_index];
      auto status = zx::vmar::root_self()->unmap(vmo_virt_addr, vmo_size);
      ZX_ASSERT(status == ZX_OK);
    }
  }

  collection_views_.erase(it);
  UpdateLayout();
}

void BufferCollage::PostShowBuffer(uint32_t collection_id, uint32_t buffer_index,
                                   zx::eventpair* release_fence,
                                   std::optional<fuchsia::math::RectF> subregion) {
  auto nonce = TRACE_NONCE();
  TRACE_DURATION("camera", "BufferCollage::PostShowBuffer");
  TRACE_FLOW_BEGIN("camera", "post_show_buffer", nonce);
  async::PostTask(loop_.dispatcher(), [=, release_fence = release_fence]() mutable {
    TRACE_DURATION("camera", "BufferCollage::PostShowBuffer.task");
    TRACE_FLOW_END("camera", "post_show_buffer", nonce);
    ShowBuffer(collection_id, buffer_index, release_fence, subregion);
  });
}

void BufferCollage::PostSetCollectionVisibility(uint32_t id, bool visible) {
  bool darkened = !visible;
  async::PostTask(loop_.dispatcher(), [=] {
    auto it = collection_views_.find(id);
    if (it == collection_views_.end()) {
      FX_LOGS(ERROR) << "Invalid collection ID " << id << ".";
      Stop();
      return;
    }
    auto& view = it->second;
    if (view.darkened != darkened) {
      view.darkened = darkened;
      UpdateLayout();
    }
  });
}

void BufferCollage::PostSetMuteIconVisibility(bool visible) {
  async::PostTask(loop_.dispatcher(), [=] {
    mute_visible_ = visible;
    UpdateLayout();
  });
}

void BufferCollage::OnNewRequest(fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request) {
  if (view_provider_binding_.is_bound()) {
    request.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }

  view_provider_binding_.Bind(std::move(request), loop_.dispatcher());
}

void BufferCollage::Stop() {
  if (view_provider_binding_.is_bound()) {
    FX_LOGS(WARNING) << "Collage closing view channel due to server error.";
    view_provider_binding_.Close(ZX_ERR_INTERNAL);
  }
  scenic_ = nullptr;
  allocator_ = nullptr;
  presenter_ = nullptr;
  view_ = nullptr;
  collection_views_.clear();
  loop_.Quit();
  if (stop_callback_) {
    stop_callback_();
    stop_callback_ = nullptr;
  }
}

template <typename T>
void BufferCollage::SetStopOnError(fidl::InterfacePtr<T>& p, std::string name) {
  p.set_error_handler([this, name, &p](zx_status_t status) {
    FX_PLOGS(ERROR, status) << name << " disconnected unexpectedly.";
    p = nullptr;
    Stop();
  });
}

template <typename T>
void BufferCollage::SetRemoveCollectionViewOnError(fidl::InterfacePtr<T>& p, uint32_t view_id,
                                                   std::string name) {
  p.set_error_handler([this, view_id, name](zx_status_t status) {
    FX_PLOGS(WARNING, status) << name << " view_id=" << view_id << " disconnected unexpectedly.";
    if (collection_views_.find(view_id) == collection_views_.end()) {
      FX_LOGS(INFO) << name << " view_id=" << view_id << " already removed.";
      return;
    }
    RemoveCollection(view_id);
  });
}

void BufferCollage::ShowBuffer(uint32_t collection_id, uint32_t buffer_index,
                               zx::eventpair* release_fence,
                               std::optional<fuchsia::math::RectF> subregion) {
  TRACE_DURATION("camera", "BufferCollage::ShowBuffer");
  auto it = collection_views_.find(collection_id);
  if (it == collection_views_.end()) {
    FX_LOGS(ERROR) << "Invalid collection ID " << collection_id << ".";
    Stop();
    return;
  }
  auto& view = it->second;
  if (buffer_index >= view.buffers.buffer_count) {
    FX_LOGS(ERROR) << "Invalid buffer index " << buffer_index << ".";
    Stop();
    return;
  }

  // Frame capture: Perform the capture if trigger is pending (> 0).
  // TODO(b/200839146): Support multiple stream use case by choosing desired stream ID.
  if (CaptureRequestPending() /* && (stream_id == ???) */) {
    // Frame capture: Make sure capture parameters are reasonable.
    uint32_t coded_width = view.image_format.coded_width;
    uint32_t coded_height = view.image_format.coded_height;
    uint32_t coded_stride = view.image_format.bytes_per_row;
    uint32_t coded_image_size = coded_stride * coded_height * 3 / 2;  // NV12 ONLY!
    ZX_ASSERT(coded_width > 0);
    ZX_ASSERT(coded_height > 0);
    ZX_ASSERT(coded_image_size >= 4096);
    ZX_ASSERT(frame_capture_);
    frame_capture_->Capture(view.buffers.buffers[buffer_index].vmo, coded_width, coded_height,
                            coded_stride, coded_image_size);
    CompletedOneCaptureRequest();
  }

  if (subregion) {
    view.highlight_node->SetScale(subregion->width * view.view_region.width,
                                  subregion->height * view.view_region.height, 1);
    view.highlight_node->SetTranslation(view.view_region.x + subregion->x * view.view_region.width,
                                        view.view_region.y + subregion->y * view.view_region.height,
                                        kHighlightDepth);
  } else {
    view.highlight_node->SetScale(1, 1, 1);
    view.highlight_node->SetTranslation(0, 0, kOffscreenDepth);
  }

  auto result = MakeEventBridge(loop_.dispatcher(), release_fence);
  if (result.is_error()) {
    FX_PLOGS(ERROR, result.error());
    Stop();
    return;
  }
  std::vector<zx::event> scenic_fences;
  scenic_fences.push_back(result.take_value());

  TRACE_FLOW_BEGIN("gfx", "image_pipe_present_image", buffer_index + 1);
  it->second.image_pipe->PresentImage(buffer_index + 1, zx::clock::get_monotonic().get(), {},
                                      std::move(scenic_fences),
                                      [](fuchsia::images::PresentationInfo info) {});
  it->second.has_content = true;
}

// Calculate the grid size needed to fit |n| elements by alternately adding rows and columns.
static std::tuple<uint32_t, uint32_t> GetGridSize(uint32_t n) {
  uint32_t rows = 0;
  uint32_t cols = 0;
  while (rows * cols < n) {
    if (rows == cols) {
      ++cols;
    } else {
      ++rows;
    }
  }
  return {rows, cols};
}

// Calculate the center of an element |index| in a grid with |n| elements.
static std::tuple<float, float> GetCenter(uint32_t index, uint32_t n) {
  auto [rows, cols] = GetGridSize(n);
  uint32_t row = index / cols;
  uint32_t col = index % cols;
  float y = (static_cast<float>(row) + 0.5f) / static_cast<float>(rows);
  float x = (static_cast<float>(col) + 0.5f) / static_cast<float>(cols);
  // Center-align the last row if it is not fully filled.
  if (row == rows - 1) {
    x += static_cast<float>(rows * cols - n) * 0.5f / static_cast<float>(cols);
  }
  return {x, y};
}

// Calculate the size of an element scaled uniformly to fit a given extent.
static std::tuple<float, float> ScaleToFit(float element_width, float element_height,
                                           float box_width, float box_height) {
  float x_scale = box_width / element_width;
  float y_scale = box_height / element_height;
  float scale = std::min(x_scale, y_scale);
  return {element_width * scale, element_height * scale};
}

// Builds a mesh that is equivalent to a scenic::Rectangle with the given |width| and |height|, but
// also includes a second smaller rectangle superimposed in the corner that shows a zoomed subregion
// of the associated material's center.
static std::unique_ptr<scenic::Mesh> BuildMesh(scenic::Session* session, float width, float height,
                                               bool magnify) {
  auto mesh = std::make_unique<scenic::Mesh>(session);
  auto format = scenic::NewMeshVertexFormat(fuchsia::ui::gfx::ValueType::kVector3,
                                            fuchsia::ui::gfx::ValueType::kNone,
                                            fuchsia::ui::gfx::ValueType::kVector2);
  constexpr float kMagnificationMargin = 0.02f;
  constexpr float kMagnificationSize = 0.4f;
  constexpr float kMagnificationAmount = 12.0f;
  const float x1 = -width / 2;
  const float x2 = width / 2;
  const float y1 = -height / 2;
  const float y2 = height / 2;
  const float x3 = x1 + width * kMagnificationMargin;
  const float x4 = x3 + width * kMagnificationSize;
  const float y3 = y1 + height * kMagnificationMargin;
  const float y4 = y3 + height * kMagnificationSize;
  const float t1 = 0.5f - 0.5f / kMagnificationAmount;
  const float t2 = 0.5f + 0.5f / kMagnificationAmount;

  constexpr size_t kVertexSize = 5;  // 5 float32's (x,y,z,u,v)

  // clang-format off
  std::vector<float> vb {
    x1, y1, 0, 0,  0,
    x2, y1, 0, 1,  0,
    x1, y2, 0, 0,  1,
    x2, y2, 0, 1,  1,
  };
  std::vector<float> vb_magnify {
    x3, y3, 0, t1, t1,
    x4, y3, 0, t2, t1,
    x3, y4, 0, t1, t2,
    x4, y4, 0, t2, t2,
  };
  std::vector<uint32_t> ib {
    0, 1, 2, 2, 1, 3,
  };
  std::vector<uint32_t> ib_magnify {
    4, 5, 6, 6, 5, 7,
  };
  // clang-format on

  if (magnify) {
    vb.insert(vb.end(), vb_magnify.begin(), vb_magnify.end());
    ib.insert(ib.end(), ib_magnify.begin(), ib_magnify.end());
  }

  zx::vmo vb_vmo;
  size_t vb_size = vb.size() * sizeof(vb[0]);
  ZX_ASSERT(zx::vmo::create(vb_size, 0, &vb_vmo) == ZX_OK);
  ZX_ASSERT(vb_vmo.write(vb.data(), 0, vb_size) == ZX_OK);
  scenic::Memory vb_mem(session, std::move(vb_vmo), vb_size,
                        fuchsia::images::MemoryType::HOST_MEMORY);
  scenic::Buffer scenic_vb(vb_mem, 0, vb_size);

  zx::vmo ib_vmo;
  size_t ib_size = ib.size() * sizeof(ib[0]);
  ZX_ASSERT(zx::vmo::create(ib_size, 0, &ib_vmo) == ZX_OK);
  ZX_ASSERT(ib_vmo.write(ib.data(), 0, ib_size) == ZX_OK);
  scenic::Memory ib_mem(session, std::move(ib_vmo), ib_size,
                        fuchsia::images::MemoryType::HOST_MEMORY);
  scenic::Buffer scenic_ib(ib_mem, 0, ib_size);

  std::array<float, 3> aabb_min{x1, y1, 0};
  std::array<float, 3> aabb_max{x2, y2, 0};

  mesh->BindBuffers(scenic_ib, fuchsia::ui::gfx::MeshIndexFormat::kUint32, 0,
                    static_cast<uint32_t>(ib.size()), scenic_vb, format, 0,
                    static_cast<uint32_t>(vb.size() / kVertexSize), aabb_min, aabb_max);

  return mesh;
}

// Builds a "bracketing" mesh shape in the bounds 0..1 comprised of four copies of the
// following:
//
// |----------| = kSpan
// |---| = kWeight
//  __________
// |         /
// |    ____/
// |   |
// |   |
// |  /
// |./

static std::unique_ptr<scenic::Mesh> BuildHighlightMesh(scenic::Session* session) {
  auto mesh = std::make_unique<scenic::Mesh>(session);

  auto format = scenic::NewMeshVertexFormat(fuchsia::ui::gfx::ValueType::kVector3,
                                            fuchsia::ui::gfx::ValueType::kNone,
                                            fuchsia::ui::gfx::ValueType::kVector2);
  constexpr float kWeight = 0.02f;
  constexpr float kSpan = 0.2f;

  constexpr size_t kVertexSize = 5;  // 5 float32's

  // clang-format off
  std::vector<std::array<float, 2>> vertices {
    { 0,               0 },
    { kSpan,           0 },
    { kSpan - kWeight, kWeight },
    { kWeight,         kWeight},
    { kWeight,         kSpan - kWeight},
    { 0,               kSpan },
  };
  std::vector<std::array<uint32_t, 3>> triangles {
    { 0, 1, 2 },
    { 0, 2, 3 },
    { 0, 3, 4 },
    { 0, 4, 5 },
  };
  // clang-format on

  auto append_mesh = [&](std::vector<float>& vb, std::vector<uint32_t>& ib, bool flip_horizontal,
                         bool flip_vertical) {
    uint32_t base_index = static_cast<uint32_t>(vb.size() / kVertexSize);
    bool flip_chirality =
        (flip_horizontal && !flip_vertical) || (flip_vertical && !flip_horizontal);
    for (auto& vertex : vertices) {
      float x = flip_horizontal ? 1 - vertex[0] : vertex[0];
      float y = flip_vertical ? 1 - vertex[1] : vertex[1];
      vb.push_back(x);
      vb.push_back(y);
      vb.push_back(0);
      vb.push_back(x);
      vb.push_back(y);
    }
    for (auto& triangle : triangles) {
      uint32_t a = triangle[0];
      uint32_t b = flip_chirality ? triangle[2] : triangle[1];
      uint32_t c = flip_chirality ? triangle[1] : triangle[2];
      ib.push_back(base_index + a);
      ib.push_back(base_index + b);
      ib.push_back(base_index + c);
    }
  };
  std::vector<float> vb;
  std::vector<uint32_t> ib;
  append_mesh(vb, ib, false, false);  // top-left
  append_mesh(vb, ib, true, false);   // top-right
  append_mesh(vb, ib, false, true);   // bottom-left
  append_mesh(vb, ib, true, true);    // bottom-right

  zx::vmo vb_vmo;
  size_t vb_size = vb.size() * sizeof(vb[0]);
  ZX_ASSERT(zx::vmo::create(vb_size, 0, &vb_vmo) == ZX_OK);
  ZX_ASSERT(vb_vmo.write(vb.data(), 0, vb_size) == ZX_OK);
  scenic::Memory vb_mem(session, std::move(vb_vmo), vb_size,
                        fuchsia::images::MemoryType::HOST_MEMORY);
  scenic::Buffer scenic_vb(vb_mem, 0, vb_size);

  zx::vmo ib_vmo;
  size_t ib_size = ib.size() * sizeof(ib[0]);
  ZX_ASSERT(zx::vmo::create(ib_size, 0, &ib_vmo) == ZX_OK);
  ZX_ASSERT(ib_vmo.write(ib.data(), 0, ib_size) == ZX_OK);
  scenic::Memory ib_mem(session, std::move(ib_vmo), ib_size,
                        fuchsia::images::MemoryType::HOST_MEMORY);
  scenic::Buffer scenic_ib(ib_mem, 0, ib_size);
  std::array<float, 3> aabb_min{0, 0, 0};
  std::array<float, 3> aabb_max{1, 1, 0};
  mesh->BindBuffers(scenic_ib, fuchsia::ui::gfx::MeshIndexFormat::kUint32, 0,
                    static_cast<uint32_t>(ib.size()), scenic_vb, format, 0,
                    static_cast<uint32_t>(vb.size() / kVertexSize), aabb_min, aabb_max);

  return mesh;
}

void BufferCollage::UpdateLayout() {
  // TODO(fxbug.dev/49070): resolve constraints even if node is not visible
  // There is no intrinsic need to present the views prior to extents being known.
  if (!view_extents_) {
    constexpr fuchsia::ui::gfx::BoundingBox kDefaultBoundingBox{
        .min{.x = 0, .y = 0, .z = 0}, .max{.x = 640, .y = 480, .z = 1024}};
    view_extents_ = kDefaultBoundingBox;
  }

  auto [rows, cols] = GetGridSize(static_cast<uint32_t>(collection_views_.size()));
  float view_width = view_extents_->max.x - view_extents_->min.x;
  float view_height = view_extents_->max.y - view_extents_->min.y;
  constexpr float kPadding = 4.0f;
  float cell_width = view_width / static_cast<float>(cols) - kPadding;
  float cell_height = view_height / static_cast<float>(rows) - kPadding;

  for (auto& [id, view] : collection_views_) {
    if (view.node) {
      view_->DetachChild(*view.node);
      view_->DetachChild(*view.highlight_node);
      view_->DetachChild(view.description_node->node);
    }
  }
  uint32_t index = 0;
  // TODO(msandy): Track hidden nodes.
  if (view_ && view_width > 0 && view_height > 0) {
    for (auto& [id, view] : collection_views_) {
      view.material = std::make_unique<scenic::Material>(session_.get());
      view.material->SetTexture(view.image_pipe_id);
      view.highlight_material = std::make_unique<scenic::Material>(session_.get());
      view.highlight_material->SetColor(0xED, 0x1D, 0x7F, 0xFF);
      if (!view.has_content) {
        view.material->SetColor(0, 0, 0, 0);
        view.description_node->material.SetColor(0, 0, 0, 0);
      } else if (view.darkened) {
        view.material->SetColor(32, 32, 32, 255);
        view.description_node->material.SetColor(32, 32, 32, 255);
      } else {
        view.material->SetColor(255, 255, 255, 255);
        // TODO(fxbug.dev/54004): workaround for transparency issues
        constexpr uint32_t kAlphaWorkaround = 254;
        view.description_node->material.SetColor(255, 255, 255, kAlphaWorkaround);
      }
      float display_width = static_cast<float>(view.image_format.coded_width);
      float display_height = static_cast<float>(view.image_format.coded_height);
      if (view.image_format.has_pixel_aspect_ratio) {
        display_width *= static_cast<float>(view.image_format.pixel_aspect_ratio_width);
        display_height *= static_cast<float>(view.image_format.pixel_aspect_ratio_height);
      }
      auto [element_width, element_height] =
          ScaleToFit(display_width, display_height, cell_width, cell_height);
      view.mesh = BuildMesh(session_.get(), element_width, element_height, show_magnify_boxes());
      view.highlight_mesh = BuildHighlightMesh(session_.get());
      view.node = std::make_unique<scenic::ShapeNode>(session_.get());
      view.highlight_node = std::make_unique<scenic::ShapeNode>(session_.get());
      view.node->SetShape(*view.mesh);
      view.highlight_node->SetShape(*view.highlight_mesh);
      view.node->SetMaterial(*view.material);
      view.highlight_node->SetMaterial(*view.highlight_material);
      auto [x, y] = GetCenter(index++, static_cast<uint32_t>(collection_views_.size()));
      view.node->SetTranslation(view_width * x, view_height * y, kCollectionDepth);
      view.highlight_node->SetTranslation(0, 0, kOffscreenDepth);
      view.view_region.width = element_width;
      view.view_region.height = element_height;
      view.view_region.x = view_width * x - element_width * 0.5f;
      view.view_region.y = view_height * y - element_height * 0.5f;
      float scale =
          (element_width - kPadding * 2) / static_cast<float>(view.description_node->width);
      view.description_node->node.SetScale(scale, scale, 1);
      view.description_node->node.SetTranslation(
          view_width * x,
          view_height * y + element_height * 0.5f -
              scale * static_cast<float>(view.description_node->height) * 0.5f - kPadding,
          kDescriptionDepth);
      view_->AddChild(*view.node);
      view_->AddChild(*view.highlight_node);
      if (show_description()) {
        view_->AddChild(view.description_node->node);
      }
    }
  }
  if (heartbeat_indicator_.node) {
    heartbeat_indicator_.node->SetTranslation(view_width * 0.5f, view_height, kHeartbeatDepth);
  }
  if (mute_indicator_) {
    mute_indicator_->node.SetTranslation(view_width * 0.5f, view_height * 0.5f,
                                         mute_visible_ ? kMuteDepth : kOffscreenDepth);
  }
}

void BufferCollage::MaybeTakeDisplay() {
  if (view_) {
    // View already created.
    return;
  }
  FX_LOGS(WARNING) << "Component host did not create a view within " << kViewRequestTimeoutMs
                   << "ms. camera-gym will now take over the display.";
  auto tokens = scenic::NewViewTokenPair();
  CreateView(std::move(tokens.first.value), nullptr, nullptr);
  presenter_->PresentOrReplaceView(std::move(tokens.second), nullptr);
}

void BufferCollage::SetupView() {
  ZX_ASSERT(view_);

  constexpr float kBackgroundSize = 16384.f;
  scenic::Material material(session_.get());
  material.SetColor(0, 0, 0, 255);  // Opaque black.
  scenic::Rectangle rectangle(session_.get(), kBackgroundSize, kBackgroundSize);
  scenic::ShapeNode node(session_.get());
  node.SetShape(rectangle);
  node.SetMaterial(material);
  node.SetTranslation(0.f, 0.f, kBackgroundDepth);  // Far plane.
  view_->AddChild(node);

  heartbeat_indicator_.material = std::make_unique<scenic::Material>(session_.get());
  constexpr float kIndicatorRadius = 12.0f;
  heartbeat_indicator_.shape = std::make_unique<scenic::Circle>(session_.get(), kIndicatorRadius);
  heartbeat_indicator_.node = std::make_unique<scenic::ShapeNode>(session_.get());
  heartbeat_indicator_.node->SetShape(*heartbeat_indicator_.shape);
  heartbeat_indicator_.node->SetMaterial(*heartbeat_indicator_.material);
  view_->AddChild(*heartbeat_indicator_.node);

  constexpr uint32_t kMuteIconSize = 64;
  mute_indicator_ =
      std::make_unique<BitmapImageNode>(session_.get(), "mute.bin", kMuteIconSize, kMuteIconSize);
  view_->AddChild(mute_indicator_->node);

  session_->set_on_frame_presented_handler(
      [this](fuchsia::scenic::scheduling::FramePresentedInfo info) {
        constexpr std::array kHeartbeatColor = {0xFF, 0x00, 0xFF};
        constexpr auto kHeartbeatPeriod = zx::sec(1);
        zx::duration t = (zx::clock::get_monotonic() - start_time_) % kHeartbeatPeriod.get();
        float phase = static_cast<float>(t.to_msecs()) / kHeartbeatPeriod.to_msecs();
        float amplitude = pow(1 - abs(1 - 2 * phase), 5.0f);
        heartbeat_indicator_.material->SetColor(
            static_cast<uint8_t>(kHeartbeatColor[0] * amplitude),
            static_cast<uint8_t>(kHeartbeatColor[1] * amplitude),
            static_cast<uint8_t>(kHeartbeatColor[2] * amplitude), 0xFF);
        session_->Present2(0, 0, [](fuchsia::scenic::scheduling::FuturePresentationTimes times) {});
      });
  session_->Present2(0, 0, [](fuchsia::scenic::scheduling::FuturePresentationTimes times) {});
  UpdateLayout();
}

void BufferCollage::OnScenicError(zx_status_t status) {
  FX_PLOGS(ERROR, status) << "Scenic session error.";
  Stop();
}

void BufferCollage::OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) {
  for (const auto& event : events) {
    if (event.is_gfx() && event.gfx().is_view_properties_changed()) {
      auto aabb = event.gfx().view_properties_changed().properties.bounding_box;
      // TODO(fxbug.dev/49069): bounding box should never be empty
      if (aabb.max.x == aabb.min.x || aabb.max.y == aabb.min.y || aabb.max.z == aabb.min.z) {
        view_extents_ = std::nullopt;
      } else {
        view_extents_ = aabb;
      }
      UpdateLayout();
    }
  }
}

void BufferCollage::OnTouchEvents(std::vector<fuchsia::ui::pointer::TouchEvent> events) {
  std::vector<fuchsia::ui::pointer::TouchResponse> responses;
  for (auto& event : events) {
    if (event.has_pointer_sample() &&
        event.pointer_sample().phase() == fuchsia::ui::pointer::EventPhase::REMOVE) {
      show_state_ = (show_state_ + 1) & kShowStateCycleMask;
      UpdateLayout();
    }

    if (event.has_pointer_sample()) {
      fuchsia::ui::pointer::TouchResponse response;
      response.set_response_type(fuchsia::ui::pointer::TouchResponseType::YES);
      responses.emplace_back(std::move(response));
    } else {
      // Add empty response for non-pointer event.
      responses.emplace_back();
    }
  }

  touch_source_->Watch(std::move(responses), fit::bind_member(this, &BufferCollage::OnTouchEvents));
}

void BufferCollage::CreateView(
    zx::eventpair view_token,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
    fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services) {
  async::PostTask(loop_.dispatcher(), [this, view_token = std::move(view_token)]() mutable {
    if (view_) {
      FX_LOGS(ERROR) << "Clients may only call this method once per view provider lifetime.";
      view_provider_binding_.Close(ZX_ERR_BAD_STATE);
      return;
    }
    view_ = std::make_unique<scenic::View>(
        session_.get(), scenic::ToViewToken(std::move(view_token)), "Camera Gym");
    SetupView();
  });
}

void BufferCollage::CreateViewWithViewRef(zx::eventpair view_token,
                                          fuchsia::ui::views::ViewRefControl view_ref_control,
                                          fuchsia::ui::views::ViewRef view_ref) {
  async::PostTask(loop_.dispatcher(), [this, view_token = std::move(view_token),
                                       view_ref_control = std::move(view_ref_control),
                                       view_ref = std::move(view_ref)]() mutable {
    if (view_) {
      FX_LOGS(ERROR) << "Clients may only call this method once per view provider lifetime.";
      view_provider_binding_.Close(ZX_ERR_BAD_STATE);
      return;
    }
    view_ = std::make_unique<scenic::View>(
        session_.get(), scenic::ToViewToken(std::move(view_token)), std::move(view_ref_control),
        std::move(view_ref), "Camera Gym");
    SetupView();
  });
}

BitmapImageNode::BitmapImageNode(scenic::Session* session, std::string filename, uint32_t width,
                                 uint32_t height)
    : material(session), node(session) {
  this->width = width;
  this->height = height;
  fsl::SizedVmo svmo;
  const size_t expected_size = width * height * 4;
  if (!fsl::VmoFromFilename("/pkg/data/" + filename, &svmo)) {
    // On failure (e.g. due to missing cipd binary), create an empty vmo.
    zx::vmo vmo;
    zx::vmo::create(expected_size, 0, &vmo);
    svmo = fsl::SizedVmo(std::move(vmo), expected_size);
  }
  memory = std::make_unique<scenic::Memory>(session, std::move(svmo.vmo()), svmo.size(),
                                            fuchsia::images::MemoryType::HOST_MEMORY);
  image = std::make_unique<scenic::Image>(
      *memory, 0,
      fuchsia::images::ImageInfo{.width = width,
                                 .height = height,
                                 .stride = width * 4,
                                 .alpha_format = fuchsia::images::AlphaFormat::PREMULTIPLIED});
  shape = std::make_unique<scenic::Rectangle>(session, width, height);
  material.SetTexture(*image);
  node.SetMaterial(material);
  node.SetShape(*shape);
}

void BufferCollage::CommandSuccessNotify() {
  CommandStatusHandler command_status_handler = std::move(command_status_handler_);
  if (command_status_handler) {
    ZX_ASSERT(controller_dispatcher_ != nullptr);
    async::PostTask(controller_dispatcher_,
                    [command_status_handler = std::move(command_status_handler)]() mutable {
                      fuchsia::camera::gym::Controller_SendCommand_Result result;
                      command_status_handler(result.WithResponse({}));
                    });
  }
}

void BufferCollage::ExecuteCommand(Command command, BufferCollage::CommandStatusHandler handler) {
  async::PostTask(loop_.dispatcher(),
                  [this, command = std::move(command), handler = std::move(handler)]() mutable {
                    PostedExecuteCommand(std::move(command), std::move(handler));
                  });
}

void BufferCollage::PostedExecuteCommand(Command command,
                                         BufferCollage::CommandStatusHandler handler) {
  command_status_handler_ = std::move(handler);
  switch (command.Which()) {
    case Command::Tag::kSetDescription:
      ExecuteSetDescriptionCommand(command.set_description());
      break;
    case Command::Tag::kCaptureFrame:
      ExecuteCaptureFrameCommand(command.capture_frame());
      break;
    default:
      ZX_ASSERT(false);
  }
}

void BufferCollage::ExecuteSetDescriptionCommand(
    fuchsia::camera::gym::SetDescriptionCommand& command) {
  set_show_description(command.enable);
  UpdateLayout();
  CommandSuccessNotify();
}

void BufferCollage::ExecuteCaptureFrameCommand(fuchsia::camera::gym::CaptureFrameCommand& command) {
  // TODO(b/200839146): Support multiple stream use case by passing in desired stream ID.
  AddOneCaptureRequest();
  CommandSuccessNotify();
}

}  // namespace camera
