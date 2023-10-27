// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dfv2-driver.h"

#include <fidl/fuchsia.hardware.gpio/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/metadata.h>
#include <lib/driver/component/cpp/driver_export.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/interrupt.h>

#include <string>

#include <bind/fuchsia/hardware/sdmmc/cpp/bind.h>

namespace aml_sdmmc {
namespace {

zx::result<aml_sdmmc_config_t> ParseMetadata(
    const fidl::VectorView<::fuchsia_driver_compat::wire::Metadata>& metadata_vector) {
  for (const auto& metadata : metadata_vector) {
    if (metadata.type == DEVICE_METADATA_PRIVATE) {
      size_t size;
      auto status = metadata.data.get_prop_content_size(&size);
      if (status != ZX_OK) {
        FDF_LOG(ERROR, "Failed to get_prop_content_size: %s", zx_status_get_string(status));
        return zx::error(status);
      }

      if (size != sizeof(aml_sdmmc_config_t)) {
        continue;
      }

      aml_sdmmc_config_t aml_sdmmc_config;
      status = metadata.data.read(&aml_sdmmc_config, 0, sizeof(aml_sdmmc_config_t));
      if (status != ZX_OK) {
        FDF_LOG(ERROR, "Failed to read metadata: %s", zx_status_get_string(status));
        return zx::error(status);
      }
      return zx::ok(aml_sdmmc_config);
    }
  }
  return zx::error(ZX_ERR_NOT_FOUND);
}

zx::result<pdev_device_info_t> FidlToBanjoDeviceInfo(
    const fuchsia_hardware_platform_device::wire::DeviceInfo& wire_dev_info) {
  pdev_device_info_t banjo_dev_info = {};
  if (wire_dev_info.has_vid()) {
    banjo_dev_info.vid = wire_dev_info.vid();
  }
  if (wire_dev_info.has_pid()) {
    banjo_dev_info.pid = wire_dev_info.pid();
  }
  if (wire_dev_info.has_did()) {
    banjo_dev_info.did = wire_dev_info.did();
  }
  if (wire_dev_info.has_mmio_count()) {
    banjo_dev_info.mmio_count = wire_dev_info.mmio_count();
  }
  if (wire_dev_info.has_irq_count()) {
    banjo_dev_info.irq_count = wire_dev_info.irq_count();
  }
  if (wire_dev_info.has_bti_count()) {
    banjo_dev_info.bti_count = wire_dev_info.bti_count();
  }
  if (wire_dev_info.has_smc_count()) {
    banjo_dev_info.smc_count = wire_dev_info.smc_count();
  }
  if (wire_dev_info.has_metadata_count()) {
    banjo_dev_info.metadata_count = wire_dev_info.metadata_count();
  }
  if (wire_dev_info.has_name()) {
    std::string name = std::string(wire_dev_info.name().get());
    if (name.size() > sizeof(banjo_dev_info.name)) {
      return zx::error(ZX_ERR_BUFFER_TOO_SMALL);
    }
    strncpy(banjo_dev_info.name, name.c_str(), sizeof(banjo_dev_info.name));
  }
  return zx::ok(banjo_dev_info);
}

}  // namespace

void DriverLogTrace(const char* message) { FDF_LOG(TRACE, "%s", message); }

void DriverLogInfo(const char* message) { FDF_LOG(INFO, "%s", message); }

void DriverLogWarning(const char* message) { FDF_LOG(WARNING, "%s", message); }

void DriverLogError(const char* message) { FDF_LOG(ERROR, "%s", message); }

zx::result<> Dfv2Driver::Start() {
  parent_.Bind(std::move(node()));

  aml_sdmmc_config_t config;
  {
    zx::result result = incoming()->Connect<fuchsia_driver_compat::Service::Device>();
    if (result.is_error() || !result->is_valid()) {
      FDF_LOG(ERROR, "Failed to connect to compat service: %s", result.status_string());
      return result.take_error();
    }
    auto parent_compat = fidl::WireSyncClient(std::move(result.value()));

    fidl::WireResult metadata = parent_compat->GetMetadata();
    if (!metadata.ok()) {
      FDF_LOG(ERROR, "Call to GetMetadata failed: %s", metadata.status_string());
      return zx::error(metadata.status());
    }
    if (metadata->is_error()) {
      FDF_LOG(ERROR, "Failed to GetMetadata: %s", zx_status_get_string(metadata->error_value()));
      return metadata->take_error();
    }

    zx::result parsed_metadata = ParseMetadata(metadata.value()->metadata);
    if (parsed_metadata.is_error()) {
      FDF_LOG(ERROR, "Failed to ParseMetadata: %s",
              zx_status_get_string(parsed_metadata.error_value()));
      return parsed_metadata.take_error();
    }
    config = parsed_metadata.value();

    fidl::WireResult parent_path = parent_compat->GetTopologicalPath();
    if (!parent_path.ok()) {
      FDF_LOG(ERROR, "Call to GetTopologicalPath failed: %s", parent_path.status_string());
      return zx::error(parent_path.status());
    }

    // TODO(fxbug.dev/135057): Automatically construct the topological path.
    std::string topological_path(parent_path.value().path.get());
    if (node_name().has_value()) {
      topological_path += "/" + node_name().value();
    }
    topological_path += "/" + std::string(name());

    compat_server_ = compat::DeviceServer(std::string(name()), ZX_PROTOCOL_SDMMC, topological_path,
                                          std::nullopt, banjo_server_.callback());
    zx_status_t status = compat_server_.Serve(dispatcher(), outgoing().get());
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to start compat server: %s", zx_status_get_string(status));
      return zx::error(status);
    }
  }

