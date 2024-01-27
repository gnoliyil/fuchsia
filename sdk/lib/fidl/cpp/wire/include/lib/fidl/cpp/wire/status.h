// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_STATUS_H_
#define LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_STATUS_H_

#include <lib/fidl/cpp/wire/internal/display_error.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <iosfwd>
#include <optional>

namespace fidl {

// Reason for a failed operation, or how the endpoint was unbound from the
// client/server message dispatcher.
//
// |Reason| is always carried inside a |fidl::Status| or |fidl::UnbindInfo|. As
// such, it is always accompanied with a |status| value. The documentation below
// describes precise semantics of the |status| under different reasons.
//
// While it is possible to write a switch-case on the |Reason| enum, note that
// some enum members may have subtle semantics, and new reasons may be
// introduced over time, hence always write a `default:` case. Furthermore,
// consider whether per-reason special casing is really needed, and consider one
// of the following instead:
//
// - Whether the `.ok()` and `is_peer_closed()` etc. accessors in
//   |fidl::UnbindInfo| are sufficient.
// - Whether the error may be propagated outwards and eventually logged at the
//   top level.
enum class Reason : uint16_t {
  // The value zero is reserved as a sentinel value that indicates an
  // uninitialized reason; it will never be returned to user code.
  // NOLINTNEXTLINE
  __DoNotUse = 0,

  // The user initiated unbinding. For example they invoked `Unbind()`
  // on the server side or destroyed their client object.
  //
  // If this reason is observed when making a call or sending an event or reply,
  // it indicates that the client/server endpoint has already been unbound, and
  // |status| will be |ZX_ERR_CANCELED|.
  //
  // If this reason is observed in an on-unbound handler in |fidl::UnbindInfo|,
  // |status| will be |ZX_OK|, since it indicates part of normal operation.
  kUnbind = 1,

  // The user invoked `Close(epitaph)` on a |fidl::ServerBindingRef| or
  // Completer and the epitaph was sent.
  //
  // This reason is only observable when part from a |fidl::UnbindInfo|.
  //
  // |status| is the result of sending the epitaph.
  kClose,

  // The endpoint expects a reply to a pending two-way call; as such, it cannot
  // be unbound.
  //
  // This reason is only observable when return from a |TryUnbind| call.
  kPendingTwoWayCallPreventsUnbind,

  // Some other operation on this connection experienced an error that requires
  // the runtime to unbind the endpoint and teardown the bindings.
  //
  // This reason is provided when the terminal error was specific to a
  // particular operation. For example, if one FIDL call triggered an encode
  // error, other in-progress FIDL call will be canceled with this reason.
  // On the other hand, if the binding received a malformed message, then
  // all operations will fail with that universal error.
  //
  // |status| will be |ZX_ERR_CANCELED|.
  kCanceledDueToOtherError,

  // The endpoint peer was closed while the bindings runtime was reading,
  // or waiting to read a message.
  //
  // In server APIs, |status| will be ZX_ERR_PEER_CLOSED. In client APIs,
  // |status| will be the epitaph.
  //
  // If no epitaph was sent, the behavior is equivalent to having received a
  // ZX_ERR_PEER_CLOSED epitaph.
  //
  // Note that if a FIDL operation only involves writing to a transport (e.g.
  // one way calls), then peer closed errors are hidden, to discourage race
  // conditions.
  kPeerClosedWhileReading,

  // An error associated with the dispatcher, or with waiting on the transport.
  //
  // |status| contains the associated error code.
  // For a server, the user is still responsible for sending an epitaph,
  // if they desire.
  kDispatcherError,

  // An error associated with reading to/writing from the transport e.g.
  // channel, that is not of type "peer closed".
  //
  // |status| contains the associated error code.
  // For a server, the user is still responsible for sending an epitaph,
  // if they desire.
  kTransportError,

  // Failure to encode an outgoing message, or converting an encoded message
  // to its incoming format (tests or in-process use cases).
  //
  // |status| contains the associated error code.
  // For a server, the user is still responsible for sending an epitaph,
  // if they desire.
  kEncodeError,

