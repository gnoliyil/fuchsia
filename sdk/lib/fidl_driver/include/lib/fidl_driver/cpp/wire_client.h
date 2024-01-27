// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_DRIVER_CPP_WIRE_CLIENT_H_
#define LIB_FIDL_DRIVER_CPP_WIRE_CLIENT_H_

#include <lib/fdf/cpp/arena.h>
#include <lib/fdf/dispatcher.h>
#include <lib/fidl/cpp/wire/client_base.h>
#include <lib/fidl_driver/cpp/internal/wire_client_details.h>
#include <lib/fidl_driver/cpp/transport.h>
#include <lib/fidl_driver/cpp/wire_messaging_declarations.h>
#include <lib/fit/result.h>

//
// Maintainer's note: when updating the documentation and function signatures
// below, please make similar updates to the one in
// //sdk/lib/fidl/cpp/wire/include/lib/fidl/cpp/wire/client.h.
//
// |fdf::WireClient| is the driver counterpart to |fidl::WireClient|, augmented
// to work with driver runtime objects, and similar for |fdf::WireSharedClient|
// and |fdf::WireSharedClient|. There is no hard-and-fast rule as to their
// interface requirements, but methods that don't concern themselves with driver
// specifics should generally stay identical between `fdf` and `fidl`.
//

namespace fdf {

namespace internal {

// Helper to chain .sync().buffer(*)->Foo(..)
template <typename Protocol>
class WireSyncBufferNeededVeneer {
 public:
  explicit WireSyncBufferNeededVeneer(fidl::internal::ClientBase* client_base)
      : client_base_(client_base) {}
  auto buffer(const fdf::Arena& arena) const {
    return fidl::internal::Arrow<fidl::internal::WireWeakSyncClientImpl<Protocol>>{client_base_,
                                                                                   arena};
  }

 private:
  fidl::internal::ClientBase* client_base_;
};
}  // namespace internal

// |fdf::WireClient| is a client for sending and receiving FIDL wire messages
// over the driver transport. It exposes similar looking interfaces as
// |fidl::WireClient|, but has driver-specific concepts such as arenas and
// driver dispatchers.
//
// See |fidl::WireClient| for lifecycle notes, which also apply to
// |fdf::WireClient|.
//
// ## Thread safety
//
// |WireClient| provides an easier to use API in exchange of a more restrictive
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
// or mutate the |WireClient| itself:
//
// - Assigning a new value to the |WireClient| variable.
// - Moving the |WireClient| to a different location.
// - Destroying the |WireClient|.
//
// See
// https://fuchsia.dev/fuchsia-src/development/languages/fidl/tutorials/cpp/topics/threading
// for thread safety notes on |fidl::WireClient|, which also largely apply to
// |fdf::WireClient|.
//
// [synchronized-dispatcher]:
// https://fuchsia.dev/fuchsia-src/development/languages/c-cpp/thread-safe-async#synchronized-dispatcher
template <typename Protocol>
class WireClient {
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
  WireClient(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
             fdf::WireAsyncEventHandler<Protocol>* event_handler = nullptr) {
    Bind(std::move(client_end), dispatcher, event_handler);
  }

  // Create an uninitialized client. The client may then be bound to an endpoint
  // later via |Bind|.
  //
  // Prefer using the constructor overload that binds the client to a channel
  // atomically during construction. Use this default constructor only when the
  // client must be constructed first before a channel could be obtained (for
  // example, if the client is an instance variable).
  WireClient() = default;

  // Returns if the |WireClient| is initialized.
  bool is_valid() const { return controller_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // The destructor of |WireClient| will initiate binding teardown.
  //
  // When the client destructs:
  // - The channel will be closed.
  // - Pointers obtained via |get| will be invalidated.
  // - Binding teardown will happen, implying:
  //   * In-progress calls will be forgotten. Async callbacks will be dropped.
  ~WireClient() = default;

  // |WireClient|s can be safely moved without affecting any in-flight FIDL
  // method calls. Note that calling methods on a client should be serialized
  // with respect to operations that consume the client, such as moving it or
  // destroying it.
  WireClient(WireClient&& other) noexcept = default;
  WireClient& operator=(WireClient&& other) noexcept = default;

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
  // |WireClient| to a different endpoint, simply replace the |WireClient|
  // variable with a new instance.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            fdf::WireAsyncEventHandler<Protocol>* event_handler = nullptr) {
    controller_.Bind(fidl::internal::MakeAnyTransport(client_end.TakeHandle()),
                     fdf_dispatcher_get_async_dispatcher(dispatcher),
                     fidl::internal::MakeAnyEventDispatcher(event_handler), event_handler,
                     fidl::AnyTeardownObserver::Noop(),
                     fidl::internal::ThreadingPolicy::kCreateAndTeardownFromDispatcherThread);
  }

