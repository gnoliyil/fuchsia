// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/net/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace net {

namespace {

[[maybe_unused]]
constexpr uint64_t kConnectivity_OnNetworkReachable_Ordinal = 0x658708c800000000lu;
extern "C" const fidl_type_t fuchsia_net_ConnectivityOnNetworkReachableEventTable;

}  // namespace
zx_status_t Connectivity::SyncClient::HandleEvents(Connectivity::EventHandlers handlers) {
  return Connectivity::Call::HandleEvents(zx::unowned_channel(channel_), std::move(handlers));
}

zx_status_t Connectivity::Call::HandleEvents(zx::unowned_channel client_end,
                                            Connectivity::EventHandlers handlers) {
  zx_status_t status = client_end->wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                            zx::time::infinite(),
                                            nullptr);
  if (status != ZX_OK) {
    return status;
  }
  constexpr uint32_t kReadAllocSize = ([]() constexpr {
    uint32_t x = 0;
    if (::fidl::internal::ClampedMessageSize<OnNetworkReachableResponse>() >= x) {
      x = ::fidl::internal::ClampedMessageSize<OnNetworkReachableResponse>();
    }
    return x;
  })();
  constexpr uint32_t kHandleAllocSize = ([]() constexpr {
    uint32_t x = 0;
    if (OnNetworkReachableResponse::MaxNumHandles >= x) {
      x = OnNetworkReachableResponse::MaxNumHandles;
    }
    if (x > ZX_CHANNEL_MAX_MSG_HANDLES) {
      x = ZX_CHANNEL_MAX_MSG_HANDLES;
    }
    return x;
  })();
  FIDL_ALIGNDECL uint8_t read_bytes[kReadAllocSize];
  zx_handle_t read_handles[kHandleAllocSize];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  status = client_end->read(ZX_CHANNEL_READ_MAY_DISCARD,
                            read_bytes, read_handles,
                            kReadAllocSize, kHandleAllocSize,
                            &actual_bytes, &actual_handles);
  if (status == ZX_ERR_BUFFER_TOO_SMALL) {
    // Message size is unexpectedly larger than calculated.
    // This can only be due to a newer version of the protocol defining a new event,
    // whose size exceeds the maximum of known events in the current protocol.
    return handlers.unknown();
  }
  if (status != ZX_OK) {
    return status;
  }
  if (actual_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(read_handles, actual_handles);
    return ZX_ERR_INVALID_ARGS;
  }
  auto msg = fidl_msg_t {
    .bytes = read_bytes,
    .handles = read_handles,
    .num_bytes = actual_bytes,
    .num_handles = actual_handles
  };
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg.bytes);
  switch (hdr->ordinal) {
    case kConnectivity_OnNetworkReachable_Ordinal:
    {
      auto result = ::fidl::DecodeAs<OnNetworkReachableResponse>(&msg);
      if (result.status != ZX_OK) {
        return result.status;
      }
      auto message = result.message.message();
      return handlers.on_network_reachable(std::move(message->reachable));
    }
    default:
      zx_handle_close_many(read_handles, actual_handles);
      return handlers.unknown();
  }
}

bool Connectivity::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  if (msg->num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_INVALID_ARGS);
    return true;
  }
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  switch (hdr->ordinal) {
    default: {
      return false;
    }
  }
}

bool Connectivity::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}


zx_status_t Connectivity::SendOnNetworkReachableEvent(::zx::unowned_channel _chan, bool reachable) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<OnNetworkReachableResponse>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<OnNetworkReachableResponse*>(_write_bytes);
  _response._hdr = {};
  _response._hdr.ordinal = kConnectivity_OnNetworkReachable_Ordinal;
  _response.reachable = std::move(reachable);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(OnNetworkReachableResponse));
  return ::fidl::Write(zx::unowned_channel(_chan), ::fidl::DecodedMessage<OnNetworkReachableResponse>(std::move(_response_bytes)));
}

