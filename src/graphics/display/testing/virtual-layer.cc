// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "virtual-layer.h"

#include <fidl/fuchsia.hardware.display/cpp/wire.h>
#include <fidl/fuchsia.images2/cpp/wire_types.h>
#include <lib/image-format/image_format.h>
#include <lib/sysmem-version/sysmem-version.h>
#include <math.h>
#include <stdio.h>
#include <zircon/pixelformat.h>

#include <algorithm>
#include <iterator>

#include <fbl/algorithm.h>

#include "utils.h"

namespace fhd = fuchsia_hardware_display;

namespace testing {
namespace display {

static constexpr uint32_t kSrcFrameBouncePeriod = 90;
static constexpr uint32_t kDestFrameBouncePeriod = 60;
static constexpr uint32_t kRotationPeriod = 24;
static constexpr uint32_t kScalePeriod = 45;

namespace {

constexpr uint64_t GetPrimaryImageImportId(unsigned display_index, int image_index) {
  ZX_ASSERT(image_index >= 0);
  ZX_ASSERT(image_index <= 1);
  return 1 + display_index * 3 + image_index;
}

constexpr uint64_t GetCursorImageImportId(unsigned display_index) {
  return 1 + display_index * 3 + 2;
}

}  // namespace

static uint32_t get_fg_color() {
  static uint32_t layer_count = 0;
  static uint32_t colors[] = {
      0xffff0000,
      0xff00ff00,
      0xff0000ff,
  };
  return colors[layer_count++ % std::size(colors)];
}

// Checks if two rectangles intersect, and if so, returns their intersection.
static bool compute_intersection(const fhd::wire::Frame& a, const fhd::wire::Frame& b,
                                 fhd::wire::Frame* intersection) {
  uint32_t left = std::max(a.x_pos, b.x_pos);
  uint32_t right = std::min(a.x_pos + a.width, b.x_pos + b.width);
  uint32_t top = std::max(a.y_pos, b.y_pos);
  uint32_t bottom = std::min(a.y_pos + a.height, b.y_pos + b.height);

  if (left >= right || top >= bottom) {
    return false;
  }

  intersection->x_pos = left;
  intersection->y_pos = top;
  intersection->width = right - left;
  intersection->height = bottom - top;

  return true;
}

static uint32_t interpolate_scaling(uint32_t x, uint32_t frame_num) {
  return x / 2 + interpolate(x / 2, frame_num, kScalePeriod);
}

VirtualLayer::VirtualLayer(Display* display) {
  displays_.push_back(display);
  width_ = display->mode().horizontal_resolution;
  height_ = display->mode().vertical_resolution;
}

VirtualLayer::VirtualLayer(const fbl::Vector<Display>& displays, bool tiled) {
  for (auto& d : displays) {
    displays_.push_back(&d);
  }

  width_ = 0;
  height_ = 0;
  for (auto* d : displays_) {
    if (tiled) {
      width_ += d->mode().horizontal_resolution;
    } else {
      width_ = std::max(width_, d->mode().horizontal_resolution);
    }
    height_ = std::max(height_, d->mode().vertical_resolution);
  }
}

custom_layer_t* VirtualLayer::CreateLayer(const fidl::WireSyncClient<fhd::Controller>& dc) {
  layers_.push_back(custom_layer_t());
  layers_[layers_.size() - 1].active = false;

  auto result = dc->CreateLayer();
  if (!result.ok() || result.value().res != ZX_OK) {
    printf("Creating layer failed\n");
    return nullptr;
  }
  layers_[layers_.size() - 1].id = result.value().layer_id;

  return &layers_[layers_.size() - 1];
}

PrimaryLayer::PrimaryLayer(Display* display) : VirtualLayer(display) {
  image_format_ = display->format();
}

PrimaryLayer::PrimaryLayer(const fbl::Vector<Display>& displays, bool mirrors)
    : VirtualLayer(displays, !mirrors), mirrors_(mirrors) {
  image_format_ = displays_[0]->format();
  SetImageDimens(width_, height_);
}

PrimaryLayer::PrimaryLayer(const fbl::Vector<Display>& displays, Image::Pattern pattern,
                           uint32_t fgcolor, uint32_t bgcolor, bool mirrors)
    : VirtualLayer(displays, !mirrors),
      image_pattern_(pattern),
      fgcolor_(fgcolor),
      bgcolor_(bgcolor),
      mirrors_(mirrors) {
  override_colors_ = true;
  image_format_ = displays_[0]->format();
  SetImageDimens(width_, height_);
}

bool PrimaryLayer::Init(const fidl::WireSyncClient<fhd::Controller>& dc) {
  if ((displays_.size() > 1 || rotates_) && scaling_) {
    printf("Unsupported config\n");
    return false;
  }
  uint32_t fg_color = override_colors_ ? fgcolor_ : get_fg_color();
  uint32_t bg_color = alpha_enable_ ? 0x3fffffff : 0xffffffff;
  if (override_colors_) {
    bg_color = bgcolor_;
  }

  images_[0] = Image::Create(dc, image_width_, image_height_, image_format_, image_pattern_,
                             fg_color, bg_color, modifier_);
  if (layer_flipping_) {
    images_[1] = Image::Create(dc, image_width_, image_height_, image_format_, image_pattern_,
                               fg_color, bg_color, modifier_);
  }
  if (!images_[0] || (layer_flipping_ && !images_[1])) {
    return false;
  }

  if (!layer_flipping_) {
    images_[0]->Render(-1, -1);
  }

  for (unsigned i = 0; i < displays_.size(); i++) {
    custom_layer_t* layer = CreateLayer(dc);
    if (layer == nullptr) {
      return false;
    }

    if (!images_[0]->Import(dc, GetPrimaryImageImportId(i, 0), &layer->import_info[0])) {
      return false;
    }
    if (layer_flipping_) {
      if (!images_[1]->Import(dc, GetPrimaryImageImportId(i, 1), &layer->import_info[1])) {
        return false;
      }
    } else {
      layer->import_info[alt_image_].events[WAIT_EVENT].signal(0, ZX_EVENT_SIGNALED);
    }

    fhd::wire::ImageConfig image_config;
    images_[0]->GetConfig(&image_config);
    auto set_config_result = dc->SetLayerPrimaryConfig(layer->id, image_config);
    if (!set_config_result.ok()) {
      printf("Setting layer config failed\n");
      return false;
    }

    auto set_alpha_result = dc->SetLayerPrimaryAlpha(
        layer->id,
        alpha_enable_ ? fhd::wire::AlphaMode::kHwMultiply : fhd::wire::AlphaMode::kDisable,
        alpha_val_);
    if (!set_alpha_result.ok()) {
      printf("Setting layer alpha config failed\n");
      return false;
    }
  }

  StepLayout(0);
  if (!layer_flipping_) {
    SetLayerImages(dc, false);
  }
  if (!(pan_src_ || pan_dest_)) {
    SetLayerPositions(dc);
  }

  return true;
}

void* PrimaryLayer::GetCurrentImageBuf() { return images_[alt_image_]->buffer(); }
size_t PrimaryLayer::GetCurrentImageSize() {
  PixelFormatAndModifier format_and_modifier(images_[alt_image_]->format(),
                                             images_[alt_image_]->modifier());
  return images_[alt_image_]->height() * images_[alt_image_]->stride() *
         ImageFormatStrideBytesPerWidthPixel(format_and_modifier);
}
void PrimaryLayer::StepLayout(int32_t frame_num) {
  if (layer_flipping_) {
    alt_image_ = frame_num % 2;
  }
  if (pan_src_) {
    src_frame_.x_pos =
        interpolate(image_width_ - src_frame_.width, frame_num, kSrcFrameBouncePeriod);
  }
  if (pan_dest_) {
    dest_frame_.x_pos = interpolate(width_ - dest_frame_.width, frame_num, kDestFrameBouncePeriod);
  }
  if (rotates_) {
    switch ((frame_num / kRotationPeriod) % 4) {
      case 0:
        rotation_ = Transform::kIdentity;
        break;
      case 1:
        rotation_ = Transform::kRot90;
        break;
      case 2:
        rotation_ = Transform::kRot180;
        break;
      case 3:
        rotation_ = Transform::kRot270;
        break;
    }

    if (frame_num % kRotationPeriod == 0 && frame_num != 0) {
      uint32_t tmp = dest_frame_.width;
      dest_frame_.width = dest_frame_.height;
      dest_frame_.height = tmp;
    }
  }

  fhd::wire::Frame display = {};
  for (unsigned i = 0; i < displays_.size(); i++) {
    display.height = displays_[i]->mode().vertical_resolution;
    display.width = displays_[i]->mode().horizontal_resolution;

    if (mirrors_) {
      layers_[i].src.x_pos = 0;
      layers_[i].src.y_pos = 0;
      layers_[i].src.width = image_width_;
      layers_[i].src.height = image_height_;
      layers_[i].dest.x_pos = 0;
      layers_[i].dest.y_pos = 0;
      layers_[i].dest.width = display.width;
      layers_[i].dest.height = display.height;
      layers_[i].active = true;
      continue;
    }

    // Calculate the portion of the dest frame which shows up on this display
    if (compute_intersection(display, dest_frame_, &layers_[i].dest)) {
      // Find the subset of the src region which shows up on this display
      if (rotation_ == Transform::kIdentity || rotation_ == Transform::kRot180) {
        if (!scaling_) {
          layers_[i].src.x_pos = src_frame_.x_pos + (layers_[i].dest.x_pos - dest_frame_.x_pos);
          layers_[i].src.y_pos = src_frame_.y_pos;
          layers_[i].src.width = layers_[i].dest.width;
          layers_[i].src.height = layers_[i].dest.height;
        } else {
          layers_[i].src.x_pos =
              src_frame_.x_pos +
              interpolate_scaling(layers_[i].dest.x_pos - dest_frame_.x_pos, frame_num);
          layers_[i].src.y_pos = src_frame_.y_pos;
          layers_[i].src.width = interpolate_scaling(layers_[i].dest.width, frame_num);
          layers_[i].src.height = interpolate_scaling(layers_[i].dest.height, frame_num);
        }
      } else {
        layers_[i].src.x_pos = src_frame_.x_pos;
        layers_[i].src.y_pos = src_frame_.y_pos + (layers_[i].dest.y_pos - dest_frame_.y_pos);
        layers_[i].src.height = layers_[i].dest.width;
        layers_[i].src.width = layers_[i].dest.height;
      }

      // Put the dest frame coordinates in the display's coord space
      layers_[i].dest.x_pos -= display.x_pos;
      layers_[i].active = true;
    } else {
      layers_[i].active = false;
    }

    display.x_pos += display.width;
  }

  if (layer_toggle_) {
    for (auto& layer : layers_) {
      layer.active = !(frame_num % 2);
    }
  }
}

void PrimaryLayer::SendLayout(const fidl::WireSyncClient<fhd::Controller>& dc) {
  if (layer_flipping_) {
    SetLayerImages(dc, alt_image_);
  }
  if (scaling_ || pan_src_ || pan_dest_) {
    SetLayerPositions(dc);
  }
}

bool PrimaryLayer::WaitForReady() { return Wait(SIGNAL_EVENT); }

void PrimaryLayer::Render(int32_t frame_num) {
  if (!layer_flipping_) {
    return;
  }
  images_[alt_image_]->Render(frame_num < 2 ? 0 : frame_num - 2, frame_num);
  for (auto& layer : layers_) {
    layer.import_info[alt_image_].events[WAIT_EVENT].signal(0, ZX_EVENT_SIGNALED);
  }
}

void PrimaryLayer::SetLayerPositions(const fidl::WireSyncClient<fhd::Controller>& dc) {
  for (auto& layer : layers_) {
    ZX_ASSERT(dc->SetLayerPrimaryPosition(layer.id, rotation_, layer.src, layer.dest).ok());
  }
}

void VirtualLayer::SetLayerImages(const fidl::WireSyncClient<fhd::Controller>& dc, bool alt_image) {
  for (auto& layer : layers_) {
    const auto& image = layer.import_info[alt_image];
    auto result = dc->SetLayerImage(layer.id, image.id, image.event_ids[WAIT_EVENT],
                                    image.event_ids[SIGNAL_EVENT]);

    ZX_ASSERT(result.ok());
  }
}

bool PrimaryLayer::Wait(uint32_t idx) {
  zx_time_t deadline = zx_deadline_after(ZX_MSEC(100));
  for (auto& layer : layers_) {
    uint32_t observed;
    if (!layer.active) {
      continue;
    }
    auto& event = layer.import_info[alt_image_].events[idx];
    zx_status_t res;
    if ((res = event.wait_one(ZX_EVENT_SIGNALED, zx::time(deadline), &observed)) == ZX_OK) {
      if (layer_flipping_) {
        event.signal(ZX_EVENT_SIGNALED, 0);
      }
    } else {
      return false;
    }
  }
  return true;
}

CursorLayer::CursorLayer(Display* display) : VirtualLayer(display) {}

CursorLayer::CursorLayer(const fbl::Vector<Display>& displays) : VirtualLayer(displays) {}

bool CursorLayer::Init(const fidl::WireSyncClient<fhd::Controller>& dc) {
  fhd::wire::CursorInfo info = displays_[0]->cursor();
  uint32_t bg_color = 0xffffffff;

  image_ = Image::Create(dc, info.width, info.height, info.pixel_format, Image::Pattern::kBorder,
                         get_fg_color(), bg_color, fuchsia_images2::wire::kFormatModifierLinear);
  if (!image_) {
    return false;
  }
  image_->Render(-1, -1);

  for (unsigned i = 0; i < displays_.size(); i++) {
    custom_layer_t* layer = CreateLayer(dc);
    if (layer == nullptr) {
      return false;
    }

    layer->active = true;
    if (!image_->Import(dc, GetCursorImageImportId(i), &layer->import_info[0])) {
      return false;
    }
    layer->import_info[0].events[WAIT_EVENT].signal(0, ZX_EVENT_SIGNALED);

    fhd::wire::ImageConfig image_config = {};
    image_config.height = info.height;
    image_config.width = info.width;
    // TODO(fxbug.dev/126113): Delete the unused `pixel_format` field.
    image_config.pixel_format = ZX_PIXEL_FORMAT_NONE;
    image_config.type = fhd::wire::kTypeSimple;
    auto result = dc->SetLayerCursorConfig(layer->id, image_config);
    if (!result.ok()) {
      printf("Setting layer config failed\n");
      return false;
    }
  }

  SetLayerImages(dc, false);

  return true;
}

void CursorLayer::StepLayout(int32_t frame_num) {
  fhd::wire::CursorInfo info = displays_[0]->cursor();

  x_pos_ = interpolate(width_ + info.width, frame_num, kDestFrameBouncePeriod) - info.width;
  y_pos_ = interpolate(height_ + info.height, frame_num, kDestFrameBouncePeriod) - info.height;
}

void CursorLayer::SendLayout(const fidl::WireSyncClient<fhd::Controller>& dc) {
  uint32_t display_start = 0;
  for (unsigned i = 0; i < displays_.size(); i++) {
    ZX_ASSERT(dc->SetLayerCursorPosition(layers_[i].id, x_pos_ - display_start, y_pos_).ok());
    display_start += displays_[i]->mode().horizontal_resolution;
  }
}

ColorLayer::ColorLayer(Display* display) : VirtualLayer(display) {}

ColorLayer::ColorLayer(const fbl::Vector<Display>& displays) : VirtualLayer(displays) {}

bool ColorLayer::Init(const fidl::WireSyncClient<fhd::Controller>& dc) {
  for (unsigned i = 0; i < displays_.size(); i++) {
    custom_layer_t* layer = CreateLayer(dc);
    if (layer == nullptr) {
      return false;
    }

    layer->active = true;

    constexpr uint32_t kColorLayerFormat = ZX_PIXEL_FORMAT_ARGB_8888;
    uint32_t kColorLayerColor = get_fg_color();

    uint32_t size = FIDL_ALIGN(ZX_PIXEL_FORMAT_BYTES(kColorLayerFormat));
    uint8_t data[size];
    *reinterpret_cast<uint32_t*>(data) = kColorLayerColor;

    auto result = dc->SetLayerColorConfig(
        layer->id, kColorLayerFormat,
        ::fidl::VectorView<uint8_t>::FromExternal(data, ZX_PIXEL_FORMAT_BYTES(kColorLayerFormat)));

    if (!result.ok()) {
      printf("Setting layer config failed\n");
      return false;
    }
  }

  return true;
}

}  // namespace display
}  // namespace testing
