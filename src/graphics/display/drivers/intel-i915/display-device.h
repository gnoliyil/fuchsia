// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_DISPLAY_DEVICE_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_DISPLAY_DEVICE_H_

#include <fidl/fuchsia.hardware.backlight/cpp/wire.h>
#include <fuchsia/hardware/display/controller/c/banjo.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/vmo.h>

#include <ddktl/device.h>
#include <region-alloc/region-alloc.h>

#include "src/graphics/display/drivers/intel-i915/gtt.h"
#include "src/graphics/display/drivers/intel-i915/pipe.h"
#include "src/graphics/display/drivers/intel-i915/power.h"
#include "src/graphics/display/drivers/intel-i915/registers-ddi.h"
#include "src/graphics/display/drivers/intel-i915/registers-pipe.h"
#include "src/graphics/display/drivers/intel-i915/registers-transcoder.h"

namespace i915 {

class Controller;
class DisplayDevice;
namespace FidlBacklight = fuchsia_hardware_backlight;

// Thread safe weak-ref to the DisplayDevice, because the backlight device
// lifecycle is managed by devmgr but the DisplayDevice lifecycle is managed
// by the display controller class.
typedef struct display_ref {
  mtx_t mtx;
  DisplayDevice* display_device __TA_GUARDED(mtx);
} display_ref_t;

class DisplayDevice : public fidl::WireServer<FidlBacklight::Device> {
 public:
  DisplayDevice(Controller* device, uint64_t id, registers::Ddi ddi);
  virtual ~DisplayDevice();

  bool AttachPipe(Pipe* pipe);
  void ApplyConfiguration(const display_config_t* config, const config_stamp_t* config_stamp);

  // TODO(fxbug.dev/86038): Initialization-related interactions between the Controller class and
  // DisplayDevice can currently take different paths, with Init() being called conditionally in
  // some cases (e.g. if the display has already been configured and powered up by the bootloader),
  // which means a DisplayDevice can hold many states before being considered fully-initialized.
  // It would be good to simplify this by:
  // 1. Eliminating the "partially initialized" DisplayDevice state from the point of its owner.
  // 2. Having a single Init factory function with options, such as the current DPLL state, which is
  // always called to construct a DisplayDevice, possibly merging Query, Init, InitWithDpllState,
  // and InitBacklight, into a single routine.
  // 3. Perhaps what represents a DDI and a display attached to a DDI should be separate
  // abstractions?

  // Query whether or not there is a display attached to this ddi. Does not
  // actually do any initialization - that is done by Init.
  virtual bool Query() = 0;
  // Does display mode agnostic ddi initialization - subclasses implement InitDdi.
  bool Init();
  // Initialize the display based on existing hardware state. This method should be used instead of
  // Init() when a display PLL has already been powered up and configured (e.g. by the bootlader)
  // when the driver discovers the display. DDI initialization will not be performed in this case.
  virtual void InitWithDpllState(struct dpll_state* dpll_state) {}
  // Initializes the display backlight for an already initialized display.
  void InitBacklight();
  // Resumes the ddi after suspend.
  bool Resume();
  // Loads ddi state from the hardware at driver startup.
  void LoadActiveMode();
  // Method to allow the display device to handle hotplug events. Returns
  // true if the device can handle the event without disconnecting. Otherwise
  // the device will be removed.
  virtual bool HandleHotplug(bool long_pulse) { return false; }

  uint64_t id() const { return id_; }
  registers::Ddi ddi() const { return ddi_; }
  Controller* controller() { return controller_; }

  virtual uint32_t i2c_bus_id() const = 0;

  Pipe* pipe() const { return pipe_; }

  bool is_hdmi() const { return is_hdmi_; }
  void set_is_hdmi(bool is_hdmi) { is_hdmi_ = is_hdmi; }

  virtual bool HasBacklight() { return false; }
  virtual zx_status_t SetBacklightState(bool power, double brightness) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  virtual zx_status_t GetBacklightState(bool* power, double* brightness) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  virtual bool CheckPixelRate(uint64_t pixel_rate) = 0;

  // FIDL calls
  void GetStateNormalized(GetStateNormalizedRequestView request,
                          GetStateNormalizedCompleter::Sync& completer) override;
  void SetStateNormalized(SetStateNormalizedRequestView request,
                          SetStateNormalizedCompleter::Sync& completer) override;
  void GetStateAbsolute(GetStateAbsoluteRequestView request,
                        GetStateAbsoluteCompleter::Sync& completer) override;
  void SetStateAbsolute(SetStateAbsoluteRequestView request,
                        SetStateAbsoluteCompleter::Sync& completer) override;
  void GetMaxAbsoluteBrightness(GetMaxAbsoluteBrightnessRequestView request,
                                GetMaxAbsoluteBrightnessCompleter::Sync& completer) override;
  void SetNormalizedBrightnessScale(
      SetNormalizedBrightnessScaleRequestView request,
      SetNormalizedBrightnessScaleCompleter::Sync& completer) override;
  void GetNormalizedBrightnessScale(
      GetNormalizedBrightnessScaleRequestView request,
      GetNormalizedBrightnessScaleCompleter::Sync& completer) override;

 protected:
  // Attempts to initialize the ddi.
  virtual bool InitDdi() = 0;
  virtual bool InitBacklightHw() { return false; }

  // Configures the hardware to display content at the given resolution.
  virtual bool DdiModeset(const display_mode_t& mode, registers::Pipe pipe,
                          registers::Trans trans) = 0;
  virtual bool ComputeDpllState(uint32_t pixel_clock_10khz, struct dpll_state* config) = 0;
  // Load the clock rate from hardware if it's necessary when changing the transcoder.
  virtual uint32_t LoadClockRateForTranscoder(registers::Trans transcoder) = 0;

  // Attaching a pipe to a display or configuring a pipe after display mode change has
  // 3 steps. The second step is generic pipe configuration, whereas PipeConfigPreamble
  // and PipeConfigEpilogue are responsible for display-type-specific configuration that
  // must be done before and after the generic configuration.
  virtual bool PipeConfigPreamble(const display_mode_t& mode, registers::Pipe pipe,
                                  registers::Trans trans) = 0;
  virtual bool PipeConfigEpilogue(const display_mode_t& mode, registers::Pipe pipe,
                                  registers::Trans trans) = 0;

  fdf::MmioBuffer* mmio_space() const;

 private:
  bool CheckNeedsModeset(const display_mode_t* mode);

  // Borrowed reference to Controller instance
  Controller* controller_;

  uint64_t id_;
  registers::Ddi ddi_;

  Pipe* pipe_ = nullptr;

  PowerWellRef ddi_power_;

  bool inited_ = false;
  display_mode_t info_ = {};
  bool is_hdmi_ = false;

  zx_device_t* backlight_device_ = nullptr;
  display_ref_t* display_ref_ = nullptr;
};

}  // namespace i915

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_DISPLAY_DEVICE_H_