zx_status_t Connectivity::SendOnNetworkReachableEvent(::zx::unowned_channel _chan, ::fidl::BytePart _buffer, bool reachable) {
  if (_buffer.capacity() < OnNetworkReachableResponse::PrimarySize) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }
  auto& _response = *reinterpret_cast<OnNetworkReachableResponse*>(_buffer.data());
  _response._hdr = {};
  _response._hdr.ordinal = kConnectivity_OnNetworkReachable_Ordinal;
  _response.reachable = std::move(reachable);
  _buffer.set_actual(sizeof(OnNetworkReachableResponse));
  return ::fidl::Write(zx::unowned_channel(_chan), ::fidl::DecodedMessage<OnNetworkReachableResponse>(std::move(_buffer)));
}

zx_status_t Connectivity::SendOnNetworkReachableEvent(::zx::unowned_channel _chan, ::fidl::DecodedMessage<OnNetworkReachableResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kConnectivity_OnNetworkReachable_Ordinal;
  return ::fidl::Write(zx::unowned_channel(_chan), std::move(params));
}


::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::NameLookup_LookupHostname_Result() {
  tag_ = Tag::Invalid;
}

::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::~NameLookup_LookupHostname_Result() {
  Destroy();
}

void ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::Destroy() {
  switch (which()) {
  case Tag::kResponse:
    response_.~NameLookup_LookupHostname_Response();
    break;
  case Tag::kErr:
    err_.~LookupError();
    break;
  default:
    break;
  }
  tag_ = Tag::Invalid;
}

void ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::MoveImpl_(NameLookup_LookupHostname_Result&& other) {
  switch (other.which()) {
  case Tag::kResponse:
    mutable_response() = std::move(other.mutable_response());
    break;
  case Tag::kErr:
    mutable_err() = std::move(other.mutable_err());
    break;
  default:
    break;
  }
  other.Destroy();
}

void ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::SizeAndOffsetAssertionHelper() {
  static_assert(offsetof(::llcpp::fuchsia::net::NameLookup_LookupHostname_Result, response_) == 8);
  static_assert(offsetof(::llcpp::fuchsia::net::NameLookup_LookupHostname_Result, err_) == 8);
  static_assert(sizeof(::llcpp::fuchsia::net::NameLookup_LookupHostname_Result) == ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::PrimarySize);
}


NameLookup_LookupHostname_Response& ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::mutable_response() {
  if (which() != Tag::kResponse) {
    Destroy();
    new (&response_) NameLookup_LookupHostname_Response;
  }
  tag_ = Tag::kResponse;
  return response_;
}

LookupError& ::llcpp::fuchsia::net::NameLookup_LookupHostname_Result::mutable_err() {
  if (which() != Tag::kErr) {
    Destroy();
    new (&err_) LookupError;
  }
  tag_ = Tag::kErr;
  return err_;
}


::llcpp::fuchsia::net::NameLookup_LookupIp_Result::NameLookup_LookupIp_Result() {
  tag_ = Tag::Invalid;
}

::llcpp::fuchsia::net::NameLookup_LookupIp_Result::~NameLookup_LookupIp_Result() {
  Destroy();
}

void ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::Destroy() {
  switch (which()) {
  case Tag::kResponse:
    response_.~NameLookup_LookupIp_Response();
    break;
  case Tag::kErr:
    err_.~LookupError();
    break;
  default:
    break;
  }
  tag_ = Tag::Invalid;
}

void ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::MoveImpl_(NameLookup_LookupIp_Result&& other) {
  switch (other.which()) {
  case Tag::kResponse:
    mutable_response() = std::move(other.mutable_response());
    break;
  case Tag::kErr:
    mutable_err() = std::move(other.mutable_err());
    break;
  default:
    break;
  }
  other.Destroy();
}

void ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::SizeAndOffsetAssertionHelper() {
  static_assert(offsetof(::llcpp::fuchsia::net::NameLookup_LookupIp_Result, response_) == 8);
  static_assert(offsetof(::llcpp::fuchsia::net::NameLookup_LookupIp_Result, err_) == 8);
  static_assert(sizeof(::llcpp::fuchsia::net::NameLookup_LookupIp_Result) == ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::PrimarySize);
}


NameLookup_LookupIp_Response& ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::mutable_response() {
  if (which() != Tag::kResponse) {
    Destroy();
    new (&response_) NameLookup_LookupIp_Response;
  }
  tag_ = Tag::kResponse;
  return response_;
}

