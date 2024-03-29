// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/amlogic-display/vout.h"

#include <fidl/fuchsia.images2/cpp/wire.h>
#include <lib/device-protocol/display-panel.h>
#include <lib/inspect/cpp/inspect.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <memory>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>

#include "src/graphics/display/drivers/amlogic-display/panel-config.h"
#include "src/graphics/display/lib/api-types-cpp/display-id.h"
#include "src/graphics/display/lib/api-types-cpp/display-timing.h"

namespace amlogic_display {

namespace {

// List of supported features
struct supported_features_t {
  bool hpd;
};

constexpr supported_features_t kDsiSupportedFeatures = supported_features_t{
    .hpd = false,
};

constexpr supported_features_t kHdmiSupportedFeatures = supported_features_t{
    .hpd = true,
};

}  // namespace

Vout::Vout(std::unique_ptr<DsiHost> dsi_host, std::unique_ptr<Clock> dsi_clock, uint32_t width,
           uint32_t height, display_setting_t display_setting, inspect::Node node)
    : type_(VoutType::kDsi),
      supports_hpd_(kDsiSupportedFeatures.hpd),
      node_(std::move(node)),
      dsi_{
          .dsi_host = std::move(dsi_host),
          .clock = std::move(dsi_clock),
          .width = width,
          .height = height,
          .disp_setting = display_setting,
      } {
  node_.RecordInt("vout_type", static_cast<int>(type()));
}

Vout::Vout(std::unique_ptr<HdmiHost> hdmi_host, inspect::Node node)
    : type_(VoutType::kHdmi),
      supports_hpd_(kHdmiSupportedFeatures.hpd),
      node_(std::move(node)),
      hdmi_{.hdmi_host = std::move(hdmi_host)} {
  node_.RecordInt("vout_type", static_cast<int>(type()));
}

zx::result<std::unique_ptr<Vout>> Vout::CreateDsiVout(zx_device_t* parent, uint32_t panel_type,
                                                      uint32_t width, uint32_t height,
                                                      inspect::Node node) {
  zx::result<std::unique_ptr<DsiHost>> dsi_host_result = DsiHost::Create(parent, panel_type);
  if (dsi_host_result.is_error()) {
    zxlogf(ERROR, "Could not create DSI host: %s", dsi_host_result.status_string());
    return dsi_host_result.take_error();
  }
  std::unique_ptr<DsiHost> dsi_host = std::move(dsi_host_result).value();

  ddk::PDevFidl pdev;
  zx_status_t status = ddk::PDevFidl::FromFragment(parent, &pdev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Could not get PDEV protocol");
    return zx::error(status);
  }
  zx::result<std::unique_ptr<Clock>> clock_result = Clock::Create(pdev, kBootloaderDisplayEnabled);
  if (clock_result.is_error()) {
    zxlogf(ERROR, "Could not create Clock: %s", clock_result.status_string());
    return clock_result.take_error();
  }
  std::unique_ptr<Clock> clock = std::move(clock_result).value();

  zxlogf(INFO, "Fixed panel type is %d", dsi_host->panel_type());
  const display_setting_t* display_setting = GetPanelDisplaySetting(dsi_host->panel_type());
  if (display_setting == nullptr) {
    zxlogf(ERROR, "Unsupported panel (panel type %" PRIu32 ") detected!", panel_type);
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  fbl::AllocChecker alloc_checker;
  std::unique_ptr<Vout> vout =
      fbl::make_unique_checked<Vout>(&alloc_checker, std::move(dsi_host), std::move(clock), width,
                                     height, *display_setting, std::move(node));
  if (!alloc_checker.check()) {
    zxlogf(ERROR, "Failed to allocate memory for Vout.");
    return zx::error(ZX_ERR_NO_MEMORY);
  }
  return zx::ok(std::move(vout));
}

zx::result<std::unique_ptr<Vout>> Vout::CreateDsiVoutForTesting(uint32_t panel_type, uint32_t width,
                                                                uint32_t height) {
  const display_setting_t* display_setting = GetPanelDisplaySetting(panel_type);
  if (display_setting == nullptr) {
    zxlogf(ERROR, "Unsupported panel (panel type %" PRIu32 ") detected!", panel_type);
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  fbl::AllocChecker alloc_checker;
  std::unique_ptr<Vout> vout =
      fbl::make_unique_checked<Vout>(&alloc_checker,
                                     /*dsi_host=*/nullptr, /*dsi_clock=*/nullptr, width, height,
                                     *display_setting, inspect::Node{});
  if (!alloc_checker.check()) {
    zxlogf(ERROR, "Failed to allocate memory for Vout.");
    return zx::error(ZX_ERR_NO_MEMORY);
  }
  return zx::ok(std::move(vout));
}

zx::result<std::unique_ptr<Vout>> Vout::CreateHdmiVout(zx_device_t* parent, inspect::Node node) {
  zx::result<std::unique_ptr<HdmiHost>> hdmi_host_result = HdmiHost::Create(parent);
  if (hdmi_host_result.is_error()) {
    zxlogf(ERROR, "Could not create HDMI host: %s", hdmi_host_result.status_string());
    return hdmi_host_result.take_error();
  }

  fbl::AllocChecker alloc_checker;
  std::unique_ptr<Vout> vout = fbl::make_unique_checked<Vout>(
      &alloc_checker, std::move(hdmi_host_result).value(), std::move(node));
  if (!alloc_checker.check()) {
    zxlogf(ERROR, "Failed to allocate memory for Vout.");
    return zx::error(ZX_ERR_NO_MEMORY);
  }
  return zx::ok(std::move(vout));
}

void Vout::PopulateAddedDisplayArgs(
    added_display_args_t* args, display::DisplayId display_id,
    cpp20::span<const fuchsia_images2_pixel_format_enum_value_t> pixel_formats) {
  switch (type_) {
    case VoutType::kDsi: {
      args->display_id = display::ToBanjoDisplayId(display_id);
      args->panel_capabilities_source = PANEL_CAPABILITIES_SOURCE_DISPLAY_MODE;
      display::DisplayTiming display_timing = display::ToDisplayTiming(dsi_.disp_setting);
      args->panel.mode = display::ToBanjoDisplayMode(display_timing);
      args->pixel_format_list = pixel_formats.data();
      args->pixel_format_count = pixel_formats.size();
      args->cursor_info_count = 0;
      return;
    }
    case VoutType::kHdmi:
      args->display_id = display::ToBanjoDisplayId(display_id);
      args->panel_capabilities_source = PANEL_CAPABILITIES_SOURCE_EDID;
      args->panel.i2c.ops = &i2c_impl_protocol_ops_;
      args->panel.i2c.ctx = this;
      args->pixel_format_list = pixel_formats.data();
      args->pixel_format_count = pixel_formats.size();
      args->cursor_info_count = 0;
      return;
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

void Vout::DisplayConnected() {
  switch (type_) {
    case VoutType::kHdmi:
      // A new connected display is not yet set up with any display timing.
      hdmi_.current_display_timing_ = {};
      return;
    case VoutType::kDsi:
      return;
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

void Vout::DisplayDisconnected() {
  switch (type_) {
    case VoutType::kHdmi:
      hdmi_.hdmi_host->HostOff();
      return;
    case VoutType::kDsi:
      return;
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

zx::result<> Vout::PowerOff() {
  switch (type_) {
    case VoutType::kDsi: {
      dsi_.clock->Disable();
      dsi_.dsi_host->Disable(dsi_.disp_setting);
      return zx::ok();
    }
    case VoutType::kHdmi: {
      hdmi_.hdmi_host->HostOff();
      return zx::ok();
    }
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

zx::result<> Vout::PowerOn() {
  switch (type_) {
    case VoutType::kDsi: {
      zx::result<> clock_enable_result = dsi_.clock->Enable(dsi_.disp_setting);
      if (!clock_enable_result.is_ok()) {
        zxlogf(ERROR, "Could not enable display clocks: %s", clock_enable_result.status_string());
        return clock_enable_result;
      }

      dsi_.clock->SetVideoOn(false);
      // Configure and enable DSI host interface.
      zx::result<> dsi_host_enable_result =
          dsi_.dsi_host->Enable(dsi_.disp_setting, dsi_.clock->GetBitrate());
      if (!dsi_host_enable_result.is_ok()) {
        zxlogf(ERROR, "Could not enable DSI Host: %s", dsi_host_enable_result.status_string());
        return dsi_host_enable_result;
      }
      dsi_.clock->SetVideoOn(true);
      return zx::ok();
    }
    case VoutType::kHdmi: {
      zx::result<> hdmi_host_on_result = zx::make_result(hdmi_.hdmi_host->HostOn());
      if (!hdmi_host_on_result.is_ok()) {
        zxlogf(ERROR, "Could not enable HDMI host: %s", hdmi_host_on_result.status_string());
        return hdmi_host_on_result;
      }

      hdmi_.current_display_timing_ = {};
      return zx::ok();
    }
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

bool Vout::IsDisplayTimingSupported(const display::DisplayTiming& timing) {
  ZX_DEBUG_ASSERT_MSG(type_ == VoutType::kHdmi,
                      "Vout display timing check is only supported for HDMI output.");
  return hdmi_.hdmi_host->IsDisplayTimingSupported(timing);
}

zx::result<> Vout::ApplyConfiguration(const display::DisplayTiming& timing) {
  ZX_DEBUG_ASSERT_MSG(type_ == VoutType::kHdmi,
                      "Vout display timing setup is only supported for HDMI output.");
  zx_status_t status = hdmi_.hdmi_host->ModeSet(timing);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to set HDMI display timing: %s", zx_status_get_string(status));
    return zx::error(status);
  }

  hdmi_.current_display_timing_ = timing;
  return zx::ok();
}

zx_status_t Vout::I2cImplTransact(const i2c_impl_op_t* op_list, size_t op_count) {
  switch (type_) {
    case VoutType::kHdmi:
      return hdmi_.hdmi_host->EdidTransfer(op_list, op_count);
    case VoutType::kDsi:
      return ZX_ERR_NOT_SUPPORTED;
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

void Vout::Dump() {
  switch (type_) {
    case VoutType::kDsi:
      zxlogf(INFO, "#############################");
      zxlogf(INFO, "Dumping disp_setting structure:");
      zxlogf(INFO, "#############################");
      zxlogf(INFO, "h_active = 0x%x (%u)", dsi_.disp_setting.h_active, dsi_.disp_setting.h_active);
      zxlogf(INFO, "v_active = 0x%x (%u)", dsi_.disp_setting.v_active, dsi_.disp_setting.v_active);
      zxlogf(INFO, "h_period = 0x%x (%u)", dsi_.disp_setting.h_period, dsi_.disp_setting.h_period);
      zxlogf(INFO, "v_period = 0x%x (%u)", dsi_.disp_setting.v_period, dsi_.disp_setting.v_period);
      zxlogf(INFO, "hsync_width = 0x%x (%u)", dsi_.disp_setting.hsync_width,
             dsi_.disp_setting.hsync_width);
      zxlogf(INFO, "hsync_bp = 0x%x (%u)", dsi_.disp_setting.hsync_bp, dsi_.disp_setting.hsync_bp);
      zxlogf(INFO, "hsync_pol = 0x%x (%u)", dsi_.disp_setting.hsync_pol,
             dsi_.disp_setting.hsync_pol);
      zxlogf(INFO, "vsync_width = 0x%x (%u)", dsi_.disp_setting.vsync_width,
             dsi_.disp_setting.vsync_width);
      zxlogf(INFO, "vsync_bp = 0x%x (%u)", dsi_.disp_setting.vsync_bp, dsi_.disp_setting.vsync_bp);
      zxlogf(INFO, "vsync_pol = 0x%x (%u)", dsi_.disp_setting.vsync_pol,
             dsi_.disp_setting.vsync_pol);
      zxlogf(INFO, "lcd_clock = 0x%x (%u)", dsi_.disp_setting.lcd_clock,
             dsi_.disp_setting.lcd_clock);
      zxlogf(INFO, "lane_num = 0x%x (%u)", dsi_.disp_setting.lane_num, dsi_.disp_setting.lane_num);
      zxlogf(INFO, "bit_rate_max = 0x%x (%u)", dsi_.disp_setting.bit_rate_max,
             dsi_.disp_setting.bit_rate_max);
      return;
    case VoutType::kHdmi:
      zxlogf(INFO, "horizontal_active_px = %d", hdmi_.current_display_timing_.horizontal_active_px);
      zxlogf(INFO, "horizontal_front_porch_px = %d",
             hdmi_.current_display_timing_.horizontal_front_porch_px);
      zxlogf(INFO, "horizontal_sync_width_px = %d",
             hdmi_.current_display_timing_.horizontal_sync_width_px);
      zxlogf(INFO, "horizontal_back_porch_px = %d",
             hdmi_.current_display_timing_.horizontal_back_porch_px);
      zxlogf(INFO, "vertical_active_lines = %d",
             hdmi_.current_display_timing_.vertical_active_lines);
      zxlogf(INFO, "vertical_front_porch_lines = %d",
             hdmi_.current_display_timing_.vertical_front_porch_lines);
      zxlogf(INFO, "vertical_sync_width_lines = %d",
             hdmi_.current_display_timing_.vertical_sync_width_lines);
      zxlogf(INFO, "vertical_back_porch_lines = %d",
             hdmi_.current_display_timing_.vertical_back_porch_lines);
      zxlogf(INFO, "pixel_clock_frequency_khz = %d",
             hdmi_.current_display_timing_.pixel_clock_frequency_khz);
      zxlogf(INFO, "fields_per_frame (enum) = %u",
             static_cast<uint32_t>(hdmi_.current_display_timing_.fields_per_frame));
      zxlogf(INFO, "hsync_polarity (enum) = %u",
             static_cast<uint32_t>(hdmi_.current_display_timing_.hsync_polarity));
      zxlogf(INFO, "vsync_polarity (enum) = %u",
             static_cast<uint32_t>(hdmi_.current_display_timing_.vsync_polarity));
      zxlogf(INFO, "vblank_alternates = %d", hdmi_.current_display_timing_.vblank_alternates);
      zxlogf(INFO, "pixel_repetition = %d", hdmi_.current_display_timing_.pixel_repetition);
      return;
  }
  ZX_ASSERT_MSG(false, "Invalid Vout type: %u", static_cast<uint8_t>(type_));
}

}  // namespace amlogic_display