  {
    zx::result pdev = incoming()->Connect<fuchsia_hardware_platform_device::Service::Device>();
    if (pdev.is_error() || !pdev->is_valid()) {
      FDF_LOG(ERROR, "Failed to connect to platform device: %s", pdev.status_string());
      return pdev.take_error();
    }

    if (zx::result status = InitResources(std::move(pdev.value()), config); status.is_error()) {
      return status.take_error();
    }
  }

  exposed_inspector_.emplace(
      inspect::ComponentInspector(outgoing()->component(), dispatcher(), inspector()));

  {
    fuchsia_hardware_sdmmc::SdmmcService::InstanceHandler handler({
        .sdmmc = fit::bind_member<&Dfv2Driver::Serve>(this),
    });
    auto result = outgoing()->AddService<fuchsia_hardware_sdmmc::SdmmcService>(std::move(handler));
    if (result.is_error()) {
      FDF_LOG(ERROR, "Failed to add service: %s", result.status_string());
      return result.take_error();
    }
  }

  zx::result controller_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
  if (!controller_endpoints.is_ok()) {
    FDF_LOG(ERROR, "Failed to create controller endpoints: %s",
            controller_endpoints.status_string());
    return controller_endpoints.take_error();
  }

  controller_.Bind(std::move(controller_endpoints->client));

  fidl::Arena arena;

  fidl::VectorView<fuchsia_driver_framework::wire::NodeProperty> properties(arena, 2);
  properties[0] = fdf::MakeProperty(arena, BIND_PROTOCOL, ZX_PROTOCOL_SDMMC);
  properties[1] = fdf::MakeProperty(arena, bind_fuchsia_hardware_sdmmc::SDMMCSERVICE,
                                    bind_fuchsia_hardware_sdmmc::SDMMCSERVICE_DRIVERTRANSPORT);

  const std::vector<fuchsia_component_decl::wire::Offer> compat_offers =
      compat_server_.CreateOffers(arena);
  fidl::VectorView<fuchsia_component_decl::wire::Offer> offers(arena, compat_offers.size() + 1);
  for (size_t i = 0; i < compat_offers.size(); i++) {
    offers[i] = compat_offers[i];
  }
  const auto sdmmc_service = fuchsia_component_decl::wire::OfferService::Builder(arena)
                                 .source_name(arena, fuchsia_hardware_sdmmc::SdmmcService::Name)
                                 .target_name(arena, fuchsia_hardware_sdmmc::SdmmcService::Name)
                                 .Build();
  offers[compat_offers.size()] =
      fuchsia_component_decl::wire::Offer::WithService(arena, sdmmc_service);

  fidl::VectorView<fuchsia_driver_framework::wire::NodeSymbol> symbols(arena, 1);
  symbols[0] = fuchsia_driver_framework::wire::NodeSymbol::Builder(arena)
                   .name(arena, banjo_server_.symbol().name().value())
                   .address(banjo_server_.symbol().address().value())
                   .Build();

  const auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(arena)
                        .name(arena, name())
                        .offers(offers)
                        .properties(properties)
                        .symbols(symbols)
                        .Build();

  auto result = parent_->AddChild(args, std::move(controller_endpoints->server), {});
  if (!result.ok()) {
    FDF_LOG(ERROR, "Failed to add child: %s", result.status_string());
    return zx::error(result.status());
  }

  FDF_LOG(INFO, "Completed start hook");

  return zx::ok();
}

