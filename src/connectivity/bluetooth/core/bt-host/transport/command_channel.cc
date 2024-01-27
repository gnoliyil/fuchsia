// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "command_channel.h"

#include <endian.h>
#include <lib/async/default.h>
#include <lib/fit/defer.h>
#include <zircon/status.h>

#include "slab_allocators.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/trace.h"
#include "src/connectivity/bluetooth/lib/cpp-string/string_printf.h"

namespace bt::hci {

namespace {

// Helper for std::variant
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// explicit deduction guide (not needed in C++20)
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

static bool IsAsync(hci_spec::EventCode code) {
  return code != hci_spec::kCommandCompleteEventCode && code != hci_spec::kCommandStatusEventCode;
}

static std::string EventTypeToString(CommandChannel::EventType event_type) {
  switch (event_type) {
    case CommandChannel::EventType::kHciEvent:
      return "hci_event";
    case CommandChannel::EventType::kLEMetaEvent:
      return "le_meta_event";
    case CommandChannel::EventType::kVendorEvent:
      return "vendor_event";
  }
}

CommandChannel::QueuedCommand::QueuedCommand(CommandPacketVariant command_packet,
                                             std::unique_ptr<TransactionData> transaction_data)
    : packet(std::move(command_packet)), data(std::move(transaction_data)) {
  BT_DEBUG_ASSERT(data);
}

CommandChannel::TransactionData::TransactionData(
    CommandChannel* channel, TransactionId transaction_id, hci_spec::OpCode opcode,
    hci_spec::EventCode complete_event_code,
    std::optional<hci_spec::EventCode> le_meta_subevent_code,
    std::unordered_set<hci_spec::OpCode> exclusions, CommandCallback callback)
    : channel_(channel),
      transaction_id_(transaction_id),
      opcode_(opcode),
      complete_event_code_(complete_event_code),
      le_meta_subevent_code_(le_meta_subevent_code),
      exclusions_(std::move(exclusions)),
      callback_(std::move(callback)),
      handler_id_(0u) {
  BT_DEBUG_ASSERT(transaction_id != 0u);
  exclusions_.insert(opcode_);
}

CommandChannel::TransactionData::~TransactionData() {
  if (callback_) {
    bt_log(DEBUG, "hci", "destroying unfinished transaction: %zu", transaction_id_);
  }
}

void CommandChannel::TransactionData::StartTimer() {
  // Transactions should only ever be started once.
  BT_DEBUG_ASSERT(!timeout_task_.is_pending());
  timeout_task_.set_handler([chan = channel_, tid = id()] { chan->OnCommandTimeout(tid); });
  timeout_task_.PostDelayed(async_get_default_dispatcher(), hci_spec::kCommandTimeout);
}

void CommandChannel::TransactionData::Complete(std::unique_ptr<EventPacket> event) {
  timeout_task_.Cancel();
  if (!callback_) {
    return;
  }

  // Call callback_ synchronously to ensure that asynchronous status & complete events are not
  // handled out of order if they are dispatched from the HCI API simultaneously.
  callback_(transaction_id_, *event);

  // Asynchronous commands will have an additional reference to callback_ in the event
  // map. Clear this reference to ensure that destruction or unexpected command complete events or
  // status events do not call this reference to callback_ twice.
  callback_ = nullptr;
}

void CommandChannel::TransactionData::Cancel() {
  timeout_task_.Cancel();
  callback_ = nullptr;
}

CommandChannel::EventCallback CommandChannel::TransactionData::MakeCallback() {
  return [transaction_id = transaction_id_, cb = callback_.share()](const EventPacket& event) {
    cb(transaction_id, event);
    return EventCallbackResult::kContinue;
  };
}

CommandChannel::CommandChannel(pw::bluetooth::Controller* hci)
    : next_transaction_id_(1u),
      next_event_handler_id_(1u),
      hci_(hci),
      allowed_command_packets_(1u),
      weak_ptr_factory_(this) {
  hci_->SetEventFunction(fit::bind_member<&CommandChannel::OnEvent>(this));

  bt_log(INFO, "hci", "CommandChannel initialized");
}

CommandChannel::~CommandChannel() {
  bt_log(INFO, "hci", "CommandChannel destroyed");
  hci_->SetEventFunction(nullptr);
}

CommandChannel::TransactionId CommandChannel::SendCommand(
    CommandPacketVariant command_packet, CommandCallback callback,
    const hci_spec::EventCode complete_event_code) {
  return SendExclusiveCommand(std::move(command_packet), std::move(callback), complete_event_code);
}

CommandChannel::TransactionId CommandChannel::SendLeAsyncCommand(
    CommandPacketVariant command_packet, CommandCallback callback,
    hci_spec::EventCode le_meta_subevent_code) {
  return SendLeAsyncExclusiveCommand(std::move(command_packet), std::move(callback),
                                     le_meta_subevent_code);
}

CommandChannel::TransactionId CommandChannel::SendExclusiveCommand(
    CommandPacketVariant command_packet, CommandCallback callback,
    const hci_spec::EventCode complete_event_code,
    std::unordered_set<hci_spec::OpCode> exclusions) {
  return SendExclusiveCommandInternal(std::move(command_packet), std::move(callback),
                                      complete_event_code, std::nullopt, std::move(exclusions));
}

CommandChannel::TransactionId CommandChannel::SendLeAsyncExclusiveCommand(
    CommandPacketVariant command_packet, CommandCallback callback,
    std::optional<hci_spec::EventCode> le_meta_subevent_code,
    std::unordered_set<hci_spec::OpCode> exclusions) {
  return SendExclusiveCommandInternal(std::move(command_packet), std::move(callback),
                                      hci_spec::kLEMetaEventCode, le_meta_subevent_code,
                                      std::move(exclusions));
}

CommandChannel::TransactionId CommandChannel::SendExclusiveCommandInternal(
    CommandPacketVariant command_packet, CommandCallback callback,
    hci_spec::EventCode complete_event_code,
    std::optional<hci_spec::EventCode> le_meta_subevent_code,
    std::unordered_set<hci_spec::OpCode> exclusions) {
  if (!active_) {
    bt_log(INFO, "hci", "ignoring command (CommandChannel is inactive)");
    return 0;
  }

  BT_ASSERT_MSG(
      (complete_event_code == hci_spec::kLEMetaEventCode) == le_meta_subevent_code.has_value(),
      "only LE Meta Event subevents are supported");

  if (IsAsync(complete_event_code)) {
    // Cannot send an asynchronous command if there's an external event handler registered for the
    // completion event.
    EventHandlerData* handler = nullptr;
    if (le_meta_subevent_code.has_value()) {
      handler = FindLEMetaEventHandler(*le_meta_subevent_code);
    } else {
      handler = FindEventHandler(complete_event_code);
    }

    if (handler && !handler->is_async()) {
      bt_log(DEBUG, "hci", "event handler already handling this event");
      return 0u;
    }
  }

  if (next_transaction_id_.value() == 0u) {
    next_transaction_id_.Set(1);
  }

  const hci_spec::OpCode opcode =
      std::visit(overloaded{[](std::unique_ptr<CommandPacket>& p) { return p->opcode(); },
                            [](EmbossCommandPacket& p) { return p.opcode(); }},
                 command_packet);
  const TransactionId transaction_id = next_transaction_id_.value();
  next_transaction_id_.Set(transaction_id + 1);
  std::unique_ptr<CommandChannel::TransactionData> data = std::make_unique<TransactionData>(
      this, transaction_id, opcode, complete_event_code, le_meta_subevent_code,
      std::move(exclusions), std::move(callback));

  QueuedCommand command(std::move(command_packet), std::move(data));

  if (IsAsync(complete_event_code)) {
    MaybeAddTransactionHandler(command.data.get());
  }

  send_queue_.push_back(std::move(command));
  TrySendQueuedCommands();

  return transaction_id;
}

bool CommandChannel::RemoveQueuedCommand(TransactionId transaction_id) {
  auto it = std::find_if(
      send_queue_.begin(), send_queue_.end(),
      [transaction_id](const QueuedCommand& cmd) { return cmd.data->id() == transaction_id; });
  if (it == send_queue_.end()) {
    // The transaction to remove has already finished or never existed.
    bt_log(TRACE, "hci", "command to remove not found, id: %zu", transaction_id);
    return false;
  }

  bt_log(TRACE, "hci", "removing queued command id: %zu", transaction_id);
  TransactionData& data = *it->data;
  data.Cancel();

  RemoveEventHandlerInternal(data.handler_id());
  send_queue_.erase(it);
  return true;
}

CommandChannel::EventHandlerId CommandChannel::AddEventHandler(
    hci_spec::EventCode event_code, EventCallbackVariant event_callback_variant) {
  if (event_code == hci_spec::kCommandStatusEventCode ||
      event_code == hci_spec::kCommandCompleteEventCode ||
      event_code == hci_spec::kLEMetaEventCode) {
    return 0u;
  }

  EventHandlerData* handler = FindEventHandler(event_code);
  if (handler && handler->is_async()) {
    bt_log(ERROR, "hci", "async event handler %zu already registered for event code %#.2x",
           handler->handler_id, event_code);
    return 0u;
  }

  EventHandlerId handler_id = NewEventHandler(event_code, EventType::kHciEvent, hci_spec::kNoOp,
                                              std::move(event_callback_variant));
  event_code_handlers_.emplace(event_code, handler_id);
  return handler_id;
}

CommandChannel::EventHandlerId CommandChannel::AddLEMetaEventHandler(
    hci_spec::EventCode le_meta_subevent_code, EventCallback event_callback) {
  EventHandlerData* handler = FindLEMetaEventHandler(le_meta_subevent_code);
  if (handler && handler->is_async()) {
    bt_log(ERROR, "hci",
           "async event handler %zu already registered for LE Meta Event subevent code %#.2x",
           handler->handler_id, le_meta_subevent_code);
    return 0u;
  }

  EventHandlerId handler_id = NewEventHandler(le_meta_subevent_code, EventType::kLEMetaEvent,
                                              hci_spec::kNoOp, std::move(event_callback));
  le_meta_subevent_code_handlers_.emplace(le_meta_subevent_code, handler_id);
  return handler_id;
}

CommandChannel::EventHandlerId CommandChannel::AddVendorEventHandler(
    hci_spec::EventCode vendor_subevent_code, EventCallback event_callback) {
  CommandChannel::EventHandlerData* handler = FindVendorEventHandler(vendor_subevent_code);
  if (handler && handler->is_async()) {
    bt_log(ERROR, "hci",
           "async event handler %zu already registered for Vendor Event subevent code %#.2x",
           handler->handler_id, vendor_subevent_code);
    return 0u;
  }

  EventHandlerId handler_id = NewEventHandler(vendor_subevent_code, EventType::kVendorEvent,
                                              hci_spec::kNoOp, std::move(event_callback));
  vendor_subevent_code_handlers_.emplace(vendor_subevent_code, handler_id);
  return handler_id;
}

void CommandChannel::RemoveEventHandler(EventHandlerId handler_id) {
  // If the ID doesn't exist or it is internal. it can't be removed.
  auto iter = event_handler_id_map_.find(handler_id);
  if (iter == event_handler_id_map_.end() || iter->second.is_async()) {
    return;
  }

  RemoveEventHandlerInternal(handler_id);
}

CommandChannel::EventHandlerData* CommandChannel::FindEventHandler(hci_spec::EventCode code) {
  auto it = event_code_handlers_.find(code);
  if (it == event_code_handlers_.end()) {
    return nullptr;
  }
  return &event_handler_id_map_[it->second];
}

CommandChannel::EventHandlerData* CommandChannel::FindLEMetaEventHandler(
    hci_spec::EventCode le_meta_subevent_code) {
  auto it = le_meta_subevent_code_handlers_.find(le_meta_subevent_code);
  if (it == le_meta_subevent_code_handlers_.end()) {
    return nullptr;
  }
  return &event_handler_id_map_[it->second];
}

CommandChannel::EventHandlerData* CommandChannel::FindVendorEventHandler(
    hci_spec::EventCode vendor_subevent_code) {
  auto it = vendor_subevent_code_handlers_.find(vendor_subevent_code);
  if (it == vendor_subevent_code_handlers_.end()) {
    return nullptr;
  }

  return &event_handler_id_map_[it->second];
}

void CommandChannel::RemoveEventHandlerInternal(EventHandlerId handler_id) {
  auto iter = event_handler_id_map_.find(handler_id);
  if (iter == event_handler_id_map_.end()) {
    return;
  }

  std::unordered_multimap<hci_spec::EventCode, EventHandlerId>* handlers = nullptr;
  switch (iter->second.event_type) {
    case EventType::kHciEvent:
      handlers = &event_code_handlers_;
      break;
    case EventType::kLEMetaEvent:
      handlers = &le_meta_subevent_code_handlers_;
      break;
    case EventType::kVendorEvent:
      handlers = &vendor_subevent_code_handlers_;
      break;
  }

  bt_log(TRACE, "hci", "removing handler for %s event code %#.2x",
         EventTypeToString(iter->second.event_type).c_str(), iter->second.event_code);

  auto range = handlers->equal_range(iter->second.event_code);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == handler_id) {
      it = handlers->erase(it);
      break;
    }
  }

