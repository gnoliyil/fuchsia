// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_WLANSOFTMAC_RUST_DRIVER_C_BINDING_BINDINGS_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_WLANSOFTMAC_RUST_DRIVER_C_BINDING_BINDINGS_H_

// Warning:
// This file was autogenerated by cbindgen.
// Do not modify this file manually.

#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/c/banjo.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <fuchsia/wlan/softmac/c/banjo.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <zircon/types.h>

typedef struct wlansoftmac_handle_t wlansoftmac_handle_t;

typedef struct {
  void (*recv)(void *ctx, const wlan_rx_packet_t *packet);
  void (*complete_tx)(void *ctx, const wlan_tx_packet_t *packet, int32_t status);
  void (*report_tx_result)(void *ctx, const wlan_tx_result_t *tx_result);
  void (*scan_complete)(void *ctx, int32_t status, uint64_t scan_id);
} rust_wlan_softmac_ifc_protocol_ops_copy_t;

/**
 * Hand-rolled Rust version of the banjo wlan_softmac_ifc_protocol for communication from the driver
 * up. Note that we copy the individual fns out of this struct into the equivalent generated struct
 * in C++. Thanks to cbindgen, this gives us a compile-time confirmation that our function
 * signatures are correct.
 */
typedef struct {
  const rust_wlan_softmac_ifc_protocol_ops_copy_t *ops;
  void *ctx;
} rust_wlan_softmac_ifc_protocol_copy_t;

/**
 * An output buffer requires its owner to manage the underlying buffer's memory themselves.
 * An output buffer is used for every buffer handed from Rust to C++.
 */
typedef struct {
  /**
   * Pointer to the buffer's underlying data structure.
   */
  void *raw;
  /**
   * Pointer to the start of the buffer's data portion and the amount of bytes written.
   */
  uint8_t *data;
  uintptr_t written_bytes;
} wlansoftmac_out_buf_t;

/**
 * A `Device` allows transmitting frames and MLME messages.
 */
typedef struct {
  void *device;
  /**
   * Start operations on the underlying device and return the SME channel.
   */
  int32_t (*start)(void *device, const rust_wlan_softmac_ifc_protocol_copy_t *ifc,
                   zx_handle_t *out_sme_channel);
  /**
   * Request to deliver an Ethernet II frame to Fuchsia's Netstack.
   */
  int32_t (*deliver_eth_frame)(void *device, const uint8_t *data, uintptr_t len);
  /**
   * Deliver a WLAN frame directly through the firmware.
   */
  int32_t (*queue_tx)(void *device, uint32_t options, wlansoftmac_out_buf_t buf,
                      wlan_tx_info_t tx_info);
  /**
   * Reports the current status to the ethernet driver.
   */
  int32_t (*set_ethernet_status)(void *device, uint32_t status);
  /**
   * Set a key on the device.
   * |key| is mutable because the underlying API does not take a const wlan_key_configuration_t.
   */
  int32_t (*set_key)(void *device, wlan_key_configuration_t *key);
  /**
   * Get discovery features supported by this WLAN interface
   */
  discovery_support_t (*get_discovery_support)(void *device);
  /**
   * Get MAC sublayer features supported by this WLAN interface
   */
  mac_sublayer_support_t (*get_mac_sublayer_support)(void *device);
  /**
   * Get security features supported by this WLAN interface
   */
  security_support_t (*get_security_support)(void *device);
  /**
   * Get spectrum management features supported by this WLAN interface
   */
  spectrum_management_support_t (*get_spectrum_management_support)(void *device);
  /**
   * Configure the device's BSS.
   * |cfg| is mutable because the underlying API does not take a const join_bss_request_t.
   */
  int32_t (*join_bss)(void *device, join_bss_request_t *cfg);
  /**
   * Enable hardware offload of beaconing on the device.
   */
  int32_t (*enable_beaconing)(void *device, wlansoftmac_out_buf_t buf, uintptr_t tim_ele_offset,
                              uint16_t beacon_interval);
} rust_device_interface_t;

/**
 * An input buffer will always be returned to its original owner when no longer being used.
 * An input buffer is used for every buffer handed from C++ to Rust.
 */
typedef struct {
  /**
   * Returns the buffer's ownership and free it.
   */
  void (*free_buffer)(void *raw);
  /**
   * Pointer to the buffer's underlying data structure.
   */
  void *raw;
  /**
   * Pointer to the start of the buffer's data portion and its length.
   */
  uint8_t *data;
  uintptr_t len;
} wlansoftmac_in_buf_t;

typedef struct {
  /**
   * Acquire a `InBuf` with a given minimum length from the provider.
   * The provider must release the underlying buffer's ownership and transfer it to this crate.
   * The buffer will be returned via the `free_buffer` callback when it's no longer used.
   */
  wlansoftmac_in_buf_t (*get_buffer)(uintptr_t min_len);
} wlansoftmac_buffer_provider_ops_t;

/**
 * A convenient C-wrapper for read-only memory that is neither owned or managed by Rust
 */
typedef struct {
  const uint8_t *data;
  uintptr_t size;
} wlan_span_t;

extern "C" wlansoftmac_handle_t *start_sta(rust_device_interface_t device,
                                           wlansoftmac_buffer_provider_ops_t buf_provider,
                                           zx_handle_t wlan_softmac_bridge_client_handle);

extern "C" void stop_sta(wlansoftmac_handle_t *softmac);

/**
 * FFI interface: Stop and delete a WlanSoftmac via the WlanSoftmacHandle.
 * Takes ownership and invalidates the passed WlanSoftmacHandle.
 *
 * # Safety
 *
 * This fn accepts a raw pointer that is held by the FFI caller as a handle to
 * the Softmac. This API is fundamentally unsafe, and relies on the caller to
 * pass the correct pointer and make no further calls on it later.
 */
extern "C" void delete_sta(wlansoftmac_handle_t *softmac);

extern "C" zx_status_t sta_queue_eth_frame_tx(wlansoftmac_handle_t *softmac, wlan_span_t frame);

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_WLANSOFTMAC_RUST_DRIVER_C_BINDING_BINDINGS_H_