zx::result<> Dfv2Driver::InitResources(
    fidl::ClientEnd<fuchsia_hardware_platform_device::Device> pdev_client,
    aml_sdmmc_config_t config) {
  fidl::WireSyncClient pdev(std::move(pdev_client));

  std::optional<fdf::MmioBuffer> mmio;
  {
    const auto result = pdev->GetMmio(0);
    if (!result.ok()) {
      FDF_LOG(ERROR, "Call to get MMIO failed: %s", result.status_string());
      return zx::error(result.status());
    }
    if (!result->is_ok()) {
      FDF_LOG(ERROR, "Failed to get MMIO: %s", zx_status_get_string(result->error_value()));
      return zx::error(result->error_value());
    }

    const auto& mmio_params = result->value();
    if (!mmio_params->has_offset() || !mmio_params->has_size() || !mmio_params->has_vmo()) {
      FDF_LOG(ERROR, "Platform device provided invalid MMIO");
      return zx::error(ZX_ERR_BAD_STATE);
    };

    auto mmio_result =
        fdf::MmioBuffer::Create(mmio_params->offset(), mmio_params->size(),
                                std::move(mmio_params->vmo()), ZX_CACHE_POLICY_UNCACHED_DEVICE);
    if (mmio_result.is_error()) {
      FDF_LOG(ERROR, "Failed to map MMIO: %s", mmio_result.status_string());
      return mmio_result.take_error();
    }
    mmio = std::move(mmio_result.value());
  }

  zx::interrupt irq;
  {
    const auto result = pdev->GetInterrupt(0, 0);
    if (!result.ok()) {
      FDF_LOG(ERROR, "Call to get interrupt failed: %s", result.status_string());
      return zx::error(result.status());
    }
    if (!result->is_ok()) {
      FDF_LOG(ERROR, "Failed to get interrupt: %s", zx_status_get_string(result->error_value()));
      return zx::error(result->error_value());
    }
    irq = std::move(result->value()->irq);
  }

  zx::bti bti;
  {
    const auto result = pdev->GetBti(0);
    if (!result.ok()) {
      FDF_LOG(ERROR, "Call to get BTI failed: %s", result.status_string());
      return zx::error(result.status());
    }
    if (!result->is_ok()) {
      FDF_LOG(ERROR, "Failed to get BTI: %s", zx_status_get_string(result->error_value()));
      return zx::error(result->error_value());
    }
    bti = std::move(result->value()->bti);
  }

  // Optional protocol.
  fidl::ClientEnd<fuchsia_hardware_gpio::Gpio> reset_gpio;
  const char* kGpioWifiFragmentNames[2] = {"gpio-wifi-power-on", "gpio"};
  for (const char* fragment : kGpioWifiFragmentNames) {
    zx::result result = incoming()->Connect<fuchsia_hardware_gpio::Service::Device>(fragment);
    if (!result.is_error() && result->is_valid()) {
      auto gpio = fidl::WireSyncClient<fuchsia_hardware_gpio::Gpio>(std::move(result.value()));
      if (gpio->GetName().ok()) {
        reset_gpio = gpio.TakeClientEnd();
        break;
      }
    }
  }

  aml_sdmmc::IoBuffer descs_buffer;
  zx_status_t status = descs_buffer.Init(bti.get(), kMaxDmaDescriptors * sizeof(aml_sdmmc_desc_t),
                                         IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to allocate dma descriptors");
    return zx::error(status);
  }

  SetUpResources(std::move(bti), std::move(*mmio), config, std::move(irq), std::move(reset_gpio),
                 std::move(descs_buffer));

  {
    const auto result = pdev->GetDeviceInfo();
    if (!result.ok()) {
      FDF_LOG(ERROR, "Call to get device info failed: %s", result.status_string());
      return zx::error(result.status());
    }
    if (!result->is_ok()) {
      FDF_LOG(ERROR, "Failed to get device info: %s", zx_status_get_string(result->error_value()));
      return zx::error(result->error_value());
    }

    zx::result dev_info = FidlToBanjoDeviceInfo(*result->value());
    if (dev_info.is_error()) {
      return dev_info.take_error();
    }

    status = Init(dev_info.value());
    if (status != ZX_OK) {
      return zx::error(status);
    }
  }

  return zx::success();
}

void Dfv2Driver::Serve(fdf::ServerEnd<fuchsia_hardware_sdmmc::Sdmmc> request) {
  fdf::BindServer(driver_dispatcher()->get(), std::move(request), this);
}

}  // namespace aml_sdmmc

FUCHSIA_DRIVER_EXPORT(aml_sdmmc::Dfv2Driver);