  event_handler_id_map_.erase(iter);
}

void CommandChannel::TrySendQueuedCommands() {
  if (allowed_command_packets_.value() == 0) {
    bt_log(TRACE, "hci", "controller queue full, waiting");
    return;
  }

  // Walk the waiting and see if any are sendable.
  for (auto it = send_queue_.begin();
       allowed_command_packets_.value() > 0 && it != send_queue_.end();) {
    // Care must be taken not to dangle this reference if its owner QueuedCommand is destroyed.
    const TransactionData& data = *it->data;

    // Can't send if another is running with an opcode this can't coexist with.
    bool excluded = false;
    for (hci_spec::OpCode excluded_opcode : data.exclusions()) {
      if (pending_transactions_.count(excluded_opcode) != 0) {
        bt_log(TRACE, "hci", "pending command (%#.4x) delayed due to running opcode %#.4x",
               it->data->opcode(), excluded_opcode);
        excluded = true;
        break;
      }
    }
    if (excluded) {
      ++it;
      continue;
    }

    bool transaction_waiting_on_event = event_code_handlers_.count(data.complete_event_code());
    bool transaction_waiting_on_subevent =
        data.le_meta_subevent_code() &&
        le_meta_subevent_code_handlers_.count(*data.le_meta_subevent_code());
    bool waiting_for_other_transaction =
        transaction_waiting_on_event || transaction_waiting_on_subevent;

    // We can send this if we only expect one update, or if we aren't waiting for another
    // transaction to complete on the same event. It is unlikely but possible to have commands with
    // different opcodes wait on the same completion event.
    if (!IsAsync(data.complete_event_code()) || data.handler_id() != 0 ||
        !waiting_for_other_transaction) {
      bt_log(TRACE, "hci", "sending previously queued command id %zu", data.id());
      SendQueuedCommand(std::move(*it));
      it = send_queue_.erase(it);
      continue;
    }
    ++it;
  }
}

