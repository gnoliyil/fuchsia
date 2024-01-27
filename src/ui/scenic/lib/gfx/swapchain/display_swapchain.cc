// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/swapchain/display_swapchain.h"

#include <fuchsia/hardware/display/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/trace/event.h>

#include "lib/fidl/cpp/comparison.h"
#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/flib/fence.h"
#include "src/ui/lib/escher/impl/naive_image.h"
#include "src/ui/lib/escher/util/bit_ops.h"
#include "src/ui/lib/escher/util/fuchsia_utils.h"
#include "src/ui/lib/escher/util/image_utils.h"
#include "src/ui/lib/escher/vk/gpu_mem.h"
#include "src/ui/scenic/lib/display/util.h"

#define VK_CHECK_RESULT(XXX) FX_CHECK(XXX.result == vk::Result::eSuccess)

namespace scenic_impl {
namespace gfx {

DisplaySwapchain::DisplaySwapchain(
    Sysmem* sysmem,
    std::shared_ptr<fuchsia::hardware::display::CoordinatorSyncPtr> display_coordinator,
    std::shared_ptr<display::DisplayCoordinatorListener> display_coordinator_listener,
    uint64_t swapchain_image_count, display::Display* display, escher::Escher* escher)
    : escher_(escher),
      sysmem_(sysmem),
      swapchain_image_count_(swapchain_image_count),
      display_(display),
      display_coordinator_(display_coordinator),
      display_coordinator_listener_(display_coordinator_listener),
      swapchain_buffers_(/*count=*/0, /*environment=*/nullptr, /*use_protected_memory=*/false),
      protected_swapchain_buffers_(/*count=*/0, /*environment=*/nullptr,
                                   /*use_protected_memory=*/true) {
  FX_DCHECK(display);
  FX_DCHECK(sysmem);

  if (escher_) {
    device_ = escher_->vk_device();
    queue_ = escher_->device()->vk_main_queue();
    display_->Claim();
    frame_records_.resize(swapchain_image_count_);

    if (!InitializeDisplayLayer()) {
      FX_LOGS(FATAL) << "Initializing display layer failed";
    }
    if (!InitializeFramebuffers(escher_->resource_recycler(), /*use_protected_memory=*/false)) {
      FX_LOGS(FATAL) << "Initializing buffers for display swapchain failed - check "
                        "whether fuchsia.sysmem.Allocator is available in this sandbox";
    }
    if (escher_->device()->caps().allow_protected_memory &&
        !InitializeFramebuffers(escher_->resource_recycler(), /*use_protected_memory=*/true)) {
      FX_LOGS(FATAL) << "Initializing protected buffers for display swapchain failed - check "
                        "whether fuchsia.sysmem.Allocator is available in this sandbox";
    }

    display_->SetVsyncCallback(fit::bind_member<&DisplaySwapchain::OnVsync>(this));

    InitializeFrameRecords();

  } else {
    device_ = vk::Device();
    queue_ = vk::Queue();

    display_->Claim();

    FX_VLOGS(2) << "Using a NULL escher in DisplaySwapchain; likely in a test.";
  }
}

bool DisplaySwapchain::InitializeFramebuffers(escher::ResourceRecycler* resource_recycler,
                                              bool use_protected_memory) {
  FX_CHECK(escher_);
  BufferPool::Environment environment = {
      .display_coordinator = display_coordinator_,
      .display = display_,
      .escher = escher_,
      .sysmem = sysmem_,
      .recycler = resource_recycler,
      .vk_device = device_,
  };
  BufferPool pool(swapchain_image_count_, &environment, use_protected_memory);
  if ((*display_coordinator_)->SetLayerPrimaryConfig(primary_layer_id_, pool.image_config()) !=
      ZX_OK) {
    FX_LOGS(ERROR) << "Failed to set layer primary config";
  }
  if (use_protected_memory) {
    protected_swapchain_buffers_ = std::move(pool);
  } else {
    swapchain_buffers_ = std::move(pool);
  }
  return true;
}

DisplaySwapchain::~DisplaySwapchain() {
  if (!escher_) {
    display_->Unclaim();
    return;
  }

  display_->SetVsyncCallback(nullptr);

  // A FrameRecord is now stale and will no longer receive the OnFramePresented
  // callback; OnFrameDropped will clean up and make the state consistent.
  for (size_t i = 0; i < frame_records_.size(); ++i) {
    const size_t idx = (i + next_frame_index_) % frame_records_.size();
    FrameRecord* record = frame_records_[idx].get();
    if (record && record->frame_timings && !record->frame_timings->finalized()) {
      if (record->render_finished_wait->is_pending()) {
        // There has not been an OnFrameRendered signal. The wait will be destroyed when this
        // function returns. Record infinite time to signal unknown render time.
        record->frame_timings->OnFrameRendered(record->swapchain_index, scheduling::kTimeDropped);
      }
      record->frame_timings->OnFrameDropped(record->swapchain_index);
    }
  }

  display_->Unclaim();

  // Stop displaying the layer, then destroy it.  Note that these are all feedforward operations, so
  // the error handling just verifies that we have a valid FIDL channel etc., it doesn't verify that
  // the display coordinator is happy with the messages that we sent it.
  if (ZX_OK != (*display_coordinator_)->SetDisplayLayers(display_->display_id(), {})) {
    FX_LOGS(ERROR) << "~DisplaySwapchain(): Failed to configure display layers";
  } else if (ZX_OK != (*display_coordinator_)->ApplyConfig()) {
    FX_LOGS(ERROR) << "~DisplaySwapchain(): Failed to apply config";
  } else if (ZX_OK != (*display_coordinator_)->DestroyLayer(primary_layer_id_)) {
    FX_DLOGS(ERROR) << "~DisplaySwapchain(): Failed to destroy layer";
  }

  swapchain_buffers_.Clear(display_coordinator_);
  protected_swapchain_buffers_.Clear(display_coordinator_);
}

std::unique_ptr<DisplaySwapchain::FrameRecord> DisplaySwapchain::NewFrameRecord() {
  auto frame_record = std::make_unique<FrameRecord>();

  //
  // Create and import .render_finished_event
  //
  frame_record->render_finished_escher_semaphore = escher::Semaphore::NewExportableSem(device_);
  frame_record->render_finished_event =
      GetEventForSemaphore(escher_->device(), frame_record->render_finished_escher_semaphore);
  frame_record->render_finished_event_id =
      scenic_impl::ImportEvent(*display_coordinator_.get(), frame_record->render_finished_event);

  if (!frame_record->render_finished_escher_semaphore ||
      (frame_record->render_finished_event_id == fuchsia::hardware::display::INVALID_DISP_ID)) {
    FX_LOGS(ERROR) << "DisplaySwapchain::NewFrameRecord() failed to create semaphores";
    return std::unique_ptr<FrameRecord>();
  }

  //
  // Create and import .retired_event
  //
  zx_status_t const status = zx::event::create(0, &frame_record->retired_event);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "DisplaySwapchain::NewFrameRecord() failed to create retired event";
    return std::unique_ptr<FrameRecord>();
  }

