// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Magma is the driver model for GPUs/NPUs on Fuchsia.
//
// Magma has two driver components: a hardware-specific library loaded into an "application"’s
// address space ("client driver", sometimes known as "Installable client driver" or "ICD");
// and a system driver that interfaces with the hardware. Magma is described in detail at [0].
//
// The Magma APIs are vendor independent. Some drivers may implement only the subset of the
// APIs that are relevant. The format of data carried inside commands/command buffers is not
// defined. Some APIs are explicitly extensible, such as magma_query, to allow for specific
// vendor/driver needs.  Vendor specifics must be contained in a separate definitions file.
//
// Since client driver implementations may be written in a variety of languages (possibly C),
// the Magma bindings have a C interface, and may be used to interact with both Magma and Sysmem.
//
// The Magma bindings are OS independent so a client driver targeting Magma can easily be built
// for Fuchsia or Linux. On Fuchsia the APIs are backed by Zircon and FIDL; for virtualized Linux
// they are backed by a virtualization transport (virtmagma). APIs prefixed by 'magma_virt_' are
// for virtualization only.  32-bit and 64-bit builds are supported for virtualized clients.
//
// On Fuchsia the system driver is a separate process, so Magma APIs follow a feed forward design
// where possible to allow for efficient pipelining of requests.
//
// [0] https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0198_magma_api_design
//

// NOTE: DO NOT EDIT THIS FILE!
// It is automatically generated by //src/graphics/lib/magma/include/magma/magma_h_gen.py

// clang-format off

#ifndef SRC_GRAPHICS_LIB_MAGMA_INCLUDE_MAGMA_MAGMA_H_
#define SRC_GRAPHICS_LIB_MAGMA_INCLUDE_MAGMA_MAGMA_H_

#include "magma/magma_common_defs.h"
#include <stdint.h>

