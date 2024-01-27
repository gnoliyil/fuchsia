// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <errno.h>
#include <fidl/fuchsia.hardware.audio/cpp/fidl.h>
#include <fidl/fuchsia.hardware.audio/cpp/natural_ostream.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fit/defer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/status.h>

#include <deque>
#include <filesystem>
#include <sstream>

#include <fbl/unique_fd.h>

constexpr char kCodecClassDir[] = "/dev/class/codec";

// LINT.IfChange
constexpr char kUsageSummary[] = R"""(
Audio hardware codec driver control.

Usage:
  audio-codec-ctl [-d|--device <device>] f[ormats]
  audio-codec-ctl [-d|--device <device>] i[nfo]
  audio-codec-ctl [-d|--device <device>] c[apabilities_plug_detect]
  audio-codec-ctl [-d|--device <device>] b[ridgeable]
  audio-codec-ctl [-d|--device <device>] r[eset]
  audio-codec-ctl [-d|--device <device>] m[ode_bridged] true|false
  audio-codec-ctl [-d|--device <device>] d[ai] <number_of_channels>
    <channels_to_use_bitmask> pdm|upcm|spcm|fpcm none|i2s|left-stereo|right-stereo|1tdm|2tdm|3tdm
    <frame_rate> <bits_per_slot> <bits_per_sample>
  audio-codec-ctl [-d|--device <device>] start
  audio-codec-ctl [-d|--device <device>] stop
  audio-codec-ctl [-d|--device <device>] p[lug_state]
  audio-codec-ctl [-d|--device <device>] e[lements]
  audio-codec-ctl [-d|--device <device>] t[opologies]
  audio-codec-ctl [-d|--device <device>] w[atch] <id>
  audio-codec-ctl [-d|--device <device>] set <id> [enable|disable] [gain <gain>] [latency <nsecs>]
    [vendor <hex> <hex> ...]
  audio-codec-ctl [-h|--help]
)""";

