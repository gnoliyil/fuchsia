// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "radar-reader-proxy.h"

namespace radar {

RadarReaderProxy::~RadarReaderProxy() {
  // Close out any outstanding requests before destruction to avoid triggerig an assert.
  for (auto& [server, completer] : connect_requests_) {
    completer.Close(ZX_ERR_PEER_CLOSED);
  }
}

void RadarReaderProxy::Connect(ConnectRequest& request, ConnectCompleter::Sync& completer) {
  if (radar_client_) {
    // Our driver client is bound, so we must have already set these fields.
    ZX_DEBUG_ASSERT(burst_size_);

    instances_.emplace_back(std::make_unique<ReaderInstance>(this, std::move(request.server())));
    completer.Reply(fit::ok());
  } else {
    connect_requests_.emplace_back(std::move(request.server()), completer.ToAsync());
  }
}

void RadarReaderProxy::DeviceAdded(fidl::UnownedClientEnd<fuchsia_io::Directory> dir,
                                   const std::string& filename) {
  if (!radar_client_) {
    connector_->ConnectToRadarDevice(
        dir, filename, [&](auto client_end) { return ValidateDevice(std::move(client_end)); });
  }
}

void RadarReaderProxy::on_fidl_error(const fidl::UnbindInfo info) {
  FX_PLOGS(ERROR, info.status()) << "Connection to radar device closed, attempting to reconnect";
  HandleFatalError(info.status());
}

void RadarReaderProxy::OnBurst(
    fidl::Event<fuchsia_hardware_radar::RadarBurstReader::OnBurst>& event) {
  ZX_DEBUG_ASSERT(burst_size_);

  const auto& response = event.result().response();
  if (!response || response->burst().vmo_id() >= vmo_pool_.size()) {
    for (auto& instance : instances_) {
      instance->SendError(
          event.result().err().value_or(fuchsia_hardware_radar::StatusCode::kVmoNotFound));
    }
    return;
  }

  const uint32_t vmo_id = response->burst().vmo_id();
  const auto* burst_data = reinterpret_cast<uint8_t*>(vmo_pool_[vmo_id].mapped_vmo.start());
  for (auto& instance : instances_) {
    instance->SendBurst({burst_data, *burst_size_}, zx::time(response->burst().timestamp()));
  }

  if (radar_client_) {
    if (auto result = radar_client_->UnlockVmo(vmo_id); result.is_error()) {
      HandleFatalError(result.error_value().status());
    }
  }
}

bool RadarReaderProxy::ValidateDevice(
    fidl::ClientEnd<fuchsia_hardware_radar::RadarBurstReaderProvider> client_end) {
  fidl::SyncClient provider(std::move(client_end));

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_radar::RadarBurstReader>();
  if (endpoints.is_error() || provider->Connect(std::move(endpoints->server)).is_error()) {
    return false;
  }

  fidl::SyncClient reader(std::move(endpoints->client));

  // If this is our first connection, save the burst size. If this is not our first connection, make
  // sure that the burst size reported by this device matches what we have saved.
  const auto burst_size = reader->GetBurstSize();
  if (burst_size.is_error() || (burst_size_ && *burst_size_ != burst_size->burst_size())) {
    return false;
  }
  if (!burst_size_) {
    burst_size_ = burst_size->burst_size();
  }

  if (!vmo_pool_.empty()) {
    // Register all of the VMOs that are currently in the pool.
    std::vector<uint32_t> vmo_ids(vmo_pool_.size());
    std::vector<zx::vmo> vmos(vmo_pool_.size());

    for (uint32_t i = 0; i < vmo_pool_.size(); i++) {
      vmo_ids[i] = i;
      zx_status_t status = vmo_pool_[i].vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmos[i]);
      if (status != ZX_OK) {
        FX_PLOGS(ERROR, status) << "Failed to duplicate VMO";
        return false;
      }
    }

    const auto result = reader->RegisterVmos({{std::move(vmo_ids), std::move(vmos)}});
    if (result.is_error() && result.error_value().is_framework_error()) {
      FX_PLOGS(ERROR, result.error_value().framework_error().status())
          << "Failed to send register VMOs request";
      return false;
    }
    if (result.is_error() && result.error_value().is_domain_error()) {
      FX_LOGS(ERROR) << "Failed to register VMOs";
      return false;
    }
  }

