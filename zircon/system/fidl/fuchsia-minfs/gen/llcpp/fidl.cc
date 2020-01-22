// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/minfs/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace minfs {

namespace {

[[maybe_unused]]
constexpr uint64_t kMinfs_GetMetrics_Ordinal = 0x4e6b106000000000lu;
[[maybe_unused]]
constexpr uint64_t kMinfs_GetMetrics_GenOrdinal = 0x410bc6db028528f9lu;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsGetMetricsRequestTable;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsGetMetricsResponseTable;
[[maybe_unused]]
constexpr uint64_t kMinfs_ToggleMetrics_Ordinal = 0x51d81eb600000000lu;
[[maybe_unused]]
constexpr uint64_t kMinfs_ToggleMetrics_GenOrdinal = 0x5c4946fb4ee6e518lu;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsToggleMetricsRequestTable;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsToggleMetricsResponseTable;
[[maybe_unused]]
constexpr uint64_t kMinfs_GetAllocatedRegions_Ordinal = 0x2d198b2a00000000lu;
[[maybe_unused]]
constexpr uint64_t kMinfs_GetAllocatedRegions_GenOrdinal = 0x495daecc5d2be4cblu;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsGetAllocatedRegionsRequestTable;
extern "C" const fidl_type_t v1_fuchsia_minfs_MinfsGetAllocatedRegionsResponseTable;

}  // namespace
template <>
Minfs::ResultOf::GetMetrics_Impl<Minfs::GetMetricsResponse>::GetMetrics_Impl(::zx::unowned_channel _client_end) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetMetricsRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, GetMetricsRequest::PrimarySize);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetMetricsRequest));
  ::fidl::DecodedMessage<GetMetricsRequest> _decoded_request(std::move(_request_bytes));
  Super::SetResult(
      Minfs::InPlace::GetMetrics(std::move(_client_end), Super::response_buffer()));
}

Minfs::ResultOf::GetMetrics Minfs::SyncClient::GetMetrics() {
    return ResultOf::GetMetrics(::zx::unowned_channel(this->channel_));
}

Minfs::ResultOf::GetMetrics Minfs::Call::GetMetrics(::zx::unowned_channel _client_end) {
  return ResultOf::GetMetrics(std::move(_client_end));
}

template <>
Minfs::UnownedResultOf::GetMetrics_Impl<Minfs::GetMetricsResponse>::GetMetrics_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  FIDL_ALIGNDECL uint8_t _write_bytes[sizeof(GetMetricsRequest)] = {};
  ::fidl::BytePart _request_buffer(_write_bytes, sizeof(_write_bytes));
  memset(_request_buffer.data(), 0, GetMetricsRequest::PrimarySize);
  _request_buffer.set_actual(sizeof(GetMetricsRequest));
  ::fidl::DecodedMessage<GetMetricsRequest> _decoded_request(std::move(_request_buffer));
  Super::SetResult(
      Minfs::InPlace::GetMetrics(std::move(_client_end), std::move(_response_buffer)));
}

Minfs::UnownedResultOf::GetMetrics Minfs::SyncClient::GetMetrics(::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetMetrics(::zx::unowned_channel(this->channel_), std::move(_response_buffer));
}

Minfs::UnownedResultOf::GetMetrics Minfs::Call::GetMetrics(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetMetrics(std::move(_client_end), std::move(_response_buffer));
}

::fidl::DecodeResult<Minfs::GetMetricsResponse> Minfs::InPlace::GetMetrics(::zx::unowned_channel _client_end, ::fidl::BytePart response_buffer) {
  constexpr uint32_t _write_num_bytes = sizeof(GetMetricsRequest);
  ::fidl::internal::AlignedBuffer<_write_num_bytes> _write_bytes;
  ::fidl::BytePart _request_buffer = _write_bytes.view();
  _request_buffer.set_actual(_write_num_bytes);
  ::fidl::DecodedMessage<GetMetricsRequest> params(std::move(_request_buffer));
  Minfs::SetTransactionHeaderFor::GetMetricsRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::GetMetricsResponse>::FromFailure(
        std::move(_encode_request_result));
  }
  auto _call_result = ::fidl::Call<GetMetricsRequest, GetMetricsResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::GetMetricsResponse>::FromFailure(
        std::move(_call_result));
  }
  return ::fidl::Decode(std::move(_call_result.message));
}

template <>
Minfs::ResultOf::ToggleMetrics_Impl<Minfs::ToggleMetricsResponse>::ToggleMetrics_Impl(::zx::unowned_channel _client_end, bool enable) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<ToggleMetricsRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, ToggleMetricsRequest::PrimarySize);
  auto& _request = *reinterpret_cast<ToggleMetricsRequest*>(_write_bytes);
  _request.enable = std::move(enable);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(ToggleMetricsRequest));
  ::fidl::DecodedMessage<ToggleMetricsRequest> _decoded_request(std::move(_request_bytes));
  Super::SetResult(
      Minfs::InPlace::ToggleMetrics(std::move(_client_end), std::move(_decoded_request), Super::response_buffer()));
}

