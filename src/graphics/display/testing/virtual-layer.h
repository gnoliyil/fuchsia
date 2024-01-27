// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_TESTING_VIRTUAL_LAYER_H_
#define SRC_GRAPHICS_DISPLAY_TESTING_VIRTUAL_LAYER_H_

#include <fidl/fuchsia.hardware.display/cpp/wire.h>
#include <fidl/fuchsia.images2/cpp/wire.h>
#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <lib/zx/channel.h>
#include <zircon/types.h>

#include "display.h"
#include "image.h"

namespace testing {
namespace display {

typedef struct custom_layer {
  uint64_t id;
  bool active;

  bool done;

  fuchsia_hardware_display::wire::Frame src;
  fuchsia_hardware_display::wire::Frame dest;

  image_import_t import_info[2];
} custom_layer_t;

// A layer whose output can appear on multiple displays.
class VirtualLayer {
 public:
  typedef fuchsia_hardware_display::Controller Controller;

  explicit VirtualLayer(Display* display);
  explicit VirtualLayer(const fbl::Vector<Display>& displays, bool tiled = true);

  virtual ~VirtualLayer() {}

  // Finish initializing the layer. All Set* methods should be called before this.
  virtual bool Init(const fidl::WireSyncClient<Controller>& dc) = 0;

  // Steps the local layout state to frame_num.
  virtual void StepLayout(int32_t frame_num) = 0;

  // Waits for the display controller to be done with the previous version of this frame.
  virtual bool WaitForReady() = 0;

  // Sets the current layout to the display controller.
  virtual void SendLayout(const fidl::WireSyncClient<Controller>& dc) = 0;

  // Renders the current frame (and signals the fence if necessary).
  virtual void Render(int32_t frame_num) = 0;

  virtual void* GetCurrentImageBuf() = 0;
  virtual size_t GetCurrentImageSize() = 0;

  // Gets the display controller layer ID for usage on the given display.
  uint64_t id(uint64_t display_id) const {
    for (unsigned i = 0; i < displays_.size(); i++) {
      if (displays_[i]->id() == display_id && layers_[i].active) {
        return layers_[i].id;
      }
    }
    return fuchsia_hardware_display::wire::kInvalidDispId;
  }

  // Gets the ID of the image on the given display.
  virtual uint64_t image_id(uint64_t display_id) const = 0;

  void set_frame_done(uint64_t display_id) {
    for (unsigned i = 0; i < displays_.size(); i++) {
      if (displays_[i]->id() == display_id) {
        layers_[i].done = true;
      }
    }
  }

  virtual bool is_done() const {
    bool done = true;
    for (unsigned i = 0; i < displays_.size(); i++) {
      done &= !layers_[i].active || layers_[i].done;
    }
    return done;
  }

  void clear_done() {
    for (unsigned i = 0; i < displays_.size(); i++) {
      layers_[i].done = false;
    }
  }

 protected:
  custom_layer_t* CreateLayer(const fidl::WireSyncClient<Controller>& dc);
  void SetLayerImages(const fidl::WireSyncClient<Controller>& dc, bool alt_image);

  fbl::Vector<Display*> displays_;
  fbl::Vector<custom_layer_t> layers_;

  uint32_t width_;
  uint32_t height_;
};

class PrimaryLayer : public VirtualLayer {
 public:
  explicit PrimaryLayer(Display* display);
  explicit PrimaryLayer(const fbl::Vector<Display>& displays, bool mirrors = false);
  explicit PrimaryLayer(const fbl::Vector<Display>& displays, Image::Pattern pattern,
                        uint32_t fgcolor, uint32_t bgcolor, bool mirrors = false);

  // Set* methods to configure the layer.
  void SetImageDimens(uint32_t width, uint32_t height) {
    image_width_ = width;
    image_height_ = height;

    src_frame_.width = width;
    src_frame_.height = height;
    dest_frame_.width = width;
    dest_frame_.height = height;
  }
  void SetSrcFrame(uint32_t width, uint32_t height) {
    src_frame_.width = width;
    src_frame_.height = height;
  }
  void SetDestFrame(uint32_t width, uint32_t height) {
    dest_frame_.width = width;
    dest_frame_.height = height;
  }
  void SetLayerFlipping(bool flip) { layer_flipping_ = flip; }
  void SetPanSrc(bool pan) { pan_src_ = pan; }
  void SetPanDest(bool pan) { pan_dest_ = pan; }
  void SetLayerToggle(bool toggle) { layer_toggle_ = toggle; }
  void SetRotates(bool rotates) { rotates_ = rotates; }
  void SetAlpha(bool enable, float val) {
    alpha_enable_ = enable;
    alpha_val_ = val;
  }
  void SetScaling(bool enable) { scaling_ = enable; }
  void SetImageFormat(fuchsia_images2::wire::PixelFormat image_format) {
    image_format_ = image_format;
  }
  void SetFormatModifier(uint64_t modifier) { modifier_ = modifier; }