void CommandChannel::SendQueuedCommand(QueuedCommand&& cmd) {
  pw::span packet_span = std::visit(
      overloaded{[](std::unique_ptr<CommandPacket>& p) { return p->view().data().subspan(); },
                 [](EmbossCommandPacket& p) { return p.data().subspan(); }},
      cmd.packet);
  hci_->SendCommand(packet_span);

  allowed_command_packets_.Set(allowed_command_packets_.value() - 1);

  std::unique_ptr<TransactionData>& transaction = cmd.data;

  transaction->StartTimer();

  MaybeAddTransactionHandler(transaction.get());

  pending_transactions_.insert(std::make_pair(transaction->opcode(), std::move(transaction)));
}

void CommandChannel::MaybeAddTransactionHandler(TransactionData* data) {
  // We don't need to add a transaction handler for synchronous transactions.
  if (!IsAsync(data->complete_event_code())) {
    return;
  }

  EventType event_type = EventType::kHciEvent;
  std::unordered_multimap<hci_spec::EventCode, EventHandlerId>* handlers = nullptr;

  if (data->le_meta_subevent_code().has_value()) {
    event_type = EventType::kLEMetaEvent;
    handlers = &le_meta_subevent_code_handlers_;
  } else {
    event_type = EventType::kHciEvent;
    handlers = &event_code_handlers_;
  }

  const hci_spec::EventCode code =
      data->le_meta_subevent_code().value_or(data->complete_event_code());

  // We already have a handler for this transaction, or another transaction is already waiting and
  // it will be queued.
  if (handlers->count(code)) {
    bt_log(TRACE, "hci", "async command %zu: already has handler", data->id());
    return;
  }

  EventHandlerId handler_id =
      NewEventHandler(code, event_type, data->opcode(), data->MakeCallback());

  BT_ASSERT(handler_id != 0u);
  data->set_handler_id(handler_id);
  handlers->emplace(code, handler_id);
  bt_log(TRACE, "hci", "async command %zu assigned handler %zu", data->id(), handler_id);
}

