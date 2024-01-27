// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/drivers/usb-bus/usb-bus.h"

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/sync/completion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/alloc_checker.h>

#include "src/devices/usb/drivers/usb-bus/usb-device.h"

namespace usb_bus {

zx_status_t UsbBus::Create(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;
  auto bus = fbl::make_unique_checked<UsbBus>(&ac, parent);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  auto status = bus->Init();
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = bus.release();
  return ZX_OK;
}

zx_status_t UsbBus::Init() {
  dispatcher_ = fdf::Dispatcher().GetCurrent()->async_dispatcher();

  // Parent must support HCI protocol.
  if (!hci_.is_valid()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto max_device_count = hci_.GetMaxDeviceCount();
  fbl::AllocChecker ac;
  devices_.reset(new (&ac) fbl::RefPtr<UsbDevice>[max_device_count], max_device_count);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  auto status = DdkAdd("usb-bus", DEVICE_ADD_NON_BINDABLE);
  if (status != ZX_OK) {
    return status;
  }

  hci_.SetBusInterface(this, &usb_bus_interface_protocol_ops_);

  return ZX_OK;
}

zx_status_t UsbBus::UsbBusInterfaceAddDevice(uint32_t device_id, uint32_t hub_id,
                                             usb_speed_t speed) {
  if (device_id >= devices_.size()) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (devices_[device_id] != nullptr) {
    return ZX_ERR_BAD_STATE;
  }

  auto client = DdkConnectFidlProtocol<fuchsia_hardware_usb_hci::UsbHciService::Device>();
  if (client.is_error()) {
    zxlogf(ERROR, "Failed to connect fidl protocol");
    return client.error_value();
  }

  // devices_[device_id] must be set before usb_device_add() creates the interface devices
  // so we pass pointer to it here rather than setting it after usb_device_add() returns.
  sync_completion_t wait;
  zx_status_t status = ZX_OK;
  async::PostTask(dispatcher_, [&]() {
    status = UsbDevice::Create(zxdev(), hci_, std::move(*client), device_id, hub_id, speed,
                               dispatcher_, &devices_[device_id]);
    sync_completion_signal(&wait);
  });
  sync_completion_wait(&wait, ZX_TIME_INFINITE);
  if (status == ZX_OK) {
    // Add an entry into the remove completion list for the new UsbDevice.
    remove_completion_[devices_[device_id].get()] = {};
  }
  return status;
}

zx_status_t UsbBus::UsbBusInterfaceRemoveDevice(uint32_t device_id) {
  if (device_id >= devices_.size()) {
    zxlogf(ERROR, "%s: device_id out of range", __func__);
    return ZX_ERR_INVALID_ARGS;
  }

  auto device = std::move(devices_[device_id]);
  if (device == nullptr) {
    return ZX_ERR_BAD_STATE;
  }
  sync_completion_t wait;
  async::PostTask(dispatcher_, [&]() {
    device->DdkAsyncRemove();
    sync_completion_signal(&wait);
  });
  sync_completion_wait(&wait, ZX_TIME_INFINITE);
  remove_completion_[device.get()].Wait();
  remove_completion_.erase(device.get());
  sync_completion_reset(&wait);
  async::PostTask(dispatcher_, [&]() {
    device.reset();
    sync_completion_signal(&wait);
  });
  sync_completion_wait(&wait, ZX_TIME_INFINITE);
  return ZX_OK;
}

zx_status_t UsbBus::UsbBusInterfaceResetPort(uint32_t hub_id, uint32_t port, bool enumerating) {
  if (hub_id >= devices_.size()) {
    zxlogf(ERROR, "%s: hub_id out of range", __func__);
    return ZX_ERR_INVALID_ARGS;
  }
  auto device = devices_[hub_id];
  if (device == nullptr) {
    zxlogf(ERROR, "hub not found in %s", __func__);
    return ZX_ERR_INVALID_ARGS;
  }

  auto status = device->HubResetPort(port);

  // If we are calling reset in the middle of enumerating,
  // the XHCI would already be trying to address the device next.
  if (!enumerating) {
    status = hci_.HubDeviceReset(hub_id, port);
  }
  return status;
}

zx_status_t UsbBus::UsbBusInterfaceReinitializeDevice(uint32_t device_id) {
  if (device_id >= devices_.size()) {
    zxlogf(ERROR, "%s: device_id out of range", __func__);
    return ZX_ERR_INVALID_ARGS;
  }

  auto device = devices_[device_id];
  if (device == nullptr) {
    zxlogf(ERROR, "could not find device %u", device_id);
    return ZX_ERR_INTERNAL;
  }

  // Check if the USB device descriptor changed, in which case we need to force the device to
  // re-enumerate so we can load the uploaded device driver.
  // This can happen during a Device Firmware Upgrade.
  usb_device_descriptor_t old_desc;
  usb_device_descriptor_t updated_desc;
  size_t actual;

  device->UsbGetDeviceDescriptor(&old_desc);
  auto status =
      device->GetDescriptor(USB_DT_DEVICE, 0, 0, &updated_desc, sizeof(updated_desc), &actual);
  if (actual != sizeof(updated_desc)) {
    status = ZX_ERR_IO;
  }
  if (status == ZX_OK) {
    // TODO(jocelyndang): we may want to check other descriptors as well.
    bool descriptors_changed =
        memcmp(&old_desc, &updated_desc, sizeof(usb_device_descriptor_t)) != 0;
    if (descriptors_changed) {
      zxlogf(INFO, "device updated from VID 0x%x PID 0x%x to VID 0x%x PID 0x%x", old_desc.id_vendor,
             old_desc.id_product, updated_desc.id_vendor, updated_desc.id_product);

      status = UsbBusInterfaceRemoveDevice(device_id);
      if (status != ZX_OK) {
        zxlogf(ERROR, "could not remove device %u, got err %d", device_id, status);
        return status;
      }
      status = UsbBusInterfaceAddDevice(device_id, device->GetHubId(), device->GetSpeed());
      if (status != ZX_OK) {
        zxlogf(ERROR, "could not add device %u, got err %d", device_id, status);
      }
      return status;
    }
  } else {
    zxlogf(ERROR, "could not get updated descriptor: %d got len %lu", status, actual);
    // We should try reinitializing the device anyway.
  }

  return device->Reinitialize();
}

zx_status_t UsbBus::GetDeviceId(/* zx_device_t* */ uint64_t device, uint32_t* out) {
  usb_protocol_t usb;
  if (device_get_protocol(reinterpret_cast<zx_device_t*>(device), ZX_PROTOCOL_USB, &usb) != ZX_OK) {
    return ZX_ERR_INTERNAL;
  }
  auto id = usb_get_device_id(&usb);
  if (id >= devices_.size()) {
    return ZX_ERR_INTERNAL;
  }
  *out = id;
  return ZX_OK;
}

zx_status_t UsbBus::UsbBusConfigureHub(/* zx_device_t* */ uint64_t hub_device, usb_speed_t speed,
                                       const usb_hub_descriptor_t* desc, bool multi_tt) {
  uint32_t hub_id;
  if (GetDeviceId(hub_device, &hub_id) != ZX_OK) {
    return ZX_ERR_INTERNAL;
  }
  return hci_.ConfigureHub(hub_id, speed, desc, multi_tt);
}

zx_status_t UsbBus::UsbBusDeviceAdded(/* zx_device_t* */ uint64_t hub_device, uint32_t port,
                                      usb_speed_t speed) {
  uint32_t hub_id;
  if (GetDeviceId(hub_device, &hub_id) != ZX_OK) {
    return ZX_ERR_INTERNAL;
  }
  return hci_.HubDeviceAdded(hub_id, port, speed);
}

zx_status_t UsbBus::UsbBusDeviceRemoved(/* zx_device_t* */ uint64_t hub_device, uint32_t port) {
  uint32_t hub_id;
  if (GetDeviceId(hub_device, &hub_id) != ZX_OK) {
    return ZX_ERR_INTERNAL;
  }
  return hci_.HubDeviceRemoved(hub_id, port);
}

zx_status_t UsbBus::UsbBusSetHubInterface(/* zx_device_t* */ uint64_t usb_device,
                                          const usb_hub_interface_protocol_t* hub) {
  uint32_t usb_device_id;
  auto status = GetDeviceId(usb_device, &usb_device_id);
  if (status != ZX_OK) {
    return status;
  }

  auto usb_dev = devices_[usb_device_id];
  if (usb_dev == nullptr) {
    zxlogf(ERROR, "%s: no device for usb_device_id %u", __func__, usb_device_id);
    return ZX_ERR_INTERNAL;
  }

  usb_dev->SetHubInterface(hub);
  return ZX_OK;
}

void UsbBus::DdkChildPreRelease(void* child_ctx) {
  auto it = remove_completion_.find(reinterpret_cast<UsbDevice*>(child_ctx));
  if (it == remove_completion_.end()) {
    zxlogf(ERROR, "Cannot find device %p in the device list.", child_ctx);
    return;
  }
  it->second.Signal();
}

void UsbBus::DdkUnbind(ddk::UnbindTxn txn) {
  hci_.SetBusInterface(nullptr, nullptr);

  for (auto device : devices_) {
    if (device != nullptr) {
      device->DdkAsyncRemove();
    }
  }
  devices_.reset();

  txn.Reply();
}

void UsbBus::DdkRelease() { delete this; }

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = UsbBus::Create;
  return ops;
}();

}  // namespace usb_bus

ZIRCON_DRIVER(usb_bus, usb_bus::driver_ops, "zircon", "0.1");