  bool Init(const fidl::WireSyncClient<Controller>& dc) override;
  void StepLayout(int32_t frame_num) override;
  bool WaitForReady() override;
  void SendLayout(const fidl::WireSyncClient<Controller>& channel) override;
  void Render(int32_t frame_num) override;

  void* GetCurrentImageBuf() override;
  size_t GetCurrentImageSize() override;

  uint64_t image_id(uint64_t display_id) const override {
    for (unsigned i = 0; i < displays_.size(); i++) {
      if (displays_[i]->id() == display_id && layers_[i].active) {
        return layers_[i].import_info[alt_image_].id;
      }
    }
    return fuchsia_hardware_display::wire::kInvalidDispId;
  }

 private:
  void SetLayerPositions(const fidl::WireSyncClient<Controller>& dc);
  bool Wait(uint32_t idx);
  void InitImageDimens();

  uint32_t image_width_ = 0;
  uint32_t image_height_ = 0;
  fuchsia_images2::wire::PixelFormat image_format_ = fuchsia_images2::wire::PixelFormat::kInvalid;
  bool override_colors_ = false;

  Image::Pattern image_pattern_ = Image::Pattern::kCheckerboard;
  uint32_t fgcolor_;
  uint32_t bgcolor_;

  fuchsia_hardware_display::wire::Frame src_frame_ = {};
  fuchsia_hardware_display::wire::Frame dest_frame_ = {};
  typedef fuchsia_hardware_display::wire::Transform Transform;
  Transform rotation_ = Transform::kIdentity;
  bool layer_flipping_ = false;
  bool pan_src_ = false;
  bool pan_dest_ = false;
  bool layer_toggle_ = false;
  bool rotates_ = false;
  bool alpha_enable_ = false;
  float alpha_val_ = 0.f;
  bool scaling_ = false;
  uint64_t modifier_ = fuchsia_sysmem::wire::kFormatModifierLinear;
  bool mirrors_ = false;

  bool alt_image_ = false;
  Image* images_[2];
};

class CursorLayer : public VirtualLayer {
 public:
  explicit CursorLayer(Display* display);
  explicit CursorLayer(const fbl::Vector<Display>& displays);

  bool Init(const fidl::WireSyncClient<Controller>& dc) override;
  void StepLayout(int32_t frame_num) override;
  void SendLayout(const fidl::WireSyncClient<Controller>& dc) override;

  bool WaitForReady() override { return true; }
  void Render(int32_t frame_num) override {}

  void* GetCurrentImageBuf() override { return nullptr; }
  size_t GetCurrentImageSize() override { return 0; }

  uint64_t image_id(uint64_t display_id) const override {
    for (unsigned i = 0; i < displays_.size(); i++) {
      if (displays_[i]->id() == display_id && layers_[i].active) {
        return layers_[i].import_info[0].id;
      }
    }
    return fuchsia_hardware_display::wire::kInvalidDispId;
  }

 private:
  uint32_t x_pos_ = 0;
  uint32_t y_pos_ = 0;

  Image* image_;
};

class ColorLayer : public VirtualLayer {
 public:
  explicit ColorLayer(Display* display);
  explicit ColorLayer(const fbl::Vector<Display>& displays);

  bool Init(const fidl::WireSyncClient<Controller>& dc) override;

  void SendLayout(const fidl::WireSyncClient<Controller>& dc) override {}
  void StepLayout(int32_t frame_num) override {}
  bool WaitForReady() override { return true; }
  void Render(int32_t frame_num) override {}
  void* GetCurrentImageBuf() override { return nullptr; }
  size_t GetCurrentImageSize() override { return 0; }
  uint64_t image_id(uint64_t display_id) const override {
    return fuchsia_hardware_display::wire::kInvalidDispId;
  }
  virtual bool is_done() const override { return true; }
};

}  // namespace display
}  // namespace testing

#endif  // SRC_GRAPHICS_DISPLAY_TESTING_VIRTUAL_LAYER_H_