  // Failure to decode an incoming message.
  //
  // |status| contains the associated error code.
  // For a server, the user is still responsible for sending an epitaph,
  // if they desire.
  kDecodeError,

  // A malformed message header, message with unknown ordinal, or unexpected
  // reply was received. Alternatively, an unhandled transitional event was
  // received during synchronous event handling.
  //
  // |status| contains the associated error code.
  // For a server, the user is still responsible for sending an epitaph,
  // if they desire.
  kUnexpectedMessage,

  // This error is used on the client to report when a flexible two-way method
  // called by the client was not recognized by the server.
  //
  // |status| contains the associated error code.  Since the method is flexible,
  // the channel will remain open unless the user explicitly decides to close
  // it.
  kUnknownMethod,
};

// |ErrorOrigin| indicates in which part of request/response processing did a
// particular error occur.
enum class ErrorOrigin {
  // Reading from the transport, decoding, running business logic, etc.
  kReceive,

  // Writing to the transport, encoding, etc.
  kSend,
};

class Status;
class OneWayStatus;
class UnbindInfo;

namespace internal {

// A sentinel value that indicates an uninitialized reason. It should never be
// exposed to the user.
constexpr inline static ::fidl::Reason kUninitializedReason = static_cast<::fidl::Reason>(0);
static_assert(static_cast<int>(::fidl::Reason{}) == static_cast<int>(kUninitializedReason));

//
// Predefined error messages.
//

extern const char* const kErrorInvalidHeader;
extern const char* const kErrorUnknownTxId;
extern const char* const kErrorUnknownOrdinal;
extern const char* const kErrorTransport;
extern const char* const kErrorChannelUnbound;
extern const char* const kErrorWaitOneFailed;
extern const char* const kErrorSyncEventBufferTooSmall;
extern const char* const kErrorSyncEventUnhandledTransitionalEvent;
extern const char* const kCallerAllocatedBufferTooSmall;
extern const char* const kUnknownMethod;
extern const char* const kUnsupportedTransportError;

// Given a fatal error, |IsFatalErrorUniversal| detects if it universally
// applies to all operations on the connection. For example, if the async
// dispatcher is going down, then all in-progress operations should be
// notified of that same error. On the other hand, an encode error is not
// universal and only specific to a particular operation that caused it.
bool IsFatalErrorUniversal(fidl::Reason error);

// When the binding is tearing down, obtain an error to fail all in-progress
// and future operations with.
fidl::Status ErrorFromUnbindInfo(fidl::UnbindInfo);

// When the binding is tearing down, obtain an error to fail all in-progress
// and future operations with. Hides peer-closed errors.
fidl::OneWayStatus OneWayErrorFromUnbindInfo(fidl::UnbindInfo);

}  // namespace internal

// |Status| represents the result of an operation.
//
// If the operation was successful:
// - `ok()` returns true.
// - `status()` returns ZX_OK.
// - `reason()` should not be used.
//
// If the operation failed:
// - `ok()` returns false.
// - `status()` contains a non-OK status code specific to the failed operation.
// - `reason()` describes the operation which failed.
//
// |Status| may be piped to an output stream (`std::cerr`, `FX_LOGS`, ...) to
// print a human-readable description for debugging purposes.
class [[nodiscard]] Status {
 public:
  constexpr Status() = default;
  ~Status() = default;

  // Constructs a result representing a success.
  constexpr static Status Ok() { return Status(ZX_OK, internal::kUninitializedReason, nullptr); }

  // Constructs a result indicating that the operation cannot proceed
  // because the corresponding endpoint has been unbound from the dispatcher
  // (applies to both client and server).
  constexpr static Status Unbound() {
    return Status(ZX_ERR_CANCELED, ::fidl::Reason::kUnbind, ::fidl::internal::kErrorChannelUnbound);
  }

  constexpr static Status PendingTwoWayCallPreventsUnbind() {
    return Status(ZX_ERR_BAD_STATE, ::fidl::Reason::kPendingTwoWayCallPreventsUnbind, nullptr);
  }

