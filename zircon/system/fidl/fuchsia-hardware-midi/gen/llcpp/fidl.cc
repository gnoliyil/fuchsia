// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/hardware/midi/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace hardware {
namespace midi {

namespace {

[[maybe_unused]]
constexpr uint64_t kDevice_GetInfo_Ordinal = 0x7eddfe3c00000000lu;
[[maybe_unused]]
constexpr uint64_t kDevice_GetInfo_GenOrdinal = 0x67d496c0a087b7dlu;
extern "C" const fidl_type_t fuchsia_hardware_midi_DeviceGetInfoRequestTable;
extern "C" const fidl_type_t fuchsia_hardware_midi_DeviceGetInfoResponseTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_midi_DeviceGetInfoResponseTable;

}  // namespace
template <>
Device::ResultOf::GetInfo_Impl<Device::GetInfoResponse>::GetInfo_Impl(zx::unowned_channel _client_end) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetInfoRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, GetInfoRequest::PrimarySize);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetInfoRequest));
  ::fidl::DecodedMessage<GetInfoRequest> _decoded_request(std::move(_request_bytes));
  Super::SetResult(
      Device::InPlace::GetInfo(std::move(_client_end), Super::response_buffer()));
}

Device::ResultOf::GetInfo Device::SyncClient::GetInfo() {
  return ResultOf::GetInfo(zx::unowned_channel(this->channel_));
}

Device::ResultOf::GetInfo Device::Call::GetInfo(zx::unowned_channel _client_end) {
  return ResultOf::GetInfo(std::move(_client_end));
}

template <>
Device::UnownedResultOf::GetInfo_Impl<Device::GetInfoResponse>::GetInfo_Impl(zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  FIDL_ALIGNDECL uint8_t _write_bytes[sizeof(GetInfoRequest)] = {};
  ::fidl::BytePart _request_buffer(_write_bytes, sizeof(_write_bytes));
  memset(_request_buffer.data(), 0, GetInfoRequest::PrimarySize);
  _request_buffer.set_actual(sizeof(GetInfoRequest));
  ::fidl::DecodedMessage<GetInfoRequest> _decoded_request(std::move(_request_buffer));
  Super::SetResult(
      Device::InPlace::GetInfo(std::move(_client_end), std::move(_response_buffer)));
}

Device::UnownedResultOf::GetInfo Device::SyncClient::GetInfo(::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetInfo(zx::unowned_channel(this->channel_), std::move(_response_buffer));
}

Device::UnownedResultOf::GetInfo Device::Call::GetInfo(zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer) {
  return UnownedResultOf::GetInfo(std::move(_client_end), std::move(_response_buffer));
}

::fidl::DecodeResult<Device::GetInfoResponse> Device::InPlace::GetInfo(zx::unowned_channel _client_end, ::fidl::BytePart response_buffer) {
  constexpr uint32_t _write_num_bytes = sizeof(GetInfoRequest);
  ::fidl::internal::AlignedBuffer<_write_num_bytes> _write_bytes;
  ::fidl::BytePart _request_buffer = _write_bytes.view();
  _request_buffer.set_actual(_write_num_bytes);
  ::fidl::DecodedMessage<GetInfoRequest> params(std::move(_request_buffer));
  Device::SetTransactionHeaderFor::GetInfoRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::GetInfoResponse>::FromFailure(
        std::move(_encode_request_result));
  }
  auto _call_result = ::fidl::Call<GetInfoRequest, GetInfoResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::GetInfoResponse>::FromFailure(
        std::move(_call_result));
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


bool Device::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
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
    case kDevice_GetInfo_Ordinal:
    case kDevice_GetInfo_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<GetInfoRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      impl->GetInfo(
          Interface::GetInfoCompleter::Sync(txn));
      return true;
    }
    default: {
      return false;
    }
  }
}

bool Device::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}


void Device::Interface::GetInfoCompleterBase::Reply(::llcpp::fuchsia::hardware::midi::Info info) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetInfoResponse, ::fidl::MessageDirection::kSending>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<GetInfoResponse*>(_write_bytes);
  Device::SetTransactionHeaderFor::GetInfoResponse(
      ::fidl::DecodedMessage<GetInfoResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetInfoResponse::PrimarySize,
              GetInfoResponse::PrimarySize)));
  _response.info = std::move(info);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetInfoResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetInfoResponse>(std::move(_response_bytes)));
}

void Device::Interface::GetInfoCompleterBase::Reply(::fidl::BytePart _buffer, ::llcpp::fuchsia::hardware::midi::Info info) {
  if (_buffer.capacity() < GetInfoResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<GetInfoResponse*>(_buffer.data());
  Device::SetTransactionHeaderFor::GetInfoResponse(
      ::fidl::DecodedMessage<GetInfoResponse>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              GetInfoResponse::PrimarySize,
              GetInfoResponse::PrimarySize)));
  _response.info = std::move(info);
  _buffer.set_actual(sizeof(GetInfoResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetInfoResponse>(std::move(_buffer)));
}

void Device::Interface::GetInfoCompleterBase::Reply(::fidl::DecodedMessage<GetInfoResponse> params) {
  Device::SetTransactionHeaderFor::GetInfoResponse(params);
  CompleterBase::SendReply(std::move(params));
}



void Device::SetTransactionHeaderFor::GetInfoRequest(const ::fidl::DecodedMessage<Device::GetInfoRequest>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kDevice_GetInfo_GenOrdinal);
}
void Device::SetTransactionHeaderFor::GetInfoResponse(const ::fidl::DecodedMessage<Device::GetInfoResponse>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kDevice_GetInfo_GenOrdinal);
}

}  // namespace midi
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp
