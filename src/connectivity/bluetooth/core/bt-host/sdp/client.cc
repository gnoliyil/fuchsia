// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client.h"

#include <lib/async/default.h>

#include <functional>
#include <optional>

namespace bt::sdp {

namespace {

// Increased after some particularly slow devices taking a long time for transactions with
// continuations.
constexpr zx::duration kTransactionTimeout = zx::sec(10);

class Impl final : public Client {
 public:
  explicit Impl(l2cap::Channel::WeakPtr channel);

  ~Impl() override;

 private:
  void ServiceSearchAttributes(std::unordered_set<UUID> search_pattern,
                               const std::unordered_set<AttributeId>& req_attributes,
                               SearchResultFunction result_cb) override;

  // Information about a transaction that hasn't finished yet.
  struct Transaction {
    Transaction(TransactionId id, ServiceSearchAttributeRequest req, SearchResultFunction cb);
    // The TransactionId used for this request.  This will be reused until the
    // transaction is complete.
    TransactionId id;
    // Request PDU for this transaction.
    ServiceSearchAttributeRequest request;
    // Callback for results.
    SearchResultFunction callback;
    // The response, built from responses from the remote server.
    ServiceSearchAttributeResponse response;
  };

  // Callbacks for l2cap::Channel
  void OnRxFrame(ByteBufferPtr data);
  void OnChannelClosed();

  // Finishes a pending transaction on this client, completing their callbacks.
  void Finish(TransactionId id);

  // Cancels a pending transaction this client has started, completing the callback with the given
  // reason as an error.
  void Cancel(TransactionId id, HostError reason);

  // Cancels all remaining transactions without sending them, with the given reason as an error.
  void CancelAll(HostError reason);

  // Get the next available transaction id
  TransactionId GetNextId();

  // Try to send the next pending request, if possible.
  void TrySendNextTransaction();

  // The channel that this client is running on.
  l2cap::ScopedChannel channel_;
  // THe next transaction id that we should use
  TransactionId next_tid_ = 0;
  // Any transactions that are not completed.
  std::unordered_map<TransactionId, Transaction> pending_;
  // Timeout for the current transaction. false if none are waiting for a response.
  std::optional<async::TaskClosure> pending_timeout_;

  WeakSelf<Impl> weak_self_{this};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Impl);
};

Impl::Impl(l2cap::Channel::WeakPtr channel) : channel_(std::move(channel)) {
  auto self = weak_self_.GetWeakPtr();
  bool activated = channel_->Activate(
      [self](auto packet) {
        if (self.is_alive()) {
          self->OnRxFrame(std::move(packet));
        }
      },
      [self] {
        if (self.is_alive()) {
          self->OnChannelClosed();
        }
      });
  if (!activated) {
    bt_log(INFO, "sdp", "failed to activate channel");
    channel_ = nullptr;
  }
}

Impl::~Impl() { CancelAll(HostError::kCanceled); }

void Impl::CancelAll(HostError reason) {
  // Avoid using |this| in case callbacks destroy this object.
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& it : pending) {
    it.second.callback(ToResult(reason).take_error());
  }
}

void Impl::TrySendNextTransaction() {
  if (pending_timeout_) {
    // Waiting on a transaction to finish.
    return;
  }

  if (!channel_) {
    bt_log(INFO, "sdp", "Failed to send %zu requests: link closed", pending_.size());
    CancelAll(HostError::kLinkDisconnected);
    return;
  }

  if (pending_.empty()) {
    return;
  }

  auto& next = pending_.begin()->second;

  if (!channel_->Send(next.request.GetPDU(next.id))) {
    bt_log(INFO, "sdp", "Failed to send request: channel send failed");
    Cancel(next.id, HostError::kFailed);
    return;
  }

  auto& timeout = pending_timeout_.emplace();

  // Timeouts are held in this so it is safe to use.
  timeout.set_handler([this, id = next.id]() {
    bt_log(WARN, "sdp", "Transaction %d timed out, removing!", id);
    Cancel(id, HostError::kTimedOut);
  });
  timeout.PostDelayed(async_get_default_dispatcher(), kTransactionTimeout);
}

void Impl::ServiceSearchAttributes(std::unordered_set<UUID> search_pattern,
                                   const std::unordered_set<AttributeId>& req_attributes,
                                   SearchResultFunction result_cb) {
  ServiceSearchAttributeRequest req;
  req.set_search_pattern(std::move(search_pattern));
  if (req_attributes.empty()) {
    req.AddAttributeRange(0, 0xFFFF);
  } else {
    for (const auto& id : req_attributes) {
      req.AddAttribute(id);
    }
  }
  TransactionId next = GetNextId();

  auto [iter, placed] = pending_.try_emplace(next, next, std::move(req), std::move(result_cb));
  BT_DEBUG_ASSERT_MSG(placed, "Should not have repeat transaction ID %u", next);

  TrySendNextTransaction();
}