constexpr char kUsageDetails[] = R"""(
Audio hardware codec driver control on <device> (full path specified e.g. /dev/class/codec/123 or
only the devfs node name specified e.g. 123) or unspecified (picks the first device in
/dev/class/codec).

Commands:

  f[ormats]                         : Retrieves the DAI formats supported by the codec.
  i[nfo]                            : Retrieves textual information about the codec.
  c[apabilities_plug_detect]        : Retrieves Plug Detect Capabilities.
  b[ridgeable]                      : Returns whether a codec is bridgeable.
  r[eset]                           : Resets the codec.
  m[ode_bridged] true|false         : Sets a codec bridged mode to true or false.
  d[ai] <number_of_channels>        : Sets the DAI format to be used in the codec interface.
    <number_of_channels>: Number of channels.
    <channels_to_use_bitmask>: Sets which channels are active via a bitmask. The least significant
      bit corresponds to channel index 0.
    pdm: Pulse Density Modulation samples.
    upcm: Signed Linear Pulse Code Modulation samples at the host endianness.
    spcm: Unsigned Linear Pulse Code Modulation samples at the host endianness.
    fpcm: Floating point samples IEEE-754 encoded.
    none: No frame format as in samples without a frame sync like PDM.
    i2s: Format as specified in the I2S specification.
    left-stereo: Left justified, 2 channels.
    right-stereo: Right justified, 2 channels.
    1tdm: Left justified, variable number of channels, data starts at frame sync changes from low to
      high clocked out at the rising edge of sclk. The frame sync must stay high for exactly 1
      clock cycle.
    2tdm: Left justified, variable number of channels, data starts one clock cycle after the frame
      sync changes from low to high clocked out at the rising edge of sclk. The frame sync must
      stay high for exactly 1 clock cycle.
    3tdm: Left justified, variable number of channels, data starts two clock cycles after the frame
      sync changes from low to high clocked out at the rising edge of sclk. The frame sync must
      stay high for exactly 1 clock cycle.
    <frame_rate>: The frame rate for all samples.
    <bits_per_slot>: The bits per slot for all channels.
    <bits_per_sample>: The bits per sample for all samples.  Must be smaller than bits per channel for
      samples to fit.
  start                             : Start/Re-start the codec operation.
  stop                              : Stops the codec operation.
  p[lug_state]                      : Get the plug detect state.
  e[lements]                        : Returns a vector of supported processing elements.
  t[opologies]                      : Returns a vector of supported topologies.
  w[atch] <id>                      : Get a processing element state.
  set <id> [enable|disable] [gain <gain>] [latency <nsecs>] [vendor <hex> <hex> ...] : Controls a
    processing element.
    <id>: Processing element id.
    enable: Process element enabled state.
    disable: Process element disabled state.
    <gain>: Current gain in GainType format reported in the supported processing elements vector.
    <nsecs>: Latency added to the pipeline in nanoseconds.
    <hex>: Vendor specific raw byte to feed to the processing element in hex format.

Examples:

  Retrieves the DAI formats supported:
  $ audio-codec-ctl f
  Executing on device /dev/class/codec/209
  [ fuchsia_hardware_audio::DaiSupportedFormats{ number_of_channels = [ 2, 4, ], sample_formats = [ fuchsia_hardware_audio::DaiSampleFormat::kPcmSigned, ], frame_formats = [ fuchsia_hardware_audio::DaiFrameFormat::frame_format_standard(fuchsia_hardware_audio::DaiFrameFormatStandard::kI2S), fuchsia_hardware_audio::DaiFrameFormat::frame_format_standard(fuchsia_hardware_audio::DaiFrameFormatStandard::k1tdm), ], frame_rates = [ 48000, 96000, ], bits_per_slot = [ 16, 32, ], bits_per_sample = [ 16, 32, ], }, ]

  Retrieves textual information:
  $ audio-codec-ctl i
  Executing on device /dev/class/codec/706
  fuchsia_hardware_audio::CodecInfo{ unique_id = "", manufacturer = "Texas Instruments", product_name = "TAS5825m", }

  Retrieves Plug Detect Capabilities:
  $ audio-codec-ctl c
  Executing on device: /dev/class/codec/706
  fuchsia_hardware_audio::PlugDetectCapabilities::kHardwired

  Returns whether the codec is bridgeable:
  $ audio-codec-ctl b
  Executing on device: /dev/class/codec/706
  Bridged mode: false

  Resets the codec:
  $ audio-codec-ctl r
  Executing on device: /dev/class/codec/706
  Reset done

  Sets a codec's bridged mode:
  $ audio-codec-ctl m true
  Setting bridged mode to: true
  Executing on device: /dev/class/codec/706

  Sets the DAI format to be used in the codec interface:
  $ audio-codec-ctl d 2 1 s i 48000 16 32
  Setting DAI format:
  fuchsia_hardware_audio::DaiFormat{ number_of_channels = 2, channels_to_use_bitmask = 1, sample_format = fuchsia_hardware_audio::DaiSampleFormat::kPcmSigned, frame_format = fuchsia_hardware_audio::DaiFrameFormat::frame_format_standard(fuchsia_hardware_audio::DaiFrameFormatStandard::kI2S), frame_rate = 48000, bits_per_slot = 16, bits_per_sample = 32, }
  Executing on device: /dev/class/codec/706

  Start/Re-start the codec operation:
  $ audio-codec-ctl start
  Executing on device: /dev/class/codec/706
  Start done

  Stops the codec operation:
  $ audio-codec-ctl stop
  Executing on device: /dev/class/codec/706
  Stop done

  Get the plug detect state:
  $ audio-codec-ctl p
  Executing on device: /dev/class/codec/706
  fuchsia_hardware_audio::PlugState{ plugged = true, plug_state_time = 1167863520, }

  Returns a vector of supported processing elements:
  $ audio-codec-ctl e
  Executing on device: /dev/class/codec/706
  [ fuchsia_hardware_audio_signalprocessing::Element{ id = 1, type = fuchsia_hardware_audio_signalprocessing::ElementType::kGain, type_specific = fuchsia_hardware_audio_signalprocessing::TypeSpecificElement::gain(fuchsia_hardware_audio_signalprocessing::Gain{ type = fuchsia_hardware_audio_signalprocessing::GainType::kDecibels, min_gain = -63.5, max_gain = 0, min_gain_step = 0.5, }), }, fuchsia_hardware_audio_signalprocessing::Element{ id = 2, type = fuchsia_hardware_audio_signalprocessing::ElementType::kMute, }, ]

  Returns a vector of supported topologies.
  $ audio-codec-ctl t
  Executing on device: /dev/class/codec/706
  [ fuchsia_hardware_audio_signalprocessing::Topology{ id = 1, processing_elements_edge_pairs = [ fuchsia_hardware_audio_signalprocessing::EdgePair{ processing_element_id_from = 1, processing_element_id_to = 2, }, fuchsia_hardware_audio_signalprocessing::EdgePair{ processing_element_id_from = 2, processing_element_id_to = 3, }, ], }, ]

  Get a processing element state.
  $ audio-codec-ctl w 1
  Executing on device: /dev/class/codec/706
  fuchsia_hardware_audio_signalprocessing::ElementState{ type_specific = fuchsia_hardware_audio_signalprocessing::TypeSpecificElementState::gain(fuchsia_hardware_audio_signalprocessing::GainElementState{ gain = 0, }), enabled = true, }

  Controls a processing element.
  $ audio-codec-ctl set 1 enable gain 1.23 vendor 0x12 0x98
  Setting element state:
  fuchsia_hardware_audio_signalprocessing::SignalProcessingSetElementStateRequest{ processing_element_id = 1, state = fuchsia_hardware_audio_signalprocessing::ElementState{ type_specific = fuchsia_hardware_audio_signalprocessing::TypeSpecificElementState::gain(fuchsia_hardware_audio_signalprocessing::GainElementState{ gain = 1.23, }), enabled = true, vendor_specific_data = [ 18, 152, ], }, }
  Executing on device: /dev/class/codec/706

  Specify device:
  $ audio-codec-ctl -d 706 p
  Executing on device: /dev/class/codec/706
  fuchsia_hardware_audio::PlugState{ plugged = true, plug_state_time = 1167863520, }
  $ audio-codec-ctl -d 123 p
  Executing on device /dev/class/codec/123
  watch plug state failed: FIDL operation failed due to peer closed, status: ZX_ERR_PEER_CLOSED (-24)
  $ audio-codec-ctl -d /dev/class/codec/706 p
  Executing on device: /dev/class/codec/706
  fuchsia_hardware_audio::PlugState{ plugged = true, plug_state_time = 1167863520, }
)""";
// LINT.ThenChange(//docs/reference/tools/hardware/audio-code-ctl.md)