  // Constructs a result indicating that the operation is being canceled due to
  // a problem somewhere else that forced the endpoint to be unbound. For
  // example, we'd like to inform the user that their FIDL call "B" now has to
  // be canceled due to a fatal error in FIDL call "A".
  static Status Canceled(fidl::UnbindInfo cause);

  // Constructs a result indicating that the operation cannot proceed
  // because a unknown message was received. Specifically, the method or event
  // ordinal is not recognized by the binding.
  constexpr static Status UnknownOrdinal() {
    return Status(ZX_ERR_NOT_SUPPORTED, ::fidl::Reason::kUnexpectedMessage,
                  ::fidl::internal::kErrorUnknownOrdinal);
  }

  // Constructs a transport error with |status| and optional |error_message|.
  // |status| must not be |ZX_OK|.
  constexpr static Status TransportError(zx_status_t status, const char* error_message = nullptr) {
    ZX_DEBUG_ASSERT(status != ZX_OK);
    // Depending on the order of operations during a remote endpoint closure, we
    // may either observe a |kTransportError| from writing to a channel or a
    // peer closed notification from the dispatcher loop, which is less than
    // ideal and racy behavior. To squash this race, if a transport failed with
    // the |ZX_ERR_PEER_CLOSED| error code, we always consider the reason to be
    // |kPeerClosed|.
    ::fidl::Reason reason = Reason::kTransportError;
    if (status == ZX_ERR_PEER_CLOSED) {
      reason = Reason::kPeerClosedWhileReading;
    }
    return Status(status, reason, error_message);
  }

  // Constructs a status for an unknown interaction.
  constexpr static Status UnknownMethod() {
    return Status(ZX_ERR_NOT_SUPPORTED, ::fidl::Reason::kUnknownMethod,
                  ::fidl::internal::kUnknownMethod);
  }

  constexpr static Status EncodeError(zx_status_t status, const char* error_message = nullptr) {
    return Status(status, ::fidl::Reason::kEncodeError, error_message);
  }

  constexpr static Status DecodeError(zx_status_t status, const char* error_message = nullptr) {
    return Status(status, ::fidl::Reason::kDecodeError, error_message);
  }

  constexpr static Status UnexpectedMessage(zx_status_t status,
                                            const char* error_message = nullptr) {
    return Status(status, ::fidl::Reason::kUnexpectedMessage, error_message);
  }

  constexpr Status(const Status& result) = default;
  constexpr Status& operator=(const Status& result) = default;

  // Status associated with the reason. See documentation on |fidl::Reason|
  // for how to interpret the status.
  //
  // Generally, logging this status alone wouldn't be very useful, since its
  // interpretation is dependent on the reason.
  // Prefer logging |error| or via |FormatDescription|.
  [[nodiscard]] constexpr zx_status_t status() const {
    if (reason_ == Reason::kCanceledDueToOtherError) {
      return ZX_ERR_CANCELED;
    }
    return status_;
  }

  // Returns the string representation of the status value.
  [[nodiscard]] const char* status_string() const { return zx_status_get_string(status()); }

  // A high-level reason for the failure.
  //
  // Generally, logging this value alone wouldn't be the most convenient for
  // debugging, since it requires developers to check back to the enum.
  // Prefer logging |error| or via |FormatDescription|.
  [[nodiscard]] constexpr ::fidl::Reason reason() const {
    ZX_ASSERT(reason_ != internal::kUninitializedReason);
    return reason_;
  }

  // A high-level reason for the underlying failure. An underlying reason
  // is present if and only if |reason| is |Reason::kCanceledDueToOtherError|,
  // that is, the operation was canceled due to a previous fatal error.
  //
  // Generally, logging this value alone wouldn't be the most convenient for
  // debugging, since it requires developers to check back to the enum.
  // Prefer logging |error| or via |FormatDescription|.
  [[nodiscard]] constexpr std::optional<::fidl::Reason> underlying_reason() const {
    if (reason() == Reason::kCanceledDueToOtherError) {
      ZX_DEBUG_ASSERT(underlying_reason_ != internal::kUninitializedReason);
      return underlying_reason_;
    }
    return std::nullopt;
  }