Minfs::ResultOf::ToggleMetrics Minfs::SyncClient::ToggleMetrics(bool enable) {
    return ResultOf::ToggleMetrics(::zx::unowned_channel(this->channel_), std::move(enable));
}

Minfs::ResultOf::ToggleMetrics Minfs::Call::ToggleMetrics(::zx::unowned_channel _client_end, bool enable) {
  return ResultOf::ToggleMetrics(std::move(_client_end), std::move(enable));
}

template <>
Minfs::UnownedResultOf::ToggleMetrics_Impl<Minfs::ToggleMetricsResponse>::ToggleMetrics_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, bool enable, ::fidl::BytePart _response_buffer) {
  if (_request_buffer.capacity() < ToggleMetricsRequest::PrimarySize) {
    Super::SetFailure(::fidl::DecodeResult<ToggleMetricsResponse>(ZX_ERR_BUFFER_TOO_SMALL, ::fidl::internal::kErrorRequestBufferTooSmall));
    return;
  }
  memset(_request_buffer.data(), 0, ToggleMetricsRequest::PrimarySize);
  auto& _request = *reinterpret_cast<ToggleMetricsRequest*>(_request_buffer.data());
  _request.enable = std::move(enable);
  _request_buffer.set_actual(sizeof(ToggleMetricsRequest));
  ::fidl::DecodedMessage<ToggleMetricsRequest> _decoded_request(std::move(_request_buffer));
  Super::SetResult(
      Minfs::InPlace::ToggleMetrics(std::move(_client_end), std::move(_decoded_request), std::move(_response_buffer)));
}

Minfs::UnownedResultOf::ToggleMetrics Minfs::SyncClient::ToggleMetrics(::fidl::BytePart _request_buffer, bool enable, ::fidl::BytePart _response_buffer) {
  return UnownedResultOf::ToggleMetrics(::zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(enable), std::move(_response_buffer));
}

Minfs::UnownedResultOf::ToggleMetrics Minfs::Call::ToggleMetrics(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, bool enable, ::fidl::BytePart _response_buffer) {
  return UnownedResultOf::ToggleMetrics(std::move(_client_end), std::move(_request_buffer), std::move(enable), std::move(_response_buffer));
}

::fidl::DecodeResult<Minfs::ToggleMetricsResponse> Minfs::InPlace::ToggleMetrics(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<ToggleMetricsRequest> params, ::fidl::BytePart response_buffer) {
  Minfs::SetTransactionHeaderFor::ToggleMetricsRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::ToggleMetricsResponse>::FromFailure(
        std::move(_encode_request_result));
  }
  auto _call_result = ::fidl::Call<ToggleMetricsRequest, ToggleMetricsResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::ToggleMetricsResponse>::FromFailure(
        std::move(_call_result));
  }
  return ::fidl::Decode(std::move(_call_result.message));
}

template <>
Minfs::ResultOf::GetAllocatedRegions_Impl<Minfs::GetAllocatedRegionsResponse>::GetAllocatedRegions_Impl(::zx::unowned_channel _client_end) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetAllocatedRegionsRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, GetAllocatedRegionsRequest::PrimarySize);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetAllocatedRegionsRequest));
  ::fidl::DecodedMessage<GetAllocatedRegionsRequest> _decoded_request(std::move(_request_bytes));
  Super::SetResult(
      Minfs::InPlace::GetAllocatedRegions(std::move(_client_end), Super::response_buffer()));
}

Minfs::ResultOf::GetAllocatedRegions Minfs::SyncClient::GetAllocatedRegions() {
    return ResultOf::GetAllocatedRegions(::zx::unowned_channel(this->channel_));
}

Minfs::ResultOf::GetAllocatedRegions Minfs::Call::GetAllocatedRegions(::zx::unowned_channel _client_end) {
  return ResultOf::GetAllocatedRegions(std::move(_client_end));
}

template <>
Minfs::UnownedResultOf::GetAllocatedRegions_Impl<Minfs::GetAllocatedRegionsResponse>::GetAllocatedRegions_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  FIDL_ALIGNDECL uint8_t _write_bytes[sizeof(GetAllocatedRegionsRequest)] = {};
  ::fidl::BytePart _request_buffer(_write_bytes, sizeof(_write_bytes));
  memset(_request_buffer.data(), 0, GetAllocatedRegionsRequest::PrimarySize);
  _request_buffer.set_actual(sizeof(GetAllocatedRegionsRequest));
  ::fidl::DecodedMessage<GetAllocatedRegionsRequest> _decoded_request(std::move(_request_buffer));
  Super::SetResult(
      Minfs::InPlace::GetAllocatedRegions(std::move(_client_end), std::move(_response_buffer)));
}

