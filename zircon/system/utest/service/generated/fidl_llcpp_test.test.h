// WARNING: This file is machine generated by fidlgen.

#pragma once

#include <lib/fidl/internal.h>
#include <lib/fidl/txn_header.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/connect_service.h>
#include <lib/fidl/llcpp/service_handler_interface.h>
#include <lib/fidl/llcpp/string_view.h>
#include <lib/fidl/llcpp/sync_call.h>
#include <lib/fidl/llcpp/traits.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fidl/llcpp/vector_view.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <zircon/fidl.h>

namespace llcpp {

namespace fidl {
namespace service {
namespace test {

class Echo;
class EchoService;

extern "C" const fidl_type_t fidl_service_test_EchoEchoStringRequestTable;
extern "C" const fidl_type_t v1_fidl_service_test_EchoEchoStringRequestTable;
extern "C" const fidl_type_t fidl_service_test_EchoEchoStringResponseTable;
extern "C" const fidl_type_t v1_fidl_service_test_EchoEchoStringResponseTable;

class Echo final {
  Echo() = delete;
 public:

  struct EchoStringResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::StringView response;

    static constexpr const fidl_type_t* Type = &v1_fidl_service_test_EchoEchoStringResponseTable;
    static constexpr const fidl_type_t* AltType = &fidl_service_test_EchoEchoStringResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 4294967295;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 4294967295;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct EchoStringRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::StringView value;

    static constexpr const fidl_type_t* Type = &v1_fidl_service_test_EchoEchoStringRequestTable;
    static constexpr const fidl_type_t* AltType = &fidl_service_test_EchoEchoStringRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 4294967295;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 4294967295;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
    using ResponseType = EchoStringResponse;
  };


  // Collection of return types of FIDL calls in this interface.
  class ResultOf final {
    ResultOf() = delete;
   private:
    template <typename ResponseType>
    class EchoString_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      EchoString_Impl(::zx::unowned_channel _client_end, ::fidl::StringView value);
      ~EchoString_Impl() = default;
      EchoString_Impl(EchoString_Impl&& other) = default;
      EchoString_Impl& operator=(EchoString_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };

   public:
    using EchoString = EchoString_Impl<EchoStringResponse>;
  };

  // Collection of return types of FIDL calls in this interface,
  // when the caller-allocate flavor or in-place call is used.
  class UnownedResultOf final {
    UnownedResultOf() = delete;
   private:
    template <typename ResponseType>
    class EchoString_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      EchoString_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::StringView value, ::fidl::BytePart _response_buffer);
      ~EchoString_Impl() = default;
      EchoString_Impl(EchoString_Impl&& other) = default;
      EchoString_Impl& operator=(EchoString_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };

   public:
    using EchoString = EchoString_Impl<EchoStringResponse>;
  };

  class SyncClient final {
   public:
    explicit SyncClient(::zx::channel channel) : channel_(std::move(channel)) {}
    ~SyncClient() = default;
    SyncClient(SyncClient&&) = default;
    SyncClient& operator=(SyncClient&&) = default;

    const ::zx::channel& channel() const { return channel_; }

    ::zx::channel* mutable_channel() { return &channel_; }

    // Request is heap-allocated. Response is heap-allocated.
    ResultOf::EchoString EchoString(::fidl::StringView value);

    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::EchoString EchoString(::fidl::BytePart _request_buffer, ::fidl::StringView value, ::fidl::BytePart _response_buffer);

   private:
    ::zx::channel channel_;
  };

  // Methods to make a sync FIDL call directly on an unowned channel, avoiding setting up a client.
  class Call final {
    Call() = delete;
   public:

    // Request is heap-allocated. Response is heap-allocated.
    static ResultOf::EchoString EchoString(::zx::unowned_channel _client_end, ::fidl::StringView value);

    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::EchoString EchoString(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::StringView value, ::fidl::BytePart _response_buffer);

  };

  // Messages are encoded and decoded in-place when these methods are used.
  // Additionally, requests must be already laid-out according to the FIDL wire-format.
  class InPlace final {
    InPlace() = delete;
   public:

    static ::fidl::DecodeResult<EchoStringResponse> EchoString(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<EchoStringRequest> params, ::fidl::BytePart response_buffer);

  };

  // Pure-virtual interface to be implemented by a server.
  class Interface {
   public:
    Interface() = default;
    virtual ~Interface() = default;
    using _Outer = Echo;
    using _Base = ::fidl::CompleterBase;

    class EchoStringCompleterBase : public _Base {
     public:
      void Reply(::fidl::StringView response);
      void Reply(::fidl::BytePart _buffer, ::fidl::StringView response);
      void Reply(::fidl::DecodedMessage<EchoStringResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using EchoStringCompleter = ::fidl::Completer<EchoStringCompleterBase>;

    virtual void EchoString(::fidl::StringView value, EchoStringCompleter::Sync _completer) = 0;

  };

  // Attempts to dispatch the incoming message to a handler function in the server implementation.
  // If there is no matching handler, it returns false, leaving the message and transaction intact.
  // In all other cases, it consumes the message and returns true.
  // It is possible to chain multiple TryDispatch functions in this manner.
  static bool TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn);

  // Dispatches the incoming message to one of the handlers functions in the interface.
  // If there is no matching handler, it closes all the handles in |msg| and closes the channel with
  // a |ZX_ERR_NOT_SUPPORTED| epitaph, before returning false. The message should then be discarded.
  static bool Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn);

