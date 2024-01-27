// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/v2/audio_core_server.h"

#include <fidl/fuchsia.media/cpp/hlcpp_conversion.h>
#include <fidl/fuchsia.media/cpp/type_conversions.h>
#include <lib/fidl/cpp/hlcpp_conversion.h>
#include <lib/fidl/cpp/wire_natural_conversions.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include "src/media/audio/audio_core/shared/policy_loader.h"
#include "src/media/audio/audio_core/shared/profile_acquirer.h"
#include "src/media/audio/lib/format2/format.h"

namespace media_audio {

namespace {

using ::media::audio::CaptureUsage;
using ::media::audio::CaptureUsageFromFidlCaptureUsage;
using ::media::audio::RenderUsage;

}  // namespace

// static
std::shared_ptr<AudioCoreServer> AudioCoreServer::Create(
    std::shared_ptr<const FidlThread> fidl_thread,
    fidl::ServerEnd<fuchsia_media::AudioCore> server_end, Args args) {
  return BaseFidlServer::Create(fidl_thread, std::move(server_end), std::move(args));
}

AudioCoreServer::AudioCoreServer(Args args)
    : creator_(std::move(args.creator)),
      route_graph_(std::move(args.route_graph)),
      stream_volume_manager_(std::move(args.stream_volume_manager)),
      audio_admin_(std::move(args.audio_admin)),
      default_volume_curve_(args.default_volume_curve) {
  // TODO(fxbug.dev/98652): the following call was copied from v1/audio_core_impl.cc. It should be
  // moved to a higher-level where the FIDL thread is configured.
  //
  // The FIDL thread is responsible for receiving audio payloads sent by applications, so it has
  // real time requirements just like mixing threads.
  media::audio::AcquireAudioCoreImplProfile(
      args.component_context, [](zx_status_t status, zx::profile profile) {
        if (status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Failed to acquire AudioCoreImpl profile";
          return;
        }
        FX_CHECK(profile);
        if (auto status = zx::thread::self()->set_profile(profile, 0); status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Failed to assign AudioCoreImpl profile";
          return;
        }
      });

  LoadDefaults();
}

void AudioCoreServer::CreateAudioRenderer(CreateAudioRendererRequestView request,
                                          CreateAudioRendererCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::CreateAudioRenderer");

  if (!request->audio_out_request) {
    FX_LOGS(WARNING) << "CreateAudioRenderer: invalid handle";
    Shutdown(ZX_ERR_INVALID_ARGS);
    return;
  }

  creator_->CreateRenderer(std::move(request->audio_out_request), RenderUsage::MEDIA,
                           /*format=*/std::nullopt, /*notify_clock=*/nullptr);
}

void AudioCoreServer::CreateAudioCapturer(CreateAudioCapturerRequestView request,
                                          CreateAudioCapturerCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::CreateAudioCapturer");

  if (!request->audio_in_request) {
    FX_LOGS(WARNING) << "CreateAudioCapturer: invalid handle";
    Shutdown(ZX_ERR_INVALID_ARGS);
    return;
  }

  creator_->CreateCapturer(std::move(request->audio_in_request),
                           request->loopback ? CaptureUsage::LOOPBACK : CaptureUsage::FOREGROUND,
                           /*format=*/std::nullopt, /*notify_clock=*/nullptr);
}

void AudioCoreServer::CreateAudioCapturerWithConfiguration(
    CreateAudioCapturerWithConfigurationRequestView request,
    CreateAudioCapturerWithConfigurationCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::CreateAudioCapturerWithConfiguration");

  if (!request->audio_capturer_request) {
    FX_LOGS(WARNING) << "CreateAudioCapturerWithConfiguration: invalid handle";
    Shutdown(ZX_ERR_INVALID_ARGS);
    return;
  }

  auto format_result = Format::CreateLegacy(request->stream_type);
  if (!format_result.is_ok()) {
    FX_LOGS(WARNING) << "CreateAudioCapturerWithConfiguration: invalid format: "
                     << format_result.error();
    Shutdown(ZX_ERR_INVALID_ARGS);
    return;
  }

  CaptureUsage usage;
  if (request->configuration.is_loopback()) {
    usage = CaptureUsage::LOOPBACK;
  } else {
    auto& input = request->configuration.input();
    usage = input.has_usage() ? static_cast<CaptureUsage>(input.usage()) : CaptureUsage::FOREGROUND;
  }

  creator_->CreateCapturer(std::move(request->audio_capturer_request), usage,
                           format_result.take_value(), /*notify_clock=*/nullptr);
}

void AudioCoreServer::SetRenderUsageGain(SetRenderUsageGainRequestView request,
                                         SetRenderUsageGainCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::SetRenderUsageGain");

  stream_volume_manager_->SetUsageGain(
      fuchsia::media::Usage::WithRenderUsage(
          static_cast<fuchsia::media::AudioRenderUsage>(request->usage)),
      request->gain_db);
}

void AudioCoreServer::SetCaptureUsageGain(SetCaptureUsageGainRequestView request,
                                          SetCaptureUsageGainCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::SetCaptureUsageGain");

  stream_volume_manager_->SetUsageGain(
      fuchsia::media::Usage::WithCaptureUsage(
          static_cast<fuchsia::media::AudioCaptureUsage>(request->usage)),
      request->gain_db);
}

void AudioCoreServer::BindUsageVolumeControl(BindUsageVolumeControlRequestView request,
                                             BindUsageVolumeControlCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::BindUsageVolumeControl");

  if (request->usage.is_render_usage()) {
    stream_volume_manager_->BindUsageVolumeClient(
        fidl::NaturalToHLCPP(fidl::ToNatural(request->usage)),
        fidl::InterfaceRequest<fuchsia::media::audio::VolumeControl>(
            request->volume_control.TakeChannel()));
  } else {
    request->volume_control.Close(ZX_ERR_NOT_SUPPORTED);
  }
}

void AudioCoreServer::GetVolumeFromDb(GetVolumeFromDbRequestView request,
                                      GetVolumeFromDbCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::GetVolumeFromDb");

  float volume;
  auto volume_curve = request->usage.is_render_usage()
                          ? route_graph_->VolumeCurveForUsage(
                                static_cast<RenderUsage>(request->usage.render_usage()))
                          : route_graph_->VolumeCurveForUsage(
                                static_cast<CaptureUsage>(request->usage.capture_usage()));
  if (volume_curve) {
    volume = volume_curve->DbToVolume(request->gain_db);
  } else {
    volume = default_volume_curve_.DbToVolume(request->gain_db);
  }
  completer.Reply(volume);
}

void AudioCoreServer::GetDbFromVolume(GetDbFromVolumeRequestView request,
                                      GetDbFromVolumeCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::GetDbFromVolume");

  float db;
  auto volume_curve = request->usage.is_render_usage()
                          ? route_graph_->VolumeCurveForUsage(
                                static_cast<RenderUsage>(request->usage.render_usage()))
                          : route_graph_->VolumeCurveForUsage(
                                static_cast<CaptureUsage>(request->usage.capture_usage()));
  if (volume_curve) {
    db = volume_curve->VolumeToDb(request->volume);
  } else {
    db = default_volume_curve_.VolumeToDb(request->volume);
  }
  completer.Reply(db);
}

void AudioCoreServer::SetInteraction(SetInteractionRequestView request,
                                     SetInteractionCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::SetInteraction");
  audio_admin_->SetInteraction(fidl::NaturalToHLCPP(fidl::ToNatural(std::move(request->active))),
                               fidl::NaturalToHLCPP(fidl::ToNatural(std::move(request->affected))),
                               static_cast<fuchsia::media::Behavior>(request->behavior));
}

void AudioCoreServer::ResetInteractions(ResetInteractionsCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::ResetInteractions");
  audio_admin_->ResetInteractions();
}

void AudioCoreServer::LoadDefaults(LoadDefaultsCompleter::Sync& completer) {
  TRACE_DURATION("audio", "AudioCoreServer::LoadDefaults");
  LoadDefaults();
}

void AudioCoreServer::LoadDefaults() {
  auto policy = media::audio::PolicyLoader::LoadPolicy();
  // TODO(fxbug.dev/98652): update idle policy
  // context_.device_router().SetIdlePowerOptionsFromPolicy(policy.idle_power_options());
  audio_admin_->SetInteractionsFromAudioPolicy(std::move(policy));
}

}  // namespace media_audio
