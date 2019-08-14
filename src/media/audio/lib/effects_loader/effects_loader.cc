// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/lib/effects_loader/effects_loader.h"

#include <dlfcn.h>

#include "src/lib/fxl/logging.h"

namespace media::audio {

zx_status_t EffectsLoader::LoadLibrary() {
  if (module_) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  module_ = EffectsModuleV1::Open(lib_name_);
  if (!module_) {
    return ZX_ERR_UNAVAILABLE;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::GetNumFx(uint32_t* num_fx_out) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (num_fx_out == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  *num_fx_out = module_->num_effects;
  return ZX_OK;
}

zx_status_t EffectsLoader::GetFxInfo(uint32_t effect_id, fuchsia_audio_effects_description* desc) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (desc == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (effect_id >= module_->num_effects) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (!module_->get_info || !module_->get_info(effect_id, desc)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

fuchsia_audio_effects_handle_t EffectsLoader::CreateFx(uint32_t effect_id, uint32_t frame_rate,
                                                       uint16_t channels_in, uint16_t channels_out,
                                                       std::string_view config) {
  if (!module_) {
    return FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
  }
  if (effect_id >= module_->num_effects) {
    return FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
  }
  if (!module_->create_effect) {
    return FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
  }
  return module_->create_effect(effect_id, frame_rate, channels_in, channels_out, config.data(),
                                config.size());
}

zx_status_t EffectsLoader::FxUpdateConfiguration(fuchsia_audio_effects_handle_t handle,
                                                 std::string_view config) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->update_effect_configuration ||
      !module_->update_effect_configuration(handle, config.data(), config.size())) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::DeleteFx(fuchsia_audio_effects_handle_t handle) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->delete_effect || !module_->delete_effect(handle)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::FxGetParameters(fuchsia_audio_effects_handle_t handle,
                                           fuchsia_audio_effects_parameters* params) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE || params == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->get_parameters || !module_->get_parameters(handle, params)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::FxProcessInPlace(fuchsia_audio_effects_handle_t handle,
                                            uint32_t num_frames, float* audio_buff_in_out) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE || audio_buff_in_out == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->process_inplace ||
      !module_->process_inplace(handle, num_frames, audio_buff_in_out)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::FxProcess(fuchsia_audio_effects_handle_t handle, uint32_t num_frames,
                                     const float* audio_buff_in, float* audio_buff_out) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE || audio_buff_in == nullptr ||
      audio_buff_out == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->process || !module_->process(handle, num_frames, audio_buff_in, audio_buff_out)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

zx_status_t EffectsLoader::FxFlush(fuchsia_audio_effects_handle_t handle) {
  if (!module_) {
    return ZX_ERR_NOT_FOUND;
  }
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!module_->flush || !module_->flush(handle)) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

}  // namespace media::audio