LookupError& ::llcpp::fuchsia::net::NameLookup_LookupIp_Result::mutable_err() {
  if (which() != Tag::kErr) {
    Destroy();
    new (&err_) LookupError;
  }
  tag_ = Tag::kErr;
  return err_;
}


::llcpp::fuchsia::net::IpAddress::IpAddress() {
  tag_ = Tag::Invalid;
}

::llcpp::fuchsia::net::IpAddress::~IpAddress() {
  Destroy();
}

void ::llcpp::fuchsia::net::IpAddress::Destroy() {
  switch (which()) {
  case Tag::kIpv4:
    ipv4_.~Ipv4Address();
    break;
  case Tag::kIpv6:
    ipv6_.~Ipv6Address();
    break;
  default:
    break;
  }
  tag_ = Tag::Invalid;
}

void ::llcpp::fuchsia::net::IpAddress::MoveImpl_(IpAddress&& other) {
  switch (other.which()) {
  case Tag::kIpv4:
    mutable_ipv4() = std::move(other.mutable_ipv4());
    break;
  case Tag::kIpv6:
    mutable_ipv6() = std::move(other.mutable_ipv6());
    break;
  default:
    break;
  }
  other.Destroy();
}

void ::llcpp::fuchsia::net::IpAddress::SizeAndOffsetAssertionHelper() {
  static_assert(offsetof(::llcpp::fuchsia::net::IpAddress, ipv4_) == 4);
  static_assert(offsetof(::llcpp::fuchsia::net::IpAddress, ipv6_) == 4);
  static_assert(sizeof(::llcpp::fuchsia::net::IpAddress) == ::llcpp::fuchsia::net::IpAddress::PrimarySize);
}


Ipv4Address& ::llcpp::fuchsia::net::IpAddress::mutable_ipv4() {
  if (which() != Tag::kIpv4) {
    Destroy();
    new (&ipv4_) Ipv4Address;
  }
  tag_ = Tag::kIpv4;
  return ipv4_;
}

Ipv6Address& ::llcpp::fuchsia::net::IpAddress::mutable_ipv6() {
  if (which() != Tag::kIpv6) {
    Destroy();
    new (&ipv6_) Ipv6Address;
  }
  tag_ = Tag::kIpv6;
  return ipv6_;
}


namespace {

[[maybe_unused]]
constexpr uint64_t kNameLookup_LookupIp_Ordinal = 0x30c22b4c00000000lu;
extern "C" const fidl_type_t fuchsia_net_NameLookupLookupIpRequestTable;
extern "C" const fidl_type_t fuchsia_net_NameLookupLookupIpResponseTable;
[[maybe_unused]]
constexpr uint64_t kNameLookup_LookupHostname_Ordinal = 0x17582c9400000000lu;
extern "C" const fidl_type_t fuchsia_net_NameLookupLookupHostnameRequestTable;
extern "C" const fidl_type_t fuchsia_net_NameLookupLookupHostnameResponseTable;

}  // namespace

::fidl::DecodeResult<NameLookup::LookupIpResponse> NameLookup::SyncClient::LookupIp_Deprecated(::fidl::BytePart _request_buffer, ::fidl::StringView hostname, LookupIpOptions options, ::fidl::BytePart _response_buffer, NameLookup_LookupIp_Result* out_result) {
  return NameLookup::Call::LookupIp_Deprecated(zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(hostname), std::move(options), std::move(_response_buffer), out_result);
}

::fidl::DecodeResult<NameLookup::LookupIpResponse> NameLookup::Call::LookupIp_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::StringView hostname, LookupIpOptions options, ::fidl::BytePart _response_buffer, NameLookup_LookupIp_Result* out_result) {
  if (_request_buffer.capacity() < LookupIpRequest::PrimarySize) {
    return ::fidl::DecodeResult<LookupIpResponse>(ZX_ERR_BUFFER_TOO_SMALL, ::fidl::internal::kErrorRequestBufferTooSmall);
  }
  LookupIpRequest _request = {};
  _request._hdr.ordinal = kNameLookup_LookupIp_Ordinal;
  _request.hostname = std::move(hostname);
  _request.options = std::move(options);
  auto _linearize_result = ::fidl::Linearize(&_request, std::move(_request_buffer));
  if (_linearize_result.status != ZX_OK) {
    return ::fidl::DecodeResult<LookupIpResponse>(_linearize_result.status, _linearize_result.error);
  }
  ::fidl::DecodedMessage<LookupIpRequest> _decoded_request = std::move(_linearize_result.message);
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<LookupIpResponse>(_encode_request_result.status, _encode_request_result.error);
  }
  auto _call_result = ::fidl::Call<LookupIpRequest, LookupIpResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<LookupIpResponse>(_call_result.status, _call_result.error);
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result;
  }
  auto& _response = *_decode_result.message.message();
  *out_result = std::move(_response.result);
  return _decode_result;
}