Minfs::UnownedResultOf::GetAllocatedRegions Minfs::SyncClient::GetAllocatedRegions(::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetAllocatedRegions(::zx::unowned_channel(this->channel_), std::move(_response_buffer));
}

Minfs::UnownedResultOf::GetAllocatedRegions Minfs::Call::GetAllocatedRegions(::zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetAllocatedRegions(std::move(_client_end), std::move(_response_buffer));
}

::fidl::DecodeResult<Minfs::GetAllocatedRegionsResponse> Minfs::InPlace::GetAllocatedRegions(::zx::unowned_channel _client_end, ::fidl::BytePart response_buffer) {
  constexpr uint32_t _write_num_bytes = sizeof(GetAllocatedRegionsRequest);
  ::fidl::internal::AlignedBuffer<_write_num_bytes> _write_bytes;
  ::fidl::BytePart _request_buffer = _write_bytes.view();
  _request_buffer.set_actual(_write_num_bytes);
  ::fidl::DecodedMessage<GetAllocatedRegionsRequest> params(std::move(_request_buffer));
  Minfs::SetTransactionHeaderFor::GetAllocatedRegionsRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::GetAllocatedRegionsResponse>::FromFailure(
        std::move(_encode_request_result));
  }
  auto _call_result = ::fidl::Call<GetAllocatedRegionsRequest, GetAllocatedRegionsResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Minfs::GetAllocatedRegionsResponse>::FromFailure(
        std::move(_call_result));
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


bool Minfs::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  if (msg->num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_INVALID_ARGS);
    return true;
  }
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  zx_status_t status = fidl_validate_txn_header(hdr);
  if (status != ZX_OK) {
    txn->Close(status);
    return true;
  }
  switch (hdr->ordinal) {
    case kMinfs_GetMetrics_Ordinal:
    case kMinfs_GetMetrics_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<GetMetricsRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      impl->GetMetrics(
          Interface::GetMetricsCompleter::Sync(txn));
      return true;
    }
    case kMinfs_ToggleMetrics_Ordinal:
    case kMinfs_ToggleMetrics_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<ToggleMetricsRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->ToggleMetrics(std::move(message->enable),
          Interface::ToggleMetricsCompleter::Sync(txn));
      return true;
    }
    case kMinfs_GetAllocatedRegions_Ordinal:
    case kMinfs_GetAllocatedRegions_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<GetAllocatedRegionsRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      impl->GetAllocatedRegions(
          Interface::GetAllocatedRegionsCompleter::Sync(txn));
      return true;
    }
    default: {
      return false;
    }
  }
}

bool Minfs::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}


void Minfs::Interface::GetMetricsCompleterBase::Reply(int32_t status, ::llcpp::fuchsia::minfs::Metrics* metrics) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetMetricsResponse, ::fidl::MessageDirection::kSending>();
  std::unique_ptr<uint8_t[]> _write_bytes_unique_ptr(new uint8_t[_kWriteAllocSize]);
  uint8_t* _write_bytes = _write_bytes_unique_ptr.get();
  GetMetricsResponse _response = {};
  Minfs::SetTransactionHeaderFor::GetMetricsResponse(
      ::fidl::DecodedMessage<GetMetricsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetMetricsResponse::PrimarySize,
              GetMetricsResponse::PrimarySize)));
  _response.status = std::move(status);
  _response.metrics = std::move(metrics);
  auto _linearize_result = ::fidl::Linearize(&_response, ::fidl::BytePart(_write_bytes,
                                                                          _kWriteAllocSize));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void Minfs::Interface::GetMetricsCompleterBase::Reply(::fidl::BytePart _buffer, int32_t status, ::llcpp::fuchsia::minfs::Metrics* metrics) {
  if (_buffer.capacity() < GetMetricsResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  GetMetricsResponse _response = {};
  Minfs::SetTransactionHeaderFor::GetMetricsResponse(
      ::fidl::DecodedMessage<GetMetricsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetMetricsResponse::PrimarySize,
              GetMetricsResponse::PrimarySize)));
  _response.status = std::move(status);
  _response.metrics = std::move(metrics);
  auto _linearize_result = ::fidl::Linearize(&_response, std::move(_buffer));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void Minfs::Interface::GetMetricsCompleterBase::Reply(::fidl::DecodedMessage<GetMetricsResponse> params) {
  Minfs::SetTransactionHeaderFor::GetMetricsResponse(params);
  CompleterBase::SendReply(std::move(params));
}


void Minfs::Interface::ToggleMetricsCompleterBase::Reply(int32_t status) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<ToggleMetricsResponse, ::fidl::MessageDirection::kSending>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<ToggleMetricsResponse*>(_write_bytes);
  Minfs::SetTransactionHeaderFor::ToggleMetricsResponse(
      ::fidl::DecodedMessage<ToggleMetricsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              ToggleMetricsResponse::PrimarySize,
              ToggleMetricsResponse::PrimarySize)));
  _response.status = std::move(status);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(ToggleMetricsResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<ToggleMetricsResponse>(std::move(_response_bytes)));
}

void Minfs::Interface::ToggleMetricsCompleterBase::Reply(::fidl::BytePart _buffer, int32_t status) {
  if (_buffer.capacity() < ToggleMetricsResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<ToggleMetricsResponse*>(_buffer.data());
  Minfs::SetTransactionHeaderFor::ToggleMetricsResponse(
      ::fidl::DecodedMessage<ToggleMetricsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              ToggleMetricsResponse::PrimarySize,
              ToggleMetricsResponse::PrimarySize)));
  _response.status = std::move(status);
  _buffer.set_actual(sizeof(ToggleMetricsResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<ToggleMetricsResponse>(std::move(_buffer)));
}

void Minfs::Interface::ToggleMetricsCompleterBase::Reply(::fidl::DecodedMessage<ToggleMetricsResponse> params) {
  Minfs::SetTransactionHeaderFor::ToggleMetricsResponse(params);
  CompleterBase::SendReply(std::move(params));
}


void Minfs::Interface::GetAllocatedRegionsCompleterBase::Reply(int32_t status, ::zx::vmo regions, uint64_t count) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetAllocatedRegionsResponse, ::fidl::MessageDirection::kSending>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<GetAllocatedRegionsResponse*>(_write_bytes);
  Minfs::SetTransactionHeaderFor::GetAllocatedRegionsResponse(
      ::fidl::DecodedMessage<GetAllocatedRegionsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetAllocatedRegionsResponse::PrimarySize,
              GetAllocatedRegionsResponse::PrimarySize)));
  _response.status = std::move(status);
  _response.regions = std::move(regions);
  _response.count = std::move(count);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetAllocatedRegionsResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetAllocatedRegionsResponse>(std::move(_response_bytes)));
}

void Minfs::Interface::GetAllocatedRegionsCompleterBase::Reply(::fidl::BytePart _buffer, int32_t status, ::zx::vmo regions, uint64_t count) {
  if (_buffer.capacity() < GetAllocatedRegionsResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<GetAllocatedRegionsResponse*>(_buffer.data());
  Minfs::SetTransactionHeaderFor::GetAllocatedRegionsResponse(
      ::fidl::DecodedMessage<GetAllocatedRegionsResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetAllocatedRegionsResponse::PrimarySize,
              GetAllocatedRegionsResponse::PrimarySize)));
  _response.status = std::move(status);
  _response.regions = std::move(regions);
  _response.count = std::move(count);
  _buffer.set_actual(sizeof(GetAllocatedRegionsResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetAllocatedRegionsResponse>(std::move(_buffer)));
}

void Minfs::Interface::GetAllocatedRegionsCompleterBase::Reply(::fidl::DecodedMessage<GetAllocatedRegionsResponse> params) {
  Minfs::SetTransactionHeaderFor::GetAllocatedRegionsResponse(params);
  CompleterBase::SendReply(std::move(params));
}



void Minfs::SetTransactionHeaderFor::GetMetricsRequest(const ::fidl::DecodedMessage<Minfs::GetMetricsRequest>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_GetMetrics_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}
void Minfs::SetTransactionHeaderFor::GetMetricsResponse(const ::fidl::DecodedMessage<Minfs::GetMetricsResponse>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_GetMetrics_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}

void Minfs::SetTransactionHeaderFor::ToggleMetricsRequest(const ::fidl::DecodedMessage<Minfs::ToggleMetricsRequest>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_ToggleMetrics_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}
void Minfs::SetTransactionHeaderFor::ToggleMetricsResponse(const ::fidl::DecodedMessage<Minfs::ToggleMetricsResponse>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_ToggleMetrics_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}

void Minfs::SetTransactionHeaderFor::GetAllocatedRegionsRequest(const ::fidl::DecodedMessage<Minfs::GetAllocatedRegionsRequest>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_GetAllocatedRegions_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}
void Minfs::SetTransactionHeaderFor::GetAllocatedRegionsResponse(const ::fidl::DecodedMessage<Minfs::GetAllocatedRegionsResponse>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kMinfs_GetAllocatedRegions_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}

}  // namespace minfs
}  // namespace fuchsia
}  // namespace llcpp