template <typename T>
std::string ToString(const T& value) {
  std::ostringstream buf;
  buf << value;
  return buf.str();
}
template <typename T>
std::string FidlString(const T& value) {
  return ToString(fidl::ostream::Formatted<T>(value));
}

void ShowUsage(bool show_details) {
  std::cout << kUsageSummary;
  if (!show_details) {
    std::cout << std::endl << "Use `audio-codec-ctl --help` to see full help text" << std::endl;
    return;
  }
  std::cout << kUsageDetails;
}

fidl::SyncClient<fuchsia_hardware_audio::Codec> GetCodecClient(std::string path) {
  if (!path.size()) {
    for (const auto& entry : std::filesystem::directory_iterator(kCodecClassDir)) {
      path = entry.path().string();
      break;
    }
  }

  std::cout << "Executing on device " << path << std::endl;
  zx::result connector = component::Connect<fuchsia_hardware_audio::CodecConnector>(path.c_str());
  if (connector.is_error()) {
    std::cerr << "could not connect to:" << path << " status:" << connector.status_string();
    return {};
  }

  fidl::SyncClient connector_client(std::move(connector.value()));
  auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_audio::Codec>();
  ZX_ASSERT(endpoints.is_ok());
  auto [local, remote] = *std::move(endpoints);
  auto connect_ret = connector_client->Connect(std::move(remote));
  ZX_ASSERT(connect_ret.is_ok());
  return fidl::SyncClient<fuchsia_hardware_audio::Codec>(std::move(local));
}