  // Returns if the operation failed because the peer endpoint was closed while
  // the bindings runtime was reading, or waiting to read a message.
  //
  // If this error happens on the client side and an epitaph was received,
  // |status| contains the value of the epitaph.
  //
  // This error is of interest since some protocol users may consider the peer
  // going away to be part of its normal operation, while others might not.
  //
  // Note that if a FIDL operation only involves writing to a transport (e.g.
  // one way calls), then peer closed errors are hidden, to discourage race
  // conditions.
  constexpr bool is_peer_closed() const { return reason_ == Reason::kPeerClosedWhileReading; }

  // Returns if the operation failed because the async dispatcher is shutting
  // down.
  constexpr bool is_dispatcher_shutdown() const {
    return reason_ == fidl::Reason::kDispatcherError && status() == ZX_ERR_CANCELED;
  }

  // Returns if the operation failed because it was canceled (i.e. the user or
  // another unrelated error tore down the binding in the meantime).
  constexpr bool is_canceled() const {
    bool directly_unbound = reason_ == fidl::Reason::kUnbind && status_ == ZX_ERR_CANCELED;
    bool indirectly_unbound = reason_ == fidl::Reason::kCanceledDueToOtherError;
    return directly_unbound || indirectly_unbound;
  }

  // Renders a full description of the success or error.
  //
  // It is more specific than |reason| alone e.g. if an encoding error was
  // encountered, it would contain a string description of the specific encoding
  // problem.
  //
  // If a logging API supports output streams (`<<` operators), piping the
  // |Status| to the log via `<<` is more efficient than calling this function.
  [[nodiscard]] std::string FormatDescription() const;

  // Returns a lossy description of the error. The returned |const char*| has
  // static lifetime, hence may be retained or passed around. If the result is a
  // success, returns |nullptr|.
  //
  // Because of this constraint, the bindings will attempt to pick a static
  // string that best represents the error, sometimes losing information. As
  // such, this method should only be used when interfacing with C APIs that are
  // unable to take a string or output stream.
  [[nodiscard]] const char* lossy_description() const;

  // If the operation was successful.
  [[nodiscard]] constexpr bool ok() const { return status_ == ZX_OK; }

  // If the operation failed, returns information about the error.
  //
  // This is meant be used by subclasses to accommodate a usage style that is
  // similar to |fit::result| types:
  //
  //   fidl::WireResult bar = fidl::WireCall(foo_client_end)->GetBar();
  //   if (!bar.ok()) {
  //     FX_LOGS(ERROR) << "GetBar failed: " << bar.error();
  //   }
  //
  constexpr const Status& error() const {
    ZX_ASSERT(status_ != ZX_OK);
    return *this;
  }

 protected:
  constexpr void SetStatus(const Status& other) { operator=(other); }

  // Returns a pointer to populate additional error message.
  [[nodiscard]] constexpr const char** error_address() { return &error_; }

  // A human readable description of |reason|.
  [[nodiscard]] const char* reason_description() const;

  // A human readable description of |underlying_reason|.
  [[nodiscard]] const char* underlying_reason_description() const;

  // Renders the description into a buffer |destination| that is of size
  // |length|. The description will cut off at `length - 1`. It inserts a
  // trailing NUL.
  //
  // |from_unbind_info| should be true iff this is invoked by |UnbindInfo|.
  //
  // Returns how many bytes were written, not counting the NUL.
  size_t FormatImpl(char* destination, size_t length, bool from_unbind_info) const;

 private:
  friend class UnbindInfo;
  friend std::ostream& operator<<(std::ostream& ostream, const Status& result);
  friend struct fidl::internal::DisplayError<fidl::Status>;

  __ALWAYS_INLINE
  constexpr Status(zx_status_t status, ::fidl::Reason reason, const char* error)
      : status_(status), reason_(reason), error_(error) {}