void Impl::Finish(TransactionId id) {
  auto node = pending_.extract(id);
  BT_DEBUG_ASSERT(node);
  auto& state = node.mapped();
  pending_timeout_.reset();
  if (!state.callback) {
    return;
  }
  BT_DEBUG_ASSERT_MSG(state.response.complete(), "Finished without complete response");

  auto self = weak_self_.GetWeakPtr();

  size_t count = state.response.num_attribute_lists();
  for (size_t idx = 0; idx <= count; idx++) {
    if (idx == count) {
      state.callback(fit::error(Error(HostError::kNotFound)));
      break;
    }
    // |count| and |idx| are at most std::numeric_limits<uint32_t>::max() + 1, which is caught by
    // the above if statement.
    BT_DEBUG_ASSERT(idx <= std::numeric_limits<uint32_t>::max());
    if (!state.callback(
            fit::ok(std::cref(state.response.attributes(static_cast<uint32_t>(idx)))))) {
      break;
    }
  }

  // Callbacks may have destroyed this object.
  if (!self.is_alive()) {
    return;
  }

  TrySendNextTransaction();
}

Impl::Transaction::Transaction(TransactionId id, ServiceSearchAttributeRequest req,
                               SearchResultFunction cb)
    : id(id), request(std::move(req)), callback(std::move(cb)) {}

void Impl::Cancel(TransactionId id, HostError reason) {
  auto node = pending_.extract(id);
  if (!node) {
    return;
  }

  auto self = weak_self_.GetWeakPtr();
  node.mapped().callback(ToResult(reason).take_error());
  if (!self.is_alive()) {
    return;
  }

  TrySendNextTransaction();
}

void Impl::OnRxFrame(ByteBufferPtr data) {
  TRACE_DURATION("bluetooth", "sdp::Client::Impl::OnRxFrame");
  // Each SDU in SDP is one request or one response. Core 5.0 Vol 3 Part B, 4.2
  PacketView<sdp::Header> packet(data.get());
  size_t pkt_params_len = data->size() - sizeof(Header);
  uint16_t params_len = betoh16(packet.header().param_length);
  if (params_len != pkt_params_len) {
    bt_log(INFO, "sdp", "bad params length (len %zu != %u), dropping", pkt_params_len, params_len);
    return;
  }
  packet.Resize(params_len);
  TransactionId tid = betoh16(packet.header().tid);
  auto it = pending_.find(tid);
  if (it == pending_.end()) {
    bt_log(INFO, "sdp", "Received unknown transaction id (%u)", tid);
    return;
  }
  auto& transaction = it->second;
  fit::result<Error<>> parse_status = transaction.response.Parse(packet.payload_data());
  if (parse_status.is_error()) {
    if (parse_status.error_value().is(HostError::kInProgress)) {
      bt_log(INFO, "sdp", "Requesting continuation of id (%u)", tid);
      transaction.request.SetContinuationState(transaction.response.ContinuationState());
      if (!channel_->Send(transaction.request.GetPDU(tid))) {
        bt_log(INFO, "sdp", "Failed to send continuation of transaction!");
      }
      return;
    }
    bt_log(INFO, "sdp", "Failed to parse packet for tid %u: %s", tid, bt_str(parse_status));
    // Drop the transaction with the error.
    Cancel(tid, parse_status.error_value().host_error());
    return;
  }
  if (transaction.response.complete()) {
    bt_log(DEBUG, "sdp", "Rx complete, finishing tid %u", tid);
    Finish(tid);
  }
}

void Impl::OnChannelClosed() {
  bt_log(INFO, "sdp", "client channel closed");
  channel_ = nullptr;
  CancelAll(HostError::kLinkDisconnected);
}

TransactionId Impl::GetNextId() {
  TransactionId next = next_tid_++;
  BT_DEBUG_ASSERT(pending_.size() < std::numeric_limits<TransactionId>::max());
  while (pending_.count(next)) {
    next = next_tid_++;  // Note: overflow is fine
  }
  return next;
}

}  // namespace

std::unique_ptr<Client> Client::Create(l2cap::Channel::WeakPtr channel) {
  BT_DEBUG_ASSERT(channel.is_alive());
  return std::make_unique<Impl>(std::move(channel));
}

}  // namespace bt::sdp
