// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "spi-child.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/trace/event.h>
#include <lib/trace-engine/types.h>

#include <variant>

#include <ddktl/fidl.h>
#include <fbl/vector.h>

namespace spi {

void SpiChild::TransmitVector(TransmitVectorRequestView request,
                              TransmitVectorCompleter::Sync& completer) {
  size_t rx_actual;
  zx_status_t status =
      spi_.Exchange(cs_, request->data.data(), request->data.count(), nullptr, 0, &rx_actual);
  if (status == ZX_OK) {
    completer.Reply(ZX_OK);
  } else {
    completer.Reply(status);
  }
}

void SpiChild::ReceiveVector(ReceiveVectorRequestView request,
                             ReceiveVectorCompleter::Sync& completer) {
  fbl::Vector<uint8_t> rxdata;
  rxdata.reserve(request->size);
  size_t rx_actual;
  zx_status_t status = spi_.Exchange(cs_, nullptr, 0, rxdata.begin(), request->size, &rx_actual);
  if (status == ZX_OK && rx_actual == request->size) {
    auto rx_vector = fidl::VectorView<uint8_t>::FromExternal(rxdata.data(), request->size);
    completer.Reply(ZX_OK, rx_vector);
  } else {
    completer.Reply(status == ZX_OK ? ZX_ERR_INTERNAL : status, fidl::VectorView<uint8_t>());
  }
}

void SpiChild::ExchangeVector(ExchangeVectorRequestView request,
                              ExchangeVectorCompleter::Sync& completer) {
  fbl::Vector<uint8_t> rxdata;
  const size_t size = request->txdata.count();
  rxdata.reserve(size);
  size_t rx_actual;
  zx_status_t status =
      spi_.Exchange(cs_, request->txdata.data(), size, rxdata.begin(), size, &rx_actual);
  if (status == ZX_OK && rx_actual == size) {
    auto rx_vector = fidl::VectorView<uint8_t>::FromExternal(rxdata.data(), size);
    completer.Reply(ZX_OK, rx_vector);
  } else {
    completer.Reply(status == ZX_OK ? ZX_ERR_INTERNAL : status, fidl::VectorView<uint8_t>());
  }
}

void SpiChild::RegisterVmo(RegisterVmoRequestView request, RegisterVmoCompleter::Sync& completer) {
  zx_status_t status =
      spi_.RegisterVmo(cs_, request->vmo_id, std::move(request->vmo.vmo), request->vmo.offset,
                       request->vmo.size, static_cast<uint32_t>(request->rights));
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void SpiChild::UnregisterVmo(UnregisterVmoRequestView request,
                             UnregisterVmoCompleter::Sync& completer) {
  zx::vmo vmo;
  if (zx_status_t status = spi_.UnregisterVmo(cs_, request->vmo_id, &vmo); status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess(std::move(vmo));
}

void SpiChild::Transmit(TransmitRequestView request, TransmitCompleter::Sync& completer) {
  TRACE_DURATION("spi", "Transmit", "cs", cs_, "size", request->buffer.size);
  zx_status_t status =
      spi_.TransmitVmo(cs_, request->buffer.vmo_id, request->buffer.offset, request->buffer.size);
  completer.Reply(zx::make_result(status));
}

void SpiChild::Receive(ReceiveRequestView request, ReceiveCompleter::Sync& completer) {
  TRACE_DURATION("spi", "Receive", "cs", cs_, "size", request->buffer.size);
  zx_status_t status =
      spi_.ReceiveVmo(cs_, request->buffer.vmo_id, request->buffer.offset, request->buffer.size);
  completer.Reply(zx::make_result(status));
}

void SpiChild::Exchange(ExchangeRequestView request, ExchangeCompleter::Sync& completer) {
  if (request->tx_buffer.size != request->rx_buffer.size) {
    completer.ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }
  TRACE_DURATION("spi", "Exchange", "cs", cs_, "size", request->tx_buffer.size);
  zx_status_t status = spi_.ExchangeVmo(cs_, request->tx_buffer.vmo_id, request->tx_buffer.offset,
                                        request->rx_buffer.vmo_id, request->rx_buffer.offset,
                                        request->tx_buffer.size);
  completer.Reply(zx::make_result(status));
}

zx_status_t SpiChild::SpiTransmit(const uint8_t* txdata_list, size_t txdata_count) {
  size_t actual;
  spi_.Exchange(cs_, txdata_list, txdata_count, nullptr, 0, &actual);
  return ZX_OK;
}
zx_status_t SpiChild::SpiReceive(uint32_t size, uint8_t* out_rxdata_list, size_t rxdata_count,
                                 size_t* out_rxdata_actual) {
  spi_.Exchange(cs_, nullptr, 0, out_rxdata_list, rxdata_count, out_rxdata_actual);
  return ZX_OK;
}

zx_status_t SpiChild::SpiExchange(const uint8_t* txdata_list, size_t txdata_count,
                                  uint8_t* out_rxdata_list, size_t rxdata_count,
                                  size_t* out_rxdata_actual) {
  spi_.Exchange(cs_, txdata_list, txdata_count, out_rxdata_list, rxdata_count, out_rxdata_actual);
  return ZX_OK;
}

void SpiChild::CanAssertCs(CanAssertCsCompleter::Sync& completer) {
  completer.Reply(!has_siblings_);
}

void SpiChild::AssertCs(AssertCsCompleter::Sync& completer) {
  if (has_siblings_) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED);
  } else {
    completer.Reply(spi_.LockBus(cs_));
  }
}

void SpiChild::DeassertCs(DeassertCsCompleter::Sync& completer) {
  if (has_siblings_) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED);
  } else {
    completer.Reply(spi_.UnlockBus(cs_));
  }
}

