// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_ATT_BEARER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_ATT_BEARER_H_

#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>

#include <map>
#include <memory>
#include <unordered_map>

#include "src/connectivity/bluetooth/core/bt-host/att/att.h"
#include "src/connectivity/bluetooth/core/bt-host/att/error.h"
#include "src/connectivity/bluetooth/core/bt-host/att/packet.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/common/weak_self.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/scoped_channel.h"

namespace bt::att {

// Implements an ATT data bearer with the following features:
//
//   * This can be used over either a LE-U or an ACL-U logical link. No
//     assumptions are made on the logical transport of the underlying L2CAP
//     channel.
//   * Can simultaneously operate in both server and client roles of the
//     protocol.
//
// Deleting a Bearer closes the underlying channel. Unlike ShutDown(), this
// does NOT notify any callbacks to prevent them from running in destructors.
//
// THREAD-SAFETY:
//
// This class is intended to be created, accessed, and destroyed on the same
// thread. All callbacks will be invoked on a Bearer's creation thread.
class Bearer final {
 public:
  // Creates a new ATT Bearer. Returns nullptr if |chan| cannot be activated.
  // This can happen if the link is closed.
  static std::unique_ptr<Bearer> Create(l2cap::Channel::WeakPtr chan);

  ~Bearer();

  // Returns true if the underlying channel is open.
  bool is_open() const { return static_cast<bool>(chan_); }

  // The bi-directional (client + server) MTU currently in use. The default
  // value is kLEMinMTU.
  //
  // NOTE: This is allowed to be initialized to something smaller than kLEMinMTU
  // for unit tests.
  uint16_t mtu() const { return mtu_; }
  void set_mtu(uint16_t value) {
    bt_log(DEBUG, "att", "bearer: new MTU %u", value);
    mtu_ = value;
  }

  // The preferred MTU. This is initially assigned based on the MTU of the
  // underlying L2CAP channel and will be used in future MTU Exchange
  // procedures.
  uint16_t preferred_mtu() const { return preferred_mtu_; }
  void set_preferred_mtu(uint16_t value) {
    BT_DEBUG_ASSERT(value >= kLEMinMTU);
    preferred_mtu_ = value;
  }

  // Returns the correct minimum ATT_MTU based on the underlying link type.
  uint16_t min_mtu() const { return min_mtu_; }

  // Returns the current link security properties.
  sm::SecurityProperties security() const {
    return chan_ ? chan_->security() : sm::SecurityProperties();
  }

  // Sets a callback to be invoked invoked when the underlying channel has
  // closed. |callback| should disconnect the underlying logical link.
  void set_closed_callback(fit::closure callback) { closed_cb_ = std::move(callback); }

  // Closes the channel. This should be called when a protocol transaction
  // warrants the link to be disconnected. Notifies any callback set via
  // |set_closed_callback()| and notifies the error callback of all pending HCI
  // transactions.
  //
  // Does nothing if the channel is not open.
  //
  // NOTE: Bearer internally shuts down the link on request timeouts and
  // sequential protocol violations.
  void ShutDown();

  // Initiates an asynchronous transaction and invokes |callback| on this
  // Bearer's creation thread when the transaction completes. |pdu| must
  // correspond to a request or indication.
  //
  // TransactionCallback is used to report the end of a request or indication
  // transaction. |packet| will contain a matching response or confirmation PDU
  // depending on the transaction in question.
  //
  // If the transaction ends with an error or cannot complete (e.g. due to a
  // timeout), |error_callback| will be called instead of |callback| if
  // provided.
  //
  // Returns false if |pdu| is empty, exceeds the current MTU, or does not
  // correspond to a request or indication.
  using TransactionResult = fit::result<std::pair<Error, Handle>, PacketReader>;
  using TransactionCallback = fit::callback<void(TransactionResult)>;
  void StartTransaction(ByteBufferPtr pdu, TransactionCallback callback);

  // Sends |pdu| without initiating a transaction. Used for command and
  // notification PDUs which do not have flow control.
  //
  // Returns false if the packet is malformed or does not correspond to a
  // command or notification.
  [[nodiscard]] bool SendWithoutResponse(ByteBufferPtr pdu);

  // A Handler is a function that gets invoked when the Bearer receives a PDU
  // that is not tied to a locally initiated transaction (see
  // StartTransaction()).
  //
  //   * |tid| will be set to kInvalidTransactionId for command and notification
  //     PDUs. These do not require a response and are not part of a
  //     transaction.
  //
  //   * A valid |tid| will be provided for request and indication PDUs. These
  //     require a response which can be sent by calling Reply().
  using TransactionId = size_t;
  static constexpr TransactionId kInvalidTransactionId = 0u;
  using Handler = fit::function<void(TransactionId tid, const PacketReader& packet)>;

  // Handler: called when |pdu| does not need flow control. This will be
  // called for commands and notifications.
  using HandlerId = size_t;
  static constexpr HandlerId kInvalidHandlerId = 0u;
  HandlerId RegisterHandler(OpCode opcode, Handler handler);

  // Unregisters a handler. |id| cannot be zero.
  void UnregisterHandler(HandlerId id);

  // Ends a currently pending transaction with the given response or
  // confirmation |pdu|. Returns false if |pdu| is malformed or if |id| and
  // |pdu| do not match a pending transaction.
  bool Reply(TransactionId tid, ByteBufferPtr pdu);

  // Ends a request transaction with an error response.
  bool ReplyWithError(TransactionId id, Handle handle, ErrorCode error_code);

  using WeakPtr = WeakSelf<Bearer>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  explicit Bearer(l2cap::Channel::WeakPtr chan);

  // Returns false if activation fails. This is called by the factory method.
  bool Activate();

  // Represents a locally initiated pending request or indication transaction.
  struct PendingTransaction {
    PendingTransaction(OpCode opcode, TransactionCallback callback, ByteBufferPtr pdu);

    // Required fields
    OpCode opcode;
    TransactionCallback callback;

    // Holds the pdu while the transaction is in the send queue.
    ByteBufferPtr pdu;

    // Contains the most recently requested security upgrade level under which
    // this transaction has been retried following an ATT security error. The
    // states look like the following:
    //
    //   - sm::SecurityLevel::kNoSecurity: The transaction has not been retried.
    //   - sm::SecurityLevel::kEncrypted: The request has been queued following
    //     a security upgrate to level "Encrypted".
    //   - sm::SecurityLevel::kAuthenticated: The request has been queued
    //     following a security upgrate to level "Authenticated".
    //
    // and so on.
    sm::SecurityLevel security_retry_level;
  };
  using PendingTransactionPtr = std::unique_ptr<PendingTransaction>;

  // Represents a remote initiated pending request or indication transaction.
  struct PendingRemoteTransaction {
    PendingRemoteTransaction(TransactionId id, OpCode opcode);
    PendingRemoteTransaction() = default;

    TransactionId id;
    OpCode opcode;
  };

  // Used the represent the state of active ATT protocol request and indication
  // transactions.
  class TransactionQueue {
   public:
    TransactionQueue() = default;
    ~TransactionQueue() = default;

    TransactionQueue(TransactionQueue&& other);

    // Returns the transaction that has been sent to the peer and is currently
    // pending completion.
    inline PendingTransaction* current() const { return current_.get(); }

    // Clears the currently pending transaction data and cancels its transaction
    // timeout task. Returns ownership of any pending transaction to the caller.
    PendingTransactionPtr ClearCurrent();

    // Tries to initiate the next transaction. Sends the PDU over |chan| if
    // successful.
    void TrySendNext(const l2cap::Channel::WeakPtr& chan, async::Task::Handler timeout_cb,
                     zx::duration timeout);

    // Adds |next| to the transaction queue.
    void Enqueue(PendingTransactionPtr transaction);

    // Resets the contents of this queue to their default state.
    void Reset();

    // Invokes the error callbacks of all transactions with |status|.
    void InvokeErrorAll(Error error);

   private:
    std::queue<std::unique_ptr<PendingTransaction>> queue_;
    std::unique_ptr<PendingTransaction> current_;
    async::Task timeout_task_;
  };

  bool SendInternal(ByteBufferPtr pdu, TransactionCallback callback);

  // Shuts down the link.
  void ShutDownInternal(bool due_to_timeout);

  // Returns false if |packet| is malformed.
  bool IsPacketValid(const ByteBuffer& packet);

  // Tries to initiate the next transaction from the given |queue|.
  void TryStartNextTransaction(TransactionQueue* tq);

  // Sends out an immediate error response.
  void SendErrorResponse(OpCode request_opcode, Handle attribute_handle, ErrorCode error_code);

  // Called when the peer sends us a response or confirmation PDU.
  void HandleEndTransaction(TransactionQueue* tq, const PacketReader& packet);

  // Returns the next handler ID and increments the counter.
  HandlerId NextHandlerId();

  // Returns the next remote-initiated transaction ID.
  TransactionId NextRemoteTransactionId();

  using RemoteTransaction = std::optional<PendingRemoteTransaction>;

  // Called when the peer initiates a request or indication transaction.
  void HandleBeginTransaction(RemoteTransaction* currently_pending, const PacketReader& packet);

  // Returns any pending peer-initiated transaction that matches |id|. Returns
  // nullptr otherwise.
  RemoteTransaction* FindRemoteTransaction(TransactionId id);

  // Called when the peer sends us a PDU that has no flow control (i.e. command
  // or notification). Routes the packet to the corresponding handler. Drops the
  // packet if no handler exists.
  void HandlePDUWithoutResponse(const PacketReader& packet);

  // l2cap::Channel callbacks:
  void OnChannelClosed();
  void OnRxBFrame(ByteBufferPtr sdu);

  l2cap::ScopedChannel chan_;
  uint16_t mtu_;
  uint16_t preferred_mtu_;
  uint16_t min_mtu_;

  // Set to true on the first call to ShutDownInternal.
  bool shut_down_ = false;

  // Channel closed callback assigned to us via set_closed_callback().
  fit::closure closed_cb_;

  // The state of outgoing ATT requests and indications
  TransactionQueue request_queue_;
  TransactionQueue indication_queue_;

  // The transaction identifier that will be assigned to the next
  // remote-initiated request or indication transaction.
  TransactionId next_remote_transaction_id_;

  // The next available remote-initiated PDU handler id.
  HandlerId next_handler_id_;

  // Data about currently registered handlers.
  std::unordered_map<HandlerId, OpCode> handler_id_map_;
  std::unordered_map<OpCode, Handler> handlers_;

  // Remote-initiated transactions in progress.
  RemoteTransaction remote_request_;
  RemoteTransaction remote_indication_;

  WeakSelf<Bearer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Bearer);
};

}  // namespace bt::att

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_ATT_BEARER_H_
