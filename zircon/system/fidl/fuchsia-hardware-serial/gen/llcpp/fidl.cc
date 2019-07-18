// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/hardware/serial/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace hardware {
namespace serial {

namespace {

[[maybe_unused]]
constexpr uint64_t kDevice_GetClass_Ordinal = 0x6549990f00000000lu;
extern "C" const fidl_type_t fuchsia_hardware_serial_DeviceGetClassResponseTable;
[[maybe_unused]]
constexpr uint64_t kDevice_SetConfig_Ordinal = 0x10bcc68c00000000lu;
extern "C" const fidl_type_t fuchsia_hardware_serial_DeviceSetConfigResponseTable;

}  // namespace

zx_status_t Device::SyncClient::GetClass_Deprecated(Class* out_device_class) {
  return Device::Call::GetClass_Deprecated(zx::unowned_channel(this->channel_), out_device_class);
}

zx_status_t Device::Call::GetClass_Deprecated(zx::unowned_channel _client_end, Class* out_device_class) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetClassRequest>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _request = *reinterpret_cast<GetClassRequest*>(_write_bytes);
  _request._hdr.ordinal = kDevice_GetClass_Ordinal;
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetClassRequest));
  ::fidl::DecodedMessage<GetClassRequest> _decoded_request(std::move(_request_bytes));
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return _encode_request_result.status;
  }
  constexpr uint32_t _kReadAllocSize = ::fidl::internal::ClampedMessageSize<GetClassResponse>();
  FIDL_ALIGNDECL uint8_t _read_bytes[_kReadAllocSize];
  ::fidl::BytePart _response_bytes(_read_bytes, _kReadAllocSize);
  auto _call_result = ::fidl::Call<GetClassRequest, GetClassResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_bytes));
  if (_call_result.status != ZX_OK) {
    return _call_result.status;
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result.status;
  }
  auto& _response = *_decode_result.message.message();
  *out_device_class = std::move(_response.device_class);
  return ZX_OK;
}

::fidl::DecodeResult<Device::GetClassResponse> Device::SyncClient::GetClass_Deprecated(::fidl::BytePart _response_buffer, Class* out_device_class) {
  return Device::Call::GetClass_Deprecated(zx::unowned_channel(this->channel_), std::move(_response_buffer), out_device_class);
}

::fidl::DecodeResult<Device::GetClassResponse> Device::Call::GetClass_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart _response_buffer, Class* out_device_class) {
  FIDL_ALIGNDECL uint8_t _write_bytes[sizeof(GetClassRequest)] = {};
  ::fidl::BytePart _request_buffer(_write_bytes, sizeof(_write_bytes));
  auto& _request = *reinterpret_cast<GetClassRequest*>(_request_buffer.data());
  _request._hdr.ordinal = kDevice_GetClass_Ordinal;
  _request_buffer.set_actual(sizeof(GetClassRequest));
  ::fidl::DecodedMessage<GetClassRequest> _decoded_request(std::move(_request_buffer));
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<GetClassResponse>(_encode_request_result.status, _encode_request_result.error);
  }
  auto _call_result = ::fidl::Call<GetClassRequest, GetClassResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<GetClassResponse>(_call_result.status, _call_result.error);
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result;
  }
  auto& _response = *_decode_result.message.message();
  *out_device_class = std::move(_response.device_class);
  return _decode_result;
}

::fidl::DecodeResult<Device::GetClassResponse> Device::SyncClient::GetClass_Deprecated(::fidl::BytePart response_buffer) {
  return Device::Call::GetClass_Deprecated(zx::unowned_channel(this->channel_), std::move(response_buffer));
}

::fidl::DecodeResult<Device::GetClassResponse> Device::Call::GetClass_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart response_buffer) {
  FIDL_ALIGNDECL uint8_t _write_bytes[sizeof(GetClassRequest)] = {};
  constexpr uint32_t _write_num_bytes = sizeof(GetClassRequest);
  ::fidl::BytePart _request_buffer(_write_bytes, sizeof(_write_bytes), _write_num_bytes);
  ::fidl::DecodedMessage<GetClassRequest> params(std::move(_request_buffer));
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kDevice_GetClass_Ordinal;
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::GetClassResponse>(
      _encode_request_result.status,
      _encode_request_result.error,
      ::fidl::DecodedMessage<Device::GetClassResponse>());
  }
  auto _call_result = ::fidl::Call<GetClassRequest, GetClassResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::GetClassResponse>(
      _call_result.status,
      _call_result.error,
      ::fidl::DecodedMessage<Device::GetClassResponse>());
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


zx_status_t Device::SyncClient::SetConfig_Deprecated(Config config, int32_t* out_s) {
  return Device::Call::SetConfig_Deprecated(zx::unowned_channel(this->channel_), std::move(config), out_s);
}

zx_status_t Device::Call::SetConfig_Deprecated(zx::unowned_channel _client_end, Config config, int32_t* out_s) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<SetConfigRequest>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _request = *reinterpret_cast<SetConfigRequest*>(_write_bytes);
  _request._hdr.ordinal = kDevice_SetConfig_Ordinal;
  _request.config = std::move(config);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(SetConfigRequest));
  ::fidl::DecodedMessage<SetConfigRequest> _decoded_request(std::move(_request_bytes));
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return _encode_request_result.status;
  }
  constexpr uint32_t _kReadAllocSize = ::fidl::internal::ClampedMessageSize<SetConfigResponse>();
  FIDL_ALIGNDECL uint8_t _read_bytes[_kReadAllocSize];
  ::fidl::BytePart _response_bytes(_read_bytes, _kReadAllocSize);
  auto _call_result = ::fidl::Call<SetConfigRequest, SetConfigResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_bytes));
  if (_call_result.status != ZX_OK) {
    return _call_result.status;
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result.status;
  }
  auto& _response = *_decode_result.message.message();
  *out_s = std::move(_response.s);
  return ZX_OK;
}