::fidl::DecodeResult<NameLookup::LookupIpResponse> NameLookup::SyncClient::LookupIp_Deprecated(::fidl::DecodedMessage<LookupIpRequest> params, ::fidl::BytePart response_buffer) {
  return NameLookup::Call::LookupIp_Deprecated(zx::unowned_channel(this->channel_), std::move(params), std::move(response_buffer));
}

::fidl::DecodeResult<NameLookup::LookupIpResponse> NameLookup::Call::LookupIp_Deprecated(zx::unowned_channel _client_end, ::fidl::DecodedMessage<LookupIpRequest> params, ::fidl::BytePart response_buffer) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kNameLookup_LookupIp_Ordinal;
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<NameLookup::LookupIpResponse>(
      _encode_request_result.status,
      _encode_request_result.error,
      ::fidl::DecodedMessage<NameLookup::LookupIpResponse>());
  }
  auto _call_result = ::fidl::Call<LookupIpRequest, LookupIpResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<NameLookup::LookupIpResponse>(
      _call_result.status,
      _call_result.error,
      ::fidl::DecodedMessage<NameLookup::LookupIpResponse>());
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


::fidl::DecodeResult<NameLookup::LookupHostnameResponse> NameLookup::SyncClient::LookupHostname_Deprecated(::fidl::BytePart _request_buffer, IpAddress addr, ::fidl::BytePart _response_buffer, NameLookup_LookupHostname_Result* out_result) {
  return NameLookup::Call::LookupHostname_Deprecated(zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(addr), std::move(_response_buffer), out_result);
}

::fidl::DecodeResult<NameLookup::LookupHostnameResponse> NameLookup::Call::LookupHostname_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, IpAddress addr, ::fidl::BytePart _response_buffer, NameLookup_LookupHostname_Result* out_result) {
  if (_request_buffer.capacity() < LookupHostnameRequest::PrimarySize) {
    return ::fidl::DecodeResult<LookupHostnameResponse>(ZX_ERR_BUFFER_TOO_SMALL, ::fidl::internal::kErrorRequestBufferTooSmall);
  }
  auto& _request = *reinterpret_cast<LookupHostnameRequest*>(_request_buffer.data());
  _request._hdr.ordinal = kNameLookup_LookupHostname_Ordinal;
  _request.addr = std::move(addr);
  _request_buffer.set_actual(sizeof(LookupHostnameRequest));
  ::fidl::DecodedMessage<LookupHostnameRequest> _decoded_request(std::move(_request_buffer));
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<LookupHostnameResponse>(_encode_request_result.status, _encode_request_result.error);
  }
  auto _call_result = ::fidl::Call<LookupHostnameRequest, LookupHostnameResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<LookupHostnameResponse>(_call_result.status, _call_result.error);
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result;
  }
  auto& _response = *_decode_result.message.message();
  *out_result = std::move(_response.result);
  return _decode_result;
}

::fidl::DecodeResult<NameLookup::LookupHostnameResponse> NameLookup::SyncClient::LookupHostname_Deprecated(::fidl::DecodedMessage<LookupHostnameRequest> params, ::fidl::BytePart response_buffer) {
  return NameLookup::Call::LookupHostname_Deprecated(zx::unowned_channel(this->channel_), std::move(params), std::move(response_buffer));
}