  // Set to signaled
  frame_record->retired_event.signal(0, ZX_EVENT_SIGNALED);

  // Import to display coordinator
  frame_record->retired_event_id =
      scenic_impl::ImportEvent(*display_coordinator_.get(), frame_record->retired_event);
  if (frame_record->retired_event_id == fuchsia::hardware::display::INVALID_DISP_ID) {
    FX_LOGS(ERROR) << "DisplaySwapchain::NewFrameRecord() failed to import retired event";
    return std::unique_ptr<FrameRecord>();
  }

  return frame_record;
}

void DisplaySwapchain::InitializeFrameRecords() {
  for (size_t idx = 0; idx < frame_records_.size(); idx++) {
    frame_records_[idx] = NewFrameRecord();
  }
}

void DisplaySwapchain::ResetFrameRecord(const std::unique_ptr<FrameRecord>& frame_record) {
  if (!frame_record)
    return;

  // Were timings finalized?
  if (auto timings = frame_record->frame_timings) {
    FX_CHECK(timings->finalized());
    frame_record->frame_timings.reset();
  }

  // We expect the retired event to already have been signaled.  Verify this without waiting.
  if (frame_record->retired_event.wait_one(ZX_EVENT_SIGNALED, zx::time(), nullptr) != ZX_OK) {
    FX_LOGS(ERROR) << "DisplaySwapchain::DrawAndPresentFrame rendering into in-use backbuffer";
  }

  // Reset .render_finished_event and .retired_event
  //
  // As noted below in ::DrawAndPresentFrame(), there must not already exist a
  // pending record.  If there is, it indicates an error in the FrameScheduler
  // logic.
  frame_record->render_finished_event.signal(ZX_EVENT_SIGNALED, 0);
  frame_record->retired_event.signal(ZX_EVENT_SIGNALED, 0);

  // Return buffer to the pool
  if (frame_record->buffer) {
    if (frame_record->use_protected_memory) {
      protected_swapchain_buffers_.Put(frame_record->buffer);
    } else {
      swapchain_buffers_.Put(frame_record->buffer);
    }
    FX_DCHECK(frame_buffer_ids_.find(frame_record->buffer->id) != frame_buffer_ids_.end());
    frame_buffer_ids_.erase(frame_record->buffer->id);
    frame_record->buffer = nullptr;
  }

  // Reset presented flag
  frame_record->completed = false;
}

void DisplaySwapchain::UpdateFrameRecord(const std::unique_ptr<FrameRecord>& frame_record,
                                         const std::shared_ptr<FrameTimings>& frame_timings,
                                         size_t swapchain_index) {
  FX_DCHECK(frame_timings);
  FX_CHECK(escher_);

  frame_record->frame_timings = frame_timings;

  frame_record->swapchain_index = swapchain_index;

  frame_record->render_finished_wait = std::make_unique<async::Wait>(
      frame_record->render_finished_event.get(), escher::kFenceSignalled, ZX_WAIT_ASYNC_TIMESTAMP,
      [this, index = next_frame_index_](async_dispatcher_t* dispatcher, async::Wait* wait,
                                        zx_status_t status, const zx_packet_signal_t* signal) {
        OnFrameRendered(index, zx::time(signal->timestamp));
      });

  // TODO(fxbug.dev/23490): What to do if rendering fails?
  frame_record->render_finished_wait->Begin(async_get_default_dispatcher());
}

bool DisplaySwapchain::DrawAndPresentFrame(const std::shared_ptr<FrameTimings>& frame_timings,
                                           size_t swapchain_index, Layer& layer,
                                           DrawCallback draw_callback) {
  FX_DCHECK(frame_timings);

  // Get the next record that can be used to notify |frame_timings| (and hence
  // ultimately the FrameScheduler) that the frame has been presented.
  //
  // There must not already exist a pending record.  If there is, it indicates
  // an error in the FrameScheduler logic (or somewhere similar), which should
  // not have scheduled another frame when there are no framebuffers available.
  auto& frame_record = frame_records_[next_frame_index_];
  FX_DCHECK(frame_record->completed);

  // Reset frame record.
  ResetFrameRecord(frame_record);

  // Update frame record.
  UpdateFrameRecord(frame_record, frame_timings, swapchain_index);

  // Find the next framebuffer to render into, and other corresponding data.
  frame_record->buffer = use_protected_memory_ ? protected_swapchain_buffers_.GetUnused()
                                               : swapchain_buffers_.GetUnused();
  frame_record->use_protected_memory = use_protected_memory_;
  FX_CHECK(frame_record->buffer != nullptr);

  FX_DCHECK(frame_buffer_ids_.find(frame_record->buffer->id) == frame_buffer_ids_.end());
  frame_buffer_ids_.insert(frame_record->buffer->id);

  // Bump the ring head.
  next_frame_index_ = (next_frame_index_ + 1) % swapchain_image_count_;

  // Render the scene.
  {
    TRACE_DURATION("gfx", "DisplaySwapchain::DrawAndPresent() draw");
    draw_callback(frame_record->buffer->escher_image, layer, escher::SemaphorePtr(),
                  frame_record->render_finished_escher_semaphore);
  }

  // When the image is completely rendered, present it.
  TRACE_DURATION("gfx", "DisplaySwapchain::DrawAndPresent() present");

  Flip(primary_layer_id_, frame_record.get());

  return true;
}

bool DisplaySwapchain::SetDisplayColorConversion(const ColorTransform& transform) {
  FX_CHECK(display_);
  const fuchsia::hardware::display::DisplayId display_id = display_->display_id();
  return SetDisplayColorConversion(display_id, *display_coordinator_, transform);
}

bool DisplaySwapchain::SetDisplayColorConversion(
    fuchsia::hardware::display::DisplayId display_id,
    fuchsia::hardware::display::CoordinatorSyncPtr& display_coordinator,
    const ColorTransform& transform) {
  // Attempt to apply color conversion.
  zx_status_t status = display_coordinator->SetDisplayColorConversion(
      display_id, transform.preoffsets, transform.matrix, transform.postoffsets);
  if (status != ZX_OK) {
    FX_LOGS(WARNING)
        << "DisplaySwapchain:SetDisplayColorConversion failed, coordinator returned status: "
        << status;
    return false;
  }

  // Now check the config.
  fuchsia::hardware::display::ConfigResult result;
  std::vector<fuchsia::hardware::display::ClientCompositionOp> ops;
  display_coordinator->CheckConfig(/*discard=*/false, &result, &ops);

  bool client_color_conversion_required = false;
  if (result != fuchsia::hardware::display::ConfigResult::OK) {
    client_color_conversion_required = true;
  }

  for (const auto& op : ops) {
    if (op.opcode == fuchsia::hardware::display::ClientCompositionOpcode::CLIENT_COLOR_CONVERSION) {
      client_color_conversion_required = true;
      break;
    }
  }

  if (client_color_conversion_required) {
    // Clear config by calling |CheckConfig| once more with "discard" set to true.
    display_coordinator->CheckConfig(/*discard=*/true, &result, &ops);
    // TODO(fxbug.dev/24591): Implement scenic software fallback for color correction.
    FX_LOGS(ERROR) << "Software fallback for color conversion not implemented.";
    return true;
  }

  return true;
}

bool DisplaySwapchain::SetMinimumRgb(uint8_t minimum_rgb) {
  fuchsia::hardware::display::Coordinator_SetMinimumRgb_Result cmd_result;
  auto status = (*display_coordinator_)->SetMinimumRgb(minimum_rgb, &cmd_result);
  if (status != ZX_OK || cmd_result.is_err()) {
    FX_LOGS(WARNING) << "gfx::DisplaySwapchain::SetMinimumRGB failed";
    return false;
  }
  return true;
}

void DisplaySwapchain::SetUseProtectedMemory(bool use_protected_memory) {
  if (use_protected_memory == use_protected_memory_)
    return;

  FX_CHECK(!use_protected_memory || !protected_swapchain_buffers_.empty());
  use_protected_memory_ = use_protected_memory;
}

vk::Format DisplaySwapchain::GetImageFormat() { return swapchain_buffers_.image_format(); }

bool DisplaySwapchain::InitializeDisplayLayer() {
  zx_status_t create_layer_status;
  zx_status_t transport_status =
      (*display_coordinator_)->CreateLayer(&create_layer_status, &primary_layer_id_);
  if (create_layer_status != ZX_OK || transport_status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to create layer, op_status=" << create_layer_status
                   << " fidl_status=" << transport_status;
    return false;
  }

  zx_status_t status =
      (*display_coordinator_)->SetDisplayLayers(display_->display_id(), {primary_layer_id_});
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to configure display layers";
    return false;
  }
  return true;
}

void DisplaySwapchain::OnFrameRendered(size_t frame_index, zx::time render_finished_time) {
  FX_DCHECK(frame_index < swapchain_image_count_);
  auto& record = frame_records_[frame_index];

  uint64_t frame_number = record->frame_timings ? record->frame_timings->frame_number() : 0u;
  // TODO(fxbug.dev/57725) Replace with more robust solution.
  uint64_t frame_trace_id = (record->use_protected_memory * 3) + frame_index + 1;

  TRACE_DURATION("gfx", "DisplaySwapchain::OnFrameRendered", "frame count", frame_number,
                 "frame index", frame_trace_id);
  TRACE_FLOW_END("gfx", "scenic_frame", frame_number);

  FX_DCHECK(record);
  if (record->frame_timings) {
    record->frame_timings->OnFrameRendered(record->swapchain_index, render_finished_time);
    // See ::OnVsync for comment about finalization.
  }
}

void DisplaySwapchain::OnVsync(zx::time timestamp,
                               fuchsia::hardware::display::ConfigStamp applied_config_stamp) {
  // Don't double-report a frame as presented if a frame is shown twice
  // due to the next frame missing its deadline.
  if (last_presented_frame_.has_value() &&
      fidl::Equals(applied_config_stamp, last_presented_frame_->config_stamp)) {
    return;
  }

  // Verify if the configuration from Vsync is in the [pending_frames_] queue.
  auto vsync_frame_it =
      std::find_if(pending_frame_.begin(), pending_frame_.end(),
                   [applied_config_stamp](const FrameInfo& frame_info) {
                     return fidl::Equals(frame_info.config_stamp, applied_config_stamp);
                   });

  // It is possible that the config stamp doesn't match any config applied by
  // this |DisplaySwapchain| instance, for example, it could be from another
  // |DisplayCompositor|. Thus we just ignore these |OnVsync| events with
  // "invalid" config stamps.
  if (vsync_frame_it == pending_frame_.end()) {
    FX_LOGS(INFO) << "The config stamp <" << applied_config_stamp.value << "> was not generated "
                  << "by current display swapchain. Vsync event skipped.";
    return;
  }

  // Handle skipped frames.
  auto it = pending_frame_.begin();
  while (it != vsync_frame_it) {
    auto* record = it->frame_record;
    FX_DCHECK(!record->completed);
    record->completed = true;
    if (record->frame_timings) {
      record->frame_timings->OnFrameDropped(record->swapchain_index);
    }
    it = pending_frame_.erase(it);
  }

  // Handle the new presented frame.
  auto* record = vsync_frame_it->frame_record;
  FX_DCHECK(!record->completed);
  record->completed = true;
  if (record->frame_timings) {
    record->frame_timings->OnFramePresented(record->swapchain_index, timestamp);
  }
  last_presented_frame_ = std::move(*vsync_frame_it);
  pending_frame_.pop_front();
}

