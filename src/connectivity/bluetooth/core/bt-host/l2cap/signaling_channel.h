// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_SIGNALING_CHANNEL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_SIGNALING_CHANNEL_H_

#include <memory>
#include <unordered_map>

#include "lib/async/cpp/task.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/common/packet_view.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/scoped_channel.h"

namespace bt::l2cap {

class Channel;

namespace internal {

using SignalingPacket = PacketView<CommandHeader>;
using MutableSignalingPacket = MutablePacketView<CommandHeader>;

using DataCallback = fit::function<void(const ByteBuffer& data)>;
using SignalingPacketHandler = fit::function<void(const SignalingPacket& packet)>;

// SignalingChannelInterface contains the procedures that command flows use to
// send and receive signaling channel transactions.
class SignalingChannelInterface {
 public:
  // Action in response to a request-type packet.
  enum class Status {
    kSuccess,  // Remote response received
    kReject,   // Remote rejection received
    kTimeOut,  // Timed out waiting for matching remote command
  };

  // ResponseHandler return value. Indicates whether additional responses are expected in this
  // transaction (e.g. in the case of receiving a response with a pending status or continuation
  // flag).
  enum class ResponseHandlerAction {
    kExpectAdditionalResponse,
    // No additional responses expected in this transaction.
    kCompleteOutboundTransaction,
  };

  // Callback invoked to handle a response received from the remote. If |status| is kSuccess or
  // kReject, then |rsp_payload| will contain any payload received. This callback is allowed to
  // destroy the SignalingChannel, but must return kCompleteOutboundTransaction if it does.
  using ResponseHandler =
      fit::function<ResponseHandlerAction(Status, const ByteBuffer& rsp_payload)>;

  // Initiate an outbound transaction. The signaling channel will send a request
  // then expect reception of one or more responses with a code one greater than
  // the request. Each response or rejection received invokes |cb|. When |cb|
  // returns false, it will be removed. Returns false if the request failed to
  // send.
  virtual bool SendRequest(CommandCode req_code, const ByteBuffer& payload, ResponseHandler cb) = 0;

  // Send a command packet in response to an incoming request.
  class Responder {
   public:
    // Send a response that corresponds to the request received
    virtual void Send(const ByteBuffer& rsp_payload) = 0;

    // Reject invalid, malformed, or unhandled request
    virtual void RejectNotUnderstood() = 0;

    // Reject request non-existent or otherwise invalid channel ID(s)
    virtual void RejectInvalidChannelId(ChannelId local_cid, ChannelId remote_cid) = 0;

   protected:
    virtual ~Responder() = default;
  };

  // Callback invoked to handle a request received from the remote.
  // |req_payload| contains any payload received, without the command header.
  // The callee can use |responder| to respond or reject. Parameters passed to
  // this handler are only guaranteed to be valid while the handler is running.
  using RequestDelegate = fit::function<void(const ByteBuffer& req_payload, Responder* responder)>;

  // Register a handler for all inbound transactions matching |req_code|, which
  // should be the code of a request. |cb| will be called with request payloads
  // received, and is expected to respond to, reject, or ignore the requests.
  // Calls to this function with a previously registered |req_code| will replace
  // the current delegate.
  virtual void ServeRequest(CommandCode req_code, RequestDelegate cb) = 0;

 protected:
  virtual ~SignalingChannelInterface() = default;
};

// SignalingChannel is an abstract class that handles the common operations
// involved in LE and BR/EDR signaling channels.
//
// TODO(armansito): Implement flow control (RTX/ERTX timers).
class SignalingChannel : public SignalingChannelInterface {
 public:
  SignalingChannel(Channel::WeakPtr chan, hci_spec::ConnectionRole role);
  ~SignalingChannel() override = default;

  // SignalingChannelInterface overrides
  bool SendRequest(CommandCode req_code, const ByteBuffer& payload, ResponseHandler cb) override;
  void ServeRequest(CommandCode req_code, RequestDelegate cb) override;

  bool is_open() const { return is_open_; }

  // Local signaling MTU (i.e. MTU_sig, per spec)
  uint16_t mtu() const { return mtu_; }
  void set_mtu(uint16_t mtu) { mtu_ = mtu; }

 protected:
  // Implementation for responding to a request that binds the request's
  // identifier and the response's code so that the client's |Send| invocation
  // does not need to supply them nor even know them.
  class ResponderImpl : public Responder {
   public:
    ResponderImpl(SignalingChannel* sig, CommandCode code, CommandId id);
    void Send(const ByteBuffer& rsp_payload) override;
    void RejectNotUnderstood() override;
    void RejectInvalidChannelId(ChannelId local_cid, ChannelId remote_cid) override;

   private:
    SignalingChannel* sig() const { return sig_; }

    SignalingChannel* const sig_;
    const CommandCode code_;
    const CommandId id_;
  };

  // Sends out a single signaling packet using the given parameters.
  bool SendPacket(CommandCode code, uint8_t identifier, const ByteBuffer& data);

  // True if the code is for a supported response-type signaling command.
  virtual bool IsSupportedResponse(CommandCode code) const = 0;