CommandChannel::EventHandlerId CommandChannel::NewEventHandler(
    hci_spec::EventCode event_code, EventType event_type, hci_spec::OpCode pending_opcode,
    EventCallbackVariant event_callback_variant) {
  BT_DEBUG_ASSERT(event_code);
  BT_DEBUG_ASSERT((std::holds_alternative<EventCallback>(event_callback_variant) &&
                   std::get<EventCallback>(event_callback_variant)) ||
                  (std::holds_alternative<EmbossEventCallback>(event_callback_variant) &&
                   std::get<EmbossEventCallback>(event_callback_variant)));

  auto handler_id = next_event_handler_id_.value();
  next_event_handler_id_.Set(handler_id + 1);
  EventHandlerData data;
  data.handler_id = handler_id;
  data.event_code = event_code;
  data.event_type = event_type;
  data.pending_opcode = pending_opcode;
  data.event_callback = std::move(event_callback_variant);

  bt_log(TRACE, "hci", "adding event handler %zu for %s event code %#.2x", handler_id,
         EventTypeToString(event_type).c_str(), event_code);
  BT_DEBUG_ASSERT(event_handler_id_map_.find(handler_id) == event_handler_id_map_.end());
  event_handler_id_map_[handler_id] = std::move(data);

  return handler_id;
}