zx_status_t SpiChild::DdkOpen(zx_device_t** dev_out, uint32_t flags) {
  if (owner_.has_value()) {
    return ZX_ERR_ALREADY_BOUND;
  }
  owner_ = std::monostate{};
  return ZX_OK;
}

void SpiChild::Bind(async_dispatcher_t* dispatcher,
                    fidl::ServerEnd<fuchsia_hardware_spi::Device> server_end) {
  if (owner_.has_value()) {
    server_end.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }
  owner_ = Binding{
      .binding = fidl::BindServer(
          dispatcher, std::move(server_end), this,
          [](SpiChild* self, fidl::UnbindInfo, fidl::ServerEnd<fuchsia_hardware_spi::Device>) {
            auto& owner = self->owner_;
            if (!owner.has_value()) {
              zxlogf(ERROR, "SpiChild: inconsistent binding state: missing owner");
              return;
            }
            SpiChild::Binding* ptr = std::get_if<SpiChild::Binding>(&owner.value());
            if (ptr == nullptr) {
              zxlogf(ERROR, "SpiChild: inconsistent binding state: incorrect owner");
              return;
            }
            SpiChild::Binding& binding = *ptr;
            if (binding.unbind_txn.has_value()) {
              binding.unbind_txn.value().Reply();
            } else {
              self->spi_.ReleaseRegisteredVmos(self->cs_);
              owner = {};
            }
          }),
  };
}

zx_status_t SpiChild::DdkClose(uint32_t flags) {
  if (!owner_.has_value()) {
    return ZX_ERR_BAD_STATE;
  }
  if (!std::holds_alternative<std::monostate>(owner_.value())) {
    return ZX_ERR_BAD_STATE;
  }
  spi_.ReleaseRegisteredVmos(cs_);
  owner_ = {};
  return ZX_OK;
}

void SpiChild::DdkUnbind(ddk::UnbindTxn txn) {
  if (owner_.has_value()) {
    Binding* ptr = std::get_if<Binding>(&owner_.value());
    // DdkUnbind should not be called if there's a DdkOpen outstanding.
    ZX_ASSERT(ptr != nullptr);
    Binding& binding = *ptr;
    ZX_ASSERT(!binding.unbind_txn.has_value());
    binding.unbind_txn.emplace(std::move(txn));
    binding.binding.Unbind();
  } else {
    txn.Reply();
  }
}

void SpiChild::DdkRelease() { delete this; }

SpiFidlChild::SpiFidlChild(zx_device_t* parent, SpiChild* spi, async_dispatcher_t* dispatcher)
    : SpiFidlChildType(parent),
      spi_(spi),
      outgoing_(component::OutgoingDirectory::Create(dispatcher)) {
  zx::result status = outgoing_.AddProtocol<fuchsia_hardware_spi::Device>(
      [spi = spi_, dispatcher](fidl::ServerEnd<fuchsia_hardware_spi::Device> server_end) {
        spi->Bind(dispatcher, std::move(server_end));
      });
  ZX_ASSERT_MSG(status.is_ok(), "%s", status.status_string());
}

void SpiFidlChild::DdkRelease() { delete this; }

zx_status_t SpiFidlChild::ServeOutgoingDirectory(
    fidl::ServerEnd<fuchsia_io::Directory> server_end) {
  return outgoing_.Serve(std::move(server_end)).status_value();
}

zx_status_t SpiBanjoChild::DdkGetProtocol(uint32_t proto_id, void* out_protocol) {
  if (proto_id != ZX_PROTOCOL_SPI) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  spi_protocol_t* spi_proto = static_cast<spi_protocol_t*>(out_protocol);
  spi_proto->ops = &spi_protocol_ops_;
  spi_proto->ctx = this;
  return ZX_OK;
}

zx_status_t SpiBanjoChild::SpiTransmit(const uint8_t* txdata_list, size_t txdata_count) {
  return spi_->SpiTransmit(txdata_list, txdata_count);
}

zx_status_t SpiBanjoChild::SpiReceive(uint32_t size, uint8_t* out_rxdata_list, size_t rxdata_count,
                                      size_t* out_rxdata_actual) {
  return spi_->SpiReceive(size, out_rxdata_list, rxdata_count, out_rxdata_actual);
}

zx_status_t SpiBanjoChild::SpiExchange(const uint8_t* txdata_list, size_t txdata_count,
                                       uint8_t* out_rxdata_list, size_t rxdata_count,
                                       size_t* out_rxdata_actual) {
  return spi_->SpiExchange(txdata_list, txdata_count, out_rxdata_list, rxdata_count,
                           out_rxdata_actual);
}

}  // namespace spi
