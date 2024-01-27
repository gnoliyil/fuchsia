// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915/display-device.h"

#include <float.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fit/function.h>
#include <lib/zx/result.h>
#include <lib/zx/vmo.h>
#include <math.h>
#include <zircon/assert.h>
#include <zircon/errors.h>

#include <ddktl/fidl.h>

#include "src/graphics/display/drivers/intel-i915/intel-i915.h"
#include "src/graphics/display/drivers/intel-i915/registers-dpll.h"
#include "src/graphics/display/drivers/intel-i915/registers-transcoder.h"
#include "src/graphics/display/drivers/intel-i915/registers.h"
#include "src/graphics/display/drivers/intel-i915/tiling.h"

namespace i915 {

namespace {

zx_status_t backlight_message(void* ctx, fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
  ddk::Transaction transaction(txn);
  DisplayDevice* ptr;
  {
    fbl::AutoLock lock(&static_cast<display_ref_t*>(ctx)->mtx);
    ptr = static_cast<display_ref_t*>(ctx)->display_device;
  }
  fidl::WireDispatch<fuchsia_hardware_backlight::Device>(
      ptr, fidl::IncomingHeaderAndMessage::FromEncodedCMessage(msg), &transaction);
  return transaction.Status();
}

void backlight_release(void* ctx) { delete static_cast<display_ref_t*>(ctx); }

constexpr zx_protocol_device_t kBacklightDeviceOps = {
    .version = DEVICE_OPS_VERSION,
    .release = &backlight_release,
    .message = &backlight_message,
};

}  // namespace

DisplayDevice::DisplayDevice(Controller* controller, uint64_t id, DdiId ddi_id,
                             DdiReference ddi_reference, Type type)
    : controller_(controller),
      id_(id),
      ddi_id_(ddi_id),
      ddi_reference_(std::move(ddi_reference)),
      type_(type) {}

DisplayDevice::~DisplayDevice() {
  if (pipe_) {
    controller_->pipe_manager()->ReturnPipe(pipe_);
    controller_->ResetPipePlaneBuffers(pipe_->pipe_id());
  }
  if (inited_) {
    controller_->ResetDdi(ddi_id(), pipe()->connected_transcoder_id());
  }
  if (ddi_reference_) {
    ddi_reference_.reset();
  }
  if (display_ref_) {
    fbl::AutoLock lock(&display_ref_->mtx);
    display_ref_->display_device = nullptr;
    device_async_remove(backlight_device_);
  }
}

fdf::MmioBuffer* DisplayDevice::mmio_space() const { return controller_->mmio_space(); }

bool DisplayDevice::Init() {
  ddi_power_ = controller_->power()->GetDdiPowerWellRef(ddi_id_);

  Pipe* pipe = controller_->pipe_manager()->RequestPipe(*this);
  if (!pipe) {
    zxlogf(ERROR, "Cannot request a new pipe!");
    return false;
  }
  set_pipe(pipe);

  if (!InitDdi()) {
    return false;
  }

  inited_ = true;

  InitBacklight();

  return true;
}

bool DisplayDevice::InitWithDdiPllConfig(const DdiPllConfig& pll_config) {
  Pipe* pipe = controller_->pipe_manager()->RequestPipeFromHardwareState(*this, mmio_space());
  if (!pipe) {
    zxlogf(ERROR, "Failed loading pipe from register!");
    return false;
  }
  set_pipe(pipe);
  return true;
}

void DisplayDevice::InitBacklight() {
  if (HasBacklight() && InitBacklightHw()) {
    fbl::AllocChecker ac;
    auto display_ref = fbl::make_unique_checked<display_ref_t>(&ac);
    zx_status_t status = ZX_ERR_NO_MEMORY;
    if (ac.check()) {
      mtx_init(&display_ref->mtx, mtx_plain);
      {
        fbl::AutoLock lock(&display_ref->mtx);
        display_ref->display_device = this;
      }

      device_add_args_t args = {
          .version = DEVICE_ADD_ARGS_VERSION,
          .name = "backlight",
          .ctx = display_ref.get(),
          .ops = &kBacklightDeviceOps,
          .proto_id = ZX_PROTOCOL_BACKLIGHT,
      };

      status = device_add(controller_->zxdev(), &args, &backlight_device_);
      if (status == ZX_OK) {
        display_ref_ = display_ref.release();
      }
    }
    if (display_ref_ == nullptr) {
      zxlogf(WARNING, "Failed to add backlight (%d)", status);
    }

    std::ignore = SetBacklightState(true, 1.0);
  }
}

bool DisplayDevice::Resume() {
  if (pipe_) {
    if (!DdiModeset(info_)) {
      return false;
    }
    controller_->interrupts()->EnablePipeInterrupts(pipe_->pipe_id(), /*enable=*/true);
  }
  return pipe_ != nullptr;
}

void DisplayDevice::LoadActiveMode() {
  pipe_->LoadActiveMode(&info_);
  info_.pixel_clock_10khz = LoadClockRateForTranscoder(pipe_->connected_transcoder_id());
  zxlogf(INFO, "Active pixel clock: %u0 kHz", info_.pixel_clock_10khz);
}

bool DisplayDevice::CheckNeedsModeset(const display_mode_t* mode) {
  // Check the clock and the flags later
  size_t cmp_start = offsetof(display_mode_t, h_addressable);
  size_t cmp_end = offsetof(display_mode_t, flags);
  if (memcmp(&mode->h_addressable, &info_.h_addressable, cmp_end - cmp_start) != 0) {
    // Modeset is necessary if display params other than the clock frequency differ
    zxlogf(DEBUG, "Modeset necessary for display params");
    return true;
  }

  // TODO(stevensd): There are still some situations where the BIOS is better at setting up
  // the display than we are. The BIOS seems to not always set the hsync/vsync polarity, so
  // don't include that in the check for already initialized displays. Once we're better at
  // initializing displays, merge the flags check back into the above memcmp.
  if ((mode->flags & MODE_FLAG_INTERLACED) != (info_.flags & MODE_FLAG_INTERLACED)) {
    zxlogf(DEBUG, "Modeset necessary for display flags");
    return true;
  }

  if (mode->pixel_clock_10khz == info_.pixel_clock_10khz) {
    // Modeset is necessary not necessary if all display params are the same
    return false;
  }

  // Check to see if the hardware was already configured properly. The is primarily to
  // prevent unnecessary modesetting at startup. The extra work this adds to regular
  // modesetting is negligible.
  DdiPllConfig desired_pll_config =
      ComputeDdiPllConfig(static_cast<int32_t>(info_.pixel_clock_10khz));
  ZX_DEBUG_ASSERT_MSG(desired_pll_config.IsEmpty(),
                      "CheckDisplayMode() should have rejected unattainable pixel rates");
  return !controller()->dpll_manager()->DdiPllMatchesConfig(ddi_id(), desired_pll_config);
}

void DisplayDevice::ApplyConfiguration(const display_config_t* config,
                                       const config_stamp_t* config_stamp) {
  ZX_ASSERT(config);

  if (CheckNeedsModeset(&config->mode)) {
    if (pipe_) {
      // TODO(fxbug.dev/116009): When ApplyConfiguration() early returns on the
      // following error conditions, we should reset the DDI, pipe and transcoder
      // so that they can be possibly reused.
      if (!DdiModeset(config->mode)) {
        zxlogf(ERROR, "Display %lu: Modeset failed; ApplyConfiguration() aborted.", id());
        return;
      }

      if (!PipeConfigPreamble(config->mode, pipe_->pipe_id(), pipe_->connected_transcoder_id())) {
        zxlogf(ERROR,
               "Display %lu: Transcoder configuration failed before pipe setup; "
               "ApplyConfiguration() aborted.",
               id());
        return;
      }
      pipe_->ApplyModeConfig(config->mode);
      if (!PipeConfigEpilogue(config->mode, pipe_->pipe_id(), pipe_->connected_transcoder_id())) {
        zxlogf(ERROR,
               "Display %lu: Transcoder configuration failed after pipe setup; "
               "ApplyConfiguration() aborted.",
               id());
        return;
      }
    }
    info_ = config->mode;
  }

  if (pipe_) {
    pipe_->ApplyConfiguration(
        config, config_stamp,
        [controller = controller_](const image_t* image, uint32_t rotation) -> const GttRegion& {
          return controller->SetupGttImage(image, rotation);
        });
  }
}

void DisplayDevice::GetStateNormalized(GetStateNormalizedCompleter::Sync& completer) {
  zx::result<FidlBacklight::wire::State> backlight_state = zx::error(ZX_ERR_BAD_STATE);

  if (display_ref_ != nullptr) {
    fbl::AutoLock lock(&display_ref_->mtx);
    if (display_ref_->display_device != nullptr) {
      backlight_state = display_ref_->display_device->GetBacklightState();
    }
  }

  if (backlight_state.is_ok()) {
    completer.ReplySuccess(backlight_state.value());
  } else {
    completer.ReplyError(backlight_state.status_value());
  }
}

void DisplayDevice::SetStateNormalized(SetStateNormalizedRequestView request,
                                       SetStateNormalizedCompleter::Sync& completer) {
  zx::result<> status = zx::error(ZX_ERR_BAD_STATE);

  if (display_ref_ != nullptr) {
    fbl::AutoLock lock(&display_ref_->mtx);
    if (display_ref_->display_device != nullptr) {
      status = display_ref_->display_device->SetBacklightState(request->state.backlight_on,
                                                               request->state.brightness);
    }
  }

  if (status.is_error()) {
    completer.ReplyError(status.status_value());
    return;
  }
  completer.ReplySuccess();
}

void DisplayDevice::GetStateAbsolute(GetStateAbsoluteCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DisplayDevice::SetStateAbsolute(SetStateAbsoluteRequestView request,
                                     SetStateAbsoluteCompleter::Sync& completer) {
  FidlBacklight::wire::DeviceSetStateAbsoluteResult result;
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DisplayDevice::GetMaxAbsoluteBrightness(GetMaxAbsoluteBrightnessCompleter::Sync& completer) {
  FidlBacklight::wire::DeviceGetMaxAbsoluteBrightnessResult result;
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DisplayDevice::SetNormalizedBrightnessScale(
    SetNormalizedBrightnessScaleRequestView request,
    SetNormalizedBrightnessScaleCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void DisplayDevice::GetNormalizedBrightnessScale(
    GetNormalizedBrightnessScaleCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

}  // namespace i915