  // Called when a frame is received to decode into L2CAP signaling command
  // packets. The derived implementation should invoke |cb| for each packet with
  // a valid payload length, send a Command Reject packet for each packet with
  // an intact ID in its header but invalid payload length, and drop any other
  // incoming data.
  virtual void DecodeRxUnit(ByteBufferPtr sdu, const SignalingPacketHandler& cb) = 0;

  // Called when a new signaling packet has been received. Returns false if
  // |packet| is rejected. Otherwise returns true and sends a response packet.
  //
  // This method is thread-safe in that a SignalingChannel cannot be deleted
  // while this is running. SendPacket() can be called safely from this method.
  // TODO(fxbug.dev/1049): make non-virtual & private after removing le signaling channel override
  virtual bool HandlePacket(const SignalingPacket& packet);

  // Sends out a command reject packet with the given parameters.
  bool SendCommandReject(uint8_t identifier, RejectReason reason, const ByteBuffer& data);

  // Returns true if called on this SignalingChannel's creation thread. Mainly
  // intended for debug assertions.

  // Returns the logical link that signaling channel is operating on.
  hci_spec::ConnectionRole role() const { return role_; }

  // Generates a command identifier in sequential order that is never
  // kInvalidId. The caller is responsible for bookkeeping when reusing command
  // IDs to prevent collisions with pending commands.
  CommandId GetNextCommandId();

 private:
  // Enqueue a response to a request with command id |id| and payload |request_packet|. Register a
  // callback |cb| that will be invoked when a response-type command packet (specified by
  // |response_code|) is received. Starts the RTX timer and handles retransmission of
  // |request_packet| and eventual timeout failure if a response isn't received. If the signaling
  // channel receives a Command Reject that matches the same |id|, the rejection packet will be
  // forwarded to the callback instead.
  void EnqueueResponse(const ByteBuffer& request_packet, CommandId id, CommandCode response_code,
                       ResponseHandler cb);

  // Called when a response-type command packet is received. Sends a Command
  // Reject if no ResponseHandler was registered for inbound packet's command
  // code and identifier.
  void OnRxResponse(const SignalingPacket& packet);

  // Called after Response Timeout eXpired (RTX) or Extended Response Timeout eXpired (ERTX) timer
  // expires. |id| must be in |pending_commands_|. If |retransmit| is true, requests will be
  // retransmitted up to the retransmission limit before timing out the response. The
  // ResponseHandler will be invoked with Status::kTimeOut and an empty ByteBuffer.
  void OnResponseTimeout(CommandId id, bool retransmit);

  // True if an outbound request-type command has registered a callback for its
  // response matching a particular |id|.
  bool IsCommandPending(CommandId id) const;

  // Sends out the given signaling packet directly via |chan_| after running
  // debug-mode assertions for validity. Packet must correspond to exactly one
  // C-frame payload.
  //
  // This method is not thread-safe (i.e. requires external locking).
  //
  // TODO(armansito): This should be generalized for ACL-U to allow multiple
  // signaling commands in a single C-frame.
  bool Send(ByteBufferPtr packet);

  // Builds a signaling packet with the given parameters and payload. The
  // backing buffer is slab allocated.
  ByteBufferPtr BuildPacket(CommandCode code, uint8_t identifier, const ByteBuffer& data);
  // Channel callbacks:
  void OnChannelClosed();
  void OnRxBFrame(ByteBufferPtr sdu);

  // Invoke the abstract packet handler |HandlePacket| for well-formed command
  // packets and send responses for command packets that exceed this host's MTU
  // or can't be handled by this host.
  void CheckAndDispatchPacket(const SignalingPacket& packet);

  // Stores copy of request, response handlers, and timeout state for requests that have been sent.
  struct PendingCommand {
    PendingCommand(const ByteBuffer& request_packet, CommandCode response_code,
                   ResponseHandler response_handler)
        : response_code(response_code),
          response_handler(std::move(response_handler)),
          command_packet(std::make_unique<DynamicByteBuffer>(request_packet)),
          transmit_count(1u),
          timer_duration(0u),
          response_timeout_task() {}
    CommandCode response_code;
    ResponseHandler response_handler;

    // Copy of request command packet. Used for retransmissions.
    ByteBufferPtr command_packet;

    // Number of times this request has been transmitted.
    size_t transmit_count;

    // The current timer duration. Used to perform exponential backoff with the RTX timer.
    zx::duration timer_duration;

    // Automatically canceled by destruction if the response is received.
    async::TaskClosure response_timeout_task;
  };

  // Retransmit the request corresponding to |pending_command| and reset the RTX timer.
  void RetransmitPendingCommand(PendingCommand& pending_command);

  bool is_open_;
  l2cap::ScopedChannel chan_;
  hci_spec::ConnectionRole role_;
  uint16_t mtu_;
  uint8_t next_cmd_id_;

  // Stores response handlers for outbound request packets with the corresponding CommandId.
  std::unordered_map<CommandId, PendingCommand> pending_commands_;

  // Stores handlers for incoming request packets.
  std::unordered_map<CommandCode, RequestDelegate> inbound_handlers_;

  WeakSelf<SignalingChannel> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SignalingChannel);
};

}  // namespace internal
}  // namespace bt::l2cap

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_SIGNALING_CHANNEL_H_
