// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_COMMAND_CHANNEL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_COMMAND_CHANNEL_H_

#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <zircon/compiler.h>

#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "pw_bluetooth/controller.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/identifier.h"
#include "src/connectivity/bluetooth/core/bt-host/common/inspectable.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/common/trace.h"
#include "src/connectivity/bluetooth/core/bt-host/common/weak_self.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/constants.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/control_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/emboss_control_packets.h"

namespace bt::hci {

// Represents the HCI Bluetooth command channel. Manages HCI command and event packet control flow.
// CommandChannel is expected to remain allocated as long as the host exists. On fatal errors, it is
// put into an inactive state where no packets are processed, but it may continue to be accessed by
// higher layers until shutdown completes.
class CommandChannel final {
 public:
  // Currently, two versions of the HCI packet infrastructure coexist. The old, packed-struct
  // approach, which is being obsoleted in favor of a new Emboss-based packet infrastructure. Until
  // all old instances of `CommandPacket` are replaced by `EmbossCommandPacket`, command packet
  // transmission will support both versions.
  //
  // TODO(fxbug.dev/86811): Finish migration away from std::unique_ptr<CommandPacket> and replace
  // with EmbossCommandPacket.
  using CommandPacketVariant = std::variant<std::unique_ptr<CommandPacket>, EmbossCommandPacket>;

  // Starts listening for HCI commands and starts handling commands and events.
  explicit CommandChannel(pw::bluetooth::Controller* hci);

  ~CommandChannel();

  // Used to identify an individual HCI command<->event transaction.
  using TransactionId = size_t;

  enum class EventType {
    kHciEvent,
    kLEMetaEvent,
    kVendorEvent,
  };

  // Queues the given |command_packet| to be sent to the controller and returns a transaction ID.
  //
  // This call takes ownership of the contents of |command_packet|. |command_packet| MUST represent
  // a valid HCI command packet.
  //
  // |callback| will be called with all events related to the transaction, unless the transaction is
  // removed with RemoveQueuedCommand. If the command results in a CommandStatus event, it will be
  // sent to this callback before the event with |complete_event_code| is sent.
  //
  // Synchronous transactions complete with a CommandComplete HCI event. This function is the only
  // way to receive a CommandComplete event.
  //
  // Most asynchronous transactions return the CommandStatus event and another event to indicate
  // completion, which should be indicated in |complete_event_code|.
  //
  // If |complete_event_code| is set to kCommandStatus, the transaction is considered complete when
  // the CommandStatus event is received.
  //
  // |complete_event_code| cannot be a code that has been registered for events via AddEventHandler
  // or its related methods.
  //
  // Returns a ID unique to the command transaction, or zero if the parameters are invalid.  This ID
  // will be supplied to |callback| in its |id| parameter to identify the transaction.
  //
  // NOTE: Commands queued are not guaranteed to be finished or sent in order, although commands
  // with the same opcode will be sent in order, and commands with the same |complete_event_code|
  // and |subevent_code| will be sent in order. If strict ordering of commands is required, use
  // SequentialCommandRunner or callbacks for sequencing.
  //
  // See Bluetooth Core Spec v5.0, Volume 2, Part E, Section 4.4 "Command Flow Control" for more
  // information about the HCI command flow control.
  using CommandCallback = fit::function<void(TransactionId id, const EventPacket& event)>;
  TransactionId SendCommand(
      CommandPacketVariant command_packet, CommandCallback callback,
      hci_spec::EventCode complete_event_code = hci_spec::kCommandCompleteEventCode);

  // As SendCommand, but the transaction completes on the LE Meta Event. |le_meta_subevent_code| is
  // a LE Meta Event subevent code as described in Core Spec v5.2, Vol 4, Part E, Sec 7.7.65.
  //
  // |le_meta_subevent_code| cannot be a code that has been registered for events via
  // AddLEMetaEventHandler.
  TransactionId SendLeAsyncCommand(CommandPacketVariant command_packet, CommandCallback callback,
                                   hci_spec::EventCode le_meta_subevent_code);

  // As SendCommand, but will wait to run this command until there are no commands with with opcodes
  // specified in |exclude| from executing. This is useful to prevent running different commands
  // that cannot run concurrently (i.e. Inquiry and Connect). Two commands with the same opcode will
  // never run simultaneously.
  TransactionId SendExclusiveCommand(
      CommandPacketVariant command_packet, CommandCallback callback,
      hci_spec::EventCode complete_event_code = hci_spec::kCommandCompleteEventCode,
      std::unordered_set<hci_spec::OpCode> exclusions = {});