// LINT.IfChange
#if defined(__cplusplus)
extern "C" {
#endif

///
/// \brief Imports and takes ownership of a channel to a device.
/// \param device_channel A channel connecting to a gpu class device.
/// \param device_out Returned device.
///
MAGMA_EXPORT magma_status_t magma_device_import(
    magma_handle_t device_channel,
    magma_device_t* device_out);

///
/// \brief Releases a handle to a device
/// \param device An open device.
///
MAGMA_EXPORT void magma_device_release(
    magma_device_t device);

///
/// \brief Performs a query synchronously. On MAGMA_STATUS_OK, a given query |id| will return either
///        a buffer in |result_buffer_out|, or a value in |result_out|. A NULL pointer may be
///        provided for whichever result parameter is not needed.
/// \param device An open device.
/// \param id A vendor-specific ID.
/// \param result_buffer_out Handle to the returned buffer.
/// \param result_out Pointer to a uint64 result.
///
MAGMA_EXPORT magma_status_t magma_device_query(
    magma_device_t device,
    uint64_t id,
    magma_handle_t* result_buffer_out,
    uint64_t* result_out);

///
/// \brief Opens a connection to a device.
/// \param device An open device
/// \param connection_out Returned connection.
///
MAGMA_EXPORT magma_status_t magma_device_create_connection(
    magma_device_t device,
    magma_connection_t* connection_out);

///
/// \brief Releases the given connection.
/// \param connection An open connection.
///
MAGMA_EXPORT void magma_connection_release(
    magma_connection_t connection);

///
/// \brief When a system driver error occurs, the connection will be closed, and interfaces can
///        return MAGMA_STATUS_CONNECTION_LOST.  In that case, this returns the system driver error.
///        This may incur a round-trip sync.
/// \param connection An open connection.
///
MAGMA_EXPORT magma_status_t magma_connection_get_error(
    magma_connection_t connection);

///
/// \brief Creates a context on the given connection.
/// \param connection An open connection.
/// \param context_id_out The returned context id.
///
MAGMA_EXPORT magma_status_t magma_connection_create_context(
    magma_connection_t connection,
    uint32_t* context_id_out);

///
/// \brief Releases the context associated with the given id.
/// \param connection An open connection.
/// \param context_id A valid context id.
///
MAGMA_EXPORT void magma_connection_release_context(
    magma_connection_t connection,
    uint32_t context_id);

///
/// \brief Creates a memory buffer of at least the given size.
/// \param connection An open connection.
/// \param size Requested buffer size.
/// \param size_out The returned buffer's actual size.
/// \param buffer_out The returned buffer.
/// \param id_out The buffer id of the buffer.
///
MAGMA_EXPORT magma_status_t magma_connection_create_buffer2(
    magma_connection_t connection,
    uint64_t size,
    uint64_t* size_out,
    magma_buffer_t* buffer_out,
    magma_buffer_id_t* id_out);

///
/// \brief Releases the given memory buffer.
/// \param connection An open connection.
/// \param buffer A valid buffer.
///
MAGMA_EXPORT void magma_connection_release_buffer(
    magma_connection_t connection,
    magma_buffer_t buffer);

///
/// \brief Imports and takes ownership of the buffer referred to by the given handle.
/// \param connection An open connection.
/// \param buffer_handle A valid handle.
/// \param size_out The size of the buffer in bytes.
/// \param buffer_out The returned buffer.
/// \param id_out The buffer id of the buffer.
///
MAGMA_EXPORT magma_status_t magma_connection_import_buffer2(
    magma_connection_t connection,
    magma_handle_t buffer_handle,
    uint64_t* size_out,
    magma_buffer_t* buffer_out,
    magma_buffer_id_t* id_out);

///
/// \brief Creates a semaphore.
/// \param connection An open connection.
/// \param semaphore_out The returned semaphore.
/// \param id_out The id of the semaphore.
///
MAGMA_EXPORT magma_status_t magma_connection_create_semaphore2(
    magma_connection_t connection,
    magma_semaphore_t* semaphore_out,
    magma_semaphore_id_t* id_out);

///
/// \brief Releases the given semaphore.
/// \param connection An open connection.
/// \param semaphore A valid semaphore.
///
MAGMA_EXPORT void magma_connection_release_semaphore(
    magma_connection_t connection,
    magma_semaphore_t semaphore);

///
/// \brief Imports and takes ownership of the semaphore referred to by the given handle.
/// \param connection An open connection.
/// \param semaphore_handle A valid semaphore handle.
/// \param semaphore_out The returned semaphore.
/// \param id_out The id of the semaphore.
///
MAGMA_EXPORT magma_status_t magma_connection_import_semaphore2(
    magma_connection_t connection,
    magma_handle_t semaphore_handle,
    magma_semaphore_t* semaphore_out,
    magma_semaphore_id_t* id_out);

///
/// \brief Perform an operation on a range of a buffer
/// \param connection An open connection.
/// \param buffer A valid buffer.
/// \param options Options for the operation.
/// \param start_offset Byte offset into the buffer.
/// \param length Length (in bytes) of the region to operate on.
///
MAGMA_EXPORT magma_status_t magma_connection_perform_buffer_op(
    magma_connection_t connection,
    magma_buffer_t buffer,
    uint32_t options,
    uint64_t start_offset,
    uint64_t length);

///
/// \brief Maps a buffer range onto the hardware in the connection's address space at the given
///        address. Depending on the MSD this may automatically commit and populate that range.
/// \param connection An open connection.
/// \param hw_va Destination virtual address for the mapping.
/// \param buffer A valid buffer.
/// \param offset Offset into the buffer.
/// \param length Length in bytes of the range to map.
/// \param map_flags A valid MAGMA_MAP_FLAGS value.
///
MAGMA_EXPORT magma_status_t magma_connection_map_buffer(
    magma_connection_t connection,
    uint64_t hw_va,
    magma_buffer_t buffer,
    uint64_t offset,
    uint64_t length,
    uint64_t map_flags);

///
/// \brief Releases the mapping at the given hardware address.
/// \param connection An open connection.
/// \param hw_va A hardware virtual address associated with an existing mapping of the given buffer.
/// \param buffer A valid buffer.
///
MAGMA_EXPORT void magma_connection_unmap_buffer(
    magma_connection_t connection,
    uint64_t hw_va,
    magma_buffer_t buffer);

///
/// \brief Submits command buffers for execution on the hardware.
/// \param connection An open connection.
/// \param context_id A valid context id.
/// \param descriptor A pointer to the command descriptor.
///
MAGMA_EXPORT magma_status_t magma_connection_execute_command(
    magma_connection_t connection,
    uint32_t context_id,
    magma_command_descriptor_t* descriptor);

///
/// \brief Submits a series of commands for execution on the hardware without using a command
///        buffer.
/// \param connection An open connection.
/// \param context_id A valid context ID.
/// \param command_count The number of commands in the provided buffer.
/// \param command_buffers An array of command_count magma_inline_command_buffer structs.
///
MAGMA_EXPORT magma_status_t magma_connection_execute_immediate_commands(
    magma_connection_t connection,
    uint32_t context_id,
    uint64_t command_count,
    magma_inline_command_buffer_t* command_buffers);

///
/// \brief Incurs a round-trip to the system driver, used to ensure all previous messages have been
///        observed, but not necessarily completed.
/// \param connection An open connection.
///
MAGMA_EXPORT magma_status_t magma_connection_flush(
    magma_connection_t connection);

///
/// \brief Returns a handle that can be waited on to determine when the connection has data in the
///        notification channel. This channel has the same lifetime as the connection and must not
///        be closed by the client.
/// \param connection An open connection.
///
MAGMA_EXPORT magma_handle_t magma_connection_get_notification_channel_handle(
    magma_connection_t connection);

///
/// \brief Reads a notification from the channel into the given buffer.  Message sizes may vary
///        depending on the MSD.  If the buffer provided is too small for the message,
///        MAGMA_STATUS_INVALID_ARGS will be returned and the size of message will be returned in
///        the buffer_size_out parameter.
/// \param connection An open connection.
/// \param buffer Buffer into which to read notification data.
/// \param buffer_size Size of the given buffer.
/// \param buffer_size_out Returned size of the notification data written to the buffer, or 0 if
///        there are no messages pending.
/// \param more_data_out True if there is more notification data waiting.
///
MAGMA_EXPORT magma_status_t magma_connection_read_notification_channel(
    magma_connection_t connection,
    void* buffer,
    uint64_t buffer_size,
    uint64_t* buffer_size_out,
    magma_bool_t* more_data_out);

///
/// \brief Cleans, and optionally invalidates, the cache for the region of memory specified by the
///        given buffer, offset, and size, and write the contents to ram.
/// \param buffer A valid buffer.
/// \param offset An offset into the buffer. Must be less than or equal to the buffer's size.
/// \param size Size of region to be cleaned. Offset + size must be less than or equal to the
///        buffer's size.
/// \param operation One of MAGMA_CACHE_OPERATION_CLEAN or MAGMA_CACHE_OPERATION_CLEAN_INVALIDATE.
///
MAGMA_EXPORT magma_status_t magma_buffer_clean_cache(
    magma_buffer_t buffer,
    uint64_t offset,
    uint64_t size,
    magma_cache_operation_t operation);

///
/// \brief Configures the cache for the given buffer.
/// \param buffer A valid buffer.
/// \param policy One of MAGMA_CACHE_POLICY_[CACHED|WRITE_COMBINING|UNCACHED].
///
MAGMA_EXPORT magma_status_t magma_buffer_set_cache_policy(
    magma_buffer_t buffer,
    magma_cache_policy_t policy);

///
/// \brief Queries the cache policy for a buffer.
/// \param buffer A valid buffer.
/// \param cache_policy_out The returned cache policy.
///
MAGMA_EXPORT magma_status_t magma_buffer_get_cache_policy(
    magma_buffer_t buffer,
    magma_cache_policy_t* cache_policy_out);

///
/// \brief Sets a name for the buffer for use in debugging tools.
/// \param buffer A valid buffer.
/// \param name The 0-terminated name of the buffer. May be truncated.
///
MAGMA_EXPORT magma_status_t magma_buffer_set_name(
    magma_buffer_t buffer,
    const char* name);

///
/// \brief Get information on a magma buffer
/// \param buffer A valid buffer.
/// \param info_out Pointer to struct that receives the buffer info.
///
MAGMA_EXPORT magma_status_t magma_buffer_get_info(
    magma_buffer_t buffer,
    magma_buffer_info_t* info_out);

///
/// \brief Gets a platform handle for the given buffer. This can be used to perform a CPU mapping of
///        the buffer using the standard syscall.  The handle may be released without invalidating
///        such CPU mappings.
/// \param buffer A valid buffer.
/// \param handle_out Pointer to the returned handle.
///
MAGMA_EXPORT magma_status_t magma_buffer_get_handle(
    magma_buffer_t buffer,
    magma_handle_t* handle_out);

///
/// \brief Exports the given buffer, returning a handle that may be imported into a connection.
/// \param buffer A valid buffer.
/// \param buffer_handle_out The returned handle.
///
MAGMA_EXPORT magma_status_t magma_buffer_export(
    magma_buffer_t buffer,
    magma_handle_t* buffer_handle_out);

///
/// \brief Signals the given semaphore.
/// \param semaphore A valid semaphore.
///
MAGMA_EXPORT void magma_semaphore_signal(
    magma_semaphore_t semaphore);

///
/// \brief Resets the given semaphore.
/// \param semaphore A valid semaphore.
///
MAGMA_EXPORT void magma_semaphore_reset(
    magma_semaphore_t semaphore);

///
/// \brief Exports the given semaphore, returning a handle that may be imported into a connection
/// \param semaphore A valid semaphore.
/// \param semaphore_handle_out The returned handle.
///
MAGMA_EXPORT magma_status_t magma_semaphore_export(
    magma_semaphore_t semaphore,
    magma_handle_t* semaphore_handle_out);

///
/// \brief Waits for at least one of the given items to meet a condition. Does not reset any
///        semaphores. Results are returned in the items array.
/// \param items Array of poll items. Type should be either MAGMA_POLL_TYPE_SEMAPHORE or
///        MAGMA_POLL_TYPE_HANDLE. Condition may be set to MAGMA_POLL_CONDITION_SIGNALED OR
///        MAGMA_POLL_CONDITION_READABLE. If condition is 0 the item is ignored. Item results are
///        set to the condition that was satisfied, otherwise 0. If the same item is given twice the
///        behavior is undefined.
/// \param count Number of poll items in the array.
/// \param timeout_ns Time in ns to wait before returning MAGMA_STATUS_TIMED_OUT.
///
MAGMA_EXPORT magma_status_t magma_poll(
    magma_poll_item_t* items,
    uint32_t count,
    uint64_t timeout_ns);

///
/// \brief Initializes tracing
/// \param channel An open connection to a tracing provider.
///
MAGMA_EXPORT magma_status_t magma_initialize_tracing(
    magma_handle_t channel);

///
/// \brief Initializes logging; used for debug and some exceptional error reporting.
/// \param channel An open connection to the syslog service.
///
MAGMA_EXPORT magma_status_t magma_initialize_logging(
    magma_handle_t channel);

///
/// \brief Tries to enable access to performance counters. Returns MAGMA_STATUS_OK if counters were
///        successfully enabled or MAGMA_STATUS_ACCESS_DENIED if channel is for the wrong device and
///        counters were not successfully enabled previously.
/// \param connection An open connection to a device.
/// \param channel A handle to a channel to a gpu-performance-counter device.
///
MAGMA_EXPORT magma_status_t magma_connection_enable_performance_counter_access(
    magma_connection_t connection,
    magma_handle_t channel);

///
/// \brief Enables a set of performance counters (the precise definition depends on the driver).
///        Disables enabled performance counters that are not in the new set. Performance counters
///        will also be automatically disabled on connection close. Performance counter access must
///        have been enabled using magma_connection_enable_performance_counter_access before calling
///        this method.
/// \param connection An open connection to a device.
/// \param counters An implementation-defined list of counters.
/// \param counters_count The number of entries in |counters|.
///
MAGMA_EXPORT magma_status_t magma_connection_enable_performance_counters(
    magma_connection_t connection,
    uint64_t* counters,
    uint64_t counters_count);

///
/// \brief Create a pool of buffers that performance counters can be dumped into. Performance
///        counter access must have been enabled using
///        magma_connection_enable_performance_counter_access before calling this method.
/// \param connection An open connection to a device.
/// \param pool_id_out A new pool id. Must not currently be in use.
/// \param notification_handle_out A handle that should be waited on.
///
MAGMA_EXPORT magma_status_t magma_connection_create_performance_counter_buffer_pool(
    magma_connection_t connection,
    magma_perf_count_pool_t* pool_id_out,
    magma_handle_t* notification_handle_out);

///
/// \brief Releases a pool of performance counter buffers. Performance counter access must have been
///        enabled using magma_connection_enable_performance_counter_access before calling this
///        method.
/// \param connection An open connection to a device.
/// \param pool_id An existing pool id.
///
MAGMA_EXPORT magma_status_t magma_connection_release_performance_counter_buffer_pool(
    magma_connection_t connection,
    magma_perf_count_pool_t pool_id);

///
/// \brief Adds a an array of buffers + offset to the pool. |offsets[n].buffer_id| is the koid of a
///        buffer that was previously imported using ImportBuffer(). The same buffer may be added to
///        multiple pools. The pool will hold on to a reference to the buffer even after
///        ReleaseBuffer is called.  When dumped into this entry, counters will be written starting
///        at |offsets[n].offset| bytes into the buffer, and up to |offsets[n].offset| +
///        |offsets[n].size|. |offsets[n].size| must be large enough to fit all enabled counters.
///        Performance counter access must have been enabled using
///        magma_connection_enable_performance_counter_access before calling this method.
/// \param connection An open connection to a device.
/// \param pool_id An existing pool.
/// \param offsets An array of offsets to add.
/// \param offsets_count The number of elements in offsets.
///
MAGMA_EXPORT magma_status_t magma_connection_add_performance_counter_buffer_offsets_to_pool(
    magma_connection_t connection,
    magma_perf_count_pool_t pool_id,
    const magma_buffer_offset_t* offsets,
    uint64_t offsets_count);

///
/// \brief Removes every offset of a buffer from the pool. Performance counter access must have been
///        enabled using magma_connection_enable_performance_counter_access before calling this
///        method.
/// \param connection An open connection to a device.
/// \param pool_id An existing pool.
/// \param buffer A magma_buffer
///
MAGMA_EXPORT magma_status_t magma_connection_remove_performance_counter_buffer_from_pool(
    magma_connection_t connection,
    magma_perf_count_pool_t pool_id,
    magma_buffer_t buffer);

///
/// \brief Triggers dumping of the performance counters into a buffer pool. May fail silently if
///        there are no buffers in the pool. |trigger_id| is an arbitrary ID assigned by the client
///        that can be returned in OnPerformanceCounterReadCompleted. Performance counter access
///        must have been enabled using magma_connection_enable_performance_counter_access before
///        calling this method.
/// \param connection An open connection to a device.
/// \param pool_id An existing pool
/// \param trigger_id An arbitrary ID assigned by the client that will be returned in
///        OnPerformanceCounterReadCompleted.
///
MAGMA_EXPORT magma_status_t magma_connection_dump_performance_counters(
    magma_connection_t connection,
    magma_perf_count_pool_t pool_id,
    uint32_t trigger_id);

///
/// \brief Sets the values of all listed performance counters to 0. May not be supported by some
///        hardware. Performance counter access must have been enabled using
///        magma_connection_enable_performance_counter_access before calling this method.
/// \param connection An open connection to a device.
/// \param counters An implementation-defined list of counters.
/// \param counters_count The number of entries in |counters|.
///
MAGMA_EXPORT magma_status_t magma_connection_clear_performance_counters(
    magma_connection_t connection,
    uint64_t* counters,
    uint64_t counters_count);

///
/// \brief Reads one performance counter completion event, if available.
/// \param connection An open connection to a device.
/// \param pool_id An existing pool.
/// \param trigger_id_out The trigger ID for this event.
/// \param buffer_id_out The buffer ID for this event.
/// \param buffer_offset_out The buffer offset for this event.
/// \param time_out The monotonic time this event happened.
/// \param result_flags_out A set of flags giving more information about this event.
///
MAGMA_EXPORT magma_status_t magma_connection_read_performance_counter_completion(
    magma_connection_t connection,
    magma_perf_count_pool_t pool_id,
    uint32_t* trigger_id_out,
    uint64_t* buffer_id_out,
    uint32_t* buffer_offset_out,
    uint64_t* time_out,
    uint32_t* result_flags_out);

///
/// \brief Creates an image buffer backed by a buffer collection given a DRM format and optional
///        modifier, as specified in the create info.
/// \param connection An open connection.
/// \param create_info Input parameters describing the image.
/// \param size_out The size of the image buffer in bytes
/// \param image_out The image buffer.
/// \param buffer_id_out The ID of the image buffer.
///
MAGMA_EXPORT magma_status_t magma_virt_connection_create_image2(
    magma_connection_t connection,
    magma_image_create_info_t* create_info,
    uint64_t* size_out,
    magma_buffer_t* image_out,
    magma_buffer_id_t* buffer_id_out);

///
/// \brief Returns parameters for an image created with virtmagma_create_image.
/// \param connection An open connection.
/// \param image The image buffer.
/// \param image_info_out Output parameters describing the image.
///
MAGMA_EXPORT magma_status_t magma_virt_connection_get_image_info(
    magma_connection_t connection,
    magma_buffer_t image,
    magma_image_info_t* image_info_out);

#if defined(__cplusplus)
}
#endif

// LINT.ThenChange(magma_common_defs.h:version)
#endif // SRC_GRAPHICS_LIB_MAGMA_INCLUDE_MAGMA_MAGMA_H_
