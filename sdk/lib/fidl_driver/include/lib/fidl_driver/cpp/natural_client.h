// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_DRIVER_CPP_NATURAL_CLIENT_H_
#define LIB_FIDL_DRIVER_CPP_NATURAL_CLIENT_H_

#include <lib/fidl/cpp/internal/make_response_context.h>
#include <lib/fidl/cpp/wire/internal/arrow.h>
#include <lib/fidl_driver/cpp/internal/natural_client_details.h>
#include <lib/fidl_driver/cpp/transport.h>
#include <lib/fidl_driver/cpp/unified_messaging_declarations.h>
#include <lib/fit/result.h>

namespace fdf {

//
// Note: when updating the documentation below, please make similar updates to
// the one in //src/lib/fidl/cpp/include/lib/fidl/cpp/client.h and
// //sdk/lib/fidl/cpp/wire/include/lib/fidl/cpp/wire/client.h
//
// The interface documentation on |fdf::Client| is largely identical to those
// on |fidl::Client|.
//
// The interface documentation on |fdf::SharedClient| is largely identical to
// those on |fdf::SharedClient|.
//

// |Client| is a client for sending and receiving FIDL wire messages, that
// must be managed and used from a [synchronized async dispatcher][synchronized-dispatcher].
// See |SharedClient| for a client that may be moved or cloned to different
// dispatchers or arbitrary threads.
//
// Generated FIDL APIs are accessed by 'dereferencing' the client value:
//
//     // Creates a client that speaks over |client_end|, on the |my_dispatcher| dispatcher.
//     fidl::Client client(std::move(client_end), my_dispatcher);
//
//     // Call the |Foo| method asynchronously, passing in a callback that will be
//     // invoked on a dispatcher thread when the server response arrives.
//     auto status = client->Foo(args, [] (Result result) {});
//
// ## Lifecycle
//
// A client must be **bound** to an endpoint before it could be used. This
// association between the endpoint and the client is called a "binding".
// Binding a client to an endpoint starts the monitoring of incoming messages.
// Those messages are appropriately dispatched: to response callbacks, to event
// handlers, etc. FIDL methods (asynchronous or synchronous) may only be invoked
// on a bound client.
//
// Internally, a client is a lightweight reference to the binding, performing
// its duties indirectly through that object, as illustrated by the simplified
// diagram below:
//
//                 references               makes
//       client  ------------->  binding  -------->  FIDL call
//
// This means that the client _object_ and the binding have overlapping but
// slightly different lifetimes. For example, the binding may terminate in
// response to fatal communication errors, leaving the client object alive but
// unable to make any calls.
//
// To stop the monitoring of incoming messages, one may **teardown** the
// binding. When teardown is initiated, the client will not monitor new messages
// on the endpoint. Ongoing callbacks will be allowed to run to completion. When
// teardown is complete, further calls on the same client will fail. Unfulfilled
// response callbacks will be dropped.
//
// Destruction of a client object will initiate teardown.
//
// Teardown will also be initiated when the binding encounters a terminal error:
//
// - The server-end of the channel was closed.
// - An epitaph was received.
// - Decoding or encoding failed.
// - An invalid or unknown message was encountered.
// - Error waiting on, reading from, or writing to the channel.
//
// In this case, the user will be notified of the detailed error via the
// |on_fidl_error| method on the event handler.
//
// ## Thread safety
//
// |Client| provides an easier to use API in exchange of a more restrictive
// threading model:
//
// - The provided |fdf_dispatcher_t| must be a [synchronized dispatcher][synchronized-dispatcher].
// - The client must be bound on a task running on that dispatcher.
// - The client must be destroyed on a task running on that dispatcher.
// - FIDL method calls must be made from tasks running on that dispatcher.
// - Responses are always delivered from dispatcher tasks, as are events.
//
// The above rules are checked in debug builds at run-time. In short, the client
// is local to its associated dispatcher.
//
// Note that FIDL method calls must be synchronized with operations that consume
// or mutate the |Client| itself:
//
// - Assigning a new value to the |Client| variable.
// - Moving the |Client| to a different location.
// - Destroying the |Client|.
//
// |Client| is suitable for systems with stronger sequential threading
// guarantees. It is intended to be used as a local variable with fixed
// lifetime, or as a member of a larger class where it is uniquely owned by
// instances of that class. Destroying the |Client| is guaranteed to stop
// message dispatch: since the client is destroyed on the dispatcher thread,
// there is no opportunity of parallel callbacks to user code, and
// use-after-free of user objects is naturally avoided during teardown.
//
// See |SharedClient| for a client that supports binding and destroying on
// arbitrary threads, at the expense of requiring two-phase shutdown.
//
// [synchronized-dispatcher]:
// https://fuchsia.dev/fuchsia-src/development/languages/c-cpp/thread-safe-async#synchronized-dispatcher
template <typename Protocol>
class Client {
 private:
  using NaturalClientImpl = fidl::internal::NaturalClientImpl<Protocol>;