  // As SendExclusiveCommand, but the transaction completes on the LE Meta Event with subevent code
  // |le_meta_subevent_code|.
  TransactionId SendLeAsyncExclusiveCommand(
      CommandPacketVariant command_packet, CommandCallback callback,
      std::optional<hci_spec::EventCode> le_meta_subevent_code,
      std::unordered_set<hci_spec::OpCode> exclusions = {});

  // If the command identified by |id| has not been sent to the controller, remove it from the send
  // queue and return true. In this case, its CommandCallback will not be notified. If the command
  // identified by |id| has already been sent to the controller or if it does not exist, this has no
  // effect and returns false.
  [[nodiscard]] bool RemoveQueuedCommand(TransactionId id);

  // Used to identify an individual HCI event handler that was registered with this CommandChannel.
  using EventHandlerId = size_t;

  // Return values for EventCallbacks.
  enum class EventCallbackResult {
    // Continue handling this event.
    kContinue,

    // Remove this event handler.
    // NOTE: The event callback may still be called again after it has been removed if the handler
    // has already been posted to the dispatcher for subsequent events.
    kRemove
  };

  // Callbacks invoked to report generic HCI events excluding CommandComplete and CommandStatus
  // events.
  //
  // TODO(fxbug.dev/86811): Finish migration away from EventCallback and replace with
  // EmbossEventCallback (renamed to EventCallback).
  using EventCallback = fit::function<EventCallbackResult(const EventPacket& event_packet)>;
  using EmbossEventCallback =
      fit::function<EventCallbackResult(const EmbossEventPacket& event_packet)>;
  using EventCallbackVariant = std::variant<EventCallback, EmbossEventCallback>;

  // Registers an event handler for HCI events that match |event_code|. Incoming HCI event packets
  // that are not associated with a pending command sequence will be posted on the given
  // |dispatcher| via the given |event_callback|. The returned ID can be used to unregister a
  // previously registered event handler.
  //
  // |event_callback| will be invoked for all HCI event packets that match |event_code|, except for:
  //
  // - HCI_CommandStatus events
  // - HCI_CommandComplete events
  // - The completion event of the currently pending command packet, if any
  //
  // Returns an ID if the handler was successfully registered. Returns zero in case of an error.
  //
  // Multiple handlers can be registered for a given |event_code| at a time. All handlers that are
  // registered will be called with a reference to the event.
  //
  // If an asynchronous command is queued which completes on |event_code|, this method returns zero.
  // It is good practice to avoid using asynchronous commands and event handlers for the same event
  // code.  SendCommand allows for queueing multiple asynchronous commands with the same callback.
  // Alternately a long-lived event handler can be registered with Commands completing on
  // CommandStatus.
  //
  // The following values for |event_code| cannot be passed to this method:
  //
  // - HCI_Command_Complete event code
  // - HCI_Command_Status event code
  // - HCI_LE_Meta event code (use AddLEMetaEventHandler instead)
  // - HCI_Vendor_Debug event code (use AddVendorEventHandler instead)
  EventHandlerId AddEventHandler(hci_spec::EventCode event_code,
                                 EventCallbackVariant event_callback_variant);

  // Works just like AddEventHandler but the passed in event code is only valid within the LE Meta
  // Event sub-event code namespace. |event_callback| will get invoked whenever the controller sends
  // a LE Meta Event with a matching subevent code.
  EventHandlerId AddLEMetaEventHandler(hci_spec::EventCode le_meta_subevent_code,
                                       EventCallback event_callback);

  // Works just like AddEventHandler but the passed in event code is only valid for vendor related
  // debugging events. The event_callback will get invoked whenever the controller sends one of
  // these vendor debugging events with a matching subevent code.
  EventHandlerId AddVendorEventHandler(hci_spec::EventCode vendor_subevent_code,
                                       EventCallback event_callback);

  // Removes a previously registered event handler. Does nothing if an event handler with the given
  // |id| could not be found.
  void RemoveEventHandler(EventHandlerId id);

  // Set callback that will be called when a command times out after kCommandTimeout. This is
  // distinct from channel closure.
  void set_channel_timeout_cb(fit::closure timeout_cb) {
    channel_timeout_cb_ = std::move(timeout_cb);
  }

  // Attach command_channel inspect node as a child node of |parent|.
  static constexpr const char* kInspectNodeName = "command_channel";
  void AttachInspect(inspect::Node& parent, const std::string& name = kInspectNodeName);