::fidl::DecodeResult<NameLookup::LookupHostnameResponse> NameLookup::Call::LookupHostname_Deprecated(zx::unowned_channel _client_end, ::fidl::DecodedMessage<LookupHostnameRequest> params, ::fidl::BytePart response_buffer) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kNameLookup_LookupHostname_Ordinal;
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<NameLookup::LookupHostnameResponse>(
      _encode_request_result.status,
      _encode_request_result.error,
      ::fidl::DecodedMessage<NameLookup::LookupHostnameResponse>());
  }
  auto _call_result = ::fidl::Call<LookupHostnameRequest, LookupHostnameResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<NameLookup::LookupHostnameResponse>(
      _call_result.status,
      _call_result.error,
      ::fidl::DecodedMessage<NameLookup::LookupHostnameResponse>());
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


bool NameLookup::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  if (msg->num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_INVALID_ARGS);
    return true;
  }
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  switch (hdr->ordinal) {
    case kNameLookup_LookupIp_Ordinal:
    {
      auto result = ::fidl::DecodeAs<LookupIpRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->LookupIp(std::move(message->hostname), std::move(message->options),
        Interface::LookupIpCompleter::Sync(txn));
      return true;
    }
    case kNameLookup_LookupHostname_Ordinal:
    {
      auto result = ::fidl::DecodeAs<LookupHostnameRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->LookupHostname(std::move(message->addr),
        Interface::LookupHostnameCompleter::Sync(txn));
      return true;
    }
    default: {
      return false;
    }
  }
}

bool NameLookup::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}


void NameLookup::Interface::LookupIpCompleterBase::Reply(NameLookup_LookupIp_Result result) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<LookupIpResponse>();
  std::unique_ptr<uint8_t[]> _write_bytes_unique_ptr(new uint8_t[_kWriteAllocSize]);
  uint8_t* _write_bytes = _write_bytes_unique_ptr.get();
  LookupIpResponse _response = {};
  _response._hdr.ordinal = kNameLookup_LookupIp_Ordinal;
  _response.result = std::move(result);
  auto _linearize_result = ::fidl::Linearize(&_response, ::fidl::BytePart(_write_bytes,
                                                                          _kWriteAllocSize));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void NameLookup::Interface::LookupIpCompleterBase::Reply(::fidl::BytePart _buffer, NameLookup_LookupIp_Result result) {
  if (_buffer.capacity() < LookupIpResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  LookupIpResponse _response = {};
  _response._hdr.ordinal = kNameLookup_LookupIp_Ordinal;
  _response.result = std::move(result);
  auto _linearize_result = ::fidl::Linearize(&_response, std::move(_buffer));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void NameLookup::Interface::LookupIpCompleterBase::Reply(::fidl::DecodedMessage<LookupIpResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kNameLookup_LookupIp_Ordinal;
  CompleterBase::SendReply(std::move(params));
}


void NameLookup::Interface::LookupHostnameCompleterBase::Reply(NameLookup_LookupHostname_Result result) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<LookupHostnameResponse>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize];
  LookupHostnameResponse _response = {};
  _response._hdr.ordinal = kNameLookup_LookupHostname_Ordinal;
  _response.result = std::move(result);
  auto _linearize_result = ::fidl::Linearize(&_response, ::fidl::BytePart(_write_bytes,
                                                                          _kWriteAllocSize));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void NameLookup::Interface::LookupHostnameCompleterBase::Reply(::fidl::BytePart _buffer, NameLookup_LookupHostname_Result result) {
  if (_buffer.capacity() < LookupHostnameResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  LookupHostnameResponse _response = {};
  _response._hdr.ordinal = kNameLookup_LookupHostname_Ordinal;
  _response.result = std::move(result);
  auto _linearize_result = ::fidl::Linearize(&_response, std::move(_buffer));
  if (_linearize_result.status != ZX_OK) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  CompleterBase::SendReply(std::move(_linearize_result.message));
}

void NameLookup::Interface::LookupHostnameCompleterBase::Reply(::fidl::DecodedMessage<LookupHostnameResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kNameLookup_LookupHostname_Ordinal;
  CompleterBase::SendReply(std::move(params));
}


namespace {

[[maybe_unused]]
constexpr uint64_t kSocketProvider_GetAddrInfo_Ordinal = 0x1dbb070500000000lu;
extern "C" const fidl_type_t fuchsia_net_SocketProviderGetAddrInfoRequestTable;
extern "C" const fidl_type_t fuchsia_net_SocketProviderGetAddrInfoResponseTable;

}  // namespace

zx_status_t SocketProvider::SyncClient::GetAddrInfo_Deprecated(::fidl::StringView node, ::fidl::StringView service, AddrInfoHints* hints, AddrInfoStatus* out_status, uint32_t* out_nres, ::fidl::Array<AddrInfo, 4>* out_res) {
  return SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel(this->channel_), std::move(node), std::move(service), std::move(hints), out_status, out_nres, out_res);
}

zx_status_t SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel _client_end, ::fidl::StringView node, ::fidl::StringView service, AddrInfoHints* hints, AddrInfoStatus* out_status, uint32_t* out_nres, ::fidl::Array<AddrInfo, 4>* out_res) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetAddrInfoRequest>();
  std::unique_ptr<uint8_t[]> _write_bytes_unique_ptr(new uint8_t[_kWriteAllocSize]);
  uint8_t* _write_bytes = _write_bytes_unique_ptr.get();
  GetAddrInfoRequest _request = {};
  _request._hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  _request.node = std::move(node);
  _request.service = std::move(service);
  _request.hints = std::move(hints);
  auto _linearize_result = ::fidl::Linearize(&_request, ::fidl::BytePart(_write_bytes,
                                                                         _kWriteAllocSize));
  if (_linearize_result.status != ZX_OK) {
    return _linearize_result.status;
  }
  ::fidl::DecodedMessage<GetAddrInfoRequest> _decoded_request = std::move(_linearize_result.message);
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return _encode_request_result.status;
  }
  constexpr uint32_t _kReadAllocSize = ::fidl::internal::ClampedMessageSize<GetAddrInfoResponse>();
  FIDL_ALIGNDECL uint8_t _read_bytes[_kReadAllocSize];
  ::fidl::BytePart _response_bytes(_read_bytes, _kReadAllocSize);
  auto _call_result = ::fidl::Call<GetAddrInfoRequest, GetAddrInfoResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_bytes));
  if (_call_result.status != ZX_OK) {
    return _call_result.status;
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result.status;
  }
  auto& _response = *_decode_result.message.message();
  *out_status = std::move(_response.status);
  *out_nres = std::move(_response.nres);
  *out_res = std::move(_response.res);
  return ZX_OK;
}