void DisplaySwapchain::Flip(fuchsia::hardware::display::LayerId layer_id,
                            FrameRecord* frame_record) {
  const uint64_t framebuffer_id = frame_record->buffer->id;
  uint64_t wait_event_id = frame_record->render_finished_event_id;
  uint64_t signal_event_id = frame_record->retired_event_id;

  zx_status_t status =
      (*display_coordinator_)
          ->SetLayerImage(layer_id, framebuffer_id, wait_event_id, signal_event_id);
  // TODO(fxbug.dev/23490): handle this more robustly.
  FX_CHECK(status == ZX_OK) << "DisplaySwapchain::Flip failed on SetLayerImage.";

  auto before = zx::clock::get_monotonic();

  status = (*display_coordinator_)->ApplyConfig();

  // TODO(fxbug.dev/23490): handle this more robustly.
  FX_CHECK(status == ZX_OK) << "DisplaySwapchain::Flip failed on ApplyConfig. Waited "
                            << (zx::clock::get_monotonic() - before).to_msecs() << "msecs";

  fuchsia::hardware::display::ConfigStamp pending_config_stamp;
  status = (*display_coordinator_)->GetLatestAppliedConfigStamp(&pending_config_stamp);
  FX_CHECK(status == ZX_OK) << "DisplaySwapchain::Flip failed on GetLatestAppliedConfigStamp: "
                            << status;

  pending_frame_.push_back({
      .config_stamp = pending_config_stamp,
      .frame_record = frame_record,
  });
}

}  // namespace gfx
}  // namespace scenic_impl