  // Returns an interface for making FIDL calls, using the provided |arena| to
  // allocate buffers necessary for each call. Requests will live on the arena.
  // Responses on the other hand live on the arena passed along with the
  // response, which may or may not be the same arena as the request.
  //
  // ## Lifecycle
  //
  // The returned object borrows from this object, hence must not outlive
  // the client object. The return object borrows the arena, hence must not
  // outlive the arena.
  //
  // The returned object may be briefly persisted for use over multiple calls:
  //
  //     fdf::Arena my_arena = /* create the arena */;
  //     fdf::WireClient client(std::move(client_end), some_dispatcher);
  //     auto buffered = client.buffer(my_arena);
  //     buffered->FooMethod(args).ThenExactlyOnce(foo_response_context);
  //     buffered->BarMethod(args).ThenExactlyOnce(bar_response_context);
  //     ...
  //
  // In this situation, those calls will all use the initially provided arena
  // to allocate their message buffers.
  auto buffer(const fdf::Arena& arena) const {
    ZX_ASSERT(is_valid());
    return fidl::internal::Arrow<fidl::internal::WireWeakAsyncBufferClientImpl<Protocol>>(&get(),
                                                                                          arena);
  }

  // Returns a veneer object exposing synchronous calls. Example:
  //
  //     fdf::WireClient client(std::move(client_end), some_dispatcher);
  //     fdf::WireResult result = client.sync().buffer(...)->FooMethod(args);
  //
  auto sync() const {
    ZX_ASSERT(is_valid());
    return fdf::internal::WireSyncBufferNeededVeneer<Protocol>(&get());
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
  fidl::internal::ClientBase& get() const { return controller_.get(); }

  WireClient(const WireClient& other) noexcept = delete;
  WireClient& operator=(const WireClient& other) noexcept = delete;

  fidl::internal::ClientController controller_;
};

// |fdf::WireSharedClient| is a client for sending and receiving FIDL wire messages
// over the driver transport. It exposes similar looking interfaces as
// |fidl::WireSharedClient|, but has driver-specific concepts such as arenas and
// driver dispatchers.
//
// |WireSharedClient| is suitable for systems with less defined threading
// guarantees, by providing the building blocks to implement a two-phase
// asynchronous shutdown pattern.
//
// In addition, |WireSharedClient| supports cloning multiple instances sharing
// the same underlying endpoint.
//
// ## Thread safety
//
// FIDL method calls on this class are thread-safe. |AsyncTeardown| and |Clone|
// are also thread-safe, and may be invoked in parallel with FIDL method calls.
// However, those operations must be synchronized with operations that consume
// or mutate the |WireSharedClient| itself:
//
// - Assigning a new value to the |WireSharedClient| variable.
// - Moving the |WireSharedClient| to a different location.
// - Destroying the |WireSharedClient| variable.
//
// See
// https://fuchsia.dev/fuchsia-src/development/languages/fidl/guides/llcpp-threading
// for thread safety notes on |fidl::WireSharedClient|, which also largely apply
// to |fdf::WireSharedClient|.
template <typename Protocol>
class WireSharedClient final {
 public:
  // Creates an initialized |WireSharedClient| which manages the binding of the
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
  WireSharedClient(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
                   std::unique_ptr<fdf::WireAsyncEventHandler<Protocol>> event_handler) {
    Bind(std::move(client_end), dispatcher, std::move(event_handler));
  }

  // Creates a |WireSharedClient| that supports custom behavior on teardown
  // completion via |teardown_observer|. Through helpers that return an
  // |AnyTeardownObserver|, users may link the completion of teardown to the
  // invocation of a callback or the lifecycle of related business objects. See
  // for example |fidl::ObserveTeardown| and |fidl::ShareUntilTeardown|.
  //
  // This overload does not demand taking ownership of |event_handler| by
  // |std::unique_ptr|, hence is suitable when the |event_handler| needs to be
  // managed independently of the client lifetime.
  //
  // See |WireSharedClient| above for other behavior aspects of the constructor.
  WireSharedClient(
      fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
      fdf::WireAsyncEventHandler<Protocol>* event_handler,
      fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    Bind(std::move(client_end), dispatcher, event_handler, std::move(teardown_observer));
  }

  // Overload of |WireSharedClient| that omits the |event_handler|, to
  // workaround C++ limitations on default arguments.
  //
  // See |WireSharedClient| above for other behavior aspects of the constructor.
  WireSharedClient(
      fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
      fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    Bind(std::move(client_end), dispatcher, nullptr, std::move(teardown_observer));
  }