 public:
  // Create an initialized client which manages the binding of the client end of
  // a channel to a dispatcher, as if that client had been default-constructed
  // then later bound to that endpoint via |Bind|.
  //
  // It is a logic error to use a dispatcher that is shutting down or already
  // shut down. Doing so will result in a panic.
  //
  // If any other error occurs during initialization, the
  // |event_handler->on_fidl_error| handler will be invoked asynchronously with
  // the reason, if specified.
  template <typename AsyncEventHandler = fdf::AsyncEventHandler<Protocol>>
  Client(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
         AsyncEventHandler* event_handler = nullptr) {
    Bind(std::move(client_end), dispatcher, event_handler);
  }

  // Create an uninitialized client. The client may then be bound to an endpoint
  // later via |Bind|.
  //
  // Prefer using the constructor overload that binds the client to a channel
  // atomically during construction. Use this default constructor only when the
  // client must be constructed first before a channel could be obtained (for
  // example, if the client is an instance variable).
  Client() = default;

  // Returns if the |Client| is initialized.
  bool is_valid() const { return controller_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // The destructor of |Client| will initiate binding teardown.
  //
  // When the client destructs:
  // - The channel will be closed.
  // - Pointers obtained via |get| will be invalidated.
  // - Binding teardown will happen, implying:
  //   * In-progress calls will be forgotten. Async callbacks will be dropped.
  ~Client() = default;

  // |Client|s can be safely moved without affecting any in-flight FIDL
  // method calls. Note that calling methods on a client should be serialized
  // with respect to operations that consume the client, such as moving it or
  // destroying it.
  Client(Client&& other) noexcept { MoveImpl(std::move(other)); }
  Client& operator=(Client&& other) noexcept {
    if (this != &other) {
      MoveImpl(std::move(other));
    }
    return *this;
  }

  // Initializes the client by binding the |client_end| endpoint to the
  // dispatcher.
  //
  // It is a logic error to invoke |Bind| on a dispatcher that is shutting down
  // or already shut down. Doing so will result in a panic.
  //
  // When other errors occur during binding, the |event_handler->on_fidl_error|
  // handler will be asynchronously invoked with the reason, if specified.
  //
  // It is not allowed to call |Bind| on an initialized client. To rebind a
  // |Client| to a different endpoint, simply replace the |Client|
  // variable with a new instance.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            fdf::AsyncEventHandler<Protocol>* event_handler = nullptr) {
    controller_.Bind(fidl::internal::MakeAnyTransport(client_end.TakeHandle()),
                     fdf_dispatcher_get_async_dispatcher(dispatcher),
                     fidl::internal::MakeAnyEventDispatcher(event_handler), event_handler,
                     fidl::AnyTeardownObserver::Noop(),
                     fidl::internal::ThreadingPolicy::kCreateAndTeardownFromDispatcherThread);
    natural_client_impl_.emplace(&controller_.get());
  }

  // Returns the interface for making outgoing FIDL calls using natural objects.
  // The client must be initialized first.
  //
  // If the binding has been torn down, calls on the interface return error with
  // status |ZX_ERR_CANCELED| and reason |fidl::Reason::kUnbind|.
  //
  // Persisting this pointer to a local variable is discouraged, since that
  // results in unsafe borrows. Always prefer making calls directly via the
  // |Client| reference-counting type.
  const NaturalClientImpl* operator->() const { return &get(); }
  const NaturalClientImpl& operator*() const { return get(); }

  // Returns the interface for making outgoing FIDL calls using wire objects.
  // The client must be initialized first.
  //
  // If the binding has been torn down, calls on the interface return error with
  // status |ZX_ERR_CANCELED| and reason |fidl::Reason::kUnbind|.
  //
  // Persisting this pointer to a local variable is discouraged, since that
  // results in unsafe borrows. Always prefer making calls directly via the
  // |Client| reference-counting type.
  auto wire() const {
    return fidl::internal::Arrow<fidl::internal::WireWeakAsyncClientImpl<Protocol>>{
        &controller_.get()};
  }