::fidl::DecodeResult<Device::SetConfigResponse> Device::SyncClient::SetConfig_Deprecated(::fidl::BytePart _request_buffer, Config config, ::fidl::BytePart _response_buffer, int32_t* out_s) {
  return Device::Call::SetConfig_Deprecated(zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(config), std::move(_response_buffer), out_s);
}

::fidl::DecodeResult<Device::SetConfigResponse> Device::Call::SetConfig_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, Config config, ::fidl::BytePart _response_buffer, int32_t* out_s) {
  if (_request_buffer.capacity() < SetConfigRequest::PrimarySize) {
    return ::fidl::DecodeResult<SetConfigResponse>(ZX_ERR_BUFFER_TOO_SMALL, ::fidl::internal::kErrorRequestBufferTooSmall);
  }
  auto& _request = *reinterpret_cast<SetConfigRequest*>(_request_buffer.data());
  _request._hdr.ordinal = kDevice_SetConfig_Ordinal;
  _request.config = std::move(config);
  _request_buffer.set_actual(sizeof(SetConfigRequest));
  ::fidl::DecodedMessage<SetConfigRequest> _decoded_request(std::move(_request_buffer));
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<SetConfigResponse>(_encode_request_result.status, _encode_request_result.error);
  }
  auto _call_result = ::fidl::Call<SetConfigRequest, SetConfigResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<SetConfigResponse>(_call_result.status, _call_result.error);
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result;
  }
  auto& _response = *_decode_result.message.message();
  *out_s = std::move(_response.s);
  return _decode_result;
}

::fidl::DecodeResult<Device::SetConfigResponse> Device::SyncClient::SetConfig_Deprecated(::fidl::DecodedMessage<SetConfigRequest> params, ::fidl::BytePart response_buffer) {
  return Device::Call::SetConfig_Deprecated(zx::unowned_channel(this->channel_), std::move(params), std::move(response_buffer));
}

::fidl::DecodeResult<Device::SetConfigResponse> Device::Call::SetConfig_Deprecated(zx::unowned_channel _client_end, ::fidl::DecodedMessage<SetConfigRequest> params, ::fidl::BytePart response_buffer) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kDevice_SetConfig_Ordinal;
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::SetConfigResponse>(
      _encode_request_result.status,
      _encode_request_result.error,
      ::fidl::DecodedMessage<Device::SetConfigResponse>());
  }
  auto _call_result = ::fidl::Call<SetConfigRequest, SetConfigResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<Device::SetConfigResponse>(
      _call_result.status,
      _call_result.error,
      ::fidl::DecodedMessage<Device::SetConfigResponse>());
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
  switch (hdr->ordinal) {
    case kDevice_GetClass_Ordinal:
    {
      auto result = ::fidl::DecodeAs<GetClassRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      impl->GetClass(
        Interface::GetClassCompleter::Sync(txn));
      return true;
    }
    case kDevice_SetConfig_Ordinal:
    {
      auto result = ::fidl::DecodeAs<SetConfigRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->SetConfig(std::move(message->config),
        Interface::SetConfigCompleter::Sync(txn));
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


void Device::Interface::GetClassCompleterBase::Reply(Class device_class) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetClassResponse>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<GetClassResponse*>(_write_bytes);
  _response._hdr.ordinal = kDevice_GetClass_Ordinal;
  _response.device_class = std::move(device_class);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetClassResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetClassResponse>(std::move(_response_bytes)));
}

void Device::Interface::GetClassCompleterBase::Reply(::fidl::BytePart _buffer, Class device_class) {
  if (_buffer.capacity() < GetClassResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<GetClassResponse*>(_buffer.data());
  _response._hdr.ordinal = kDevice_GetClass_Ordinal;
  _response.device_class = std::move(device_class);
  _buffer.set_actual(sizeof(GetClassResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetClassResponse>(std::move(_buffer)));
}

void Device::Interface::GetClassCompleterBase::Reply(::fidl::DecodedMessage<GetClassResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kDevice_GetClass_Ordinal;
  CompleterBase::SendReply(std::move(params));
}


void Device::Interface::SetConfigCompleterBase::Reply(int32_t s) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<SetConfigResponse>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<SetConfigResponse*>(_write_bytes);
  _response._hdr.ordinal = kDevice_SetConfig_Ordinal;
  _response.s = std::move(s);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(SetConfigResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<SetConfigResponse>(std::move(_response_bytes)));
}

void Device::Interface::SetConfigCompleterBase::Reply(::fidl::BytePart _buffer, int32_t s) {
  if (_buffer.capacity() < SetConfigResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<SetConfigResponse*>(_buffer.data());
  _response._hdr.ordinal = kDevice_SetConfig_Ordinal;
  _response.s = std::move(s);
  _buffer.set_actual(sizeof(SetConfigResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<SetConfigResponse>(std::move(_buffer)));
}

void Device::Interface::SetConfigCompleterBase::Reply(::fidl::DecodedMessage<SetConfigResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kDevice_SetConfig_Ordinal;
  CompleterBase::SendReply(std::move(params));
}


}  // namespace serial
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp
