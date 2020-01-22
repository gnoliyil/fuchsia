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
#include <lib/zx/eventpair.h>
#include <zircon/fidl.h>

namespace llcpp {

namespace fidl {
namespace test {
namespace llcpp {
namespace dirent {

struct DirEnt;
class DirEntTestInterface;

constexpr uint32_t TEST_MAX_PATH = 10u;

constexpr uint32_t SMALL_DIR_VECTOR_SIZE = 3u;

extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTable;

// Fake dirent structure to exercise linearization codepaths.
struct DirEnt {
  static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTable;
  static constexpr uint32_t MaxNumHandles = 0;
  static constexpr uint32_t PrimarySize = 32;
  [[maybe_unused]]
  static constexpr uint32_t MaxOutOfLine = 16;
  static constexpr bool HasPointer = true;

  bool is_dir = {};

  ::fidl::StringView name = {};

  int32_t some_flags = {};
};

extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceCountNumDirectoriesRequestTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceCountNumDirectoriesResponseTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceReadDirRequestTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceReadDirResponseTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceConsumeDirectoriesRequestTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceConsumeDirectoriesResponseTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOnDirentsRequestTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOnDirentsEventTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOneWayDirentsRequestTable;
extern "C" const fidl_type_t v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOneWayDirentsResponseTable;

// Test interface implemented by LLCPP, with a manually written server,
// since types with more than one level of indirections are not handled by the C binding.
class DirEntTestInterface final {
  DirEntTestInterface() = delete;
 public:

  struct CountNumDirectoriesResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int64_t num_dir;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceCountNumDirectoriesResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 24;
    static constexpr uint32_t MaxOutOfLine = 0;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = false;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct CountNumDirectoriesRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceCountNumDirectoriesRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 48000;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 48000;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
    using ResponseType = CountNumDirectoriesResponse;
  };

  struct ReadDirResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceReadDirResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 144;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  using ReadDirRequest = ::fidl::AnyZeroArgMessage;

  using ConsumeDirectoriesResponse = ::fidl::AnyZeroArgMessage;
  struct ConsumeDirectoriesRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceConsumeDirectoriesRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 144;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 144;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
  };

  struct OnDirentsResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOnDirentsEventTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 48000;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct OneWayDirentsRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents;
    ::zx::eventpair ep;

    static constexpr const fidl_type_t* Type = &v1_fidl_test_llcpp_dirent_DirEntTestInterfaceOneWayDirentsRequestTable;
    static constexpr uint32_t MaxNumHandles = 1;
    static constexpr uint32_t PrimarySize = 40;
    static constexpr uint32_t MaxOutOfLine = 48000;
    static constexpr uint32_t AltPrimarySize = 40;
    static constexpr uint32_t AltMaxOutOfLine = 48000;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
  };


  struct EventHandlers {
    // Event
    fit::callback<zx_status_t(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents)> on_dirents;

    // Fallback handler when an unknown ordinal is received.
    // Caller may put custom error handling logic here.
    fit::callback<zx_status_t()> unknown;
  };

