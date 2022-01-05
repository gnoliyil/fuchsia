// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_LLCPP_TRANSACTION_H_
#define LIB_FIDL_LLCPP_TRANSACTION_H_

#include <lib/fidl/llcpp/message_storage.h>
#include <lib/fidl/llcpp/result.h>
#include <lib/fidl/llcpp/traits.h>
#include <lib/fidl/txn_header.h>
#include <zircon/assert.h>
#include <zircon/fidl.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace fidl {

class IncomingMessage;
class OutgoingMessage;

// An abstract transaction, encapsulating the logic of sending reply messages.
// The transaction type is polymorphic, to cater to a variety of transports and usage patterns.
// A basic transaction implementation using channels is defined in fidl-async, so most clients
// do not have to worry about implementing |fidl::Transaction|, and the higher-level APIs from
// fidl-async would suffice for a lot of use cases.
//
// Should greater flexibility over managing in-flight transactions and server lifecycle be desired,
// clients usually manually dispatch the FIDL messages. This is accomplished in the C bindings by
// creating a transaction type that is memory layout-compatible with |fidl_txn_t|, and passing that
// when calling the |foo_bar_try_dispatch| functions generated by the C bindings. Similarly here,
// clients can manually invoke the [Try]Dispatch functions generated by the low-level C++ bindings,
// passing in their own transaction type by inheriting from this class and implementing the
// pure virtual interfaces, e.g.
//
//     // Imaginary use-case: suppose drivers send messages in their own ways
//     class DriverRpcTransaction : public Transaction {
//     public:
//         // Implementation left-out
//         DriverRpcTransaction(/* ...some args... */);
//         ~DriverRpcTransaction();
//         DriverRpcTransaction(DriverRpcTransaction&& other) noexcept
//             : Transaction(std::move(other));
//         DriverRpcTransaction& operator=(DriverRpcTransaction&& other) noexcept;
//
//     protected:
//         zx_status_t Reply(fidl::internal::Message* message) final {
//           /* Send to another driver etc. */
//         }
//         void Close(zx_status_t epitaph) final { /* Send epitaph and close down transport. */ }
//
//         std::unique_ptr<Transaction> TakeOwnership() final {
//             return std::make_unique<DriverRpcTransaction>(std::move(*this));
//         }
//
//     private:
//         // Driver-specific state...
//     };
//
// and then dispatch to the server e.g.
//
//     fidl::WireServer<fuchsia::device::driver>* server_impl = /* construct/obtain server */;
//     fidl_incoming_msg_t msg = /* read FIDL message */;
//     DriverRpcTransaction txn(/* ...some args... */);
//     fuchsia::device::driver::Dispatch(server_impl, &msg, &txn);
//     /* Inspect |txn| */
//
// The LLCPP runtime provides strong invariants around each method in the interface. It is possible
// to implement a variety of dispatching behaviors by relying on them.
//
class Transaction {
 public:
  Transaction() = default;

  // Move the contents of this transaction to heap, for an asynchronous reply.
  // Called exactly once when a sync completer is converted to an async completer.
  // Implementation may pause message dispatching here, in which case there will be
  // at most one in-flight transaction.
  // |Reply| and |Close| are never called on a transaction that has been moved from.
  virtual std::unique_ptr<Transaction> TakeOwnership() = 0;

  // Called at most once for a two-way FIDL method, to reply to a two-way call.
  // Never called in case of a one-way call.
  // Implementation must fill in the correct transaction ID.
  // |Reply| usually consumes the handles (in which case |ReleaseHandles()| has been called).
  // If not, the destructor of |FidlMessage| will close them.
  virtual zx_status_t Reply(fidl::OutgoingMessage* message,
                            fidl::WriteOptions write_options = {}) = 0;

  // Should send an epitaph and then close the underlying transport e.g. channel.
  virtual void Close(zx_status_t epitaph) = 0;

  // Implementations which support a user-specified unbound hook should propagate `error` to that
  // hook. Otherwise, by default, InternalError() just closes the connection to the client.
  virtual void InternalError(UnbindInfo error, ErrorOrigin origin) { Close(ZX_ERR_PEER_CLOSED); }

  // Resumes the asynchronous wait on the underlying channel. This allows at least one more
  // dispatcher thread to enter the message handler for this binding in parallel.
  virtual void EnableNextDispatch() {
    ZX_ASSERT_MSG(false, "Transaction does not support explicitly resuming dispatch.");
  }

  // Returns true if the transaction references some internal binding, and that binding is no longer
  // accessible. This is used to determine whether a reply is still required. The default
  // implementation simply returns false in order to maintain the behavior for the binding
  // implementations outside of libfidl.
  virtual bool IsUnbound() { return false; }

  // A transaction will only be destroyed after one of three actions happens to it:
  // the Completer containing it is destroyed, the transaction is closed, or ownership is taken from
  // the transaction.
  // An async transaction is destroyed immediately after the request has been closed or the
  // Completer containing it is destroyed.
  // A synchronous transaction lives on the stack, and may provide status/errors to the binding
  // dispatch loop.
  // If the implementation paused message dispatching above, it may resume dispatching here.
  virtual ~Transaction() = default;
};

// Manages the lifetime of a transaction and serves as the base class of FIDL
// method-specific completers. It ensures that no two-way calls will be silently
// dropped without replying or closing, unless the server endpoint has been
// unbound.
//
// An individual |CompleterBase| object (and by extension any method completer)
// is thread-unsafe. Multi-threaded uses of a |CompleterBase| object requires
// external synchronization.
class CompleterBase {
 public:
  CompleterBase(const CompleterBase&) = delete;
  CompleterBase& operator=(const CompleterBase&) = delete;

  // Instructs the transaction to send an epitaph and then close the underlying transport.
  // |status| may be an error status, or |ZX_OK|, which indicates normal (expected) closure.
  //
  // It is an error to close a completer twice.
  void Close(zx_status_t status);

  // Returns if a reply is needed on the completer. A reply is needed when both:
  //
  // - The FIDL method is two-way.
  // - The server method handler has yet to make a reply.
  //
  // unless any of the following apply:
  //
  // - The completer is moved or converted to an async completer (a reply may be
  //   needed on the new completer).
  // - The server method handler calls |Close| on the completer.
  // - The server endpoint is unbound from the message dispatcher due to an
  //   error or an explicit |Unbind| request from a |fidl::ServerBindingRef|.
  //
  // It is safe to destroy a completer without replying inside the |on_unbound|
  // hook provided to |fidl::BindServer|.
  bool is_reply_needed() const;

 protected:
  explicit CompleterBase(Transaction* transaction, bool owned, bool method_expects_reply)
      : transaction_(transaction), owned_(owned), needs_to_reply_(method_expects_reply) {}

  CompleterBase(CompleterBase&& other) noexcept;
  CompleterBase& operator=(CompleterBase&& other) noexcept;

  ~CompleterBase();

  fidl::Result SendReply(::fidl::OutgoingMessage* message);

  // Move the contents of |transaction_| to heap and return it.
  std::unique_ptr<Transaction> TakeOwnership();

  // Resumes the asynchronous wait on the underlying channel, enabling another dispatcher thread to
  // enter the message handler for this binding. This must only be called from the scope of the
  // message handler.
  void EnableNextDispatch();

 private:
  // Scoped "lock" to which asserts that only one thread enters the given scope at a time.
  class ScopedLock {
   public:
    explicit ScopedLock(std::atomic_flag& lock) : lock_(lock) {
      ZX_ASSERT_MSG(!lock_.test_and_set(std::memory_order_acquire),
                    "Completer accessed from multiple threads concurrently.");
    }
    ~ScopedLock() {
      if (!released_)
        lock_.clear(std::memory_order_release);
    }
    void release() {
      ZX_ASSERT_MSG(!released_, "Cannot release ScopedLock twice.");
      released_ = true;
      lock_.clear(std::memory_order_release);
    }

   private:
    std::atomic_flag& lock_;
    bool released_ = false;
  };

  void EnsureHasTransaction(ScopedLock* lock);

  void DropTransaction();

  Transaction* transaction_;
  bool owned_;
  bool needs_to_reply_;

  // An individual |CompleterBase| object is thread-unsafe. We use an atomic
  // flag to catch any parallel access to |CompleterBase| that happens at
  // run-time.
  mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

// Completers of a FIDL method call.
//
// |Base| is a FIDL method-specific sub-class of |CompleterBase| generated
// by the FIDL compiler which provides the necessary Reply() implementations.
// For one-way FIDL methods, |Base| is |CompleterBase| itself.
template <typename Base = CompleterBase>
struct Completer final {
  static constexpr bool kExpectingReply = !std::is_same<Base, CompleterBase>::value;

  // An asynchronous responder owns the underlying transaction.
  // It may be stored in the server implementation for delayed reply. It is okay for the handler
  // to return without replying when the Completer::Sync is converted to Completer::Async, e.g.
  //
  //     virtual void MyMethod(Foo foo, Bar bar, MyMethodCompleter::Sync& completer) {
  //         PerformLongOperation(foo, bar).then([completer = completer.ToAsync()] () {
  //             // Here the type of |completer| is |MyMethodCompleter::Async|.
  //             completer.Reply(...);
  //         });
  //     }
  //
  class Async final : public Base {
   public:
    Async() : Base() {}

    explicit Async(std::unique_ptr<Transaction> owned_transaction)
        : Base(owned_transaction.release(), true /*owned*/, kExpectingReply) {}

   private:
    // EnableNextDispatch() must only be invoked within the message handler. This is an attempt to
    // enforce such behavior by restricting it to the fidl::Completer<T>::Sync variant.
    using Base::EnableNextDispatch;
  };

  // The server handler function will be given FooCompleter::Sync&, an object tailor made for
  // the specific method call. It only exposes methods to send the corresponding reply type
  // given the request. The completer wraps a |fidl::Transaction|. The handler must interact
  // with the sync responder in one way or another. If the handler forgets to e.g. reply the
  // request, it will result in a run-time assert.
  class Sync final : public Base {
   public:
    explicit Sync(Transaction* borrowed_transaction)
        : Base(borrowed_transaction, false /*owned*/, kExpectingReply) {}

    // The sync completer is provided to message handlers as a reference;
    // it cannot be moved and must be used up synchronously in the case
    // of a two-way FIDL method.
    Sync(Sync&& other) = delete;
    Sync& operator=(Sync&& other) = delete;

    // Move a sync responder to its async counterpart, such that the reply could be
    // issued asynchronously. This causes the transaction to be moved to heap.
    Async ToAsync() { return Async(Base::TakeOwnership()); }

    using Base::EnableNextDispatch;
  };
};

}  // namespace fidl

#endif  // LIB_FIDL_LLCPP_TRANSACTION_H_