  using WeakPtr = WeakSelf<CommandChannel>::WeakPtr;
  WeakPtr AsWeakPtr() { return weak_ptr_factory_.GetWeakPtr(); }

 private:
  TransactionId SendExclusiveCommandInternal(
      CommandPacketVariant command_packet, CommandCallback callback,
      hci_spec::EventCode complete_event_code,
      std::optional<hci_spec::EventCode> le_meta_subevent_code = std::nullopt,
      std::unordered_set<hci_spec::OpCode> exclusions = {});

  // Data related to a queued or running command.
  //
  // When a command is sent to the controller, it is placed in the pending_transactions_ map.  It
  // remains in the pending_transactions_ map until it is completed - asynchronous commands remain
  // until their complete_event_code_ is received.
  //
  // There are a number of reasons a command may be queued before sending:
  //
  // - An asynchronous command is waiting on the same event code
  // - A command with an opcode that is in the exclusions set for this transaction is pending
  // - We cannot send any commands because of the limit of outstanding command packets from the
  //   controller Queued commands are held in the send_queue_ and are sent when possible, FIFO (but
  //   skipping commands that cannot be sent)
  class TransactionData {
   public:
    TransactionData(CommandChannel* channel, TransactionId id, hci_spec::OpCode opcode,
                    hci_spec::EventCode complete_event_code,
                    std::optional<hci_spec::EventCode> le_meta_subevent_code,
                    std::unordered_set<hci_spec::OpCode> exclusions, CommandCallback callback);
    ~TransactionData();

    // Starts the transaction timer, which will call CommandChannel::OnCommandTimeout if it's not
    // completed in time.
    void StartTimer();

    // Completes the transaction with |event|. For asynchronous commands, this should be called with
    // the status event (the complete event is handled separately by the event handler).
    void Complete(std::unique_ptr<EventPacket> event);

    // Cancels the transaction timeout and erases the callback so it isn't called upon destruction.
    void Cancel();

    // Makes an EventCallback that calls |callback_| correctly.
    EventCallback MakeCallback();

    hci_spec::EventCode complete_event_code() const { return complete_event_code_; }
    std::optional<hci_spec::EventCode> le_meta_subevent_code() const {
      return le_meta_subevent_code_;
    }
    hci_spec::OpCode opcode() const { return opcode_; }
    TransactionId id() const { return transaction_id_; }

    // The set of opcodes in progress that will hold this transaction in queue.
    const std::unordered_set<hci_spec::OpCode>& exclusions() const { return exclusions_; }

    EventHandlerId handler_id() const { return handler_id_; }
    void set_handler_id(EventHandlerId id) { handler_id_ = id; }

   private:
    CommandChannel* channel_;
    TransactionId transaction_id_;
    hci_spec::OpCode opcode_;
    hci_spec::EventCode complete_event_code_;
    std::optional<hci_spec::EventCode> le_meta_subevent_code_;
    std::unordered_set<hci_spec::OpCode> exclusions_;
    CommandCallback callback_;
    async::TaskClosure timeout_task_;

    // If non-zero, the id of the handler registered for this transaction.
    // Always zero if this transaction is synchronous.
    EventHandlerId handler_id_;

    BT_DISALLOW_COPY_ASSIGN_AND_MOVE(TransactionData);
  };

  // Adds an internal event handler for |data| if one does not exist yet and another transaction is
  // not waiting on the same event. Used to add expiring event handlers for asynchronous commands.
  void MaybeAddTransactionHandler(TransactionData* data);

  // Represents a queued command packet.
  struct QueuedCommand {
    QueuedCommand(CommandPacketVariant command_packet, std::unique_ptr<TransactionData> data);
    QueuedCommand() = default;

    QueuedCommand(QueuedCommand&& other) = default;
    QueuedCommand& operator=(QueuedCommand&& other) = default;

    CommandPacketVariant packet;
    std::unique_ptr<TransactionData> data;
  };

  // Data stored for each event handler registered.
  struct EventHandlerData {
    EventHandlerId handler_id;
    hci_spec::EventCode event_code;

    // Defines how event_code should be interpreted. For example, if the event_type is kLEMetaEvent,
    // the event_code is an LE Meta Subevent code.
    EventType event_type;

    // For asynchronous transaction event handlers, the pending command opcode.
    // kNoOp if this is a static event handler.
    hci_spec::OpCode pending_opcode;

