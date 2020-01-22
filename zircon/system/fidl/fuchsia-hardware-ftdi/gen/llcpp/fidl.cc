// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/hardware/ftdi/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace hardware {
namespace ftdi {

namespace {

[[maybe_unused]]
constexpr uint64_t kDevice_CreateI2C_Ordinal = 0x6e215cdf00000000lu;
[[maybe_unused]]
constexpr uint64_t kDevice_CreateI2C_GenOrdinal = 0x1d8122e93efc5fdblu;
extern "C" const fidl_type_t v1_fuchsia_hardware_ftdi_DeviceCreateI2CRequestTable;
extern "C" const fidl_type_t v1_fuchsia_hardware_ftdi_DeviceCreateI2CResponseTable;

}  // namespace

Device::ResultOf::CreateI2C_Impl::CreateI2C_Impl(::zx::unowned_channel _client_end, ::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
  constexpr uint32_t _kWriteAllocSize = ::fidl::internal::ClampedMessageSize<CreateI2CRequest, ::fidl::MessageDirection::kSending>();
  ::fidl::internal::AlignedBuffer<_kWriteAllocSize> _write_bytes_inlined;
  auto& _write_bytes_array = _write_bytes_inlined;
  uint8_t* _write_bytes = _write_bytes_array.view().data();
  memset(_write_bytes, 0, CreateI2CRequest::PrimarySize);
  auto& _request = *reinterpret_cast<CreateI2CRequest*>(_write_bytes);
  _request.layout = std::move(layout);
  _request.device = std::move(device);
  ::fidl::BytePart _request_bytes(_write_bytes, _kWriteAllocSize, sizeof(CreateI2CRequest));
  ::fidl::DecodedMessage<CreateI2CRequest> _decoded_request(std::move(_request_bytes));
  Super::operator=(
      Device::InPlace::CreateI2C(std::move(_client_end), std::move(_decoded_request)));
}

Device::ResultOf::CreateI2C Device::SyncClient::CreateI2C(::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
    return ResultOf::CreateI2C(::zx::unowned_channel(this->channel_), std::move(layout), std::move(device));
}

Device::ResultOf::CreateI2C Device::Call::CreateI2C(::zx::unowned_channel _client_end, ::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
  return ResultOf::CreateI2C(std::move(_client_end), std::move(layout), std::move(device));
}


Device::UnownedResultOf::CreateI2C_Impl::CreateI2C_Impl(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
  if (_request_buffer.capacity() < CreateI2CRequest::PrimarySize) {
    Super::status_ = ZX_ERR_BUFFER_TOO_SMALL;
    Super::error_ = ::fidl::internal::kErrorRequestBufferTooSmall;
    return;
  }
  memset(_request_buffer.data(), 0, CreateI2CRequest::PrimarySize);
  auto& _request = *reinterpret_cast<CreateI2CRequest*>(_request_buffer.data());
  _request.layout = std::move(layout);
  _request.device = std::move(device);
  _request_buffer.set_actual(sizeof(CreateI2CRequest));
  ::fidl::DecodedMessage<CreateI2CRequest> _decoded_request(std::move(_request_buffer));
  Super::operator=(
      Device::InPlace::CreateI2C(std::move(_client_end), std::move(_decoded_request)));
}

Device::UnownedResultOf::CreateI2C Device::SyncClient::CreateI2C(::fidl::BytePart _request_buffer, ::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
  return UnownedResultOf::CreateI2C(::zx::unowned_channel(this->channel_), std::move(_request_buffer), std::move(layout), std::move(device));
}

Device::UnownedResultOf::CreateI2C Device::Call::CreateI2C(::zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::llcpp::fuchsia::hardware::ftdi::I2cBusLayout layout, ::llcpp::fuchsia::hardware::ftdi::I2cDevice device) {
  return UnownedResultOf::CreateI2C(std::move(_client_end), std::move(_request_buffer), std::move(layout), std::move(device));
}

::fidl::internal::StatusAndError Device::InPlace::CreateI2C(::zx::unowned_channel _client_end, ::fidl::DecodedMessage<CreateI2CRequest> params) {
  Device::SetTransactionHeaderFor::CreateI2CRequest(params);
  auto _encode_request_result = ::fidl::Encode(std::move(params));
  if (_encode_request_result.status != ZX_OK) {
    return ::fidl::internal::StatusAndError::FromFailure(
        std::move(_encode_request_result));
  }
  zx_status_t _write_status =
      ::fidl::Write(std::move(_client_end), std::move(_encode_request_result.message));
  if (_write_status != ZX_OK) {
    return ::fidl::internal::StatusAndError(_write_status, ::fidl::internal::kErrorWriteFailed);
  } else {
    return ::fidl::internal::StatusAndError(ZX_OK, nullptr);
  }
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
    case kDevice_CreateI2C_Ordinal:
    case kDevice_CreateI2C_GenOrdinal:
    {
      auto result = ::fidl::DecodeAs<CreateI2CRequest>(msg);
      if (result.status != ZX_OK) {
        txn->Close(ZX_ERR_INVALID_ARGS);
        return true;
      }
      auto message = result.message.message();
      impl->CreateI2C(std::move(message->layout), std::move(message->device),
          Interface::CreateI2CCompleter::Sync(txn));
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



void Device::SetTransactionHeaderFor::CreateI2CRequest(const ::fidl::DecodedMessage<Device::CreateI2CRequest>& _msg) {
  fidl_init_txn_header(&_msg.message()->_hdr, 0, kDevice_CreateI2C_GenOrdinal);
  _msg.message()->_hdr.flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
}

}  // namespace ftdi
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp
