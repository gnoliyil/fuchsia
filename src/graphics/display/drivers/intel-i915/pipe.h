// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_PIPE_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_PIPE_H_

#include <fuchsia/hardware/display/controller/c/banjo.h>
#include <lib/fit/function.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/mmio/mmio.h>
#include <lib/sysmem-version/sysmem-version.h>
#include <lib/zx/vmo.h>

#include <cstdint>
#include <list>
#include <unordered_map>

#include <ddktl/device.h>
#include <region-alloc/region-alloc.h>

#include "src/graphics/display/drivers/intel-i915/gtt.h"
#include "src/graphics/display/drivers/intel-i915/hardware-common.h"
#include "src/graphics/display/drivers/intel-i915/power.h"
#include "src/graphics/display/drivers/intel-i915/registers-ddi.h"
#include "src/graphics/display/drivers/intel-i915/registers-pipe.h"
#include "src/graphics/display/drivers/intel-i915/registers-transcoder.h"

namespace i915 {

class Controller;
class DisplayDevice;

class Pipe {
 public:
  Pipe(fdf::MmioBuffer* mmio_space, registers::Platform platform, PipeId pipe_id,
       PowerWellRef pipe_power);
  virtual ~Pipe() = default;

  Pipe(const Pipe&) = delete;
  Pipe(Pipe&&) = delete;
  Pipe& operator=(const Pipe&) = delete;
  Pipe& operator=(Pipe&&) = delete;

  void AttachToDisplay(uint64_t display_id, bool is_edp);
  void Detach();

  void ApplyModeConfig(const display_mode_t& mode);

  using GetImagePixelFormatFunc = fit::function<PixelFormatAndModifier(const image_t* image)>;
  using SetupGttImageFunc =
      fit::function<const GttRegion&(const image_t* image, uint32_t rotation)>;
  void ApplyConfiguration(const display_config_t* config, const config_stamp_t* config_stamp,
                          const SetupGttImageFunc& setup_gtt_image,
                          const GetImagePixelFormatFunc& get_pixel_format);

  // Reset pipe registers and transcoders.
  void Reset();

  // Reset the pipe planes (layers).
  void ResetPlanes();

  // Resets the transcoder identified by `transcoder_id`.
  static void ResetTranscoder(TranscoderId transcoder_id, registers::Platform platform,
                              fdf::MmioBuffer* mmio_space);

  void LoadActiveMode(display_mode_t* mode);

  PipeId pipe_id() const { return pipe_id_; }

  // Identifies the transcoder that is always tied to the pipe.
  //
  // Each pipe has a transcoder tied to it, which can output most display
  // protocols (DisplayPort, HDMI, DVI). This method identifies the pipe's tied
  // transcoder. The return value never changes, for a given pipe.
  //
  // See `connected_transcoder_id()` for identifying the transcoder that the
  // pipe is currently using.
  TranscoderId tied_transcoder_id() const { return static_cast<TranscoderId>(pipe_id_); }

  // Identifies the transcoder that is currently receiving the pipe's output.
  //
  // Each pipe has a tied transcoder, which can output most display protocols.
  // The display engine also has some specialized transcoders, which can be
  // connected to any pipe. The specialized transcoders are tied to DDIs that
  // use specialized protocols (Embedded DisplayPort, DDI), and used for writing
  // back to memory ("WD / Wireless Display" in Intel's docs).
  //
  // This method returns the transcoder that is currently connected to the pipe
  // output, which can be the general-purpose transcoder tied to the pipe, or
  // one of the shared specialized transcoders. The return value depends on how
  // we configure the display engine.
  virtual TranscoderId connected_transcoder_id() const = 0;

  uint64_t attached_display_id() const { return attached_display_; }
  bool in_use() const { return attached_display_ != INVALID_DISPLAY_ID; }

  // Display device registers only store image handles / addresses. We should
  // convert the handles to corresponding config stamps using the existing
  // mapping updated in |ApplyConfig()|.
  std::optional<config_stamp_t> GetVsyncConfigStamp(const std::vector<uint64_t>& image_handles);

