// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_TOOLS_BT_INTEL_TOOL_COMMAND_CHANNEL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_TOOLS_BT_INTEL_TOOL_COMMAND_CHANNEL_H_

#include <fidl/fuchsia.hardware.bluetooth/cpp/wire.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <zircon/types.h>

#include "src/connectivity/bluetooth/core/bt-host/transport/control_packets.h"

// Sends and receives events from a command channel that it retrieves from a
// Zircon Bluetooth HCI device.  It parses the incoming event packets, only
// returning complete and valid event packets on to the event handler set.
class CommandChannel {
 public:
  explicit CommandChannel(fidl::ClientEnd<fuchsia_hardware_bluetooth::Hci> device);
  ~CommandChannel();

  // Indicates whether this channel is valid.  This should be checked after
  // construction.
  bool is_valid() const { return valid_; }

  // Sets the event callback to be called when an HCI Event arrives on the
  // channel.
  using EventCallback = fit::function<void(const ::bt::hci::EventPacket& event_packet)>;
  void SetEventCallback(EventCallback callback);

  // Sends the command in |command| to the controller. The channel must
  // be Ready when this is called.
  void SendCommand(const ::bt::PacketView<::bt::hci_spec::CommandHeader>& command);

  // Sends the command in |command| to the controller and waits for
  // an Event, which is delivered to |callback| before this function
  // returns.
  void SendCommandSync(const ::bt::PacketView<::bt::hci_spec::CommandHeader>& command,
                       EventCallback callback);

 private:
  // Common read handler implemntation
  void HandleChannelReady(const zx::channel& channel, async_dispatcher_t* dispatcher,
                          async::WaitBase* wait, zx_status_t status,
                          const zx_packet_signal_t* signal);

  // Read ready handler for |cmd_channel_|
  void OnCmdChannelReady(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                         const zx_packet_signal_t* signal);

  // Read ready handler for |acl_channel_|
  void OnAclChannelReady(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                         const zx_packet_signal_t* signal);

  bool valid_;
  EventCallback event_callback_;
  fidl::WireSyncClient<fuchsia_hardware_bluetooth::Hci> client_;
  zx::channel cmd_channel_;
  async::WaitMethod<CommandChannel, &CommandChannel::OnCmdChannelReady> cmd_channel_wait_{this};
  zx::channel acl_channel_;
  async::WaitMethod<CommandChannel, &CommandChannel::OnAclChannelReady> acl_channel_wait_{this};
};

#endif  // SRC_CONNECTIVITY_BLUETOOTH_TOOLS_BT_INTEL_TOOL_COMMAND_CHANNEL_H_