  // Same as |Dispatch|, but takes a |void*| instead of |Interface*|. Only used with |fidl::Bind|
  // to reduce template expansion.
  // Do not call this method manually. Use |Dispatch| instead.
  static bool TypeErasedDispatch(void* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
    return Dispatch(static_cast<Interface*>(impl), msg, txn);
  }


  // Helper functions to fill in the transaction header in a |DecodedMessage<TransactionalMessage>|.
  class SetTransactionHeaderFor final {
    SetTransactionHeaderFor() = delete;
   public:
    static void EchoStringRequest(const ::fidl::DecodedMessage<Echo::EchoStringRequest>& _msg);
    static void EchoStringResponse(const ::fidl::DecodedMessage<Echo::EchoStringResponse>& _msg);
  };
};

// A service that provides multiple Echo implementations.
class EchoService final {
  EchoService() = default;
 public:
  static constexpr char Name[] = "fidl.service.test.EchoService";

  // Client interface for connecting to member protocols of a service instance.
  class ServiceClient final {
    ServiceClient() = delete;
   public:
    ServiceClient(::zx::channel dir, ::fidl::internal::ConnectMemberFunc connect_func)
    : dir_(std::move(dir)), connect_func_(connect_func) {}
  
    // Connects to the member protocol "foo". Returns a |fidl::ClientChannel| on
    // success, which can be used with |fidl::BindSyncClient| to create a synchronous
    // client.
    //
    // # Errors
    //
    // On failure, returns a fit::error with zx_status_t != ZX_OK.
    // Failures can occur if channel creation failed, or if there was an issue making
    // a |fuchsia.io.Directory::Open| call.
    //
    // Since the call to |Open| is asynchronous, an error sent by the remote end will not
    // result in a failure of this method. Any errors sent by the remote will appear on
    // the |ClientChannel| returned from this method.
    ::fidl::result<::fidl::ClientChannel<Echo>> connect_foo() {
      ::zx::channel local, remote;
      zx_status_t result = ::zx::channel::create(0, &local, &remote);
      if (result != ZX_OK) {
        return ::fit::error(result);
      }
      result =
          connect_func_(::zx::unowned_channel(dir_), ::fidl::StringView("foo"), std::move(remote));
      if (result != ZX_OK) {
        return ::fit::error(result);
      }
      return ::fit::ok(::fidl::ClientChannel<Echo>(std::move(local)));
    }
  
    // Connects to the member protocol "bar". Returns a |fidl::ClientChannel| on
    // success, which can be used with |fidl::BindSyncClient| to create a synchronous
    // client.
    //
    // # Errors
    //
    // On failure, returns a fit::error with zx_status_t != ZX_OK.
    // Failures can occur if channel creation failed, or if there was an issue making
    // a |fuchsia.io.Directory::Open| call.
    //
    // Since the call to |Open| is asynchronous, an error sent by the remote end will not
    // result in a failure of this method. Any errors sent by the remote will appear on
    // the |ClientChannel| returned from this method.
    ::fidl::result<::fidl::ClientChannel<Echo>> connect_bar() {
      ::zx::channel local, remote;
      zx_status_t result = ::zx::channel::create(0, &local, &remote);
      if (result != ZX_OK) {
        return ::fit::error(result);
      }
      result =
          connect_func_(::zx::unowned_channel(dir_), ::fidl::StringView("bar"), std::move(remote));
      if (result != ZX_OK) {
        return ::fit::error(result);
      }
      return ::fit::ok(::fidl::ClientChannel<Echo>(std::move(local)));
    }

   private:
    ::zx::channel dir_;
    ::fidl::internal::ConnectMemberFunc connect_func_;
  };

  // Facilitates member protocol registration for servers.
  class Handler final {
   public:
    // Constructs a FIDL Service-typed handler. Does not take ownership of |service_handler|.
    explicit Handler(::llcpp::fidl::ServiceHandlerInterface* service_handler)
        : service_handler_(service_handler) {}
    
    // Adds member "foo" to the service instance. |handler| will be invoked on connection
    // attempts.
    //
    // # Errors
    //
    // Returns ZX_ERR_ALREADY_EXISTS if the member was already added.
    zx_status_t add_foo(::llcpp::fidl::ServiceHandlerInterface::MemberHandler handler) {
      return service_handler_->AddMember("foo", std::move(handler));
    }
    
    // Adds member "bar" to the service instance. |handler| will be invoked on connection
    // attempts.
    //
    // # Errors
    //
    // Returns ZX_ERR_ALREADY_EXISTS if the member was already added.
    zx_status_t add_bar(::llcpp::fidl::ServiceHandlerInterface::MemberHandler handler) {
      return service_handler_->AddMember("bar", std::move(handler));
    }

   private:
    ::llcpp::fidl::ServiceHandlerInterface* service_handler_;  // Not owned.
  };
};

}  // namespace test
}  // namespace service
}  // namespace fidl
}  // namespace llcpp

namespace fidl {

template <>
struct IsFidlType<::llcpp::fidl::service::test::Echo::EchoStringRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::service::test::Echo::EchoStringRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::service::test::Echo::EchoStringRequest)
    == ::llcpp::fidl::service::test::Echo::EchoStringRequest::PrimarySize);
static_assert(offsetof(::llcpp::fidl::service::test::Echo::EchoStringRequest, value) == 16);

template <>
struct IsFidlType<::llcpp::fidl::service::test::Echo::EchoStringResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::service::test::Echo::EchoStringResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::service::test::Echo::EchoStringResponse)
    == ::llcpp::fidl::service::test::Echo::EchoStringResponse::PrimarySize);
static_assert(offsetof(::llcpp::fidl::service::test::Echo::EchoStringResponse, response) == 16);

}  // namespace fidl