 protected:
  bool attached_edp() const { return attached_edp_; }
  registers::Platform platform() const { return platform_; }

 private:
  void ConfigurePrimaryPlane(uint32_t plane_num, const primary_layer_t* primary, bool enable_csc,
                             bool* scaler_1_claimed, registers::pipe_arming_regs* regs,
                             uint64_t config_stamp_seqno, const SetupGttImageFunc& setup_gtt_image,
                             const GetImagePixelFormatFunc& get_pixel_format);
  void ConfigureCursorPlane(const cursor_layer_t* cursor, bool enable_csc,
                            registers::pipe_arming_regs* regs, uint64_t config_stamp_seqno);
  void SetColorConversionOffsets(bool preoffsets, const float vals[3]);
  void ResetActiveTranscoder();
  void ResetScaler();

  // Borrowed reference to Controller instance
  fdf::MmioBuffer* mmio_space_ = nullptr;

  uint64_t attached_display_ = INVALID_DISPLAY_ID;
  bool attached_edp_ = false;

  registers::Platform platform_;
  PipeId pipe_id_;

  PowerWellRef pipe_power_;

  // For any scaled planes, this contains the (1-based) index of the active scaler
  uint32_t scaled_planes_[PipeIds<registers::Platform::kKabyLake>().size()]
                         [registers::kImagePlaneCount] = {};

  // On each Vsync, the driver should return the stamp of the *oldest*
  // configuration that has been fully applied to the device. We use the
  // following way to keep track of images and config stamps:
  //
  // Config stamps can be of random values (per definition in display Controller
  // banjo protocol), so while we keep all the stamps in a linked list sorted
  // chronologically, we also keep a sequence number of the first config stamp
  // in the list.
  //
  // Every time a config is applied, a new stamp will be added to the list. An
  // config stamp is removed from the list when it is older than all the current
  // config stamps used in the display layers. In this case, the front old
  // stamps will be removed and |config_stamps_front_seqno_| will be updated
  // accordingly.
  //
  // A linked list of configuration stamps in chronological order.
  // Unused configuration stamps will be evicted from the list.
  std::list<config_stamp_t> config_stamps_;

  // Consecutive sequence numbers are assigned to each configuration applied to
  // the device; this keeps track the seqno of the front (oldest configuration)
  // that is still in the linked list |config_stamps_|.
  // If no configuration has been applied to the device, it stores
  // |std::nullopt|.
  // TODO(fxbug.dev/126254): Remove once the config stamps are guaranteed to be
  // in strictly increasing order.
  std::optional<int64_t> sequence_number_of_config_stamps_front;

  // The pipe registers only store the handle (address) of the images that are
  // being displayed. In order to get the config stamp for each layer and for
  // each configuration, we need to keep a mapping from *image handle* to the
  // *seqno of the configuration* so that we can know which layer has the oldest
  // configuration.
  // TODO(fxbug.dev/126254): Remove once the config stamps are guaranteed to be
  // in strictly increasing order.
  std::unordered_map<uintptr_t, int64_t> sequence_number_of_latest_config_stamp_with_image_;
};

class PipeSkylake : public Pipe {
 public:
  PipeSkylake(fdf::MmioBuffer* mmio_space, PipeId pipe_id, PowerWellRef pipe_power)
      : Pipe(mmio_space, registers::Platform::kSkylake, pipe_id, std::move(pipe_power)) {}
  ~PipeSkylake() override = default;

  TranscoderId connected_transcoder_id() const override {
    return attached_edp() ? TranscoderId::TRANSCODER_EDP : tied_transcoder_id();
  }
};

class PipeTigerLake : public Pipe {
 public:
  PipeTigerLake(fdf::MmioBuffer* mmio_space, PipeId pipe_id, PowerWellRef pipe_power)
      : Pipe(mmio_space, registers::Platform::kTigerLake, pipe_id, std::move(pipe_power)) {}
  ~PipeTigerLake() override = default;

  TranscoderId connected_transcoder_id() const override { return tied_transcoder_id(); }
};

}  // namespace i915

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_PIPE_H_
