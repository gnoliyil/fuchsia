// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_V2_GRAPH_TYPES_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_V2_GRAPH_TYPES_H_

#include <fidl/fuchsia.audio.mixer/cpp/natural_types.h>

#include "src/media/audio/lib/timeline/timeline_function.h"
#include "src/media/audio/lib/timeline/timeline_rate.h"

namespace media_audio {

// TODO(fxbug.dev/98652): deduplicate with ../services/mixer/common/basic_types.h

// FIDL IDs.
using NodeId = uint64_t;
using ThreadId = uint64_t;
using GainControlId = uint64_t;
using TokenId = uint64_t;

// This ID shall not be used.
constexpr uint64_t kInvalidId = ::fuchsia_audio_mixer::kInvalidId;

// Alias common types into this namespace.
using TimelineFunction = ::media::TimelineFunction;
using TimelineRate = ::media::TimelineRate;
using PipelineDirection = ::fuchsia_audio_mixer::PipelineDirection;

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_V2_GRAPH_TYPES_H_