  radar_client_ = fidl::Client(reader.TakeClientEnd(), dispatcher_, this);

  // Now that we are connected to a device, complete any connect requests that are outstanding.
  for (auto& [server, completer] : connect_requests_) {
    instances_.emplace_back(std::make_unique<ReaderInstance>(this, std::move(server)));
    completer.Reply(fit::ok());
  }
  connect_requests_.clear();

  return true;
}

void RadarReaderProxy::HandleFatalError(const zx_status_t status) {
  // Tear down the client so that we stop receiving bursts. We may be handling a FIDL error here, in
  // which case the client has already been unbound.
  radar_client_ = {};

  // Close all existing client connections and let them reconnect if needed.
  for (auto& instance : instances_) {
    instance->Close(status);
  }

  // Check for available devices now, just in case one was added before the connection closed. If
  // not, the DeviceWatcher will signal to connect when a new device becomes available.
  connector_->ConnectToFirstRadarDevice(
      [&](auto client_end) { return ValidateDevice(std::move(client_end)); });
}

void RadarReaderProxy::UpdateVmoCount(const size_t count,
                                      ReaderInstance::RegisterVmosCompleter::Async completer) {
  ZX_DEBUG_ASSERT(burst_size_);

  if (count <= vmo_pool_.size()) {
    completer.Reply(fit::success());
    return;
  }

  const size_t vmos_to_register = count - vmo_pool_.size();

  std::vector<zx::vmo> vmos(vmos_to_register);
  std::vector<uint32_t> vmo_ids(vmos_to_register);

  for (size_t i = 0; i < vmos.size(); i++) {
    vmo_ids[i] = static_cast<uint32_t>(vmo_pool_.size());

    MappedVmo vmo;

    // The radar driver writes to these VMOs, so we only need read access.
    zx_status_t status =
        vmo.mapped_vmo.CreateAndMap(*burst_size_, ZX_VM_PERM_READ, nullptr, &vmo.vmo);
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to create and map VMO";
      completer.Close(status);
      HandleFatalError(status);
      return;
    }

    if ((status = vmo.vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmos[i])) != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to duplicate VMO";
      completer.Close(status);
      HandleFatalError(status);
      return;
    }

    vmo_pool_.push_back(std::move(vmo));
  }

  radar_client_->RegisterVmos({std::move(vmo_ids), std::move(vmos)})
      .Then([&, comp = std::move(completer)](const auto& result) mutable {
        if (result.is_error()) {
          zx_status_t status = ZX_ERR_BAD_STATE;
          if (result.error_value().is_framework_error()) {
            status = result.error_value().is_framework_error();
            FX_PLOGS(ERROR, status) << "Failed to send register VMOs request";
          } else {
            FX_LOGS(ERROR) << "Failed to register VMOs";
          }
          comp.Close(status);
          HandleFatalError(status);
        } else {
          comp.Reply(fit::success());
        }
      });
}

void RadarReaderProxy::StartBursts() {
  if (auto result = radar_client_->StartBursts(); result.is_error()) {
    FX_PLOGS(ERROR, result.error_value().status()) << "Failed to send start bursts request";
    HandleFatalError(result.error_value().status());
  }
}

void RadarReaderProxy::StopBursts(
    std::optional<ReaderInstance::StopBurstsCompleter::Async> completer) {
  for (const auto& instance : instances_) {
    if (instance->bursts_started()) {
      if (completer) {
        completer->Reply();
      }
      return;
    }
  }

  radar_client_->StopBursts().Then([&, comp = std::move(completer)](const auto& result) mutable {
    if (result.is_error()) {
      FX_PLOGS(ERROR, result.error_value().status()) << "Failed to send stop bursts request";
      if (comp) {
        comp->Close(result.error_value().status());
      }
      HandleFatalError(result.error_value().status());
    } else if (comp) {
      comp->Reply();
    }
  });
}