void CommandChannel::UpdateTransaction(std::unique_ptr<EventPacket> event) {
  hci_spec::EventCode event_code = event->event_code();

  BT_DEBUG_ASSERT(event_code == hci_spec::kCommandStatusEventCode ||
                  event_code == hci_spec::kCommandCompleteEventCode);

  hci_spec::OpCode matching_opcode;

  // The HCI Command Status event with an error status might indicate that an async command failed.
  // We use this to unregister async command handlers below.
  bool unregister_async_handler = false;

  if (event->event_code() == hci_spec::kCommandCompleteEventCode) {
    const hci_spec::CommandCompleteEventParams& params =
        event->params<hci_spec::CommandCompleteEventParams>();
    matching_opcode = le16toh(params.command_opcode);
    allowed_command_packets_.Set(params.num_hci_command_packets);
  } else {  //  hci_spec::kCommandStatusEventCode
    const hci_spec::CommandStatusEventParams& params =
        event->params<hci_spec::CommandStatusEventParams>();
    matching_opcode = le16toh(params.command_opcode);
    allowed_command_packets_.Set(params.num_hci_command_packets);
    unregister_async_handler = params.status != pw::bluetooth::emboss::StatusCode::SUCCESS;
  }
  bt_log(TRACE, "hci", "allowed packets update: %zu", allowed_command_packets_.value());

  if (matching_opcode == hci_spec::kNoOp) {
    return;
  }

  auto it = pending_transactions_.find(matching_opcode);
  if (it == pending_transactions_.end()) {
    bt_log(ERROR, "hci", "update for unexpected opcode: %#.4x", matching_opcode);
    return;
  }

  std::unique_ptr<TransactionData>& transaction_ref = it->second;
  BT_DEBUG_ASSERT(transaction_ref->opcode() == matching_opcode);

  // If the command is synchronous or there's no handler to cleanup, we're done.
  if (transaction_ref->handler_id() == 0u) {
    std::unique_ptr<TransactionData> transaction = std::move(it->second);
    pending_transactions_.erase(it);
    transaction->Complete(std::move(event));
    return;
  }

  // TODO(fxbug.dev/1109): Do not allow asynchronous commands to finish with Command Complete.
  if (event_code == hci_spec::kCommandCompleteEventCode) {
    bt_log(WARN, "hci", "async command received CommandComplete");
    unregister_async_handler = true;
  }

  // If an asynchronous command failed, then remove its event handler.
  if (unregister_async_handler) {
    bt_log(TRACE, "hci", "async command failed; removing its handler");
    RemoveEventHandlerInternal(transaction_ref->handler_id());
    std::unique_ptr<TransactionData> transaction = std::move(it->second);
    pending_transactions_.erase(it);
    transaction->Complete(std::move(event));
  } else {
    // Send the status event to the async transaction.
    transaction_ref->Complete(std::move(event));
  }
}

