// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_V1_ROUTE_GRAPH_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_V1_ROUTE_GRAPH_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/fpromise/bridge.h>

#include <array>
#include <deque>
#include <unordered_map>

#include <fbl/ref_ptr.h>

#include "src/media/audio/audio_core/shared/device_config.h"
#include "src/media/audio/audio_core/v1/audio_input.h"
#include "src/media/audio/audio_core/v1/audio_output.h"
#include "src/media/audio/audio_core/v1/device_registry.h"
#include "src/media/audio/audio_core/v1/idle_policy.h"
#include "src/media/audio/audio_core/v1/link_matrix.h"
#include "src/media/audio/audio_core/v1/threading_model.h"

namespace media::audio {

// |RoutingProfile| informs the |RouteGraph| on how or whether it should establish links for a
// particular input or output to the mixer.
struct RoutingProfile {
  bool routable = false;
  StreamUsage usage = {};
};

// |RouteGraph| is responsible for managing connections between inputs and outputs of the mixer.
//
// |RouteGraph| owns user-level inputs and outputs (|AudioRenderer|s and |AudioCapturer|s).
//
// Renderers are routed by Usage to the most recently plugged output that supports their Usage.
//
// Capturers are routed to the most recently plugged device that supports their usage.
class RouteGraph : public DeviceRouter {
 public:
  // Constructs a graph from the given config and link matrix. Each parameter must outlive the
  // RouteGraph.
  RouteGraph(LinkMatrix* link_matrix);

  ~RouteGraph();

  // DeviceRouter implementation
  //
  void SetIdlePowerOptionsFromPolicy(AudioPolicy::IdlePowerOptions options) override {}

  // Adds an |AudioInput| or |AudioOutput| to the route graph. An audio input may be connected to
  // transmit samples to an |AudioCapturer|; an audio output receives samples from an
  // |AudioRenderer|.
  void AddDeviceToRoutes(AudioDevice* device) final;

  // Removes an |AudioInput| or |AudioOutput| from the route graph. Any connected |AudioCapturer|s
  // or |AudioRenderer|s will be rerouted.
  void RemoveDeviceFromRoutes(AudioDevice* device) final;

  //
  // TODO(fxbug.dev/13339): Remove throttle_output_.
  // Sets a throttle output which is linked to all AudioRenderers to throttle the rate at which we
  // return packets to clients.
  void SetThrottleOutput(ThreadingModel* threading_model,
                         const std::shared_ptr<AudioOutput>& throttle_output);

  // Returns a boolean indicating if a |device| is contained in the route graph.
  bool ContainsDevice(const AudioDevice* device);

  // Adds an |AudioRenderer| to the route graph. An |AudioRenderer| may be connected to
  // |AudioOutput|s.
  void AddRenderer(std::shared_ptr<AudioObject> renderer);

  // Sets the routing profile with which the route graph selects |AudioOutput|s for the
  // |AudioRenderer|.
  void SetRendererRoutingProfile(const AudioObject& renderer, RoutingProfile profile);

  void RemoveRenderer(const AudioObject& renderer);

  // Adds an |AudioCapturer| to the route graph. An |AudioCapturer| may be connected to
  // |AudioInput|s to receive samples from them.
  void AddCapturer(std::shared_ptr<AudioObject> capturer);

  // Sets the routing profile with which the route graph selects |AudioInput|s for the
  // |AudioCapturer|.
  void SetCapturerRoutingProfile(const AudioObject& capturer, RoutingProfile profile);

  void RemoveCapturer(const AudioObject& capturer);

  std::unordered_set<AudioDevice*> TargetsForRenderUsage(const RenderUsage& usage);

  std::shared_ptr<LoudnessTransform> LoudnessTransformForUsage(const StreamUsage& usage);

 private:
  struct RoutableOwnedObject {
    std::shared_ptr<AudioObject> ref;
    RoutingProfile profile;
  };

  struct Target {
    Target() = default;

    Target(AudioDevice* _device, std::shared_ptr<LoudnessTransform> _transform)
        : device(_device), transform(_transform) {
      FX_DCHECK(!!device == !!transform);
    }

    bool is_linkable() const { return device; }

    AudioDevice* device = nullptr;
    std::shared_ptr<LoudnessTransform> transform = nullptr;
  };
  using Targets = std::array<Target, kStreamUsageCount>;
  using UnlinkCommand = std::array<bool, kStreamUsageCount>;

  void UpdateGraphForDeviceChange();

  // Calculate the new targets based on our routing policy and available devices.
  // Returns the new targets and an unlink command to unlink any out of date
  // routing links.
  std::pair<Targets, UnlinkCommand> CalculateTargets() const;

  void Unlink(UnlinkCommand unlink_command);

  Target TargetForUsage(const StreamUsage& usage) const;

  LinkMatrix& link_matrix_;

  Targets targets_ = {};

  // TODO(fxbug.dev/39624): convert to weak_ptr when ownership is explicit.
  std::deque<AudioDevice*> devices_;

  std::unordered_map<const AudioObject*, RoutableOwnedObject> capturers_;
  std::unordered_map<const AudioObject*, RoutableOwnedObject> renderers_;
  std::unordered_map<const AudioObject*, RoutableOwnedObject> loopback_capturers_;

  // TODO(fxbug.dev/13339): Remove throttle_output_.
  std::optional<fpromise::completer<void, void>> throttle_release_fence_;
  std::shared_ptr<AudioOutput> throttle_output_;

  // For debugging purposes only
  void DisplayRenderers();
  void DisplayCapturers();
  void DisplayDevices();
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_V1_ROUTE_GRAPH_H_