void RadarReaderProxy::InstanceUnbound(ReaderInstance* const instance) {
  // The instance was unbound, remove it from our list and let it be deleted.
  for (auto it = instances_.begin(); it != instances_.end(); it++) {
    if (it->get() == instance) {
      instances_.erase(it);

      // Stop bursts if this was the last client that wanted to receive them. We may be handling a
      // fatal error in which case the client has already been unbound.
      if (radar_client_) {
        StopBursts({});
      }
      return;
    }
  }
}

RadarReaderProxy::ReaderInstance::ReaderInstance(
    RadarReaderProxy* const parent,
    fidl::ServerEnd<fuchsia_hardware_radar::RadarBurstReader> server_end)
    : parent_(parent),
      server_(fidl::BindServer(parent->dispatcher_, std::move(server_end), this,
                               [](ReaderInstance* instance, auto, auto) {
                                 instance->parent_->InstanceUnbound(instance);
                               })),
      manager_(parent->burst_size_.value_or(0)) {}

void RadarReaderProxy::ReaderInstance::GetBurstSize(GetBurstSizeCompleter::Sync& completer) {
  ZX_DEBUG_ASSERT(parent_->burst_size_);
  completer.Reply(*parent_->burst_size_);
}

void RadarReaderProxy::ReaderInstance::RegisterVmos(RegisterVmosRequest& request,
                                                    RegisterVmosCompleter::Sync& completer) {
  const fit::result result = manager_.RegisterVmos(request.vmo_ids(), std::move(request.vmos()));
  if (result.is_ok()) {
    parent_->UpdateVmoCount(vmo_count_ += request.vmo_ids().size(), completer.ToAsync());
  } else {
    completer.Reply(result);
  }
}

void RadarReaderProxy::ReaderInstance::UnregisterVmos(UnregisterVmosRequest& request,
                                                      UnregisterVmosCompleter::Sync& completer) {
  completer.Reply(manager_.UnregisterVmos(request.vmo_ids()));
}

void RadarReaderProxy::ReaderInstance::StartBursts(StartBurstsCompleter::Sync& completer) {
  bursts_started_ = true;
  sent_error_ = false;
  parent_->StartBursts();
}

void RadarReaderProxy::ReaderInstance::StopBursts(StopBurstsCompleter::Sync& completer) {
  bursts_started_ = false;
  parent_->StopBursts(completer.ToAsync());
}

void RadarReaderProxy::ReaderInstance::UnlockVmo(UnlockVmoRequest& request,
                                                 UnlockVmoCompleter::Sync& completer) {
  manager_.UnlockVmo(request.vmo_id());
}

void RadarReaderProxy::ReaderInstance::SendBurst(const cpp20::span<const uint8_t> burst,
                                                 const zx::time timestamp) {
  if (!bursts_started_) {
    return;
  }

  fit::result vmo_id = manager_.WriteUnlockedVmoAndGetId(burst);
  if (vmo_id.is_error()) {
    SendError(vmo_id.error_value());
    return;
  }

  sent_error_ = false;

  const auto result = fuchsia_hardware_radar::RadarBurstReaderOnBurstResult::WithResponse(
      {{*vmo_id, timestamp.get()}});
  if (auto event_result = fidl::SendEvent(server_)->OnBurst(result); event_result.is_error()) {
    FX_PLOGS(ERROR, event_result.error_value().status()) << "Failed to send burst";
  }
}

void RadarReaderProxy::ReaderInstance::SendError(const fuchsia_hardware_radar::StatusCode error) {
  if (!bursts_started_ || sent_error_) {
    return;
  }

  sent_error_ = true;

  const auto result = fuchsia_hardware_radar::RadarBurstReaderOnBurstResult::WithErr(error);
  if (auto event_result = fidl::SendEvent(server_)->OnBurst(result); event_result.is_error()) {
    FX_PLOGS(ERROR, event_result.error_value().status()) << "Failed to send burst error";
  }
}

}  // namespace radar