  __ALWAYS_INLINE
  constexpr Status(zx_status_t status, ::fidl::Reason reason, ::fidl::Reason underlying_reason,
                   const char* error)
      : status_(status), reason_(reason), underlying_reason_(underlying_reason), error_(error) {}

  zx_status_t status_ = ZX_ERR_INTERNAL;
  ::fidl::Reason reason_ = internal::kUninitializedReason;
  ::fidl::Reason underlying_reason_ = internal::kUninitializedReason;
  const char* error_ = nullptr;
};

// Logs a full description of the result to an output stream.
std::ostream& operator<<(std::ostream& ostream, const Status& result);

// Implement |DisplayError| for |fidl::Result|.
template <>
struct fidl::internal::DisplayError<fidl::Status> {
  static size_t Format(const fidl::Status& value, char* destination, size_t capacity);
};

// |OneWayStatus| represents the result of a one-way FIDL operation:
// - One-way client call.
// - Send server reply.
// - Send event.
//
// The main difference between |OneWayStatus| and |Status| is that the former
// does not expose peer-closed errors. The FIDL runtime only surfaces
// peer-closed errors when it is encountered while reading or waiting. Otherwise,
// they support the same APIs.
//
// |OneWayStatus| may implicitly decay into a |Status|, but one must explicitly
// perform the inverse transformation by calling the constructor.
class [[nodiscard]] OneWayStatus : public Status {
 public:
  explicit OneWayStatus(const Status& other) : Status(other) {
    ZX_DEBUG_ASSERT(!other.is_peer_closed());
  }

 private:
  using Status::is_peer_closed;
  using Status::Status;
};

template <>
struct fidl::internal::DisplayError<fidl::OneWayStatus> {
  static size_t Format(const fidl::OneWayStatus& value, char* destination, size_t capacity) {
    return fidl::internal::DisplayError<fidl::Status>::Format(value, destination, capacity);
  }
};

// |Error| is a type alias for when the result of an operation is an error.
using Error = Status;

// |OneWayError| is a type alias for when the result of an one-way operation is an error.
using OneWayError = OneWayStatus;

// |UnbindInfo| describes how the channel was unbound from a server or client.
//
// The reason is always initialized when part of a |fidl::UnbindInfo|.
//
// |UnbindInfo| is passed to |OnUnboundFn| and |AsyncEventHandler::Unbound| if
// provided by the user.
class UnbindInfo : private Status {
 public:
  // Creates an invalid |UnbindInfo|.
  constexpr UnbindInfo() = default;
  ~UnbindInfo() = default;

  constexpr explicit UnbindInfo(const Status& result) : Status(result) {
    ZX_DEBUG_ASSERT(reason() != internal::kUninitializedReason);
  }

  constexpr static UnbindInfo UnknownOrdinal() { return UnbindInfo{Status::UnknownOrdinal()}; }

  // Constructs an |UnbindInfo| indicating that the user explicitly requested
  // unbinding the server endpoint from the dispatcher.
  //
  // **Note that this is not the same as |Status::Unbound|**:
  // |Status::Unbound| means an operation failed because the required endpoint
  // has been unbound, and is an error. |UnbindInfo::Unbind| on the other hand
  // is an expected result from user initiation.
  constexpr static UnbindInfo Unbind() {
    return UnbindInfo{Status(ZX_OK, ::fidl::Reason::kUnbind, nullptr)};
  }

  // Constructs an |UnbindInfo| indicating that the server connection is
  // closed explicitly by the user. |status| is the status of writing
  // the epitaph to the channel. This is specific to the server bindings.
  //
  // Internally in the bindings runtime, |status| is also used to indicate
  // which epitaph value should be sent. But this is not re-exposed to the user
  // since the user provided the epitaph in the first place.
  constexpr static UnbindInfo Close(zx_status_t status) {
    return UnbindInfo{Status(status, ::fidl::Reason::kClose, nullptr)};
  }

  // Constructs an |UnbindInfo| indicating that the endpoint peer has
  // closed.
  constexpr static UnbindInfo PeerClosed(zx_status_t status) {
    return UnbindInfo{Status(status, ::fidl::Reason::kPeerClosedWhileReading, nullptr)};
  }