  // Creates an uninitialized |WireSharedClient|.
  //
  // Prefer using the constructor overload that binds the client to a channel
  // atomically during construction. Use this default constructor only when the
  // client must be constructed first before a channel could be obtained (for
  // example, if the client is an instance variable).
  WireSharedClient() = default;

  // Returns if the |WireSharedClient| is initialized.
  bool is_valid() const { return controller_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // If the current |WireSharedClient| is the last instance controlling the
  // current connection, the destructor of this |WireSharedClient| will trigger
  // teardown.
  //
  // When the last |WireSharedClient| destructs:
  // - The channel will be closed.
  // - Pointers obtained via |get| will be invalidated.
  // - Teardown will be initiated. See the **Lifecycle** section from the
  //   class documentation of |fidl::WireClient|.
  //
  // See also: |AsyncTeardown|.
  ~WireSharedClient() = default;

  // |fidl::WireSharedClient|s can be safely moved without affecting any in-progress
  // operations. Note that calling methods on a client should be serialized with
  // respect to operations that consume the client, such as moving it or
  // destroying it.
  WireSharedClient(WireSharedClient&& other) noexcept = default;
  WireSharedClient& operator=(WireSharedClient&& other) noexcept = default;

  // Initializes the client by binding the |client_end| endpoint to the dispatcher.
  //
  // It is a logic error to invoke |Bind| on a dispatcher that is shutting down
  // or already shut down. Doing so will result in a panic.
  //
  // It is not allowed to call |Bind| on an initialized client. To rebind a
  // |WireSharedClient| to a different endpoint, simply replace the
  // |WireSharedClient| variable with a new instance.
  //
  // When other error occurs during binding, the |event_handler->on_fidl_error|
  // handler will be asynchronously invoked with the reason, if specified.
  //
  // |event_handler| will be destroyed when teardown completes.
  void Bind(fdf::ClientEnd<Protocol> client_end, fdf_dispatcher_t* dispatcher,
            std::unique_ptr<fdf::WireAsyncEventHandler<Protocol>> event_handler) {
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
            fdf::WireAsyncEventHandler<Protocol>* event_handler,
            fidl::AnyTeardownObserver teardown_observer = fidl::AnyTeardownObserver::Noop()) {
    controller_.Bind(fidl::internal::MakeAnyTransport(client_end.TakeHandle()),
                     fdf_dispatcher_get_async_dispatcher(dispatcher),
                     fidl::internal::MakeAnyEventDispatcher(event_handler), event_handler,
                     std::move(teardown_observer),
                     fidl::internal::ThreadingPolicy::kCreateAndTeardownFromAnyThread);
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
  // section from |fidl::WireSharedClient|.
  //
  // |Bind| must have been called before this.
  //
  // While it is safe to invoke |AsyncTeardown| from any thread, it is unsafe to
  // wait for teardown to complete from a dispatcher thread, as that will likely
  // deadlock.
  void AsyncTeardown() { controller_.Unbind(); }

  // Returns another |WireSharedClient| instance sharing the same channel.
  //
  // Prefer to |Clone| only when necessary e.g. extending the lifetime of a
  // |SharedClient| to a different scope. Any clone will prevent the cleanup
  // of the channel while the binding is alive.
  WireSharedClient Clone() { return WireSharedClient(*this); }

  // Returns a veneer object which exposes the caller-allocating API, using
  // the provided |resource| to allocate buffers necessary for each call.
  // See documentation on |WireClient::buffer| for detailed behavior.
  //
  // TODO(fxbug.dev/91107): Consider taking |const fdf::Arena&| or similar.
  auto buffer(const fdf::Arena& arena) const {
    ZX_ASSERT(is_valid());
    return fidl::internal::Arrow<fidl::internal::WireWeakAsyncBufferClientImpl<Protocol>>(&get(),
                                                                                          arena);
  }

  // Returns a veneer object exposing synchronous calls. Example:
  //
  //     fidl::WireClient client(std::move(client_end), some_dispatcher);
  //     fidl::WireResult result = client.sync().buffer(...)->FooMethod(args);
  //
  auto sync() const {
    ZX_ASSERT(is_valid());
    return fdf::internal::WireSyncBufferNeededVeneer<Protocol>(&get());
  }

 private:
  // Allow unit tests to peek into the internals of this class.
  friend ::fidl_testing::ClientChecker;

  fidl::internal::ClientBase& get() const { return controller_.get(); }

  WireSharedClient(const WireSharedClient& other) noexcept = default;
  WireSharedClient& operator=(const WireSharedClient& other) noexcept = default;

  fidl::internal::ClientController controller_;
};

}  // namespace fdf

#endif  // LIB_FIDL_DRIVER_CPP_WIRE_CLIENT_H_