void CommandChannel::NotifyEventHandler(std::unique_ptr<EventPacket> event) {
  struct PendingCallback {
    EventCallbackVariant callback_variant;
    EventHandlerId handler_id;
  };
  std::vector<PendingCallback> pending_callbacks;

  hci_spec::EventCode event_code;
  const std::unordered_multimap<hci_spec::EventCode, EventHandlerId>* event_handlers;

  EventType event_type;
  switch (event->event_code()) {
    case hci_spec::kLEMetaEventCode:
      event_type = EventType::kLEMetaEvent;
      event_code = event->params<hci_spec::LEMetaEventParams>().subevent_code;
      event_handlers = &le_meta_subevent_code_handlers_;
      break;
    case hci_spec::kVendorDebugEventCode:
      event_type = EventType::kVendorEvent;
      event_code = event->params<hci_spec::VendorEventParams>().subevent_code;
      event_handlers = &vendor_subevent_code_handlers_;
      break;
    default:
      event_type = EventType::kHciEvent;
      event_code = event->event_code();
      event_handlers = &event_code_handlers_;
      break;
  }

  auto range = event_handlers->equal_range(event_code);
  if (range.first == range.second) {
    bt_log(DEBUG, "hci", "%s event %#.2x received with no handler",
           EventTypeToString(event_type).c_str(), event_code);
    return;
  }

  auto iter = range.first;
  while (iter != range.second) {
    EventHandlerId event_id = iter->second;
    bt_log(TRACE, "hci", "notifying handler (id %zu) for event code %#.2x", event_id, event_code);
    auto handler_iter = event_handler_id_map_.find(event_id);
    BT_DEBUG_ASSERT(handler_iter != event_handler_id_map_.end());

    EventHandlerData& handler = handler_iter->second;
    BT_DEBUG_ASSERT(handler.event_code == event_code);

    std::visit(
        [&pending_callbacks, event_id](auto& callback) {
          pending_callbacks.push_back({callback.share(), event_id});
        },
        handler.event_callback);

    ++iter;  // Advance so we don't point to an invalid iterator.
    if (handler.is_async()) {
      bt_log(TRACE, "hci", "removing completed async handler (id %zu, event code: %#.2x)", event_id,
             event_code);
      pending_transactions_.erase(handler.pending_opcode);
      RemoveEventHandlerInternal(event_id);  // |handler| is now dangling.
    }
  }

  // Process queue so callbacks can't add a handler if another queued command finishes on the same
  // event.
  TrySendQueuedCommands();

  EventPacket& event_packet = *event;
  for (auto it = pending_callbacks.begin(); it != pending_callbacks.end(); ++it) {
    // Execute the event callback.
    EventCallbackResult result = std::visit(
        overloaded{[&event_packet](EventCallback& callback) { return callback(event_packet); },
                   [&event_packet](EmbossEventCallback& callback) {
                     auto emboss_packet =
                         EmbossEventPacket::New<pw::bluetooth::emboss::EventHeaderView>(
                             event_packet.view().size());
                     bt::MutableBufferView dest = emboss_packet.mutable_data();
                     event_packet.view().data().Copy(&dest);
                     return callback(emboss_packet);
                   }},
        it->callback_variant);

    if (result == EventCallbackResult::kRemove) {
      RemoveEventHandler(it->handler_id);
    }
  }
}

