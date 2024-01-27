// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "netdevice.h"

#include <lib/ddk/debug.h>
#include <lib/fit/defer.h>
#include <lib/virtio/ring.h>
#include <lib/zircon-internal/align.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>
#include <utility>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <virtio/net.h>
#include <virtio/virtio.h>

#include "src/devices/bus/lib/virtio/trace.h"

// Enables/disables debugging info
#define LOCAL_TRACE 0

namespace virtio {

namespace {

bool IsLinkActive(const virtio_net_config& config, bool is_status_supported) {
  // 5.1.4.2 Driver Requirements: Device configuration layout
  //
  // If the driver does not negotiate the VIRTIO_NET_F_STATUS feature, it SHOULD assume the link
  // is active, otherwise it SHOULD read the link status from the bottom bit of status.
  //
  // https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html#x1-2000004
  return is_status_supported ? config.status & VIRTIO_NET_S_LINK_UP : true;
}

uint16_t MaxVirtqueuePairs(const virtio_net_config& config, bool is_mq_supported) {
  // 5.1.5 Device Initialization
  //
  // Identify and initialize the receive and transmission virtqueues, up to N of each kind. If
  // VIRTIO_NET_F_MQ feature bit is negotiated, N=max_virtqueue_pairs, otherwise identify N=1.
  //
  // https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html#x1-2040005
  return is_mq_supported ? config.max_virtqueue_pairs : 1;
}

}  // namespace

NetworkDevice::NetworkDevice(zx_device_t* bus_device, zx::bti bti_handle,
                             std::unique_ptr<Backend> backend)
    : virtio::Device(bus_device, std::move(bti_handle), std::move(backend)),
      DeviceType(bus_device),
      rx_(this),
      tx_(this),
      vmo_store_({
          .map =
              vmo_store::MapOptions{
                  .vm_option = ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_REQUIRE_NON_RESIZABLE,
                  .vmar = nullptr,
              },
          .pin =
              vmo_store::PinOptions{
                  .bti = zx::unowned_bti(bti()),
                  .bti_pin_options = ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE,
                  .index = true,
              },
      }) {}

NetworkDevice::~NetworkDevice() {}

zx_status_t NetworkDevice::Init() {
  fbl::AutoLock lock(&state_lock_);

  // Reset the device.
  DeviceReset();

  // Ack and set the driver status bit.
  DriverStatusAck();

  // Ack features. We do DeviceStatusFeaturesOk() when we actually start the network device (in
  // NetworkDeviceImplStart()).
  if (zx_status_t status =
          AckFeatures(&is_status_supported_, &is_multiqueue_supported_, &virtio_hdr_len_);
      status != ZX_OK) {
    zxlogf(ERROR, "failed to ack features: %s", zx_status_get_string(status));
    return status;
  }

  // Read device configuration.
  virtio_net_config_t config;
  CopyDeviceConfig(&config, sizeof(config));

  // We've checked that the config.mac field is valid (VIRTIO_NET_F_MAC) in AckFeatures().
  zxlogf(DEBUG, "mac: %02x:%02x:%02x:%02x:%02x:%02x", config.mac[0], config.mac[1], config.mac[2],
         config.mac[3], config.mac[4], config.mac[5]);
  zxlogf(DEBUG, "link active: %u", IsLinkActive(config, is_status_supported_));
  zxlogf(DEBUG, "max virtqueue pairs: %u", MaxVirtqueuePairs(config, is_multiqueue_supported_));

  static_assert(sizeof(config.mac) == sizeof(mac_.octets));
  std::copy(std::begin(config.mac), std::end(config.mac), mac_.octets.begin());

  if (zx_status_t status = vmo_store_.Reserve(MAX_VMOS); status != ZX_OK) {
    zxlogf(ERROR, "failed to initialize vmo store: %s", zx_status_get_string(status));
    return status;
  }

  // Initialize the zx_device and publish us.
  if (zx_status_t status = DdkAdd("virtio-net"); status != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s", zx_status_get_string(status));
    return status;
  }
  device_ = zxdev();
  tx_depth_ = std::min(GetRingSize(kTxId), kMaxDepth);
  rx_depth_ = std::min(GetRingSize(kRxId), kMaxDepth);

  // Start the interrupt thread.
  StartIrqThread();

  return ZX_OK;
}

zx_status_t NetworkDevice::AckFeatures(bool* is_status_supported, bool* is_multiqueue_supported,
                                       uint16_t* virtio_hdr_len) {
  const uint64_t supported_features = DeviceFeaturesSupported();

  if (!(supported_features & VIRTIO_NET_F_MAC)) {
    zxlogf(ERROR, "device does not have a given MAC address.");
    return ZX_ERR_NOT_SUPPORTED;
  }
  uint64_t enable_features = VIRTIO_NET_F_MAC;

  if (supported_features & VIRTIO_NET_F_STATUS) {
    enable_features |= VIRTIO_NET_F_STATUS;
    *is_status_supported = true;
  } else {
    *is_status_supported = false;
  }

  if (supported_features & VIRTIO_NET_F_MQ) {
    enable_features |= VIRTIO_NET_F_MQ;
    *is_multiqueue_supported = true;
  } else {
    *is_multiqueue_supported = false;
  }

  if (supported_features & VIRTIO_F_VERSION_1) {
    enable_features |= VIRTIO_F_VERSION_1;
    *virtio_hdr_len = sizeof(virtio_net_hdr_t);
  } else {
    // 5.1.6.1 Legacy Interface: Device Operation.
    //
    // The legacy driver only presented num_buffers in the struct
    // virtio_net_hdr when VIRTIO_NET_F_MRG_RXBUF was negotiated; without
    // that feature the structure was 2 bytes shorter.
    //
    // https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html#x1-2050006
    *virtio_hdr_len = sizeof(virtio_legacy_net_hdr_t);
  }

  DriverFeaturesAck(enable_features);
  return ZX_OK;
}

void NetworkDevice::DdkRelease() {
  {
    fbl::AutoLock lock(&state_lock_);
    ifc_.clear();
  }
  virtio::Device::Release();
  delete this;
}

void NetworkDevice::IrqRingUpdate() {
  for (;;) {
    bool again = IrqRingUpdateInternal();
    if (!again) {
      break;
    }
  }
}

bool NetworkDevice::IrqRingUpdateInternal() {
  network::SharedAutoLock state_lock(&state_lock_);
  if (!ifc_.is_valid()) {
    return false;
  }

  bool more_work = false;

  std::array<tx_result_t, kMaxDepth> tx_results;
  auto tx_it = tx_results.begin();
  {
    std::lock_guard lock(tx_lock_);
    tx_.SetNoInterrupt();
    // Ring::IrqRingUpdate will call this lambda on each tx buffer completed
    // by the underlying device since the last IRQ.
    tx_.IrqRingUpdate([this, &tx_it](vring_used_elem* used_elem) {
      []() __TA_ASSERT(tx_lock_) {}();
      uint16_t id = static_cast<uint16_t>(used_elem->id & 0xffff);
      Descriptor desc = tx_in_flight_.Pop();
      ZX_ASSERT_MSG(desc.ring_id == id, "tx ring and FIFO id mismatch (%d != %d for buffer %d)",
                    desc.ring_id, id, desc.buffer_id);
      *tx_it++ = {.id = desc.buffer_id, .status = ZX_OK};
      tx_.FreeDesc(id);
    });
    more_work |= tx_.ClearNoInterruptCheckHasWork();
  }
  if (size_t count = std::distance(tx_results.begin(), tx_it); count != 0) {
    ifc_.CompleteTx(tx_results.data(), count);
  }

  std::array<rx_buffer_t, kMaxDepth> rx_buffers;
  std::array<rx_buffer_part_t, kMaxDepth> rx_buffers_parts;
  auto rx_part_it = rx_buffers_parts.begin();
  auto rx_it = rx_buffers.begin();
  {
    std::lock_guard lock(rx_lock_);
    rx_.SetNoInterrupt();
    // Ring::IrqRingUpdate will call this lambda on each rx buffer filled by
    // the underlying device since the last IRQ.
    rx_.IrqRingUpdate([this, &rx_it, &rx_part_it](vring_used_elem* used_elem) {
      []() __TA_ASSERT(rx_lock_) {}();
      uint16_t id = static_cast<uint16_t>(used_elem->id & 0xffff);
      Descriptor in_flight = rx_in_flight_.Pop();
      ZX_ASSERT_MSG(in_flight.ring_id == id,
                    "rx ring and FIFO id mismatch (%d != %d for buffer %d)", in_flight.ring_id, id,
                    in_flight.buffer_id);
      vring_desc& desc = *rx_.DescFromIndex(id);
      // Driver does not merge rx buffers.
      ZX_ASSERT_MSG((desc.flags & VRING_DESC_F_NEXT) == 0, "descriptor chaining not supported");

      auto parts_list = rx_part_it;
      uint32_t len = used_elem->len - virtio_hdr_len_;
      ZX_ASSERT_MSG(used_elem->len >= virtio_hdr_len_,
                    "got buffer (%u) smaller than virtio header (%u)", used_elem->len,
                    virtio_hdr_len_);
      zxlogf(TRACE, "Receiving %d bytes (hdrlen = %u):", len, virtio_hdr_len_);
      if (zxlog_level_enabled(TRACE)) {
        virtio_dump_desc(&desc);
      }
      *rx_part_it++ = {
          .id = in_flight.buffer_id,
          .offset = virtio_hdr_len_,
          .length = len,
      };
      *rx_it++ = {
          .meta =
              {
                  .port = kPortId,
                  .frame_type =
                      static_cast<uint8_t>(fuchsia_hardware_network::wire::FrameType::kEthernet),
              },
          .data_list = parts_list,
          .data_count = 1,
      };
      rx_.FreeDesc(id);
    });
    more_work |= rx_.ClearNoInterruptCheckHasWork();
  }
  if (size_t count = std::distance(rx_buffers.begin(), rx_it); count != 0) {
    ifc_.CompleteRx(rx_buffers.data(), count);
  }

  return more_work;
}

void NetworkDevice::IrqConfigChange() {
  network::SharedAutoLock lock(&state_lock_);
  if (!ifc_.is_valid()) {
    return;
  }

  const port_status_t status = ReadStatus();
  ifc_.PortStatusChanged(kPortId, &status);
}

port_status_t NetworkDevice::ReadStatus() const {
  virtio_net_config config;
  CopyDeviceConfig(&config, sizeof(config));
  return {
      .mtu = kMtu,
      .flags = IsLinkActive(config, is_status_supported_)
                   ? static_cast<uint32_t>(fuchsia_hardware_network::wire::StatusFlags::kOnline)
                   : 0,
  };
}

zx_status_t NetworkDevice::NetworkDeviceImplInit(const network_device_ifc_protocol_t* iface) {
  fbl::AutoLock lock(&state_lock_);
  ifc_ = ddk::NetworkDeviceIfcProtocolClient(iface);
  ifc_.AddPort(kPortId, this, &network_port_protocol_ops_);
  return ZX_OK;
}

void NetworkDevice::NetworkDeviceImplStart(network_device_impl_start_callback callback,
                                           void* cookie) {
  zx_status_t status = [this]() {
    // Always reset the device and reconfigure so we know where we are.
    DeviceReset();
    WaitForDeviceReset();
    DriverStatusAck();
    bool is_status_supported, is_multiqueue_supported;
    uint16_t header_length;
    if (zx_status_t status =
            AckFeatures(&is_status_supported, &is_multiqueue_supported, &header_length);
        status != ZX_OK) {
      zxlogf(ERROR, "failed to ack features: %s", zx_status_get_string(status));
      return status;
    }
    ZX_ASSERT_MSG(is_status_supported == is_status_supported_,
                  "status support changed from %u to %u between init and start",
                  is_status_supported_, is_status_supported);
    ZX_ASSERT_MSG(is_multiqueue_supported == is_multiqueue_supported_,
                  "max queue support changed from %u to %u between init and start",
                  is_multiqueue_supported_, is_multiqueue_supported);
    ZX_ASSERT_MSG(header_length == virtio_hdr_len_,
                  "header length changed from %u to %u between init and start", virtio_hdr_len_,
                  header_length);

    if (zx_status_t status = DeviceStatusFeaturesOk(); status != ZX_OK) {
      zxlogf(ERROR, "%s: Feature negotiation failed (%s)", tag(), zx_status_get_string(status));
      return status;
    }

    // Allocate virtqueues.
    {
      std::lock_guard rx_lock(rx_lock_);
      std::lock_guard tx_lock(tx_lock_);
      Ring rx_queue(this);
      if (zx_status_t status = rx_queue.Init(kRxId, rx_depth_); status != ZX_OK) {
        zxlogf(ERROR, "failed to allocate rx virtqueue: %s", zx_status_get_string(status));
        return status;
      }
      rx_ = std::move(rx_queue);
      Ring tx_queue(this);
      if (zx_status_t status = tx_queue.Init(kTxId, tx_depth_); status != ZX_OK) {
        zxlogf(ERROR, "failed to allocate tx virtqueue: %s", zx_status_get_string(status));
        return status;
      }
      tx_ = std::move(tx_queue);
    }
    DriverStatusOk();
    return ZX_OK;
  }();
  callback(cookie, status);
}

void NetworkDevice::NetworkDeviceImplStop(network_device_impl_stop_callback callback,
                                          void* cookie) {
  DeviceReset();
  WaitForDeviceReset();

  // Return all pending buffers.
  {
    network::SharedAutoLock state_lock(&state_lock_);
    // Pending tx buffers.
    {
      std::array<tx_result_t, kMaxDepth> tx_return;
      auto iter = tx_return.begin();
      {
        std::lock_guard lock(tx_lock_);
        while (!tx_in_flight_.Empty()) {
          Descriptor d = tx_in_flight_.Pop();
          *iter++ = {
              .id = d.buffer_id,
              .status = ZX_ERR_BAD_STATE,
          };
        }
      }
      if (iter != tx_return.begin()) {
        ifc_.CompleteTx(tx_return.data(), std::distance(tx_return.begin(), iter));
      }
    }
    // Pending rx buffers.
    {
      std::array<rx_buffer_t, kMaxDepth> rx_return;
      std::array<rx_buffer_part_t, kMaxDepth> rx_return_parts;
      auto iter = rx_return.begin();
      auto parts_iter = rx_return_parts.begin();
      {
        std::lock_guard lock(rx_lock_);
        while (!rx_in_flight_.Empty()) {
          Descriptor d = rx_in_flight_.Pop();
          *iter++ = {
              .data_list = &*parts_iter,
              .data_count = 1,
          };
          *parts_iter++ = {.id = d.buffer_id};
        }
      }
      if (iter != rx_return.begin()) {
        ifc_.CompleteRx(rx_return.data(), std::distance(rx_return.begin(), iter));
      }
    }
  }

  callback(cookie);
}

void NetworkDevice::NetworkDeviceImplGetInfo(device_info_t* out_info) {
  *out_info = {
      .tx_depth = tx_depth_,
      .rx_depth = rx_depth_,
      .rx_threshold = static_cast<uint16_t>(rx_depth_ / 2),
      .max_buffer_parts = 1,
      .max_buffer_length = kFrameSize,
      .buffer_alignment = ZX_PAGE_SIZE / 2,
      .min_rx_buffer_length = kFrameSize,
      // Minimum Ethernet frame size on the wire according to IEEE 802.3, minus
      // the frame check sequence.
      .min_tx_buffer_length = 60,
      .tx_head_length = virtio_hdr_len_,
  };
}

void NetworkDevice::NetworkDeviceImplQueueTx(const tx_buffer_t* buf_list, size_t buf_count) {
  network::SharedAutoLock lock(&state_lock_);
  std::lock_guard tx_lock(tx_lock_);
  for (const auto& buffer : cpp20::span(buf_list, buf_count)) {
    ZX_DEBUG_ASSERT_MSG(buffer.data_count == 1, "received unsupported scatter gather buffer %zu",
                        buffer.data_count);

    const buffer_region_t& data = buffer.data_list[0];

    // Grab a free descriptor.
    uint16_t id;
    vring_desc* desc = tx_.AllocDescChain(1, &id);
    ZX_ASSERT_MSG(desc != nullptr, "failed to allocate descriptor");

    // Add the data to be sent.
    VmoStore::StoredVmo* stored_vmo = vmo_store_.GetVmo(data.vmo);
    ZX_ASSERT_MSG(stored_vmo != nullptr, "invalid VMO id %d", data.vmo);

    // Get a pointer to the header. Casting it to net header structs is valid
    // because we requested alignment and tx header in
    // NetworkDeviceImpl.GetInfo.
    void* tx_hdr = stored_vmo->data().subspan(data.offset, virtio_hdr_len_).data();

    constexpr virtio_legacy_net_hdr_t kBaseHeader = {
        // If VIRTIO_NET_F_CSUM is not negotiated, the driver MUST set flags to
        // zero and SHOULD supply a fully checksummed packet to the device.
        .flags = 0,
        // If none of the VIRTIO_NET_F_HOST_TSO4, TSO6 or UFO options have been
        // negotiated, the driver MUST set gso_type to VIRTIO_NET_HDR_GSO_NONE.
        .gso_type = VIRTIO_NET_HDR_GSO_NONE,
    };

    switch (virtio_hdr_len_) {
      case sizeof(virtio_net_hdr_t):
        *static_cast<virtio_net_hdr_t*>(tx_hdr) = {
            .base = kBaseHeader,
            // 5.1.6.2.1 Driver Requirements: Packet Transmission
            //
            // The driver MUST set num_buffers to zero.
            //
            // Implementation note: This field doesn't exist if neither
            // |VIRTIO_F_VERSION_1| or |VIRTIO_F_MRG_RXBUF| have been negotiated.
            //
            // https://docs.oasis-open.org/virtio/virtio/v1.1/csprd01/virtio-v1.1-csprd01.html#x1-2050006
            .num_buffers = 0,
        };
        break;
      case sizeof(virtio_legacy_net_hdr_t):
        *static_cast<virtio_legacy_net_hdr_t*>(tx_hdr) = kBaseHeader;
        break;
      default:
        ZX_PANIC("invalid virtio header length %d", virtio_hdr_len_);
    }

    fzl::PinnedVmo::Region region;
    size_t actual_regions = 0;
    zx_status_t status =
        stored_vmo->GetPinnedRegions(data.offset, data.length, &region, 1, &actual_regions);
    ZX_ASSERT_MSG(status == ZX_OK, "failed to retrieve pinned region %s (actual=%zu)",
                  zx_status_get_string(status), actual_regions);

    *desc = {
        .addr = region.phys_addr,
        .len = static_cast<uint32_t>(data.length),
    };
    tx_in_flight_.Push({
        .buffer_id = buffer.id,
        .ring_id = id,
    });
    // Submit the descriptor and notify the back-end.
    if (zxlog_level_enabled(TRACE)) {
      virtio_dump_desc(desc);
    }
    zxlogf(TRACE, "Sending %zu bytes (hdrlen = %u):", data.length, virtio_hdr_len_);
    tx_.SubmitChain(id);
  }
  if (!tx_.NoNotify()) {
    tx_.Kick();
  }
}

void NetworkDevice::NetworkDeviceImplQueueRxSpace(const rx_space_buffer_t* buf_list,
                                                  size_t buf_count) {
  network::SharedAutoLock lock(&state_lock_);
  std::lock_guard rx_lock(rx_lock_);
  for (const auto& buffer : cpp20::span(buf_list, buf_count)) {
    const buffer_region_t& data = buffer.region;

    // Grab a free descriptor.
    uint16_t id;
    vring_desc* desc = rx_.AllocDescChain(1, &id);
    ZX_ASSERT_MSG(desc != nullptr, "failed to allocate descriptor");

    // Add the data to be sent.
    VmoStore::StoredVmo* stored_vmo = vmo_store_.GetVmo(data.vmo);
    ZX_ASSERT_MSG(stored_vmo != nullptr, "invalid VMO id %d", data.vmo);

    fzl::PinnedVmo::Region region;
    size_t actual_regions = 0;
    zx_status_t status =
        stored_vmo->GetPinnedRegions(data.offset, data.length, &region, 1, &actual_regions);
    ZX_ASSERT_MSG(status == ZX_OK, "failed to retrieve pinned region %s (actual=%zu)",
                  zx_status_get_string(status), actual_regions);
    *desc = {
        .addr = region.phys_addr,
        .len = static_cast<uint32_t>(data.length),
        .flags = VRING_DESC_F_WRITE,
    };
    rx_in_flight_.Push({
        .buffer_id = buffer.id,
        .ring_id = id,
    });
    // Submit the descriptor and notify the back-end.
    if (zxlog_level_enabled(TRACE)) {
      virtio_dump_desc(desc);
    }
    zxlogf(TRACE, "Queueing rx space with %zu bytes:", data.length);
    rx_.SubmitChain(id);
  }
  if (!rx_.NoNotify()) {
    rx_.Kick();
  }
}

void NetworkDevice::NetworkDeviceImplPrepareVmo(uint8_t vmo_id, zx::vmo vmo,
                                                network_device_impl_prepare_vmo_callback callback,
                                                void* cookie) {
  zx_status_t status = [this, &vmo_id, &vmo]() {
    fbl::AutoLock vmo_lock(&state_lock_);
    return vmo_store_.RegisterWithKey(vmo_id, std::move(vmo));
  }();
  callback(cookie, status);
}

void NetworkDevice::NetworkDeviceImplReleaseVmo(uint8_t vmo_id) {
  fbl::AutoLock vmo_lock(&state_lock_);
  if (zx::result<zx::vmo> status = vmo_store_.Unregister(vmo_id); status.status_value() != ZX_OK) {
    zxlogf(ERROR, "failed to release vmo id = %d: %s", vmo_id, status.status_string());
  }
}

void NetworkDevice::NetworkPortGetInfo(port_info_t* out_info) {
  static constexpr uint8_t kRxTypesList[] = {
      static_cast<uint8_t>(fuchsia_hardware_network::wire::FrameType::kEthernet)};
  static constexpr tx_support_t kTxTypesList[] = {{
      .type = static_cast<uint8_t>(fuchsia_hardware_network::wire::FrameType::kEthernet),
      .features = fuchsia_hardware_network::wire::kFrameFeaturesRaw,
  }};
  *out_info = {
      .port_class = static_cast<uint32_t>(fuchsia_hardware_network::wire::DeviceClass::kEthernet),
      .rx_types_list = kRxTypesList,
      .rx_types_count = std::size(kRxTypesList),
      .tx_types_list = kTxTypesList,
      .tx_types_count = std::size(kTxTypesList),
  };
}

void NetworkDevice::NetworkPortGetStatus(port_status_t* out_status) { *out_status = ReadStatus(); }

void NetworkDevice::NetworkPortSetActive(bool active) {}
void NetworkDevice::NetworkPortGetMac(mac_addr_protocol_t* out_mac_ifc) {
  *out_mac_ifc = {
      .ops = &mac_addr_protocol_ops_,
      .ctx = this,
  };
}

void NetworkDevice::MacAddrGetAddress(uint8_t* out_mac) {
  std::copy(mac_.octets.begin(), mac_.octets.end(), out_mac);
}

void NetworkDevice::MacAddrGetFeatures(features_t* out_features) {
  *out_features = {
      .multicast_filter_count = 0,
      .supported_modes = SUPPORTED_MAC_FILTER_MODE_PROMISCUOUS,
  };
}

void NetworkDevice::MacAddrSetMode(mode_t mode, const uint8_t* multicast_macs_list,
                                   size_t multicast_macs_count) {
  /* We only support promiscuous mode, nothing to do */
  ZX_ASSERT_MSG(mode == MODE_PROMISCUOUS, "unsupported mode %d", mode);
  ZX_ASSERT_MSG(multicast_macs_count == 0, "unsupported multicast count %zu", multicast_macs_count);
}

}  // namespace virtio
