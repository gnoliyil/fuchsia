// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "i2c-child.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/sync/completion.h>

#include <fbl/alloc_checker.h>

#include "src/devices/i2c/drivers/i2c/i2c_bind.h"

namespace i2c {

zx_status_t I2cChild::CreateAndAddDevice(
    zx_device_t* parent, const fuchsia_hardware_i2c_businfo::wire::I2CChannel& channel,
    const fbl::RefPtr<I2cBus>& bus, async_dispatcher_t* dispatcher) {
  const uint32_t bus_id = channel.has_bus_id() ? channel.bus_id() : 0;
  const uint16_t address = channel.has_address() ? channel.address() : 0;
  const uint32_t i2c_class = channel.has_i2c_class() ? channel.i2c_class() : 0;
  const uint32_t vid = channel.has_vid() ? channel.vid() : 0;
  const uint32_t pid = channel.has_pid() ? channel.pid() : 0;
  const uint32_t did = channel.has_did() ? channel.did() : 0;
  const std::string friendly_name = channel.has_name() ? std::string(channel.name().get()) : "";

  fuchsia_hardware_i2c_businfo::wire::I2CChannel local_channel(channel);
  fit::result metadata = fidl::Persist(local_channel);
  if (!metadata.is_ok()) {
    zxlogf(ERROR, "Failed to fidl-encode channel: %s",
           metadata.error_value().FormatDescription().data());
    return metadata.error_value().status();
  }
  cpp20::span<const uint8_t> metadata_span(metadata.value());

  const zx_device_prop_t id_props[] = {
      {BIND_I2C_BUS_ID, 0, bus_id},    {BIND_I2C_ADDRESS, 0, address},
      {BIND_PLATFORM_DEV_VID, 0, vid}, {BIND_PLATFORM_DEV_PID, 0, pid},
      {BIND_PLATFORM_DEV_DID, 0, did}, {BIND_I2C_CLASS, 0, i2c_class},
  };

  const zx_device_prop_t no_id_props[] = {
      {BIND_I2C_BUS_ID, 0, bus_id},
      {BIND_I2C_ADDRESS, 0, address},
      {BIND_I2C_CLASS, 0, i2c_class},
  };

  cpp20::span<const zx_device_prop_t> props = no_id_props;
  if (vid || pid || did) {
    props = id_props;
  }

  fbl::AllocChecker ac;
  std::unique_ptr<I2cChild> dev(new (&ac)
                                    I2cChild(parent, bus, address, dispatcher, friendly_name));
  if (!ac.check()) {
    zxlogf(ERROR, "Failed to create child device: %s", zx_status_get_string(ZX_ERR_NO_MEMORY));
    return ZX_ERR_NO_MEMORY;
  }

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }

  auto result = dev->outgoing_dir_.AddService<fidl_i2c::Service>(fidl_i2c::Service::InstanceHandler(
      {.device = [dev = dev.get()](fidl::ServerEnd<fidl_i2c::Device> request) mutable {
        dev->Bind(std::move(request));
      }}));

  if (result.is_error()) {
    zxlogf(ERROR, "Failed to AddService: %s", result.status_string());
    return result.error_value();
  }

  result = dev->outgoing_dir_.Serve(std::move(endpoints->server));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to service the outgoing directory: %s", result.status_string());
    return result.error_value();
  }

  std::array service_offers = {
      fidl_i2c::Service::Name,
  };

  char name[32];
  snprintf(name, sizeof(name), "i2c-%u-%u", bus_id, address);
  auto status = dev->DdkAdd(ddk::DeviceAddArgs(name)
                                // Add the Banjo protocol ID to create aliases under /dev/class/i2c.
                                .set_proto_id(ZX_PROTOCOL_I2C)
                                .set_flags(DEVICE_ADD_MUST_ISOLATE)
                                .set_props(props)
                                .set_fidl_service_offers(service_offers)
                                .set_outgoing_dir(endpoints->client.TakeChannel()));

  if (status != ZX_OK) {
    zxlogf(ERROR, "DdkAdd failed: %s", zx_status_get_string(status));
    return status;
  }

  status = dev->DdkAddMetadata(DEVICE_METADATA_I2C_DEVICE, metadata_span.data(),
                               metadata_span.size_bytes());
  if (status != ZX_OK) {
    zxlogf(ERROR, "DdkAddMetadata failed: %s", zx_status_get_string(status));
  }

  [[maybe_unused]] auto ptr = dev.release();
  return status;
}

void I2cChild::Transfer(fidl::WireServer<fidl_i2c::Device>::TransferRequestView request,
                        fidl::WireServer<fidl_i2c::Device>::TransferCompleter::Sync& completer) {
  if (request->transactions.count() < 1) {
    completer.ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  auto op_list = std::make_unique<I2cBus::TransactOp[]>(request->transactions.count());
  for (size_t i = 0; i < request->transactions.count(); ++i) {
    if (!request->transactions[i].has_data_transfer()) {
      completer.ReplyError(ZX_ERR_INVALID_ARGS);
      return;
    }

    const auto& transfer = request->transactions[i].data_transfer();
    const bool stop = request->transactions[i].has_stop() && request->transactions[i].stop();

    if (transfer.is_write_data()) {
      if (transfer.write_data().count() <= 0) {
        completer.ReplyError(ZX_ERR_INVALID_ARGS);
        return;
      }
      op_list[i].data_buffer = transfer.write_data().data();
      op_list[i].data_size = transfer.write_data().count();
      op_list[i].is_read = false;
      op_list[i].stop = stop;
    } else {
      if (transfer.read_size() <= 0) {
        completer.ReplyError(ZX_ERR_INVALID_ARGS);
        return;
      }
      op_list[i].data_buffer = nullptr;  // unused.
      op_list[i].data_size = transfer.read_size();
      op_list[i].is_read = true;
      op_list[i].stop = stop;
    }
  }
  op_list[request->transactions.count() - 1].stop = true;

  struct Ctx {
    sync_completion_t done = {};
    fidl::WireServer<fidl_i2c::Device>::TransferCompleter::Sync* completer;
  } ctx;
  ctx.completer = &completer;
  auto callback = [](void* ctx, zx_status_t status, const I2cBus::TransactOp* op_list,
                     size_t op_count) {
    auto ctx2 = static_cast<Ctx*>(ctx);
    if (status == ZX_OK) {
      auto reads = std::make_unique<fidl::VectorView<uint8_t>[]>(op_count);
      for (size_t i = 0; i < op_count; ++i) {
        reads[i] = fidl::VectorView<uint8_t>::FromExternal(
            const_cast<uint8_t*>(op_list[i].data_buffer), op_list[i].data_size);
      }
      auto all_reads =
          fidl::VectorView<fidl::VectorView<uint8_t>>::FromExternal(reads.get(), op_count);
      ctx2->completer->ReplySuccess(all_reads);
    } else {
      ctx2->completer->ReplyError(status);
    }
    sync_completion_signal(&ctx2->done);
  };
  bus_->Transact(address_, op_list.get(), request->transactions.count(), callback, &ctx);
  sync_completion_wait(&ctx.done, zx::duration::infinite().get());
}

void I2cChild::GetName(fidl::WireServer<fidl_i2c::Device>::GetNameCompleter::Sync& completer) {
  if (name_.empty()) {
    completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
    return;
  }

  completer.ReplySuccess(::fidl::StringView::FromExternal(name_));
}

}  // namespace i2c
