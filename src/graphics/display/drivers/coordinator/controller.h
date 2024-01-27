// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CONTROLLER_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CONTROLLER_H_

#include <fidl/fuchsia.hardware.display/cpp/wire.h>
#include <fuchsia/hardware/audiotypes/c/banjo.h>
#include <fuchsia/hardware/display/clamprgb/cpp/banjo.h>
#include <fuchsia/hardware/display/controller/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <queue>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/array.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_hash_table.h>
#include <fbl/vector.h>

#include "src/graphics/display/drivers/coordinator/capture-image.h"
#include "src/graphics/display/drivers/coordinator/client-id.h"
#include "src/graphics/display/drivers/coordinator/display-info.h"
#include "src/graphics/display/drivers/coordinator/id-map.h"
#include "src/graphics/display/drivers/coordinator/image.h"
#include "src/graphics/display/drivers/coordinator/migration-util.h"
#include "src/graphics/display/lib/api-types-cpp/capture-image-id.h"
#include "src/graphics/display/lib/api-types-cpp/config-stamp.h"
#include "src/graphics/display/lib/api-types-cpp/display-id.h"
#include "src/graphics/display/lib/api-types-cpp/driver-buffer-collection-id.h"
#include "src/graphics/display/lib/api-types-cpp/driver-capture-image-id.h"
#include "src/graphics/display/lib/edid/edid.h"
#include "src/lib/async-watchdog/watchdog.h"