fidl::SyncClient<fuchsia_hardware_audio_signalprocessing::SignalProcessing> GetSignalClient(
    std::string path) {
  auto endpoints =
      fidl::CreateEndpoints<fuchsia_hardware_audio_signalprocessing::SignalProcessing>();
  ZX_ASSERT(endpoints.is_ok());
  auto [local, remote] = *std::move(endpoints);
  auto connect_ret = GetCodecClient(path)->SignalProcessingConnect(std::move(remote));
  ZX_ASSERT(connect_ret.is_ok());
  return fidl::SyncClient<fuchsia_hardware_audio_signalprocessing::SignalProcessing>(
      std::move(local));
}

int main(int argc, char** argv) {
  std::string path = {};

  std::deque<std::string> args(argv + 1, argv + argc);
  if (args.size() == 0) {  // Must have a parameter.
    ShowUsage(false);
    return 0;
  }
  if (!args.front().compare(0, 2, "-d") || !args.front().compare(0, 3, "--d")) {
    args.pop_front();
    if (args.size() == 0) {  // Must have a <device> if -d or --d
      ShowUsage(false);
      return 0;
    }
    // Allows using only the devfs node number, for instance "123" instead of
    // "/dev/class/codec/123".
    path = args.front();
    args.pop_front();
    int id = -1;
    if (sscanf(path.c_str(), "%u", &id) == 1) {
      path = std::string(kCodecClassDir) + "/" + path;
    }
  } else if (!args.front().compare(0, 2, "-h") || !args.front().compare(0, 3, "--h")) {
    ShowUsage(true);
    return 0;
  }

  if (args.size() == 0) {  // Must have a command.
    ShowUsage(false);
    return 0;
  }

  std::string_view cmd(args.front());
  args.pop_front();
  switch (cmd[0]) {
    case 'f': {
      auto result = GetCodecClient(path)->GetDaiFormats();
      if (result.is_error()) {
        std::cerr << "get DAI formats failed: " << result.error_value().FormatDescription()
                  << std::endl;
        return -1;
      }
      std::cout << FidlString(result->formats()) << std::endl;
      return 0;
    }

    case 'i': {
      auto result = GetCodecClient(path)->GetInfo();
      if (result.is_error()) {
        std::cerr << "get info failed: " << result.error_value().FormatDescription() << std::endl;
        return -1;
      } else {
        std::cout << FidlString(result->info()) << std::endl;
      }
      return 0;
    }

    case 'c': {
      auto result = GetCodecClient(path)->GetPlugDetectCapabilities();
      if (!result.is_ok()) {
        std::cerr << "get plug detect capabilities failed: "
                  << result.error_value().FormatDescription() << std::endl;
        return -1;
      }
      std::cout << FidlString(result->plug_detect_capabilities()) << std::endl;
      return 0;
    }

    case 'b': {
      auto result = GetCodecClient(path)->IsBridgeable();
      if (!result.is_ok()) {
        std::cerr << "is bridgeable failed: " << result.error_value().FormatDescription()
                  << std::endl;
        return -1;
      }
      std::cout << "Is bridgeable: " << FidlString(result->supports_bridged_mode()) << std::endl;
      return 0;
    }

    case 'm': {
      bool mode = false;
      if (args.size() == 0) {  // Must have a mode.
        ShowUsage(false);
        return -1;
      }
      if (args.front()[0] == 't') {
        mode = true;
      }
      args.pop_front();
      auto result = GetCodecClient(path)->SetBridgedMode(mode);
      std::cout << "Setting bridged mode to: " << (mode ? "true" : "false") << std::endl;
      if (!result.is_ok()) {
        std::cerr << "set bridged mode failed: " << result.error_value().FormatDescription()
                  << std::endl;
        return -1;
      }
      return 0;
    }

    case 'd': {
      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      uint32_t number_of_channels = 0;
      if (sscanf(args.front().c_str(), "%u", &number_of_channels) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      uint64_t channels_to_use_bitmask = 0;
      if (sscanf(args.front().c_str(), "%lx", &channels_to_use_bitmask) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      fuchsia_hardware_audio::DaiSampleFormat sample_format = {};
      switch (args.front()[0]) {
        case 'p':
          sample_format = fuchsia_hardware_audio::DaiSampleFormat::kPdm;
          break;
        case 'u':
          sample_format = fuchsia_hardware_audio::DaiSampleFormat::kPcmUnsigned;
          break;
        case 's':
        default:
          sample_format = fuchsia_hardware_audio::DaiSampleFormat::kPcmSigned;
          break;
        case 'f':
          sample_format = fuchsia_hardware_audio::DaiSampleFormat::kPcmFloat;
          break;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      auto frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard({});
      switch (args.front()[0]) {
        case 'n':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kNone);
          break;
        case 'i':
        default:
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kI2S);
          break;
        case 'l':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kStereoLeft);
          break;
        case 'r':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kStereoRight);
          break;
        case '1':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kTdm1);
          break;
        case '2':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kTdm2);
          break;
        case '3':
          frame_format = fuchsia_hardware_audio::DaiFrameFormat::WithFrameFormatStandard(
              fuchsia_hardware_audio::DaiFrameFormatStandard::kTdm3);
          break;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      uint32_t frame_rate = 0;
      if (sscanf(args.front().c_str(), "%u", &frame_rate) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      uint32_t bits_per_slot = 0;
      if (sscanf(args.front().c_str(), "%u", &bits_per_slot) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();

      if (args.size() == 0) {
        ShowUsage(false);
        return -1;
      }
      uint32_t bits_per_sample = 0;
      if (sscanf(args.front().c_str(), "%u", &bits_per_sample) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();

      fuchsia_hardware_audio::DaiFormat format(number_of_channels, channels_to_use_bitmask,
                                               std::move(sample_format), std::move(frame_format),
                                               frame_rate, static_cast<uint8_t>(bits_per_slot),
                                               static_cast<uint8_t>(bits_per_sample));
      std::cout << "Setting DAI format:" << std::endl;
      std::cout << FidlString(format) << std::endl;
      auto result = GetCodecClient(path)->SetDaiFormat(std::move(format));
      if (!result.is_ok()) {
        std::cerr << "set DAI format failed: " << result.error_value().FormatDescription()
                  << std::endl;
        return -1;
      }
      return 0;
    }

    case 'r': {
      auto result = GetCodecClient(path)->Reset();
      if (!result.is_ok()) {
        std::cerr << "reset failed: " << result.error_value().FormatDescription() << std::endl;

        return -1;
      }
      std::cout << "Reset done" << std::endl;
      return 0;
    }

    case 's':
      if (cmd == "start") {
        auto result = GetCodecClient(path)->Start();
        if (!result.is_ok()) {
          std::cerr << "start failed: " << result.error_value().FormatDescription() << std::endl;
          return -1;
        }
        std::cout << "Start done" << std::endl;
        return 0;
      } else if (cmd == "stop") {
        auto result = GetCodecClient(path)->Stop();
        if (!result.is_ok()) {
          std::cerr << "stop failed: " << result.error_value().FormatDescription() << std::endl;
          return -1;
        }
        std::cout << "Stop done" << std::endl;
        return 0;
      } else if (cmd == "set") {
        if (args.size() == 0) {  // Must have an id.
          ShowUsage(false);
          return -1;
        }
        uint64_t id = 0;
        if (sscanf(args.front().c_str(), "%lu", &id) != 1) {
          ShowUsage(false);
          return -1;
        }
        args.pop_front();

        fuchsia_hardware_audio_signalprocessing::ElementState state;

        if (args.size() > 0) {
          if (args.front() == "enable") {
            args.pop_front();
            state.enabled(true);
          } else if (args.front() == "disable") {
            args.pop_front();
            state.enabled(false);
          }
        }

        if (args.size() > 0) {
          if (args.front() == "gain") {
            args.pop_front();
            if (args.size() > 0) {
              fuchsia_hardware_audio_signalprocessing::GainElementState gain_state;
              float gain = std::stof(args.front());
              args.pop_front();
              gain_state.gain(gain);
              state.type_specific(
                  fuchsia_hardware_audio_signalprocessing::TypeSpecificElementState::WithGain(
                      std::move(gain_state)));
            } else {
              std::cerr << "set processing element state failed: no gain specified" << std::endl;
              return -1;
            }
          }
        }

        if (args.size() > 0) {
          if (args.front() == "latency") {
            args.pop_front();
            if (args.size() > 0) {
              uint64_t latency = 0;
              if (sscanf(args.front().c_str(), "%lu", &latency) != 1) {
                ShowUsage(false);
                return -1;
              }
              args.pop_front();
              state.latency(
                  fuchsia_hardware_audio_signalprocessing::Latency::WithLatencyTime(latency));
            } else {
              std::cerr << "set processing element state failed: no latency specified" << std::endl;
              return -1;
            }
          }
        }

        if (args.size() > 0) {
          if (args.front() == "vendor") {
            args.pop_front();
            std::vector<uint8_t> bytes;
            while (args.size() > 0) {
              uint32_t hex = 0;
              if (sscanf(args.front().c_str(), "%x", &hex) != 1) {
                ShowUsage(false);
                return -1;
              }
              args.pop_front();
              bytes.push_back(static_cast<uint8_t>(hex));
            }
            state.vendor_specific_data(std::move(bytes));
          }
        }

        if (args.size() > 0) {  // Error if we have unparsed parameters.
          ShowUsage(false);
          return -1;
        }

        fuchsia_hardware_audio_signalprocessing::SignalProcessingSetElementStateRequest request(
            id, std::move(state));
        std::cout << "Setting element state:" << std::endl;
        std::cout << FidlString(request) << std::endl;
        auto result = GetSignalClient(path)->SetElementState(std::move(request));
        if (result.is_error()) {
          std::cerr << "set processing element state failed: "
                    << result.error_value().FormatDescription() << std::endl;
          return -1;
        }
        return 0;
      }
      break;

    case 'p': {
      auto result = GetCodecClient(path)->WatchPlugState();
      if (!result.is_ok()) {
        std::cerr << "watch plug state failed: " << result.error_value().FormatDescription()
                  << std::endl;
        return -1;
      }
      std::cout << FidlString(result->plug_state()) << std::endl;
      return 0;
    }

    case 'e': {
      auto result = GetSignalClient(path)->GetElements();
      if (result.is_error()) {
        std::cerr << "get signal processing elements failed: "
                  << result.error_value().FormatDescription() << std::endl;
        return -1;
      }
      std::cout << FidlString(result->processing_elements()) << std::endl;
      return 0;
    }

    case 't': {
      auto result = GetSignalClient(path)->GetTopologies();
      if (result.is_error()) {
        std::cerr << "get signal processing topologies failed: "
                  << result.error_value().FormatDescription() << std::endl;
        return -1;
      }
      std::cout << FidlString(result->topologies()) << std::endl;
      return 0;
    }

    case 'w': {
      if (args.size() == 0) {  // Must have an id.

        ShowUsage(false);
        return -1;
      }
      uint64_t id = 0;
      if (sscanf(args.front().c_str(), "%lu", &id) != 1) {
        ShowUsage(false);
        return -1;
      }
      args.pop_front();
      auto result = GetSignalClient(path)->WatchElementState(id);
      if (result.is_error()) {
        std::cerr << "watch processing element state failed: "
                  << result.error_value().FormatDescription() << std::endl;
        return -1;
      }
      std::cout << FidlString(result->state()) << std::endl;
      return 0;
    }

    default:
      ShowUsage(false);
      return -1;
  }

  ShowUsage(false);
  return 0;
}
