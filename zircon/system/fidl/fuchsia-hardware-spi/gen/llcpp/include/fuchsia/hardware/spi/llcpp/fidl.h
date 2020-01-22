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

namespace fuchsia {
namespace hardware {
namespace spi {

class Device;

extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceTransmitRequestTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceTransmitResponseTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceReceiveRequestTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceReceiveResponseTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceExchangeRequestTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_spi_DeviceExchangeResponseTable;

class Device final {
  Device() = delete;
 public:

  struct TransmitResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceTransmitResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 24;
    static constexpr uint32_t MaxOutOfLine = 0;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = false;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct TransmitRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<uint8_t> data;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceTransmitRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 8200;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 8200;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
    using ResponseType = TransmitResponse;
  };

  struct ReceiveResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;
    ::fidl::VectorView<uint8_t> data;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceReceiveResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 40;
    static constexpr uint32_t MaxOutOfLine = 8200;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct ReceiveRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    uint32_t size;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceReceiveRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 24;
    static constexpr uint32_t MaxOutOfLine = 0;
    static constexpr uint32_t AltPrimarySize = 24;
    static constexpr uint32_t AltMaxOutOfLine = 0;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = false;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
    using ResponseType = ReceiveResponse;
  };

  struct ExchangeResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;
    ::fidl::VectorView<uint8_t> rxdata;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceExchangeResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 40;
    static constexpr uint32_t MaxOutOfLine = 8200;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;
  };
  struct ExchangeRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<uint8_t> txdata;