namespace display {

class ClientProxy;
class Controller;
class ControllerTest;
class DisplayConfig;
class IntegrationTest;

using DeviceType = ddk::Device<Controller, ddk::Unbindable,
                               ddk::Messageable<fuchsia_hardware_display::Provider>::Mixin>;

// Multiplexes between display controller clients and display engine drivers.
class Controller : public DeviceType,
                   public ddk::DisplayControllerInterfaceProtocol<Controller>,
                   public ddk::DisplayCaptureInterfaceProtocol<Controller>,
                   public ddk::EmptyProtocol<ZX_PROTOCOL_DISPLAY_COORDINATOR> {
 public:
  // Creates a new coordinator Controller device. It creates a new Inspector
  // which will be solely owned by the Controller device.
  explicit Controller(zx_device_t* parent);

  // Creates a new coordinator Controller device with an injected `inspector`.
  // The `inspector` and inspect data may be duplicated and shared.
  Controller(zx_device_t* parent, inspect::Inspector inspector);

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  ~Controller() override;

  static void PopulateDisplayMode(const edid::timing_params_t& params, display_mode_t* mode);

  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t Bind(std::unique_ptr<display::Controller>* device_ptr);

  void DisplayControllerInterfaceOnDisplaysChanged(const added_display_args_t* displays_added,
                                                   size_t added_count,
                                                   const uint64_t* displays_removed,
                                                   size_t removed_count,
                                                   added_display_info_t* out_display_info_list,
                                                   size_t display_info_count,
                                                   size_t* display_info_actual);
  void DisplayControllerInterfaceOnDisplayVsync(uint64_t banjo_display_id, zx_time_t timestamp,
                                                const config_stamp_t* config_stamp);
  zx_status_t DisplayControllerInterfaceGetAudioFormat(
      uint64_t banjo_display_id, uint32_t fmt_idx,
      audio_types_audio_stream_format_range_t* fmt_out);

  void DisplayCaptureInterfaceOnCaptureComplete();
  void OnClientDead(ClientProxy* client);
  void SetVcMode(fuchsia_hardware_display::wire::VirtconMode mode);
  void ShowActiveDisplay();

  void ApplyConfig(DisplayConfig* configs[], int32_t count, bool vc_client,
                   ConfigStamp config_stamp, uint32_t layer_stamp, ClientId client_id)
      __TA_EXCLUDES(mtx());

  void ReleaseImage(Image* image);
  void ReleaseCaptureImage(DriverCaptureImageId driver_capture_image_id);

  // |mtx()| must be held for as long as |edid| and |params| are retained.
  bool GetPanelConfig(DisplayId display_id, const fbl::Vector<edid::timing_params_t>** timings,
                      const display_params_t** params) __TA_REQUIRES(mtx());

  zx::result<fbl::Array<CoordinatorPixelFormat>> GetSupportedPixelFormats(DisplayId display_id)
      __TA_REQUIRES(mtx());

  zx::result<fbl::Array<CoordinatorCursorInfo>> GetCursorInfo(DisplayId display_id)
      __TA_REQUIRES(mtx());
  bool GetDisplayIdentifiers(DisplayId display_id, const char** manufacturer_name,
                             const char** monitor_name, const char** monitor_serial)
      __TA_REQUIRES(mtx());
  bool GetDisplayPhysicalDimensions(DisplayId display_id, uint32_t* horizontal_size_mm,
                                    uint32_t* vertical_size_mm) __TA_REQUIRES(mtx());
  ddk::DisplayControllerImplProtocolClient* dc() { return &dc_; }
  ddk::DisplayClampRgbImplProtocolClient* dc_clamp_rgb() {
    if (dc_clamp_rgb_.is_valid()) {
      return &dc_clamp_rgb_;
    }
    return nullptr;
  }
  bool supports_capture() { return supports_capture_; }

  async::Loop& loop() { return loop_; }
  bool current_thread_is_loop() { return thrd_current() == loop_thread_; }
  // Thread-safety annotations currently don't deal with pointer aliases. Use this to document
  // places where we believe a mutex aliases mtx()
  void AssertMtxAliasHeld(mtx_t* m) __TA_ASSERT(m) { ZX_DEBUG_ASSERT(m == mtx()); }
  mtx_t* mtx() const { return &mtx_; }

  // Test helpers
  size_t TEST_imported_images_count() const;
  ConfigStamp TEST_controller_stamp() const;

  // Typically called by OpenController/OpenVirtconController.  However, this is made public
  // for use by testing services which provide a fake display controller.
  zx_status_t CreateClient(bool is_vc,
                           fidl::ServerEnd<fuchsia_hardware_display::Coordinator> client,
                           fit::function<void()> on_client_dead = nullptr);

  display::DriverBufferCollectionId GetNextDriverBufferCollectionId();

 private:
  friend ControllerTest;
  friend IntegrationTest;

  void HandleClientOwnershipChanges() __TA_REQUIRES(mtx());
  void PopulateDisplayTimings(const fbl::RefPtr<DisplayInfo>& info) __TA_EXCLUDES(mtx());

  void OpenCoordinatorForVirtcon(OpenCoordinatorForVirtconRequestView request,
                                 OpenCoordinatorForVirtconCompleter::Sync& _completer) override;
  void OpenCoordinatorForPrimary(OpenCoordinatorForPrimaryRequestView request,
                                 OpenCoordinatorForPrimaryCompleter::Sync& _completer) override;

  // Periodically reads |last_vsync_timestamp_| and increments |vsync_stalls_detected_| if no vsync
  // has been observed in a given time period.
  void OnVsyncMonitor();

  inspect::Inspector inspector_;
  // Currently located at bootstrap/driver_manager:root/display.
  inspect::Node root_;

  // mtx_ is a global lock on state shared among clients.
  mutable mtx_t mtx_;
  bool unbinding_ __TA_GUARDED(mtx()) = false;

  bool kernel_framebuffer_released_ = false;

  DisplayInfo::Map displays_ __TA_GUARDED(mtx());
  bool vc_applied_ = false;
  uint32_t applied_layer_stamp_ = UINT32_MAX;
  ClientId applied_client_id_ = kInvalidClientId;
  DriverCaptureImageId pending_release_capture_image_id_ = kInvalidDriverCaptureImageId;

  bool supports_capture_ = false;

  display::DriverBufferCollectionId next_driver_buffer_collection_id_ __TA_GUARDED(mtx()) =
      display::DriverBufferCollectionId(1);
  ClientId next_client_id_ __TA_GUARDED(mtx()) = ClientId(1);
  ClientProxy* vc_client_ __TA_GUARDED(mtx()) = nullptr;
  bool vc_ready_ __TA_GUARDED(mtx());
  ClientProxy* primary_client_ __TA_GUARDED(mtx()) = nullptr;
  bool primary_ready_ __TA_GUARDED(mtx());
  fuchsia_hardware_display::wire::VirtconMode vc_mode_ __TA_GUARDED(mtx()) =
      fuchsia_hardware_display::wire::VirtconMode::kInactive;
  ClientProxy* active_client_ __TA_GUARDED(mtx()) = nullptr;

  async::Loop loop_;
  thrd_t loop_thread_;
  async_watchdog::Watchdog watchdog_;
  ddk::DisplayControllerImplProtocolClient dc_;
  ddk::DisplayClampRgbImplProtocolClient dc_clamp_rgb_;

  std::list<std::unique_ptr<ClientProxy>> clients_ __TA_GUARDED(mtx());

  std::atomic<zx::time> last_vsync_timestamp_{};
  inspect::UintProperty last_vsync_ns_property_;
  inspect::UintProperty last_vsync_interval_ns_property_;
  inspect::UintProperty last_vsync_config_stamp_property_;

  // Fields that track how often vsync was detected to have been stalled.
  std::atomic_bool vsync_stalled_ = false;
  inspect::UintProperty vsync_stalls_detected_;
  async::TaskClosureMethod<Controller, &Controller::OnVsyncMonitor> vsync_monitor_{this};

  zx_time_t last_valid_apply_config_timestamp_{};
  inspect::UintProperty last_valid_apply_config_timestamp_ns_property_;
  inspect::UintProperty last_valid_apply_config_interval_ns_property_;
  inspect::UintProperty last_valid_apply_config_config_stamp_property_;

  ConfigStamp controller_stamp_ __TA_GUARDED(mtx()) = kInvalidConfigStamp;
};

}  // namespace display

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CONTROLLER_H_