  // Attempts to disassociate the client object from its endpoint and stop
  // monitoring it for messages. After this call, subsequent operations will
  // fail with an unbound error.
  //
  // If there are pending two-way async calls, the endpoint is closed and this
  // method will fail with |fidl::Reason::kPendingTwoWayCallPreventsUnbind|. The
  // caller needs to arrange things such that unbinding happens after any
  // replies to two-way calls.
  //
  // If the endpoint was already closed due to an earlier error, that error will
  // be returned here.
  //
  // Otherwise, returns the client endpoint.
  fit::result<fidl::Error, fdf::ClientEnd<Protocol>> UnbindMaybeGetEndpoint() {
    fit::result result = controller_.UnbindMaybeGetEndpoint();
    if (result.is_error()) {
      return result.take_error();
    }
    return fit::ok(
        fdf::ClientEnd<Protocol>(result.value().release<fidl::internal::DriverTransport>()));
  }

 private:
  const NaturalClientImpl& get() const { return natural_client_impl_.value(); }

  Client(const Client& other) noexcept = delete;
  Client& operator=(const Client& other) noexcept = delete;

  void MoveImpl(Client&& other) noexcept {
    controller_ = std::move(other.controller_);

    // The move operation of |std::optional| does not clear the source optional.
    // As such, we need to manually reset it to prevent two clients referencing
    // the same |ClientBase|.
    natural_client_impl_ = std::move(other.natural_client_impl_);
    other.natural_client_impl_.reset();
  }

  fidl::internal::ClientController controller_;
  cpp17::optional<NaturalClientImpl> natural_client_impl_;
};

template <typename Protocol, typename AsyncEventHandlerReference>
Client(fdf::ClientEnd<Protocol>, fdf_dispatcher_t*, AsyncEventHandlerReference&&)
    -> Client<Protocol>;

template <typename Protocol>
Client(fdf::ClientEnd<Protocol>, fdf_dispatcher_t*) -> Client<Protocol>;

// |SharedClient| is a client for sending and receiving wire and natural
// messages. It is suitable for systems with less defined threading guarantees,
// by providing the building blocks to implement a two-phase asynchronous
// shutdown pattern.
//
// During teardown, |SharedClient| exposes a synchronization point beyond
// which it will not make any more upcalls to user code. The user may then
// arrange any objects that are the recipient of client callbacks to be
// destroyed after the synchronization point. As a result, when destroying an
// entire subsystem, the teardown of the client may be requested from an
// arbitrary thread, in parallel with any callbacks to user code, while
// avoiding use-after-free of user objects.
//
// In addition, |SharedClient| supports cloning multiple instances sharing
// the same underlying endpoint.
//
// ## Lifecycle
//
// See lifecycle notes on |Client| for general lifecycle information. Here
// we note the additional subtleties and two-phase shutdown features exclusive
// to |SharedClient|.
//
// Teardown of the binding is an asynchronous process, to account for the
// possibility of in-progress calls to user code. For example, the bindings
// runtime could be invoking a response callback from a dispatcher thread, while
// the user initiates teardown from an unrelated thread.
//
// There are a number of ways to monitor the completion of teardown:
//
// - Owned event handler: transfer the ownership of an event handler to the
//   bindings as a |std::unique_ptr| when binding the client. After teardown is
//   complete, the event handler will be destroyed. It is safe to destroy the
//   user objects referenced by any client callbacks from within the event
//   handler destructor.
//
// - Teardown observer: provide an instance of |fidl::AnyTeardownObserver| to
//   the bindings. The observer will be notified when teardown is complete.
//
// See
// https://fuchsia.dev/fuchsia-src/development/languages/fidl/guides/llcpp-threading
// for detailed examples.
//
// A |SharedClient| may be |Clone|d, with the clone referencing the same
// endpoint. Automatic teardown occurs when the last clone bound to the
// endpoint is destructed.
//
// |AsyncTeardown| may be called on a |SharedClient| to explicitly initiate
// teardown.
//
// ## Thread safety
//
// FIDL method calls on this class are thread-safe. |AsyncTeardown| and |Clone|
// are also thread-safe, and may be invoked in parallel with FIDL method calls.
// However, those operations must be synchronized with operations that consume
// or mutate the |SharedClient| itself:
//
// - Assigning a new value to the |SharedClient| variable.
// - Moving the |SharedClient| to a different location.
// - Destroying the |SharedClient| variable.
//
// When teardown completes, the binding will notify the user from a |dispatcher|
// thread, unless the user shuts down the |dispatcher| while there are active
// clients associated with it. In that case, those clients will be synchronously
// torn down, and the notification (e.g. destroying the event handler) will
// happen on the thread invoking dispatcher shutdown.
template <typename Protocol>
class SharedClient final {
 private:
  using NaturalClientImpl = fidl::internal::NaturalClientImpl<Protocol>;