  // Collection of return types of FIDL calls in this interface.
  class ResultOf final {
    ResultOf() = delete;
   private:
    template <typename ResponseType>
    class CountNumDirectories_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      CountNumDirectories_Impl(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);
      ~CountNumDirectories_Impl() = default;
      CountNumDirectories_Impl(CountNumDirectories_Impl&& other) = default;
      CountNumDirectories_Impl& operator=(CountNumDirectories_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class ReadDir_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      ReadDir_Impl(::zx::unowned_channel _client_end);
      ~ReadDir_Impl() = default;
      ReadDir_Impl(ReadDir_Impl&& other) = default;
      ReadDir_Impl& operator=(ReadDir_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class ConsumeDirectories_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      ConsumeDirectories_Impl(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);
      ~ConsumeDirectories_Impl() = default;
      ConsumeDirectories_Impl(ConsumeDirectories_Impl&& other) = default;
      ConsumeDirectories_Impl& operator=(ConsumeDirectories_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    class OneWayDirents_Impl final : private ::fidl::internal::StatusAndError {
      using Super = ::fidl::internal::StatusAndError;
     public:
      OneWayDirents_Impl(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);
      ~OneWayDirents_Impl() = default;
      OneWayDirents_Impl(OneWayDirents_Impl&& other) = default;
      OneWayDirents_Impl& operator=(OneWayDirents_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
    };

   public:
    using CountNumDirectories = CountNumDirectories_Impl<CountNumDirectoriesResponse>;
    using ReadDir = ReadDir_Impl<ReadDirResponse>;
    using ConsumeDirectories = ConsumeDirectories_Impl<ConsumeDirectoriesResponse>;
    using OneWayDirents = OneWayDirents_Impl;
  };

  // Collection of return types of FIDL calls in this interface,
  // when the caller-allocate flavor or in-place call is used.
  class UnownedResultOf final {
    UnownedResultOf() = delete;
   private:
    template <typename ResponseType>
    class CountNumDirectories_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      CountNumDirectories_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);
      ~CountNumDirectories_Impl() = default;
      CountNumDirectories_Impl(CountNumDirectories_Impl&& other) = default;
      CountNumDirectories_Impl& operator=(CountNumDirectories_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class ReadDir_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      ReadDir_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer);
      ~ReadDir_Impl() = default;
      ReadDir_Impl(ReadDir_Impl&& other) = default;
      ReadDir_Impl& operator=(ReadDir_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class ConsumeDirectories_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      ConsumeDirectories_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);
      ~ConsumeDirectories_Impl() = default;
      ConsumeDirectories_Impl(ConsumeDirectories_Impl&& other) = default;
      ConsumeDirectories_Impl& operator=(ConsumeDirectories_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    class OneWayDirents_Impl final : private ::fidl::internal::StatusAndError {
      using Super = ::fidl::internal::StatusAndError;
     public:
      OneWayDirents_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);
      ~OneWayDirents_Impl() = default;
      OneWayDirents_Impl(OneWayDirents_Impl&& other) = default;
      OneWayDirents_Impl& operator=(OneWayDirents_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
    };

   public:
    using CountNumDirectories = CountNumDirectories_Impl<CountNumDirectoriesResponse>;
    using ReadDir = ReadDir_Impl<ReadDirResponse>;
    using ConsumeDirectories = ConsumeDirectories_Impl<ConsumeDirectoriesResponse>;
    using OneWayDirents = OneWayDirents_Impl;
  };

  class SyncClient final {
   public:
    explicit SyncClient(::zx::channel channel) : channel_(std::move(channel)) {}
    ~SyncClient() = default;
    SyncClient(SyncClient&&) = default;
    SyncClient& operator=(SyncClient&&) = default;

    const ::zx::channel& channel() const { return channel_; }

    ::zx::channel* mutable_channel() { return &channel_; }

    // Iterate over the dirents and return the number of directories within them.
    // Allocates 24 bytes of response buffer on the stack. Request is heap-allocated.
    ResultOf::CountNumDirectories CountNumDirectories(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

    // Iterate over the dirents and return the number of directories within them.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::CountNumDirectories CountNumDirectories(::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);

    // Return a vector of dirents. Empty request. Response may stack-allocate.
    // Allocates 192 bytes of message buffer on the stack. No heap allocation necessary.
    ResultOf::ReadDir ReadDir();

    // Return a vector of dirents. Empty request. Response may stack-allocate.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::ReadDir ReadDir(::fidl::BytePart _response_buffer);

    // Consume dirents. Empty response. Request may stack-allocate.
    // Allocates 192 bytes of message buffer on the stack. No heap allocation necessary.
    ResultOf::ConsumeDirectories ConsumeDirectories(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

    // Consume dirents. Empty response. Request may stack-allocate.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::ConsumeDirectories ConsumeDirectories(::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);

    // Binding will not wait for response.
    // But here we send an eventpair which the server will signal upon receipt of message.
    // Request is heap-allocated.
    ResultOf::OneWayDirents OneWayDirents(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);

    // Binding will not wait for response.
    // But here we send an eventpair which the server will signal upon receipt of message.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::OneWayDirents OneWayDirents(::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);

    // Handle all possible events defined in this protocol.
    // Blocks to consume exactly one message from the channel, then call the corresponding handler
    // defined in |EventHandlers|. The return status of the handler function is folded with any
    // transport-level errors and returned.
    zx_status_t HandleEvents(EventHandlers handlers);
   private:
    ::zx::channel channel_;
  };

  // Methods to make a sync FIDL call directly on an unowned channel, avoiding setting up a client.
  class Call final {
    Call() = delete;
   public:

    // Iterate over the dirents and return the number of directories within them.
    // Allocates 24 bytes of response buffer on the stack. Request is heap-allocated.
    static ResultOf::CountNumDirectories CountNumDirectories(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

    // Iterate over the dirents and return the number of directories within them.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::CountNumDirectories CountNumDirectories(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);

    // Return a vector of dirents. Empty request. Response may stack-allocate.
    // Allocates 192 bytes of message buffer on the stack. No heap allocation necessary.
    static ResultOf::ReadDir ReadDir(::zx::unowned_channel _client_end);

    // Return a vector of dirents. Empty request. Response may stack-allocate.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::ReadDir ReadDir(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer);

    // Consume dirents. Empty response. Request may stack-allocate.
    // Allocates 192 bytes of message buffer on the stack. No heap allocation necessary.
    static ResultOf::ConsumeDirectories ConsumeDirectories(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

    // Consume dirents. Empty response. Request may stack-allocate.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::ConsumeDirectories ConsumeDirectories(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::fidl::BytePart _response_buffer);

    // Binding will not wait for response.
    // But here we send an eventpair which the server will signal upon receipt of message.
    // Request is heap-allocated.
    static ResultOf::OneWayDirents OneWayDirents(::zx::unowned_channel _client_end, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);

    // Binding will not wait for response.
    // But here we send an eventpair which the server will signal upon receipt of message.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::OneWayDirents OneWayDirents(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep);

    // Handle all possible events defined in this protocol.
    // Blocks to consume exactly one message from the channel, then call the corresponding handler
    // defined in |EventHandlers|. The return status of the handler function is folded with any
    // transport-level errors and returned.
    static zx_status_t HandleEvents(::zx::unowned_channel client_end, EventHandlers handlers);
  };

  // Messages are encoded and decoded in-place when these methods are used.
  // Additionally, requests must be already laid-out according to the FIDL wire-format.
  class InPlace final {
    InPlace() = delete;
   public:

    // Iterate over the dirents and return the number of directories within them.
    static ::fidl::DecodeResult<CountNumDirectoriesResponse> CountNumDirectories(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<CountNumDirectoriesRequest> params, ::fidl::BytePart response_buffer);

    // Return a vector of dirents. Empty request. Response may stack-allocate.
    static ::fidl::DecodeResult<ReadDirResponse> ReadDir(::zx::unowned_channel _client_end, ::fidl::BytePart response_buffer);

    // Consume dirents. Empty response. Request may stack-allocate.
    static ::fidl::DecodeResult<ConsumeDirectoriesResponse> ConsumeDirectories(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<ConsumeDirectoriesRequest> params, ::fidl::BytePart response_buffer);

    // Binding will not wait for response.
    // But here we send an eventpair which the server will signal upon receipt of message.
    static ::fidl::internal::StatusAndError OneWayDirents(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<OneWayDirentsRequest> params);

  };

  // Pure-virtual interface to be implemented by a server.
  class Interface {
   public:
    Interface() = default;
    virtual ~Interface() = default;
    using _Outer = DirEntTestInterface;
    using _Base = ::fidl::CompleterBase;

    class CountNumDirectoriesCompleterBase : public _Base {
     public:
      void Reply(int64_t num_dir);
      void Reply(::fidl::BytePart _buffer, int64_t num_dir);
      void Reply(::fidl::DecodedMessage<CountNumDirectoriesResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using CountNumDirectoriesCompleter = ::fidl::Completer<CountNumDirectoriesCompleterBase>;

    virtual void CountNumDirectories(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, CountNumDirectoriesCompleter::Sync _completer) = 0;

    class ReadDirCompleterBase : public _Base {
     public:
      void Reply(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);
      void Reply(::fidl::BytePart _buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);
      void Reply(::fidl::DecodedMessage<ReadDirResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ReadDirCompleter = ::fidl::Completer<ReadDirCompleterBase>;

    virtual void ReadDir(ReadDirCompleter::Sync _completer) = 0;

    class ConsumeDirectoriesCompleterBase : public _Base {
     public:
      void Reply();

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ConsumeDirectoriesCompleter = ::fidl::Completer<ConsumeDirectoriesCompleterBase>;

    virtual void ConsumeDirectories(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ConsumeDirectoriesCompleter::Sync _completer) = 0;

    using OneWayDirentsCompleter = ::fidl::Completer<>;

    virtual void OneWayDirents(::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents, ::zx::eventpair ep, OneWayDirentsCompleter::Sync _completer) = 0;

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

  // Event
  static zx_status_t SendOnDirentsEvent(::zx::unowned_channel _chan, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

  // Event
  // Caller provides the backing storage for FIDL message via response buffers.
  static zx_status_t SendOnDirentsEvent(::zx::unowned_channel _chan, ::fidl::BytePart _buffer, ::fidl::VectorView<::llcpp::fidl::test::llcpp::dirent::DirEnt> dirents);

  // Event
  // Messages are encoded in-place.
  static zx_status_t SendOnDirentsEvent(::zx::unowned_channel _chan, ::fidl::DecodedMessage<OnDirentsResponse> params);


  // Helper functions to fill in the transaction header in a |DecodedMessage<TransactionalMessage>|.
  class SetTransactionHeaderFor final {
    SetTransactionHeaderFor() = delete;
   public:
    static void CountNumDirectoriesRequest(const ::fidl::DecodedMessage<DirEntTestInterface::CountNumDirectoriesRequest>& _msg);
    static void CountNumDirectoriesResponse(const ::fidl::DecodedMessage<DirEntTestInterface::CountNumDirectoriesResponse>& _msg);
    static void ReadDirRequest(const ::fidl::DecodedMessage<DirEntTestInterface::ReadDirRequest>& _msg);
    static void ReadDirResponse(const ::fidl::DecodedMessage<DirEntTestInterface::ReadDirResponse>& _msg);
    static void ConsumeDirectoriesRequest(const ::fidl::DecodedMessage<DirEntTestInterface::ConsumeDirectoriesRequest>& _msg);
    static void ConsumeDirectoriesResponse(const ::fidl::DecodedMessage<DirEntTestInterface::ConsumeDirectoriesResponse>& _msg);
    static void OnDirentsResponse(const ::fidl::DecodedMessage<DirEntTestInterface::OnDirentsResponse>& _msg);
    static void OneWayDirentsRequest(const ::fidl::DecodedMessage<DirEntTestInterface::OneWayDirentsRequest>& _msg);
  };
};

}  // namespace dirent
}  // namespace llcpp
}  // namespace test
}  // namespace fidl
}  // namespace llcpp

namespace fidl {

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEnt> : public std::true_type {};
static_assert(std::is_standard_layout_v<::llcpp::fidl::test::llcpp::dirent::DirEnt>);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEnt, is_dir) == 0);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEnt, name) == 8);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEnt, some_flags) == 24);
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEnt) == ::llcpp::fidl::test::llcpp::dirent::DirEnt::PrimarySize);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesRequest)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesRequest::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesRequest, dirents) == 16);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesResponse)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesResponse::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::CountNumDirectoriesResponse, num_dir) == 16);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ReadDirResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ReadDirResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ReadDirResponse)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ReadDirResponse::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ReadDirResponse, dirents) == 16);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ConsumeDirectoriesRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ConsumeDirectoriesRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ConsumeDirectoriesRequest)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ConsumeDirectoriesRequest::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::ConsumeDirectoriesRequest, dirents) == 16);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OnDirentsResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OnDirentsResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OnDirentsResponse)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OnDirentsResponse::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OnDirentsResponse, dirents) == 16);

template <>
struct IsFidlType<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest)
    == ::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest::PrimarySize);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest, dirents) == 16);
static_assert(offsetof(::llcpp::fidl::test::llcpp::dirent::DirEntTestInterface::OneWayDirentsRequest, ep) == 32);

}  // namespace fidl