  // Constructs an |UnbindInfo| indicating the async dispatcher returned
  // an error |status|.
  constexpr static UnbindInfo DispatcherError(zx_status_t status) {
    return UnbindInfo{Status(status, ::fidl::Reason::kDispatcherError, nullptr)};
  }

  UnbindInfo(const UnbindInfo&) = default;
  UnbindInfo& operator=(const UnbindInfo&) = default;

  // Reason for unbinding the channel.
  //
  // Generally, logging this value alone wouldn't be the most convenient for
  // debugging, since it requires developers to check back to the enum.
  // Prefer logging the |UnbindInfo| itself or via |FormatDescription|.
  using Status::reason;

  // Status associated with the reason. See documentation on |fidl::Reason|
  // for how to interpret the status.
  //
  // Generally, logging this status alone wouldn't be very useful, since its
  // interpretation is dependent on the reason.
  // Prefer logging the |UnbindInfo| itself or via |FormatDescription|.
  using Status::status;

  // Returns the string representation of the status value.
  using Status::status_string;

  // Renders a full description of the cause of the unbinding.
  //
  // It is more specific than |reason| alone e.g. if an encoding error was
  // encountered, it would contain a string description of the specific encoding
  // problem.
  //
  // If a logging API supports output streams (`<<` operators), piping the
  // |UnbindInfo| to the log via `<<` is more efficient than calling this
  // function.
  [[nodiscard]] std::string FormatDescription() const;

  // Returns a lossy description of the unbind cause. The returned |const char*| has
  // static lifetime, hence may be retained or passed around. If the result is a
  // success, returns |nullptr|.
  //
  // Because of this constraint, the bindings will attempt to pick a static
  // string that best represents the cause, sometimes losing information. As
  // such, this method should only be used when interfacing with C APIs that are
  // unable to take a string or output stream.
  using Status::lossy_description;

  // Returns true if the unbinding was initiated by the user, that is, the user
  // called Unbind/Close on the server side to proactively teardown the
  // connection.
  //
  // This case is only observable from the server side. Note that the client
  // side `on_fidl_error` method on the event handler is never called with an
  // |UnbindInfo| that is user initiated - `on_fidl_error` is meant to handle
  // errors.
  [[nodiscard]] constexpr bool is_user_initiated() const {
    switch (reason()) {
      case internal::kUninitializedReason:
        return false;
      case Reason::kUnbind:
      case Reason::kClose:
        return true;
      default:
        return false;
    }
  }

  // Returns if the transport was unbound because the peer endpoint was closed.
  //
  // This error is of interest since some protocol users may consider the peer
  // going away to be part of its normal operation, while others might not.
  constexpr bool is_peer_closed() const { return reason_ == Reason::kPeerClosedWhileReading; }

  // Returns if the transport was unbound because the async dispatcher is
  // shutting down.
  using Status::is_dispatcher_shutdown;

  // Returns if the user invoked `Close(epitaph)` on a |fidl::ServerBindingRef| or
  // completer and the epitaph was sent.
  //
  // This case is only observable from the server side.
  //
  // |status| is the result of sending the epitaph.
  constexpr bool did_send_epitaph() const { return reason() == Reason::kClose; }

  // Reinterprets the |UnbindInfo| as the cause of an operation failure.
  constexpr fidl::Error ToError() const {
    if (reason_ == Reason::kUnbind) {
      // See |UnbindInfo::Unbind| for reason behind this conversion.
      return Status::Unbound();
    }
    return Status(*this);
  }

 private:
  friend std::ostream& operator<<(std::ostream& ostream, const UnbindInfo& info);
};

// Logs a full description of the cause of unbinding to an output stream.
std::ostream& operator<<(std::ostream& ostream, const UnbindInfo& info);

static_assert(sizeof(UnbindInfo) == sizeof(uintptr_t) * 2, "UnbindInfo should be reasonably small");

}  // namespace fidl

#endif  // LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_STATUS_H_