::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse> SocketProvider::SyncClient::GetAddrInfo_Deprecated(::fidl::BytePart _request_buffer, ::fidl::StringView node, ::fidl::StringView service, AddrInfoHints* hints, ::fidl::BytePart _response_buffer, AddrInfoStatus* out_status, uint32_t* out_nres, ::fidl::Array<AddrInfo, 4>* out_res) {
  return SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(node), std::move(service), std::move(hints), std::move(_response_buffer), out_status, out_nres, out_res);
}

::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse> SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::StringView node, ::fidl::StringView service, AddrInfoHints* hints, ::fidl::BytePart _response_buffer, AddrInfoStatus* out_status, uint32_t* out_nres, ::fidl::Array<AddrInfo, 4>* out_res) {
  if (_request_buffer.capacity() < GetAddrInfoRequest::PrimarySize) {
    return ::fidl::DecodeResult<GetAddrInfoResponse>(ZX_ERR_BUFFER_TOO_SMALL, ::fidl::internal::kErrorRequestBufferTooSmall);
  }
  GetAddrInfoRequest _request = {};
  _request._hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  _request.node = std::move(node);
  _request.service = std::move(service);
  _request.hints = std::move(hints);
  auto _linearize_result = ::fidl::Linearize(&_request, std::move(_request_buffer));
  if (_linearize_result.status != ZX_OK) {
    return ::fidl::DecodeResult<GetAddrInfoResponse>(_linearize_result.status, _linearize_result.error);
  }
  ::fidl::DecodedMessage<GetAddrInfoRequest> _decoded_request = std::move(_linearize_result.message);
  auto _encode_request_result = ::fidl::Encode(std::move(_decoded_request));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<GetAddrInfoResponse>(_encode_request_result.status, _encode_request_result.error);
  }
  auto _call_result = ::fidl::Call<GetAddrInfoRequest, GetAddrInfoResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(_response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<GetAddrInfoResponse>(_call_result.status, _call_result.error);
  }
  auto _decode_result = ::fidl::Decode(std::move(_call_result.message));
  if (_decode_result.status != ZX_OK) {
    return _decode_result;
  }
  auto& _response = *_decode_result.message.message();
  *out_status = std::move(_response.status);
  *out_nres = std::move(_response.nres);
  *out_res = std::move(_response.res);
  return _decode_result;
}

