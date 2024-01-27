// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_

#include <lib/fit/function.h>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/command_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/control_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/error.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/link_type.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::hci {

// A Connection represents a logical link connection to a peer. It maintains link-specific
// configuration parameters (such as the connection handle) and state (e.g
// kConnected/kDisconnected). Controller procedures that are related to managing a logical link are
// performed by a Connection, e.g. disconnecting the link.
//
// Connection instances are intended to be uniquely owned. The owner of an instance is also the
// owner of the underlying link and the lifetime of a Connection determines the lifetime of the
// link.
//
// Connection is not expected to be constructed directly. Users should instead construct a
// specialization based on the link type: LowEnergyConnection, BrEdrConnection, or ScoConnection,
class Connection {
 public:
  enum class State {
    // Default state of a newly created Connection. This is the only connection state that is
    // considered "open".
    kConnected,

    // HCI Disconnect command has been sent, but HCI Disconnection Complete event has not yet been
    // received. This state is skipped when the disconnection is initiated by the peer.
    kWaitingForDisconnectionComplete,

    // HCI Disconnection Complete event has been received.
    kDisconnected
  };

  // The destructor closes this connection.
  virtual ~Connection();

  // Returns a string representation.
  virtual std::string ToString() const;

  // Returns the 12-bit connection handle of this connection. This handle is
  // used to identify an individual logical link maintained by the controller.
  hci_spec::ConnectionHandle handle() const { return handle_; }

  // The local device address used while establishing the connection.
  const DeviceAddress& local_address() const { return local_address_; }

  // The peer address used while establishing the connection.
  const DeviceAddress& peer_address() const { return peer_address_; }

  State state() const { return conn_state_; }

  // Assigns a callback that will be run when the peer disconnects.
  using PeerDisconnectCallback =
      fit::function<void(const Connection& connection, pw::bluetooth::emboss::StatusCode reason)>;
  void set_peer_disconnect_callback(PeerDisconnectCallback callback) {
    peer_disconnect_callback_ = std::move(callback);
  }

  // Send HCI Disconnect and set state to closed. Must not be called on an already disconnected
  // connection.
  virtual void Disconnect(pw::bluetooth::emboss::StatusCode reason);

 protected:
  // |on_disconnection_complete| will be called when the disconnection complete event is received,
  // which may be after this object is destroyed (which is why this isn't a virtual method).
  Connection(hci_spec::ConnectionHandle handle, const DeviceAddress& local_address,
             const DeviceAddress& peer_address, const Transport::WeakPtr& hci,
             fit::callback<void()> on_disconnection_complete);

  const Transport::WeakPtr& hci() { return hci_; }

  PeerDisconnectCallback& peer_disconnect_callback() { return peer_disconnect_callback_; }

 private:
  // Checks |event|, unregisters link, and clears pending packets count.
  // If the disconnection was initiated by the peer, call |peer_disconnect_callback|.
  // Returns true if event was valid and for this connection.
  // This method is static so that it can be called in an event handler
  // after this object has been destroyed.
  static CommandChannel::EventCallbackResult OnDisconnectionComplete(
      const WeakSelf<Connection>::WeakPtr& self, hci_spec::ConnectionHandle handle,
      const EventPacket& event, fit::callback<void()> on_disconnection_complete);

  hci_spec::ConnectionHandle handle_;

  // Addresses used while creating the link.
  DeviceAddress local_address_;
  DeviceAddress peer_address_;

  PeerDisconnectCallback peer_disconnect_callback_;

  State conn_state_;

  Transport::WeakPtr hci_;

  WeakSelf<Connection> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Connection);
};

}  // namespace bt::hci

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_