    static constexpr const fidl_type_t* Type = &v1_fuchsia_hardware_spi_DeviceExchangeRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 8200;
    static constexpr uint32_t AltPrimarySize = 32;
    static constexpr uint32_t AltMaxOutOfLine = 8200;
    static constexpr bool HasFlexibleEnvelope = false;
    static constexpr bool HasPointer = true;
    static constexpr bool ContainsUnion = false;
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;
    using ResponseType = ExchangeResponse;
  };


  // Collection of return types of FIDL calls in this interface.
  class ResultOf final {
    ResultOf() = delete;
   private:
    template <typename ResponseType>
    class Transmit_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      Transmit_Impl(::zx::unowned_channel _client_end, ::fidl::VectorView<uint8_t> data);
      ~Transmit_Impl() = default;
      Transmit_Impl(Transmit_Impl&& other) = default;
      Transmit_Impl& operator=(Transmit_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class Receive_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      Receive_Impl(::zx::unowned_channel _client_end, uint32_t size);
      ~Receive_Impl() = default;
      Receive_Impl(Receive_Impl&& other) = default;
      Receive_Impl& operator=(Receive_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class Exchange_Impl final : private ::fidl::internal::OwnedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::OwnedSyncCallBase<ResponseType>;
     public:
      Exchange_Impl(::zx::unowned_channel _client_end, ::fidl::VectorView<uint8_t> txdata);
      ~Exchange_Impl() = default;
      Exchange_Impl(Exchange_Impl&& other) = default;
      Exchange_Impl& operator=(Exchange_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };

   public:
    using Transmit = Transmit_Impl<TransmitResponse>;
    using Receive = Receive_Impl<ReceiveResponse>;
    using Exchange = Exchange_Impl<ExchangeResponse>;
  };

  // Collection of return types of FIDL calls in this interface,
  // when the caller-allocate flavor or in-place call is used.
  class UnownedResultOf final {
    UnownedResultOf() = delete;
   private:
    template <typename ResponseType>
    class Transmit_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      Transmit_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> data, ::fidl::BytePart _response_buffer);
      ~Transmit_Impl() = default;
      Transmit_Impl(Transmit_Impl&& other) = default;
      Transmit_Impl& operator=(Transmit_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class Receive_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      Receive_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, uint32_t size, ::fidl::BytePart _response_buffer);
      ~Receive_Impl() = default;
      Receive_Impl(Receive_Impl&& other) = default;
      Receive_Impl& operator=(Receive_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };
    template <typename ResponseType>
    class Exchange_Impl final : private ::fidl::internal::UnownedSyncCallBase<ResponseType> {
      using Super = ::fidl::internal::UnownedSyncCallBase<ResponseType>;
     public:
      Exchange_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> txdata, ::fidl::BytePart _response_buffer);
      ~Exchange_Impl() = default;
      Exchange_Impl(Exchange_Impl&& other) = default;
      Exchange_Impl& operator=(Exchange_Impl&& other) = default;
      using Super::status;
      using Super::error;
      using Super::ok;
      using Super::Unwrap;
      using Super::value;
      using Super::operator->;
      using Super::operator*;
    };

   public:
    using Transmit = Transmit_Impl<TransmitResponse>;
    using Receive = Receive_Impl<ReceiveResponse>;
    using Exchange = Exchange_Impl<ExchangeResponse>;
  };

  class SyncClient final {
   public:
    explicit SyncClient(::zx::channel channel) : channel_(std::move(channel)) {}
    ~SyncClient() = default;
    SyncClient(SyncClient&&) = default;
    SyncClient& operator=(SyncClient&&) = default;

    const ::zx::channel& channel() const { return channel_; }

    ::zx::channel* mutable_channel() { return &channel_; }

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Allocates 24 bytes of response buffer on the stack. Request is heap-allocated.
    ResultOf::Transmit Transmit(::fidl::VectorView<uint8_t> data);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::Transmit Transmit(::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> data, ::fidl::BytePart _response_buffer);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Allocates 24 bytes of request buffer on the stack. Response is heap-allocated.
    ResultOf::Receive Receive(uint32_t size);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::Receive Receive(::fidl::BytePart _request_buffer, uint32_t size, ::fidl::BytePart _response_buffer);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Request is heap-allocated. Response is heap-allocated.
    ResultOf::Exchange Exchange(::fidl::VectorView<uint8_t> txdata);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::Exchange Exchange(::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> txdata, ::fidl::BytePart _response_buffer);

   private:
    ::zx::channel channel_;
  };

  // Methods to make a sync FIDL call directly on an unowned channel, avoiding setting up a client.
  class Call final {
    Call() = delete;
   public:

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Allocates 24 bytes of response buffer on the stack. Request is heap-allocated.
    static ResultOf::Transmit Transmit(::zx::unowned_channel _client_end, ::fidl::VectorView<uint8_t> data);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::Transmit Transmit(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> data, ::fidl::BytePart _response_buffer);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Allocates 24 bytes of request buffer on the stack. Response is heap-allocated.
    static ResultOf::Receive Receive(::zx::unowned_channel _client_end, uint32_t size);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::Receive Receive(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, uint32_t size, ::fidl::BytePart _response_buffer);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Request is heap-allocated. Response is heap-allocated.
    static ResultOf::Exchange Exchange(::zx::unowned_channel _client_end, ::fidl::VectorView<uint8_t> txdata);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::Exchange Exchange(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> txdata, ::fidl::BytePart _response_buffer);

  };

  // Messages are encoded and decoded in-place when these methods are used.
  // Additionally, requests must be already laid-out according to the FIDL wire-format.
  class InPlace final {
    InPlace() = delete;
   public:

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    static ::fidl::DecodeResult<TransmitResponse> Transmit(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<TransmitRequest> params, ::fidl::BytePart response_buffer);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    static ::fidl::DecodeResult<ReceiveResponse> Receive(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<ReceiveRequest> params, ::fidl::BytePart response_buffer);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    static ::fidl::DecodeResult<ExchangeResponse> Exchange(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<ExchangeRequest> params, ::fidl::BytePart response_buffer);

  };

  // Pure-virtual interface to be implemented by a server.
  class Interface {
   public:
    Interface() = default;
    virtual ~Interface() = default;
    using _Outer = Device;
    using _Base = ::fidl::CompleterBase;

    class TransmitCompleterBase : public _Base {
     public:
      void Reply(int32_t status);
      void Reply(::fidl::BytePart _buffer, int32_t status);
      void Reply(::fidl::DecodedMessage<TransmitResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using TransmitCompleter = ::fidl::Completer<TransmitCompleterBase>;

    virtual void Transmit(::fidl::VectorView<uint8_t> data, TransmitCompleter::Sync _completer) = 0;

    class ReceiveCompleterBase : public _Base {
     public:
      void Reply(int32_t status, ::fidl::VectorView<uint8_t> data);
      void Reply(::fidl::BytePart _buffer, int32_t status, ::fidl::VectorView<uint8_t> data);
      void Reply(::fidl::DecodedMessage<ReceiveResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ReceiveCompleter = ::fidl::Completer<ReceiveCompleterBase>;

    virtual void Receive(uint32_t size, ReceiveCompleter::Sync _completer) = 0;

    class ExchangeCompleterBase : public _Base {
     public:
      void Reply(int32_t status, ::fidl::VectorView<uint8_t> rxdata);
      void Reply(::fidl::BytePart _buffer, int32_t status, ::fidl::VectorView<uint8_t> rxdata);
      void Reply(::fidl::DecodedMessage<ExchangeResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ExchangeCompleter = ::fidl::Completer<ExchangeCompleterBase>;

    virtual void Exchange(::fidl::VectorView<uint8_t> txdata, ExchangeCompleter::Sync _completer) = 0;

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
    static void TransmitRequest(const ::fidl::DecodedMessage<Device::TransmitRequest>& _msg);
    static void TransmitResponse(const ::fidl::DecodedMessage<Device::TransmitResponse>& _msg);
    static void ReceiveRequest(const ::fidl::DecodedMessage<Device::ReceiveRequest>& _msg);
    static void ReceiveResponse(const ::fidl::DecodedMessage<Device::ReceiveResponse>& _msg);
    static void ExchangeRequest(const ::fidl::DecodedMessage<Device::ExchangeRequest>& _msg);
    static void ExchangeResponse(const ::fidl::DecodedMessage<Device::ExchangeResponse>& _msg);
  };
};

constexpr uint32_t MAX_TRANSFER_SIZE = 8196u;

}  // namespace spi
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp

namespace fidl {

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::TransmitRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::TransmitRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::TransmitRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::TransmitRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::TransmitRequest, data) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::TransmitResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::TransmitResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::TransmitResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::TransmitResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::TransmitResponse, status) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest, size) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse, status) == 16);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse, data) == 24);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest, txdata) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse, status) == 16);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse, rxdata) == 24);

}  // namespace fidl