void CommandChannel::OnEvent(pw::span<const std::byte> buffer) {
  if (!active_) {
    bt_log(INFO, "hci", "ignoring event (CommandChannel is inactive)");
    return;
  }

  if (buffer.size() < sizeof(hci_spec::EventHeader)) {
    // TODO(fxbug.dev/97362): Handle these types of errors by signaling Transport.
    bt_log(ERROR, "hci", "malformed packet - expected at least %zu bytes, got %zu",
           sizeof(hci_spec::EventHeader), buffer.size());
    return;
  }

  const size_t payload_size = buffer.size() - sizeof(hci_spec::EventHeader);

  std::unique_ptr<EventPacket> event = EventPacket::New(payload_size);
  event->mutable_view()->mutable_data().Write(reinterpret_cast<const uint8_t*>(buffer.data()),
                                              buffer.size());
  event->InitializeFromBuffer();

  if (event->view().header().parameter_total_size != payload_size) {
    // TODO(fxbug.dev/97362): Handle these types of errors by signaling Transport.
    bt_log(ERROR, "hci",
           "malformed packet - payload size from header (%hu) does not match"
           " received payload size: %zu",
           event->view().header().parameter_total_size, payload_size);
    return;
  }

  if (event->event_code() == hci_spec::kCommandStatusEventCode ||
      event->event_code() == hci_spec::kCommandCompleteEventCode) {
    UpdateTransaction(std::move(event));
    TrySendQueuedCommands();
  } else {
    NotifyEventHandler(std::move(event));
  }
}

void CommandChannel::OnCommandTimeout(TransactionId transaction_id) {
  if (!active_) {
    return;
  }
  bt_log(ERROR, "hci", "command %zu timed out, notifying error", transaction_id);
  active_ = false;
  if (channel_timeout_cb_) {
    fit::closure cb = std::move(channel_timeout_cb_);
    // The callback may destroy CommandChannel, so no state should be accessed after this line.
    cb();
  }
}

void CommandChannel::AttachInspect(inspect::Node& parent, const std::string& name) {
  command_channel_node_ = parent.CreateChild(name);
  next_transaction_id_.AttachInspect(command_channel_node_, "next_transaction_id");
  next_event_handler_id_.AttachInspect(command_channel_node_, "next_event_handler_id");
  allowed_command_packets_.AttachInspect(command_channel_node_, "allowed_command_packets");
}

}  // namespace bt::hci
