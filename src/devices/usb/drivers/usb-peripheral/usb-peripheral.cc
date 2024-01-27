// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/drivers/usb-peripheral/usb-peripheral.h"

#include <assert.h>
#include <fuchsia/hardware/usb/dci/c/banjo.h>
#include <fuchsia/hardware/usb/function/c/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/fit/defer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/listnode.h>

#include <ddk/usb-peripheral-config.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <usb/cdc.h>
#include <usb/peripheral.h>
#include <usb/usb.h>

#include "src/devices/usb/drivers/usb-peripheral/usb-function.h"

namespace usb_peripheral {

zx_status_t UsbPeripheral::Create(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;
  auto device = fbl::make_unique_checked<UsbPeripheral>(&ac, parent);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  auto status = device->Init();
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = device.release();
  return ZX_OK;
}

zx_status_t UsbPeripheral::UsbDciCancelAll(uint8_t ep_address) {
  return dci_.CancelAll(ep_address);
}

void UsbPeripheral::RequestComplete(usb_request_t* req) {
  fbl::AutoLock l(&pending_requests_lock_);
  usb::BorrowedRequest<void> request(req, dci_.GetRequestSize());

  pending_requests_.erase(&request);
  l.release();
  request.Complete(request.request()->response.status, request.request()->response.actual);
  usb_monitor_.AddRecord(req);
}

void UsbPeripheral::UsbPeripheralRequestQueue(usb_request_t* usb_request,
                                              const usb_request_complete_callback_t* complete_cb) {
  if (shutting_down_) {
    usb_request_complete(usb_request, ZX_ERR_IO_NOT_PRESENT, 0, complete_cb);
    return;
  }
  fbl::AutoLock l(&pending_requests_lock_);
  usb::BorrowedRequest<void> request(usb_request, *complete_cb, dci_.GetRequestSize());
  [[maybe_unused]] usb_request_complete_callback_t completion;
  completion.ctx = this;
  completion.callback = [](void* ctx, usb_request_t* req) {
    reinterpret_cast<UsbPeripheral*>(ctx)->RequestComplete(req);
  };
  pending_requests_.push_back(&request);
  l.release();
  usb_monitor_.AddRecord(usb_request);
  dci_.RequestQueue(request.take(), &completion);
}

zx_status_t UsbPeripheral::Init() {
  // Parent must support DCI protocol. USB Mode Switch is optional.
  if (!dci_.is_valid()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto client = DdkConnectFidlProtocol<fuchsia_hardware_usb_dci::UsbDciService::Device>();
  if (client.is_error()) {
    zxlogf(ERROR, "Failed to connect fidl protocol");
    return client.error_value();
  }
  dci_new_.Bind(std::move(*client));

  // Starting USB mode is determined from device metadata.
  // We read initial value and store it in dev->usb_mode, but do not actually
  // enable it until after all of our functions have bound.
  size_t actual;
  auto status = device_get_metadata(parent(), DEVICE_METADATA_USB_MODE, &usb_mode_,
                                    sizeof(usb_mode_), &actual);
  if (status == ZX_ERR_NOT_FOUND) {
    fbl::AutoLock lock(&lock_);
    // Assume peripheral mode by default.
    usb_mode_ = USB_MODE_PERIPHERAL;
  } else if (status != ZX_OK || actual != sizeof(usb_mode_)) {
    zxlogf(ERROR, "%s: DEVICE_METADATA_USB_MODE failed", __func__);
    return status;
  }

  parent_request_size_ = usb::BorrowedRequest<void>::RequestSize(dci_.GetRequestSize());

  status = DdkAdd("usb-peripheral", DEVICE_ADD_NON_BINDABLE);
  if (status != ZX_OK) {
    return status;
  }

  dci_.SetInterface(this, &usb_dci_interface_protocol_ops_);
  size_t metasize = 0;
  status = device_get_metadata_size(parent(), DEVICE_METADATA_USB_CONFIG, &metasize);
  if (status != ZX_OK) {
    return ZX_OK;
  }
  constexpr auto alignment = []() {
    return alignof(UsbConfig) > __STDCPP_DEFAULT_NEW_ALIGNMENT__ ? alignof(UsbConfig)
                                                                 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }();
  fbl::AllocChecker ac;
  UsbConfig* config =
      reinterpret_cast<UsbConfig*>(new (std::align_val_t(alignment), &ac) unsigned char[metasize]);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto call = fit::defer(
      [=]() { operator delete[](reinterpret_cast<char*>(config), std::align_val_t(alignment)); });
  status = device_get_metadata(parent(), DEVICE_METADATA_USB_CONFIG, config, metasize, &metasize);
  if (status != ZX_OK) {
    return ZX_OK;
  }
  device_desc_.id_vendor = config->vid;
  device_desc_.id_product = config->pid;

  size_t max_str_len = strnlen(config->manufacturer, sizeof(config->manufacturer));
  status =
      AllocStringDesc(fbl::String(config->manufacturer, max_str_len), &device_desc_.i_manufacturer);
  if (status != ZX_OK) {
    return status;
  }

  max_str_len = strnlen(config->product, sizeof(config->product));
  status = AllocStringDesc(fbl::String(config->product, max_str_len), &device_desc_.i_product);
  if (status != ZX_OK) {
    return status;
  }
  uint8_t raw_mac_addr[6];
  status = device_get_metadata(parent(), DEVICE_METADATA_MAC_ADDRESS, &raw_mac_addr,
                               sizeof(raw_mac_addr), &actual);

  if (status != ZX_OK || actual != sizeof(raw_mac_addr)) {
    zxlogf(INFO,
           "Serial number/MAC address not found. Using generic (non-unique) serial number.\n");
  } else {
    char buffer[sizeof(raw_mac_addr) * 3];
    snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X", raw_mac_addr[0], raw_mac_addr[1],
             raw_mac_addr[2], raw_mac_addr[3], raw_mac_addr[4], raw_mac_addr[5]);
    memcpy(config->serial, buffer, sizeof(buffer));
  }
  max_str_len = strnlen(config->serial, sizeof(config->serial));
  actual = 0;
  char buffer[256];
  size_t metadata_size;
  status = device_get_metadata_size(parent(), DEVICE_METADATA_SERIAL_NUMBER, &metadata_size);
  if (status == ZX_OK) {
    if (metadata_size >= sizeof(buffer)) {
      return ZX_ERR_OUT_OF_RANGE;
    }
    status = device_get_metadata(parent(), DEVICE_METADATA_SERIAL_NUMBER, buffer, sizeof(buffer),
                                 &actual);
    if (actual >= sizeof(buffer)) {
      return ZX_ERR_OUT_OF_RANGE;
    }
    buffer[actual] = 0;
  }
  if ((status != ZX_OK) || (actual == 0)) {
    status =
        AllocStringDesc(fbl::String(config->serial, max_str_len), &device_desc_.i_serial_number);
  } else {
    status = AllocStringDesc(fbl::String(buffer, actual), &device_desc_.i_serial_number);
  }
  if (status != ZX_OK) {
    return status;
  }
  SetDefaultConfig(reinterpret_cast<FunctionDescriptor*>(config->functions),
                   (metasize - sizeof(UsbConfig)) / sizeof(FunctionDescriptor));

  usb_monitor_.Start();
  return ZX_OK;
}

zx_status_t UsbPeripheral::AllocStringDesc(fbl::String desc, uint8_t* out_index) {
  fbl::AutoLock lock(&lock_);

  if (strings_.size() >= MAX_STRINGS) {
    return ZX_ERR_NO_RESOURCES;
  }
  strings_.push_back(std::move(desc));

  // String indices are 1-based.
  *out_index = static_cast<uint8_t>(strings_.size());
  return ZX_OK;
}

zx_status_t UsbPeripheral::ValidateFunction(fbl::RefPtr<UsbFunction> function, void* descriptors,
                                            size_t length, uint8_t* out_num_interfaces) {
  auto* intf_desc = static_cast<usb_interface_descriptor_t*>(descriptors);
  if (intf_desc->b_descriptor_type == USB_DT_INTERFACE) {
    if (intf_desc->b_length != sizeof(usb_interface_descriptor_t)) {
      zxlogf(ERROR, "%s: interface descriptor is invalid", __func__);
      return ZX_ERR_INVALID_ARGS;
    }
  } else if (intf_desc->b_descriptor_type == USB_DT_INTERFACE_ASSOCIATION) {
    if (intf_desc->b_length != sizeof(usb_interface_assoc_descriptor_t)) {
      zxlogf(ERROR, "%s: interface association descriptor is invalid", __func__);
      return ZX_ERR_INVALID_ARGS;
    }
  } else {
    zxlogf(ERROR, "%s: first descriptor not an interface descriptor", __func__);
    return ZX_ERR_INVALID_ARGS;
  }

  auto* end =
      reinterpret_cast<const usb_descriptor_header_t*>(static_cast<uint8_t*>(descriptors) + length);
  auto* header = reinterpret_cast<const usb_descriptor_header_t*>(descriptors);

  while (header < end) {
    if (header->b_descriptor_type == USB_DT_INTERFACE) {
      auto* desc = reinterpret_cast<const usb_interface_descriptor_t*>(header);
      ZX_ASSERT(function->configuration() < configurations_.size());
      auto configuration = configurations_[function->configuration()];
      auto& interface_map = configuration->interface_map;
      if (desc->b_interface_number >= std::size(interface_map) ||
          interface_map[desc->b_interface_number] != function) {
        zxlogf(ERROR, "usb_func_set_interface: bInterfaceNumber %u", desc->b_interface_number);
        return ZX_ERR_INVALID_ARGS;
      }
      if (desc->b_alternate_setting == 0) {
        if (*out_num_interfaces == UINT8_MAX) {
          return ZX_ERR_INVALID_ARGS;
        }
        (*out_num_interfaces)++;
      }
    } else if (header->b_descriptor_type == USB_DT_ENDPOINT) {
      auto* desc = reinterpret_cast<const usb_endpoint_descriptor_t*>(header);
      auto index = EpAddressToIndex(desc->b_endpoint_address);
      if (index == 0 || index >= std::size(endpoint_map_) || endpoint_map_[index] != function) {
        zxlogf(ERROR, "usb_func_set_interface: bad endpoint address 0x%X",
               desc->b_endpoint_address);
        return ZX_ERR_INVALID_ARGS;
      }
    }

    if (header->b_length == 0) {
      zxlogf(ERROR, "usb_func_set_interface: zero length descriptor");
      return ZX_ERR_INVALID_ARGS;
    }
    header = reinterpret_cast<const usb_descriptor_header_t*>(
        reinterpret_cast<const uint8_t*>(header) + header->b_length);
  }

  return ZX_OK;
}

zx_status_t UsbPeripheral::FunctionRegistered() {
  fbl::AutoLock lock(&lock_);
  ZX_ASSERT(configurations_.size() > 0);
  if (configurations_[0]->config_desc.size() != 0) {
    return ZX_ERR_BAD_STATE;
  }

  // Check to see if we have all our functions registered.
  // If so, we can build our configuration descriptor and tell the DCI driver we are ready.
  fbl::Vector<size_t> lengths;
  for (auto& config : configurations_) {
    auto& functions = config->functions;
    size_t length = sizeof(usb_configuration_descriptor_t);

    for (size_t i = 0; i < functions.size(); i++) {
      auto* function = functions[i].get();
      size_t descriptors_length;
      if (function->GetDescriptors(&descriptors_length) != nullptr) {
        length += descriptors_length;
      } else {
        // Need to wait for more functions to register.
        return ZX_OK;
      }
    }
    lengths.push_back(length);
  }
  size_t config_idx = 0;
  // build our configuration descriptor
  for (auto& config : configurations_) {
    auto& functions = config->functions;
    size_t length = lengths[config_idx];
    fbl::AllocChecker ac;
    auto* config_desc_bytes = new (&ac) uint8_t[length];
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
    auto* config_desc = reinterpret_cast<usb_configuration_descriptor_t*>(config_desc_bytes);

    config_desc->b_length = sizeof(*config_desc);
    config_desc->b_descriptor_type = USB_DT_CONFIG;
    config_desc->w_total_length = htole16(length);
    config_desc->b_num_interfaces = 0;
    config_desc->b_configuration_value = static_cast<uint8_t>(1 + config_idx);
    config_desc->i_configuration = 0;
    // TODO(voydanoff) add a way to configure bm_attributes and bMaxPower
    config_desc->bm_attributes = USB_CONFIGURATION_SELF_POWERED | USB_CONFIGURATION_RESERVED_7;
    config_desc->b_max_power = 0;

    uint8_t* dest = reinterpret_cast<uint8_t*>(config_desc + 1);
    for (size_t i = 0; i < functions.size(); i++) {
      auto* function = functions[i].get();
      size_t descriptors_length;
      auto* descriptors = function->GetDescriptors(&descriptors_length);
      memcpy(dest, descriptors, descriptors_length);
      dest += descriptors_length;
      config_desc->b_num_interfaces =
          static_cast<uint8_t>(config_desc->b_num_interfaces + function->GetNumInterfaces());
    }
    config->config_desc.reset(config_desc_bytes, length);
    config_idx++;
  }

  zxlogf(DEBUG, "usb_device_function_registered functions_registered = true");
  functions_registered_ = true;
  if (listener_.is_valid()) {
    if (fidl::Status status = fidl::WireCall(listener_)->FunctionRegistered(); !status.ok()) {
      return status.status();
    }
  }
  return DeviceStateChanged();
}

void UsbPeripheral::FunctionCleared() {
  zxlogf(DEBUG, "%s", __func__);
  fbl::AutoLock lock(&lock_);

  if (num_functions_to_clear_ == 0 || !shutting_down_) {
    zxlogf(ERROR, "unexpected FunctionCleared event, num_functions: %lu is_shutting_down: %d",
           num_functions_to_clear_, shutting_down_);
    return;
  }
  num_functions_to_clear_--;
  if (num_functions_to_clear_ > 0) {
    // Still waiting for more functions to clear.
    return;
  }
  ClearFunctionsComplete();
}

zx_status_t UsbPeripheral::AllocInterface(fbl::RefPtr<UsbFunction> function,
                                          uint8_t* out_intf_num) {
  fbl::AutoLock lock(&lock_);
  ZX_ASSERT(function->configuration() < configurations_.size());
  auto configuration = configurations_[function->configuration()];
  auto& interface_map = configuration->interface_map;
  for (uint8_t i = 0; i < std::size(interface_map); i++) {
    if (interface_map[i] == nullptr) {
      interface_map[i] = function;
      *out_intf_num = i;
      return ZX_OK;
    }
  }

  return ZX_ERR_NO_RESOURCES;
}

zx_status_t UsbPeripheral::AllocEndpoint(fbl::RefPtr<UsbFunction> function, uint8_t direction,
                                         uint8_t* out_address) {
  uint8_t start, end;

  if (direction == USB_DIR_OUT) {
    start = OUT_EP_START;
    end = OUT_EP_END;
  } else if (direction == USB_DIR_IN) {
    start = IN_EP_START;
    end = IN_EP_END;
  } else {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&lock_);
  for (uint8_t index = start; index <= end; index++) {
    if (endpoint_map_[index] == nullptr) {
      endpoint_map_[index] = function;
      *out_address = EpIndexToAddress(index);
      return ZX_OK;
    }
  }

  return ZX_ERR_NO_RESOURCES;
}

zx_status_t UsbPeripheral::GetDescriptor(uint8_t request_type, uint16_t value, uint16_t index,
                                         void* buffer, size_t length, size_t* out_actual) {
  uint8_t type = request_type & USB_TYPE_MASK;

  if (type != USB_TYPE_STANDARD) {
    zxlogf(DEBUG, "%s unsupported value: %d index: %d", __func__, value, index);
    return ZX_ERR_NOT_SUPPORTED;
  }

  fbl::AutoLock lock(&lock_);

  auto desc_type = static_cast<uint8_t>(value >> 8);
  if (desc_type == USB_DT_DEVICE && index == 0) {
    if (device_desc_.b_length == 0) {
      zxlogf(ERROR, "%s: device descriptor not set", __func__);
      return ZX_ERR_INTERNAL;
    }
    if (length > sizeof(device_desc_))
      length = sizeof(device_desc_);
    memcpy(buffer, &device_desc_, length);
    *out_actual = length;
    return ZX_OK;
  } else if (desc_type == USB_DT_CONFIG && index == 0) {
    index = value & 0xff;
    if (index >= configurations_.size()) {
      return ZX_ERR_INVALID_ARGS;
    }
    auto& config_desc = configurations_[index]->config_desc;
    if (config_desc.size() == 0) {
      zxlogf(ERROR, "%s: configuration descriptor not set", __func__);
      return ZX_ERR_INTERNAL;
    }
    auto desc_length = config_desc.size();
    if (length > desc_length) {
      length = desc_length;
    }
    memcpy(buffer, config_desc.data(), length);
    *out_actual = length;
    return ZX_OK;
  } else if (value >> 8 == USB_DT_STRING) {
    uint8_t desc[255];
    auto* header = reinterpret_cast<usb_descriptor_header_t*>(desc);
    header->b_descriptor_type = USB_DT_STRING;

    auto string_index = static_cast<uint8_t>(value & 0xFF);
    if (string_index == 0) {
      // special case - return language list
      header->b_length = 4;
      desc[2] = 0x09;  // language ID
      desc[3] = 0x04;
    } else {
      // String indices are 1-based.
      string_index--;
      if (string_index >= strings_.size()) {
        return ZX_ERR_INVALID_ARGS;
      }
      const char* string = strings_[string_index].c_str();
      unsigned index = 2;

      // convert ASCII to UTF16
      if (string) {
        while (*string && index < sizeof(desc) - 2) {
          desc[index++] = *string++;
          desc[index++] = 0;
        }
      }
      header->b_length = static_cast<uint8_t>(index);
    }

    if (header->b_length < length)
      length = header->b_length;
    memcpy(buffer, desc, length);
    *out_actual = length;
    return ZX_OK;
  }

  zxlogf(DEBUG, "%s unsupported value: %d index: %d", __func__, value, index);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t UsbPeripheral::SetConfiguration(uint8_t configuration) {
  bool configured = configuration > 0;

  fbl::AutoLock lock(&lock_);
  for (auto& config : configurations_) {
    auto& functions = config->functions;
    for (auto& function : functions) {
      auto status =
          function->SetConfigured(function->configuration() == (configuration - 1), speed_);
      if (status != ZX_OK && configured) {
        return status;
      }
    }
  }

  configuration_ = configuration;

  return ZX_OK;
}

zx_status_t UsbPeripheral::SetInterface(uint8_t interface, uint8_t alt_setting) {
  auto configuration = configurations_[configuration_ - 1];
  if (interface >= std::size(configuration->interface_map)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  auto function = configuration->interface_map[interface];
  if (function != nullptr) {
    return function->SetInterface(interface, alt_setting);
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t UsbPeripheral::AddFunction(UsbConfiguration& config, FunctionDescriptor desc) {
  fbl::AutoLock lock(&lock_);

  if (functions_bound_) {
    return ZX_ERR_BAD_STATE;
  }

  fbl::AllocChecker ac;
  auto function =
      fbl::MakeRefCountedChecked<UsbFunction>(&ac, zxdev(), this, std::move(desc), config.index,
                                              fdf::Dispatcher().GetCurrent()->async_dispatcher());
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  config.functions.push_back(function);

  return ZX_OK;
}

zx_status_t UsbPeripheral::BindFunctions() {
  fbl::AutoLock lock(&lock_);
  if (functions_bound_) {
    zxlogf(ERROR, "%s: already bound!", __func__);
    return ZX_ERR_BAD_STATE;
  }

  if (device_desc_.b_length == 0) {
    zxlogf(ERROR, "%s: device descriptor not set", __func__);
    return ZX_ERR_BAD_STATE;
  }
  if (!configurations_.size()) {
    zxlogf(ERROR, "%s: no configurations found", __func__);
    return ZX_ERR_BAD_STATE;
  }
  if (configurations_[0]->functions.size() == 0) {
    zxlogf(ERROR, "%s: no functions to bind", __func__);
    return ZX_ERR_BAD_STATE;
  }

  zxlogf(DEBUG, "%s: functions_bound = true", __func__);
  functions_bound_ = true;
  return DeviceStateChanged();
}

void UsbPeripheral::ClearFunctions() {
  zxlogf(DEBUG, "%s", __func__);
  {
    fbl::AutoLock lock(&lock_);
    if (shutting_down_) {
      zxlogf(INFO, "%s: already in process of clearing the functions", __func__);
      return;
    }
    shutting_down_ = true;
    for (size_t i = 0; i < 256; i++) {
      dci_.CancelAll(static_cast<uint8_t>(i));
    }
    for (auto& configuration : configurations_) {
      auto& functions = configuration->functions;
      for (size_t i = 0; i < functions.size(); i++) {
        auto* function = functions[i].get();
        if (function->zxdev()) {
          num_functions_to_clear_++;
        }
      }
    }
    zxlogf(DEBUG, "%s: found %lu functions", __func__, num_functions_to_clear_);
    if (num_functions_to_clear_ == 0) {
      // Don't need to wait for anything to be removed, update our state now.
      ClearFunctionsComplete();
      return;
    }
  }

  // TODO(jocelyndang): we can call DdkRemove inside the lock above once DdkRemove becomes async.
  for (auto& configuration : configurations_) {
    auto& functions = configuration->functions;
    for (size_t i = 0; i < functions.size(); i++) {
      auto* function = functions[i].get();
      if (function->zxdev()) {
        function->DdkAsyncRemove();
      }
    }
  }
}

void UsbPeripheral::ClearFunctionsComplete() {
  zxlogf(DEBUG, "%s", __func__);

  shutting_down_ = false;
  configurations_.reset();
  functions_bound_ = false;
  functions_registered_ = false;
  function_devs_added_ = false;
  configurations_.reset();
  for (size_t i = 0; i < std::size(endpoint_map_); i++) {
    endpoint_map_[i].reset();
  }
  strings_.reset();

  DeviceStateChanged();

  if (listener_.is_valid()) {
    if (fidl::Status status = fidl::WireCall(listener_)->FunctionsCleared(); !status.ok()) {
      zxlogf(ERROR, "%s: %s", __func__, status.status_string());
    }
  }
}

zx_status_t UsbPeripheral::AddFunctionDevices() {
  zxlogf(DEBUG, "%s", __func__);
  if (function_devs_added_) {
    return ZX_OK;
  }
  int func_index = 0;
  for (auto& configuration : configurations_) {
    auto& functions = configuration->functions;
    for (unsigned i = 0; i < functions.size(); i++) {
      auto function = functions[i];
      char name[16];
      snprintf(name, sizeof(name), "function-%03u", func_index);

      auto& desc = function->GetFunctionDescriptor();

      zx_device_prop_t props[] = {
          {BIND_PROTOCOL, 0, ZX_PROTOCOL_USB_FUNCTION},
          {BIND_USB_CLASS, 0, desc.interface_class},
          {BIND_USB_SUBCLASS, 0, desc.interface_subclass},
          {BIND_USB_PROTOCOL, 0, desc.interface_protocol},
          {BIND_USB_VID, 0, device_desc_.id_vendor},
          {BIND_USB_PID, 0, device_desc_.id_product},
      };

      auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
      if (endpoints.is_error()) {
        return endpoints.status_value();
      }
      auto status = function->AddService(std::move(endpoints->server));
      if (status != ZX_OK) {
        zxlogf(ERROR, "Could not add service %d", status);
        return status;
      }

      std::array offers = {
          fuchsia_hardware_usb_function::UsbFunctionService::Name,
      };
      status = function->DdkAdd(ddk::DeviceAddArgs(name)
                                    .set_props(props)
                                    .forward_metadata(parent(), DEVICE_METADATA_MAC_ADDRESS)
                                    .forward_metadata(parent(), DEVICE_METADATA_SERIAL_NUMBER)
                                    .set_fidl_service_offers(offers)
                                    .set_outgoing_dir(endpoints->client.TakeChannel()));
      if (status != ZX_OK) {
        zxlogf(ERROR, "usb_dev_bind_functions add_device failed %d", status);
        return status;
      }
      // Hold a reference while devmgr has a pointer to the function.
      function->AddRef();
      func_index++;
    }
  }

  function_devs_added_ = true;
  return ZX_OK;
}

zx_status_t UsbPeripheral::DeviceStateChanged() {
  zxlogf(DEBUG, "%s usb_mode: %d dci_usb_mode: %d", __func__, usb_mode_, dci_usb_mode_);

  usb_mode_t new_dci_usb_mode = dci_usb_mode_;
  bool add_function_devs = (usb_mode_ == USB_MODE_PERIPHERAL && functions_bound_);
  zx_status_t status = ZX_OK;

  if (usb_mode_ == USB_MODE_PERIPHERAL) {
    if (functions_registered_) {
      // switch DCI to device mode
      new_dci_usb_mode = USB_MODE_PERIPHERAL;
    } else {
      new_dci_usb_mode = USB_MODE_NONE;
    }
  } else {
    new_dci_usb_mode = usb_mode_;
  }

  if (add_function_devs) {
    // publish child devices if necessary
    if (!function_devs_added_) {
      status = AddFunctionDevices();
      if (status != ZX_OK) {
        return status;
      }
    }
  }

  if (dci_usb_mode_ != new_dci_usb_mode) {
    zxlogf(DEBUG, "%s: set DCI mode %d", __func__, new_dci_usb_mode);
    dci_usb_mode_ = new_dci_usb_mode;
  }

  return status;
}

zx_status_t UsbPeripheral::UsbDciInterfaceControl(const usb_setup_t* setup,
                                                  const uint8_t* write_buffer, size_t write_size,
                                                  uint8_t* read_buffer, size_t read_size,
                                                  size_t* out_read_actual) {
  uint8_t request_type = setup->bm_request_type;
  uint8_t direction = request_type & USB_DIR_MASK;
  uint8_t request = setup->b_request;
  uint16_t value = le16toh(setup->w_value);
  uint16_t index = le16toh(setup->w_index);
  uint16_t length = le16toh(setup->w_length);

  if (direction == USB_DIR_IN && length > read_size) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  } else if (direction == USB_DIR_OUT && length > write_size) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }
  if ((write_size > 0 && write_buffer == NULL) || (read_size > 0 && read_buffer == NULL)) {
    return ZX_ERR_INVALID_ARGS;
  }

  zxlogf(DEBUG, "usb_dev_control type: 0x%02X req: %d value: %d index: %d length: %d", request_type,
         request, value, index, length);

  switch (request_type & USB_RECIP_MASK) {
    case USB_RECIP_DEVICE:
      // handle standard device requests
      if ((request_type & (USB_DIR_MASK | USB_TYPE_MASK)) == (USB_DIR_IN | USB_TYPE_STANDARD) &&
          request == USB_REQ_GET_DESCRIPTOR) {
        return GetDescriptor(request_type, value, index, read_buffer, length, out_read_actual);
      } else if (request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                 request == USB_REQ_SET_CONFIGURATION && length == 0) {
        return SetConfiguration(static_cast<uint8_t>(value));
      } else if (request_type == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                 request == USB_REQ_GET_CONFIGURATION && length > 0) {
        *static_cast<uint8_t*>(read_buffer) = configuration_;
        *out_read_actual = sizeof(uint8_t);
        return ZX_OK;
      } else if (request_type == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                 request == USB_REQ_GET_STATUS && length == 2) {
        static_cast<uint8_t*>(read_buffer)[1] = 1 << USB_DEVICE_SELF_POWERED;
        *out_read_actual = read_size;
      } else {
        // Delegate to one of the function drivers.
        // USB_RECIP_DEVICE should only be used when there is a single active interface.
        // But just to be conservative, try all the available interfaces.
        if (configuration_ == 0) {
          return ZX_ERR_BAD_STATE;
        }
        ZX_ASSERT(configuration_ <= configurations_.size());
        auto configuration = configurations_[configuration_ - 1];
        auto& interface_map = configuration->interface_map;
        for (size_t i = 0; i < std::size(interface_map); i++) {
          auto function = interface_map[i];
          if (function != nullptr) {
            auto status = function->Control(setup, write_buffer, write_size, read_buffer, read_size,
                                            out_read_actual);
            if (status == ZX_OK) {
              return ZX_OK;
            }
          }
        }
      }
      break;
    case USB_RECIP_INTERFACE: {
      if (request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) &&
          request == USB_REQ_SET_INTERFACE && length == 0) {
        return SetInterface(static_cast<uint8_t>(index), static_cast<uint8_t>(value));
      } else {
        auto configuration = configurations_[configuration_ - 1];
        auto& interface_map = configuration->interface_map;
        if (index >= std::size(interface_map)) {
          return ZX_ERR_OUT_OF_RANGE;
        }
        // delegate to the function driver for the interface
        auto function = interface_map[index];
        if (function != nullptr) {
          return function->Control(setup, write_buffer, write_size, read_buffer, read_size,
                                   out_read_actual);
        }
      }
      break;
    }
    case USB_RECIP_ENDPOINT: {
      // delegate to the function driver for the endpoint
      index = EpAddressToIndex(static_cast<uint8_t>(index));
      if (index == 0 || index >= USB_MAX_EPS) {
        return ZX_ERR_INVALID_ARGS;
      }
      if (index >= std::size(endpoint_map_)) {
        return ZX_ERR_OUT_OF_RANGE;
      }
      auto function = endpoint_map_[index];
      if (function != nullptr) {
        return function->Control(setup, write_buffer, write_size, read_buffer, read_size,
                                 out_read_actual);
      }
      break;
    }
    case USB_RECIP_OTHER:
      // TODO(voydanoff) - how to handle this?
    default:
      break;
  }

  return ZX_ERR_NOT_SUPPORTED;
}

void UsbPeripheral::UsbDciInterfaceSetConnected(bool connected) {
  bool was_connected = connected;
  {
    fbl::AutoLock lock(&lock_);
    std::swap(connected_, was_connected);
  }

  if (was_connected != connected) {
    if (!connected) {
      for (auto& configuration : configurations_) {
        auto& functions = configuration->functions;
        for (size_t i = 0; i < functions.size(); i++) {
          auto* function = functions[i].get();
          function->SetConfigured(false, USB_SPEED_UNDEFINED);
        }
      }
    }
  }
}

void UsbPeripheral::UsbDciInterfaceSetSpeed(usb_speed_t speed) { speed_ = speed; }

void UsbPeripheral::SetConfiguration(SetConfigurationRequestView request,
                                     SetConfigurationCompleter::Sync& completer) {
  zxlogf(DEBUG, "%s", __func__);
  ZX_ASSERT(!request->config_descriptors.empty());
  uint8_t index = 0;
  for (auto& func_descs : request->config_descriptors) {
    auto descriptor = fbl::MakeRefCounted<UsbConfiguration>();
    descriptor->index = index;
    configurations_.push_back(descriptor);
    {
      fbl::AutoLock lock(&lock_);
      if (shutting_down_) {
        zxlogf(ERROR, "%s: cannot set configuration while clearing functions", __func__);
        completer.ReplyError(ZX_ERR_BAD_STATE);
        return;
      }
    }

    if (func_descs.count() == 0) {
      completer.ReplyError(ZX_ERR_INVALID_ARGS);
      return;
    }

    zx_status_t status = SetDeviceDescriptor(std::move(request->device_desc));
    if (status != ZX_OK) {
      completer.ReplyError(status);
      return;
    }
    for (auto func_desc : func_descs) {
      AddFunction(*descriptor, func_desc);
    }
    index++;
  }
  zx_status_t status = BindFunctions();
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess();
}

zx_status_t UsbPeripheral::SetDeviceDescriptor(DeviceDescriptor desc) {
  if (desc.b_num_configurations == 0) {
    zxlogf(ERROR, "usb_device_ioctl: bNumConfigurations must be non-zero");
    return ZX_ERR_INVALID_ARGS;
  } else {
    device_desc_.b_length = sizeof(usb_device_descriptor_t);
    device_desc_.b_descriptor_type = USB_DT_DEVICE;
    device_desc_.bcd_usb = desc.bcd_usb;
    device_desc_.b_device_class = desc.b_device_class;
    device_desc_.b_device_sub_class = desc.b_device_sub_class;
    device_desc_.b_device_protocol = desc.b_device_protocol;
    device_desc_.b_max_packet_size0 = desc.b_max_packet_size0;
    device_desc_.id_vendor = desc.id_vendor;
    device_desc_.id_product = desc.id_product;
    device_desc_.bcd_device = desc.bcd_device;
    zx_status_t status =
        AllocStringDesc(fbl::String(desc.manufacturer.data(), desc.manufacturer.size()),
                        &device_desc_.i_manufacturer);
    if (status != ZX_OK) {
      return status;
    }
    status = AllocStringDesc(fbl::String(desc.product.data(), desc.product.size()),
                             &device_desc_.i_product);
    if (status != ZX_OK) {
      return status;
    }
    status = AllocStringDesc(fbl::String(desc.serial.data(), desc.serial.size()),
                             &device_desc_.i_serial_number);
    if (status != ZX_OK) {
      return status;
    }
    device_desc_.b_num_configurations = desc.b_num_configurations;
    return ZX_OK;
  }
}

void UsbPeripheral::ClearFunctions(ClearFunctionsCompleter::Sync& completer) {
  zxlogf(DEBUG, "%s", __func__);
  ClearFunctions();
  completer.Reply();
}

int UsbPeripheral::ListenerCleanupThread() {
  zx_signals_t observed = 0;
  listener_.channel().wait_one(ZX_CHANNEL_PEER_CLOSED | __ZX_OBJECT_HANDLE_CLOSED,
                               zx::time::infinite(), &observed);
  fbl::AutoLock l(&lock_);
  listener_.reset();
  return 0;
}

void UsbPeripheral::SetStateChangeListener(SetStateChangeListenerRequestView request,
                                           SetStateChangeListenerCompleter::Sync& completer) {
  // This code is wrapped in a loop
  // to prevent a race condition in the event that multiple
  // clients try to set the handle at once.
  while (1) {
    fbl::AutoLock lock(&lock_);
    if (listener_.is_valid() && thread_) {
      thrd_t thread = thread_;
      thread_ = 0;
      lock.release();
      int output;
      thrd_join(thread, &output);
      continue;
    }
    if (listener_.is_valid()) {
      completer.Close(ZX_ERR_BAD_STATE);
      return;
    }
    if (thread_) {
      int output;
      thrd_t thread = thread_;
      thread_ = 0;
      lock.release();
      // We now own the thread, but not the listener.
      thrd_join(thread, &output);
      // Go back and try to re-set the listener_.
      // another caller may have tried to do this while we were blocked on thrd_join.
      continue;
    }
    listener_ = std::move(request->listener);
    if (thrd_create(
            &thread_,
            [](void* arg) -> int {
              return reinterpret_cast<UsbPeripheral*>(arg)->ListenerCleanupThread();
            },
            reinterpret_cast<void*>(this)) != thrd_success) {
      listener_.reset();
      completer.Close(ZX_ERR_INTERNAL);
      return;
    }
    return;
  }
}

void UsbPeripheral::DdkUnbind(ddk::UnbindTxn txn) {
  zxlogf(DEBUG, "%s", __func__);
  ClearFunctions();
  txn.Reply();
  usb_monitor_.Stop();
}

void UsbPeripheral::DdkRelease() {
  zxlogf(DEBUG, "%s", __func__);
  {
    fbl::AutoLock l(&lock_);
    if (listener_) {
      listener_.reset();
    }
  }
  if (thread_) {
    int output;
    thrd_join(thread_, &output);
    thread_ = 0;
  }
  delete this;
}

zx_status_t UsbPeripheral::SetDefaultConfig(FunctionDescriptor* descriptors, size_t length) {
  auto descriptor = fbl::MakeRefCounted<UsbConfiguration>();
  configurations_.push_back(descriptor);
  device_desc_.b_length = sizeof(usb_device_descriptor_t),
  device_desc_.b_descriptor_type = USB_DT_DEVICE;
  device_desc_.bcd_usb = htole16(0x0200);
  device_desc_.b_device_class = 0;
  device_desc_.b_device_sub_class = 0;
  device_desc_.b_device_protocol = 0;
  device_desc_.b_max_packet_size0 = 64;
  device_desc_.bcd_device = htole16(0x0100);
  device_desc_.b_num_configurations = 1;

  zx_status_t status = ZX_OK;
  for (size_t i = 0; i < length; i++) {
    status = AddFunction(*descriptor, std::move(*(descriptors + i)));
    if (status != ZX_OK) {
      return status;
    }
  }

  if (status != ZX_OK)
    return status;

  return BindFunctions();
}

static constexpr zx_driver_ops_t ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = UsbPeripheral::Create;
  return ops;
}();

}  // namespace usb_peripheral

ZIRCON_DRIVER(usb_device, usb_peripheral::ops, "zircon", "0.1");