::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse> SocketProvider::SyncClient::GetAddrInfo_Deprecated(::fidl::DecodedMessage<GetAddrInfoRequest> params, ::fidl::BytePart response_buffer) {
  return SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel(this->channel_), std::move(params), std::move(response_buffer));
}

::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse> SocketProvider::Call::GetAddrInfo_Deprecated(zx::unowned_channel _client_end, ::fidl::DecodedMessage<GetAddrInfoRequest> params, ::fidl::BytePart response_buffer) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse>(
      _encode_request_result.status,
      _encode_request_result.error,
      ::fidl::DecodedMessage<SocketProvider::GetAddrInfoResponse>());
  }
  auto _call_result = ::fidl::Call<GetAddrInfoRequest, GetAddrInfoResponse>(
    std::move(_client_end), std::move(_encode_request_result.message), std::move(response_buffer));
  if (_call_result.status != ZX_OK) {
    return ::fidl::DecodeResult<SocketProvider::GetAddrInfoResponse>(
      _call_result.status,
      _call_result.error,
      ::fidl::DecodedMessage<SocketProvider::GetAddrInfoResponse>());
  }
  return ::fidl::Decode(std::move(_call_result.message));
}


bool SocketProvider::TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  if (msg->num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_INVALID_ARGS);
    return true;
  }
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  switch (hdr->ordinal) {
    case kSocketProvider_GetAddrInfo_Ordinal:
    {
      auto result = ::fidl::DecodeAs<GetAddrInfoRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->GetAddrInfo(std::move(message->node), std::move(message->service), std::move(message->hints),
        Interface::GetAddrInfoCompleter::Sync(txn));
      return true;
    }
    default: {
      return false;
    }
  }
}

bool SocketProvider::Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
  bool found = TryDispatch(impl, msg, txn);
  if (!found) {
    zx_handle_close_many(msg->handles, msg->num_handles);
    txn->Close(ZX_ERR_NOT_SUPPORTED);
  }
  return found;
}


void SocketProvider::Interface::GetAddrInfoCompleterBase::Reply(AddrInfoStatus status, uint32_t nres, ::fidl::Array<AddrInfo, 4> res) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<GetAddrInfoResponse>();
  FIDL_ALIGNDECL uint8_t _write_bytes[_kWriteAllocSize] = {};
  auto& _response = *reinterpret_cast<GetAddrInfoResponse*>(_write_bytes);
  _response._hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  _response.status = std::move(status);
  _response.nres = std::move(nres);
  _response.res = std::move(res);
  ::fidl::BytePart _response_bytes(_write_bytes, _kWriteAllocSize, sizeof(GetAddrInfoResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetAddrInfoResponse>(std::move(_response_bytes)));
}

void SocketProvider::Interface::GetAddrInfoCompleterBase::Reply(::fidl::BytePart _buffer, AddrInfoStatus status, uint32_t nres, ::fidl::Array<AddrInfo, 4> res) {
  if (_buffer.capacity() < GetAddrInfoResponse::PrimarySize) {
    CompleterBase::Close(ZX_ERR_INTERNAL);
    return;
  }
  auto& _response = *reinterpret_cast<GetAddrInfoResponse*>(_buffer.data());
  _response._hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  _response.status = std::move(status);
  _response.nres = std::move(nres);
  _response.res = std::move(res);
  _buffer.set_actual(sizeof(GetAddrInfoResponse));
  CompleterBase::SendReply(::fidl::DecodedMessage<GetAddrInfoResponse>(std::move(_buffer)));
}

void SocketProvider::Interface::GetAddrInfoCompleterBase::Reply(::fidl::DecodedMessage<GetAddrInfoResponse> params) {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = kSocketProvider_GetAddrInfo_Ordinal;
  CompleterBase::SendReply(std::move(params));
}


}  // namespace net
}  // namespace fuchsia
}  // namespace llcpp