 public:
  // Creates an initialized |SharedClient| which manages the binding of the
  // client end of a channel to a dispatcher.
  //
  // It is a logic error to use a dispatcher that is shutting down or already
  // shut down. Doing so will result in a panic.
  //
  // If any other error occurs during initialization, the
  // |event_handler->on_fidl_error| handler will be invoked asynchronously with
  // the reason, if specified.
  //
  // |event_handler| will be destroyed when teardown completes.
  template <typename AsyncEventHandler = fdf::AsyncEventHandler<Protocol>>
  SharedClient(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
               std::unique_ptr<AsyncEventHandler> event_handler) {
    Bind(std::move(client_end), dispatcher, std::move(event_handler));
  }

  // Creates a |SharedClient| that supports custom behavior on teardown
  // completion via |teardown_observer|. Through helpers that return an
  // |AnyTeardownObserver|, users may link the completion of teardown to the
  // invocation of a callback or the lifecycle of related business objects. See
  // for example |fidl::ObserveTeardown| and |fidl::ShareUntilTeardown|.
  //
  // This overload does not demand taking ownership of |event_handler| by
  // |std::unique_ptr|, hence is suitable when the |event_handler| needs to be
  // managed independently of the client lifetime.
  //
  // See |SharedClient| above for other behavior aspects of the constructor.
  template <typename AsyncEventHandler = fdf::AsyncEventHandler<Protocol>>
  SharedClient(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
               AsyncEventHandler* event_handler,
               fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    Bind(std::move(client_end), dispatcher, event_handler, std::move(teardown_observer));
  }

  // Overload of |SharedClient| that omits the |event_handler|, to
  // workaround C++ limitations on default arguments.
  //
  // See |SharedClient| above for other behavior aspects of the constructor.
  template <typename AsyncEventHandler = fdf::AsyncEventHandler<Protocol>>
  SharedClient(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
               fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    Bind(std::move(client_end), dispatcher, nullptr, std::move(teardown_observer));
  }

  // Creates an uninitialized |SharedClient|.
  //
  // Prefer using the constructor overload that binds the client to a channel
  // atomically during construction. Use this default constructor only when the
  // client must be constructed first before a channel could be obtained (for
  // example, if the client is an instance variable).
  SharedClient() = default;

  // Returns if the |SharedClient| is initialized.
  bool is_valid() const { return controller_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // If the current |SharedClient| is the last instance controlling the
  // current connection, the destructor of this |SharedClient| will trigger
  // teardown.
  //
  // When the last |SharedClient| destructs:
  // - The channel will be closed.
  // - Pointers obtained via |get| will be invalidated.
  // - Teardown will be initiated. See the **Lifecycle** section from the
  //   class documentation of |Client|.
  //
  // See also: |AsyncTeardown|.
  ~SharedClient() = default;

  // |fdf::SharedClient|s can be safely moved without affecting any in-progress
  // operations. Note that calling methods on a client should be serialized with
  // respect to operations that consume the client, such as moving it or
  // destroying it.
  SharedClient(SharedClient&& other) noexcept = default;
  SharedClient& operator=(SharedClient&& other) noexcept = default;

  // Initializes the client by binding the |client_end| endpoint to the dispatcher.
  //
  // It is a logic error to invoke |Bind| on a dispatcher that is shutting down
  // or already shut down. Doing so will result in a panic.
  //
  // It is not allowed to call |Bind| on an initialized client. To rebind a
  // |SharedClient| to a different endpoint, simply replace the
  // |SharedClient| variable with a new instance.
  //
  // When other error occurs during binding, the |event_handler->on_fidl_error|
  // handler will be asynchronously invoked with the reason, if specified.
  //
  // |event_handler| will be destroyed when teardown completes.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            std::unique_ptr<fdf::AsyncEventHandler<Protocol>> event_handler) {
    auto event_handler_raw = event_handler.get();
    Bind(std::move(client_end), dispatcher, event_handler_raw,
         fidl::AnyTeardownObserver::ByOwning(std::move(event_handler)));
  }

  // Overload of |Bind| that supports custom behavior on teardown completion via
  // |teardown_observer|. Through helpers that return an |AnyTeardownObserver|,
  // users may link the completion of teardown to the invocation of a callback
  // or the lifecycle of related business objects. See for example
  // |fidl::ObserveTeardown| and |fidl::ShareUntilTeardown|.
  //
  // This overload does not demand taking ownership of |event_handler| by
  // |std::unique_ptr|, hence is suitable when the |event_handler| needs to be
  // managed independently of the client lifetime.
  //
  // See |Bind| above for other behavior aspects of the function.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            fdf::AsyncEventHandler<Protocol>* event_handler,
            fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    controller_.Bind(fidl::internal::MakeAnyTransport(client_end.TakeHandle()),
                     fdf_dispatcher_get_async_dispatcher(dispatcher),
                     fidl::internal::MakeAnyEventDispatcher(event_handler), event_handler,
                     std::move(teardown_observer),
                     fidl::internal::ThreadingPolicy::kCreateAndTeardownFromAnyThread);
    natural_client_impl_ = std::make_shared<NaturalClientImpl>(&controller_.get());
  }

  // Overload of |Bind| that omits the |event_handler|, to
  // workaround C++ limitations on default arguments.
  //
  // See |Bind| above for other behavior aspects of the constructor.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    Bind(std::move(client_end), dispatcher, nullptr, std::move(teardown_observer));
  }

  // Initiates asynchronous teardown of the bindings. See the **Lifecycle**
  // section from the class documentation.
  //
  // |Bind| must have been called before this.
  //
  // While it is safe to invoke |AsyncTeardown| from any thread, it is unsafe to
  // wait for teardown to complete from a dispatcher thread, as that will likely
  // deadlock.
  void AsyncTeardown() { controller_.Unbind(); }

  // Returns another |SharedClient| instance sharing the same channel.
  //
  // Prefer to |Clone| only when necessary e.g. extending the lifetime of a
  // |SharedClient| to a different scope. Any clone will prevent the cleanup
  // of the channel while the binding is alive.
  SharedClient Clone() { return SharedClient(*this); }

  // Returns the interface for making outgoing FIDL calls using natural objects.
  // The client must be initialized first.
  //
  // If the binding has been torn down, calls on the interface return error with
  // status |ZX_ERR_CANCELED| and reason |fidl::Reason::kUnbind|.
  //
  // Persisting this pointer to a local variable is discouraged, since that
  // results in unsafe borrows. Always prefer making calls directly via the
  // |Client| reference-counting type.
  const NaturalClientImpl* operator->() const { return &get(); }
  const NaturalClientImpl& operator*() const { return get(); }

  // Returns the interface for making outgoing FIDL calls using wire objects.
  // The client must be initialized first.
  //
  // If the binding has been torn down, calls on the interface return error with
  // status |ZX_ERR_CANCELED| and reason |fidl::Reason::kUnbind|.
  //
  // Persisting this pointer to a local variable is discouraged, since that
  // results in unsafe borrows. Always prefer making calls directly via the
  // |Client| reference-counting type.
  auto wire() const {
    return fidl::internal::Arrow<fidl::internal::WireWeakAsyncClientImpl<Protocol>>{
        &controller_.get()};
  }

 private:
  const NaturalClientImpl& get() const {
    auto* impl = natural_client_impl_.get();
    ZX_ASSERT(impl != nullptr);
    return *impl;
  }

  SharedClient(const SharedClient& other) noexcept = default;
  SharedClient& operator=(const SharedClient& other) noexcept = default;

  fidl::internal::ClientController controller_;
  std::shared_ptr<NaturalClientImpl> natural_client_impl_;
};

template <typename Protocol, typename AsyncEventHandlerReference>
SharedClient(fdf::ClientEnd<Protocol>, fdf_dispatcher_t*, AsyncEventHandlerReference&&,
             fidl::AnyTeardownObserver) -> SharedClient<Protocol>;

template <typename Protocol, typename AsyncEventHandlerReference>
SharedClient(fdf::ClientEnd<Protocol>, fdf_dispatcher_t*, AsyncEventHandlerReference&&)
    -> SharedClient<Protocol>;

template <typename Protocol>
SharedClient(fdf::ClientEnd<Protocol>, fdf_dispatcher_t*) -> SharedClient<Protocol>;

}  // namespace fdf

#endif  // LIB_FIDL_DRIVER_CPP_NATURAL_CLIENT_H_
