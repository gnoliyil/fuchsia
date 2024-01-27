// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd.h"

// These entrypoints are used when the driver doesn't implement an extension to the interface yet.

magma_status_t __attribute__((weak))
msd_device_get_icd_list(struct msd_device_t* device, uint64_t count, msd_icd_info_t* icd_info_out,
                        uint64_t* actual_count_out) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

uint32_t __attribute__((weak)) msd_driver_duplicate_inspect_handle(msd_driver_t* driver) {
  return 0;
}

void __attribute__((weak))
msd_device_set_memory_pressure_level(struct msd_device_t* dev, MagmaMemoryPressureLevel level) {}

magma_status_t __attribute__((weak))
msd_connection_enable_performance_counters(msd_connection_t* connection, const uint64_t* counters,
                                           uint64_t counter_count) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_create_performance_counter_buffer_pool(struct msd_connection_t* connection,
                                                      uint64_t pool_id,
                                                      struct msd_perf_count_pool** pool_out) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_release_performance_counter_buffer_pool(struct msd_connection_t* connection,
                                                       struct msd_perf_count_pool* pool) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_dump_performance_counters(struct msd_connection_t* connection,
                                         struct msd_perf_count_pool* pool, uint32_t trigger_id) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_clear_performance_counters(struct msd_connection_t* connection,
                                          const uint64_t* counters, uint64_t counter_count) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak)) msd_connection_add_performance_counter_buffer_offset_to_pool(
    struct msd_connection_t*, struct msd_perf_count_pool* pool, struct msd_buffer_t* buffer,
    uint64_t buffer_id, uint64_t buffer_offset, uint64_t buffer_size) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_remove_performance_counter_buffer_from_pool(struct msd_connection_t*,
                                                           struct msd_perf_count_pool* pool,
                                                           struct msd_buffer_t* buffer) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t __attribute__((weak))
msd_connection_buffer_range_op(struct msd_connection_t* connection, struct msd_buffer_t* buffer,
                               uint32_t options, uint64_t start_offset, uint64_t length) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}