    EventCallbackVariant event_callback;

    // Returns true if handler is for async command transaction, or false if handler is a static
    // event handler.
    bool is_async() const { return pending_opcode != hci_spec::kNoOp; }
  };

  // Finds the event handler for |code|. Returns nullptr if one doesn't exist.
  EventHandlerData* FindEventHandler(hci_spec::EventCode code);

  // Finds the LE Meta Event handler for |le_meta_subevent_code|. Returns nullptr if one doesn't
  // exist.
  EventHandlerData* FindLEMetaEventHandler(hci_spec::EventCode le_meta_subevent_code);

  // Finds the Vendor Event handler for |vendor_subevent_code|. Returns nullptr if one doesn't
  // exist.
  EventHandlerData* FindVendorEventHandler(hci_spec::EventCode vendor_subevent_code);

  // Removes internal event handler structures for |id|.
  void RemoveEventHandlerInternal(EventHandlerId id);

  // Sends any queued commands that can be processed unambiguously and complete.
  void TrySendQueuedCommands();

  // Sends |command|, adding an internal event handler if asynchronous.
  void SendQueuedCommand(QueuedCommand&& cmd);

  // Creates a new event handler entry in the event handler map and returns its ID. The event_code
  // should correspond to the event_type provided. For example, if event_type is kLEMetaEvent, then
  // event_code will be interpreted as a LE Meta Subevent code.
  EventHandlerId NewEventHandler(hci_spec::EventCode event_code, EventType event_type,
                                 hci_spec::OpCode pending_opcode,
                                 EventCallbackVariant event_callback_variant);

  // Notifies any matching event handler for |event|.
  void NotifyEventHandler(std::unique_ptr<EventPacket> event);

  // Notifies handlers for Command Status and Command Complete Events. This function marks opcodes
  // that have transactions pending as complete by removing them and calling their callbacks.
  void UpdateTransaction(std::unique_ptr<EventPacket> event);

  // Event handler.
  void OnEvent(pw::span<const std::byte> buffer);

  // Called when a command times out. Notifies upper layers of the error.
  void OnCommandTimeout(TransactionId transaction_id);

  // True if CommandChannel is still processing packets. Set to false upon fatal errors.
  bool active_ = true;

  // Opcodes of commands that we have sent to the controller but not received a status update from.
  // New commands with these opcodes can't be sent because they are indistinguishable from ones we
  // need to get the result of.
  std::unordered_map<hci_spec::OpCode, std::unique_ptr<TransactionData>> pending_transactions_;

  // TransactionId counter.
  UintInspectable<TransactionId> next_transaction_id_;

  // EventHandlerId counter.
  UintInspectable<size_t> next_event_handler_id_;

  // The HCI we use to send/receive HCI commands/events.
  pw::bluetooth::Controller* hci_;

  // Callback called when a command times out.
  fit::closure channel_timeout_cb_;

  // The HCI command queue. This queue is not necessarily sent in order, but commands with the same
  // opcode or that wait on the same completion event code are sent first-in, first-out.
  std::list<QueuedCommand> send_queue_;

  // Contains the current count of commands we ae allowed to send to the controller.  This is
  // decremented when we send a command and updated when we receive a CommandStatus or
  // CommandComplete event with the Num HCI Command Packets parameter.
  //
  // Accessed only from the I/O thread and thus not guarded.
  UintInspectable<size_t> allowed_command_packets_;

  // Mapping from event handler IDs to handler data.
  std::unordered_map<EventHandlerId, EventHandlerData> event_handler_id_map_;

  // Mapping from event code to the event handlers that were registered to handle that event code.
  std::unordered_multimap<hci_spec::EventCode, EventHandlerId> event_code_handlers_;

  // Mapping from LE Meta Event Subevent code to the event handlers that were registered to handle
  // that event code.
  std::unordered_multimap<hci_spec::EventCode, EventHandlerId> le_meta_subevent_code_handlers_;

  // Mapping from Vendor Subevent code to the event handlers that were registered to handle that
  // event code.
  std::unordered_multimap<hci_spec::EventCode, EventHandlerId> vendor_subevent_code_handlers_;

  // Command channel inspect node.
  inspect::Node command_channel_node_;

  // As events can arrive in the event thread at any time, we should invalidate our weak pointers
  // early.
  WeakSelf<CommandChannel> weak_ptr_factory_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(CommandChannel);
};

}  // namespace bt::hci

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_COMMAND_CHANNEL_H_
