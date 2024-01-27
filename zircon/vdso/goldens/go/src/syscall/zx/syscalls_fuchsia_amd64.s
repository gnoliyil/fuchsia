// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// DO NOT EDIT. Generated from FIDL library zx by zither, a Fuchsia platform
// tool.

#include "textflag.h"


// func Sys_bti_create(iommu Handle, options uint32, bti_id uint64, out *Handle) Status
TEXT ·Sys_bti_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_bti_create(SB)

// func Sys_bti_pin(handle Handle, options uint32, vmo Handle, offset uint64, size uint64, addrs *Paddr, num_addrs uint, pmt *Handle) Status
TEXT ·Sys_bti_pin(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_bti_pin(SB)

// func Sys_bti_release_quarantine(handle Handle) Status
TEXT ·Sys_bti_release_quarantine(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_bti_release_quarantine(SB)

// func Sys_cache_flush(addr unsafe.Pointer, size uint, options uint32) Status
TEXT ·Sys_cache_flush(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_cache_flush(SB)

// func Sys_channel_call(handle Handle, options uint32, deadline Time, args *ChannelCallArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call(SB)

// func Sys_channel_call_etc(handle Handle, options uint32, deadline Time, args *ChannelCallEtcArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call_etc(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call_etc(SB)

// func Sys_channel_call_etc_finish(deadline Time, args *ChannelCallEtcArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call_etc_finish(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call_etc_finish(SB)

// func Sys_channel_call_etc_noretry(handle Handle, options uint32, deadline Time, args *ChannelCallEtcArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call_etc_noretry(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call_etc_noretry(SB)

// func Sys_channel_call_finish(deadline Time, args *ChannelCallArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call_finish(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call_finish(SB)

// func Sys_channel_call_noretry(handle Handle, options uint32, deadline Time, args *ChannelCallArgs, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_call_noretry(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_call_noretry(SB)

// func Sys_channel_create(options uint32, out0 *Handle, out1 *Handle) Status
TEXT ·Sys_channel_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_create(SB)

// func Sys_channel_read(handle Handle, options uint32, bytes unsafe.Pointer, handles *Handle, num_bytes uint32, num_handles uint32, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_read(SB)

// func Sys_channel_read_etc(handle Handle, options uint32, bytes unsafe.Pointer, handles *HandleInfo, num_bytes uint32, num_handles uint32, actual_bytes *uint32, actual_handles *uint32) Status
TEXT ·Sys_channel_read_etc(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_read_etc(SB)

// func Sys_channel_write(handle Handle, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles *Handle, num_handles uint32) Status
TEXT ·Sys_channel_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_write(SB)

// func Sys_channel_write_etc(handle Handle, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles *HandleDisposition, num_handles uint32) Status
TEXT ·Sys_channel_write_etc(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_channel_write_etc(SB)

// func Sys_clock_create(options uint64, args unsafe.Pointer, out *Handle) Status
TEXT ·Sys_clock_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_create(SB)

// func Sys_clock_get_details(handle Handle, options uint64, details unsafe.Pointer) Status
TEXT ·Sys_clock_get_details(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_get_details(SB)

// func Sys_clock_get_monotonic() Time
TEXT ·Sys_clock_get_monotonic(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_get_monotonic(SB)

// func Sys_clock_get_monotonic_via_kernel() Time
TEXT ·Sys_clock_get_monotonic_via_kernel(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_get_monotonic_via_kernel(SB)

// func Sys_clock_read(handle Handle, now *Time) Status
TEXT ·Sys_clock_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_read(SB)

// func Sys_clock_update(handle Handle, options uint64, args unsafe.Pointer) Status
TEXT ·Sys_clock_update(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_clock_update(SB)

// func Sys_cprng_add_entropy(buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_cprng_add_entropy(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_cprng_add_entropy(SB)

// func Sys_cprng_draw(buffer unsafe.Pointer, buffer_size uint)
TEXT ·Sys_cprng_draw(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_cprng_draw(SB)

// func Sys_cprng_draw_once(buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_cprng_draw_once(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_cprng_draw_once(SB)

// func Sys_deadline_after(nanoseconds Duration) Time
TEXT ·Sys_deadline_after(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_deadline_after(SB)

// func Sys_debug_read(handle Handle, buffer *byte, buffer_size uint, actual *uint) Status
TEXT ·Sys_debug_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debug_read(SB)

// func Sys_debug_send_command(resource Handle, buffer *byte, buffer_size uint) Status
TEXT ·Sys_debug_send_command(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debug_send_command(SB)

// func Sys_debug_write(buffer *byte, buffer_size uint) Status
TEXT ·Sys_debug_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debug_write(SB)

// func Sys_debuglog_create(resource Handle, options uint32, out *Handle) Status
TEXT ·Sys_debuglog_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debuglog_create(SB)

// func Sys_debuglog_read(handle Handle, options uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_debuglog_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debuglog_read(SB)

// func Sys_debuglog_write(handle Handle, options uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_debuglog_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_debuglog_write(SB)

// func Sys_event_create(options uint32, out *Handle) Status
TEXT ·Sys_event_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_event_create(SB)

// func Sys_eventpair_create(options uint32, out0 *Handle, out1 *Handle) Status
TEXT ·Sys_eventpair_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_eventpair_create(SB)

// func Sys_exception_get_process(handle Handle, out *Handle) Status
TEXT ·Sys_exception_get_process(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_exception_get_process(SB)

// func Sys_exception_get_thread(handle Handle, out *Handle) Status
TEXT ·Sys_exception_get_thread(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_exception_get_thread(SB)

// func Sys_fifo_create(elem_count uint, elem_size uint, options uint32, out0 *Handle, out1 *Handle) Status
TEXT ·Sys_fifo_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_fifo_create(SB)

// func Sys_fifo_read(handle Handle, elem_size uint, data unsafe.Pointer, data_size uint, actual_count *uint) Status
TEXT ·Sys_fifo_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_fifo_read(SB)

// func Sys_fifo_write(handle Handle, elem_size uint, data unsafe.Pointer, count uint, actual_count *uint) Status
TEXT ·Sys_fifo_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_fifo_write(SB)

// func Sys_framebuffer_get_info(resource Handle, format *uint32, width *uint32, height *uint32, stride *uint32) Status
TEXT ·Sys_framebuffer_get_info(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_framebuffer_get_info(SB)

// func Sys_framebuffer_set_range(resource Handle, vmo Handle, len uint32, format uint32, width uint32, height uint32, stride uint32) Status
TEXT ·Sys_framebuffer_set_range(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_framebuffer_set_range(SB)

// func Sys_futex_get_owner(value_ptr *Futex, koid *Koid) Status
TEXT ·Sys_futex_get_owner(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_get_owner(SB)

// func Sys_futex_requeue(value_ptr *Futex, wake_count uint32, current_value Futex, requeue_ptr *Futex, requeue_count uint32, new_requeue_owner Handle) Status
TEXT ·Sys_futex_requeue(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_requeue(SB)

// func Sys_futex_requeue_single_owner(value_ptr *Futex, current_value Futex, requeue_ptr *Futex, requeue_count uint32, new_requeue_owner Handle) Status
TEXT ·Sys_futex_requeue_single_owner(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_requeue_single_owner(SB)

// func Sys_futex_wait(value_ptr *Futex, current_value Futex, new_futex_owner Handle, deadline Time) Status
TEXT ·Sys_futex_wait(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_wait(SB)

// func Sys_futex_wake(value_ptr *Futex, wake_count uint32) Status
TEXT ·Sys_futex_wake(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_wake(SB)

// func Sys_futex_wake_handle_close_thread_exit(value_ptr *Futex, wake_count uint32, new_value int32, close_handle Handle)
TEXT ·Sys_futex_wake_handle_close_thread_exit(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_wake_handle_close_thread_exit(SB)

// func Sys_futex_wake_single_owner(value_ptr *Futex) Status
TEXT ·Sys_futex_wake_single_owner(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_futex_wake_single_owner(SB)

// func Sys_guest_create(resource Handle, options uint32, guest_handle *Handle, vmar_handle *Handle) Status
TEXT ·Sys_guest_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_guest_create(SB)

// func Sys_guest_set_trap(handle Handle, kind uint32, addr Vaddr, size uint, port_handle Handle, key uint64) Status
TEXT ·Sys_guest_set_trap(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_guest_set_trap(SB)

// func Sys_handle_close(handle Handle) Status
TEXT ·Sys_handle_close(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_handle_close(SB)

// func Sys_handle_close_many(handles *Handle, num_handles uint) Status
TEXT ·Sys_handle_close_many(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_handle_close_many(SB)

// func Sys_handle_duplicate(handle Handle, rights Rights, out *Handle) Status
TEXT ·Sys_handle_duplicate(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_handle_duplicate(SB)

// func Sys_handle_replace(handle Handle, rights Rights, out *Handle) Status
TEXT ·Sys_handle_replace(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_handle_replace(SB)

// func Sys_interrupt_ack(handle Handle) Status
TEXT ·Sys_interrupt_ack(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_ack(SB)

// func Sys_interrupt_bind(handle Handle, port_handle Handle, key uint64, options uint32) Status
TEXT ·Sys_interrupt_bind(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_bind(SB)

// func Sys_interrupt_create(src_obj Handle, src_num uint32, options uint32, out_handle *Handle) Status
TEXT ·Sys_interrupt_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_create(SB)

// func Sys_interrupt_destroy(handle Handle) Status
TEXT ·Sys_interrupt_destroy(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_destroy(SB)

// func Sys_interrupt_trigger(handle Handle, options uint32, timestamp Time) Status
TEXT ·Sys_interrupt_trigger(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_trigger(SB)

// func Sys_interrupt_wait(handle Handle, out_timestamp *Time) Status
TEXT ·Sys_interrupt_wait(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_interrupt_wait(SB)

// func Sys_iommu_create(resource Handle, typ uint32, desc unsafe.Pointer, desc_size uint, out *Handle) Status
TEXT ·Sys_iommu_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_iommu_create(SB)

// func Sys_ioports_release(resource Handle, io_addr uint16, len uint32) Status
TEXT ·Sys_ioports_release(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ioports_release(SB)

// func Sys_ioports_request(resource Handle, io_addr uint16, len uint32) Status
TEXT ·Sys_ioports_request(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ioports_request(SB)

// func Sys_job_create(parent_job Handle, options uint32, out *Handle) Status
TEXT ·Sys_job_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_job_create(SB)

// func Sys_job_set_critical(job Handle, options uint32, process Handle) Status
TEXT ·Sys_job_set_critical(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_job_set_critical(SB)

// func Sys_job_set_policy(handle Handle, options uint32, topic uint32, policy unsafe.Pointer, policy_size uint32) Status
TEXT ·Sys_job_set_policy(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_job_set_policy(SB)

// func Sys_ktrace_control(handle Handle, action uint32, options uint32, ptr unsafe.Pointer) Status
TEXT ·Sys_ktrace_control(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ktrace_control(SB)

// func Sys_ktrace_read(handle Handle, data unsafe.Pointer, offset uint32, data_size uint, actual *uint) Status
TEXT ·Sys_ktrace_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ktrace_read(SB)

// func Sys_ktrace_write(handle Handle, id uint32, arg0 uint32, arg1 uint32) Status
TEXT ·Sys_ktrace_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ktrace_write(SB)

// func Sys_msi_allocate(handle Handle, count uint32, out_allocation *Handle) Status
TEXT ·Sys_msi_allocate(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_msi_allocate(SB)

// func Sys_msi_create(handle Handle, options uint32, msi_id uint32, vmo Handle, vmo_offset uint, out_interrupt *Handle) Status
TEXT ·Sys_msi_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_msi_create(SB)

// func Sys_mtrace_control(handle Handle, kind uint32, action uint32, options uint32, ptr unsafe.Pointer, ptr_size uint) Status
TEXT ·Sys_mtrace_control(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_mtrace_control(SB)

// func Sys_nanosleep(deadline Time) Status
TEXT ·Sys_nanosleep(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_nanosleep(SB)

// func Sys_object_get_child(handle Handle, koid uint64, rights Rights, out *Handle) Status
TEXT ·Sys_object_get_child(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_get_child(SB)

// func Sys_object_get_info(handle Handle, topic uint32, buffer unsafe.Pointer, buffer_size uint, actual *uint, avail *uint) Status
TEXT ·Sys_object_get_info(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_get_info(SB)

// func Sys_object_get_property(handle Handle, property uint32, value unsafe.Pointer, value_size uint) Status
TEXT ·Sys_object_get_property(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_get_property(SB)

// func Sys_object_set_profile(handle Handle, profile Handle, options uint32) Status
TEXT ·Sys_object_set_profile(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_set_profile(SB)

// func Sys_object_set_property(handle Handle, property uint32, value unsafe.Pointer, value_size uint) Status
TEXT ·Sys_object_set_property(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_set_property(SB)

// func Sys_object_signal(handle Handle, clear_mask uint32, set_mask uint32) Status
TEXT ·Sys_object_signal(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_signal(SB)

// func Sys_object_signal_peer(handle Handle, clear_mask uint32, set_mask uint32) Status
TEXT ·Sys_object_signal_peer(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_signal_peer(SB)

// func Sys_object_wait_async(handle Handle, port Handle, key uint64, signals Signals, options uint32) Status
TEXT ·Sys_object_wait_async(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_wait_async(SB)

// func Sys_object_wait_many(items *WaitItem, num_items uint, deadline Time) Status
TEXT ·Sys_object_wait_many(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_wait_many(SB)

// func Sys_object_wait_one(handle Handle, signals Signals, deadline Time, observed *Signals) Status
TEXT ·Sys_object_wait_one(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_object_wait_one(SB)

// func Sys_pager_create(options uint32, out *Handle) Status
TEXT ·Sys_pager_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_create(SB)

// func Sys_pager_create_vmo(pager Handle, options uint32, port Handle, key uint64, size uint64, out *Handle) Status
TEXT ·Sys_pager_create_vmo(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_create_vmo(SB)

// func Sys_pager_detach_vmo(pager Handle, vmo Handle) Status
TEXT ·Sys_pager_detach_vmo(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_detach_vmo(SB)

// func Sys_pager_op_range(pager Handle, op uint32, pager_vmo Handle, offset uint64, length uint64, data uint64) Status
TEXT ·Sys_pager_op_range(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_op_range(SB)

// func Sys_pager_query_dirty_ranges(pager Handle, pager_vmo Handle, offset uint64, length uint64, buffer unsafe.Pointer, buffer_size uint, actual *uint, avail *uint) Status
TEXT ·Sys_pager_query_dirty_ranges(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_query_dirty_ranges(SB)

// func Sys_pager_query_vmo_stats(pager Handle, pager_vmo Handle, options uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_pager_query_vmo_stats(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_query_vmo_stats(SB)

// func Sys_pager_supply_pages(pager Handle, pager_vmo Handle, offset uint64, length uint64, aux_vmo Handle, aux_offset uint64) Status
TEXT ·Sys_pager_supply_pages(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pager_supply_pages(SB)

// func Sys_pc_firmware_tables(handle Handle, acpi_rsdp *Paddr, smbios *Paddr) Status
TEXT ·Sys_pc_firmware_tables(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pc_firmware_tables(SB)

// func Sys_pci_add_subtract_io_range(handle Handle, mmio uint32, base uint64, len uint64, add uint32) Status
TEXT ·Sys_pci_add_subtract_io_range(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_add_subtract_io_range(SB)

// func Sys_pci_cfg_pio_rw(handle Handle, bus uint8, dev uint8, funk uint8, offset uint8, val *uint32, width uint, write uint32) Status
TEXT ·Sys_pci_cfg_pio_rw(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_cfg_pio_rw(SB)

// func Sys_pci_config_read(handle Handle, offset uint16, width uint, out_val *uint32) Status
TEXT ·Sys_pci_config_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_config_read(SB)

// func Sys_pci_config_write(handle Handle, offset uint16, width uint, val uint32) Status
TEXT ·Sys_pci_config_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_config_write(SB)

// func Sys_pci_enable_bus_master(handle Handle, enable uint32) Status
TEXT ·Sys_pci_enable_bus_master(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_enable_bus_master(SB)

// func Sys_pci_get_bar(handle Handle, bar_num uint32, out_bar *PciBar, out_handle *Handle) Status
TEXT ·Sys_pci_get_bar(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_get_bar(SB)

// func Sys_pci_get_nth_device(handle Handle, index uint32, out_info *PcieDeviceInfo, out_handle *Handle) Status
TEXT ·Sys_pci_get_nth_device(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_get_nth_device(SB)

// func Sys_pci_init(handle Handle, init_buf *PciInitArg, len uint32) Status
TEXT ·Sys_pci_init(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_init(SB)

// func Sys_pci_map_interrupt(handle Handle, which_irq int32, out_handle *Handle) Status
TEXT ·Sys_pci_map_interrupt(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_map_interrupt(SB)

// func Sys_pci_query_irq_mode(handle Handle, mode uint32, out_max_irqs *uint32) Status
TEXT ·Sys_pci_query_irq_mode(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_query_irq_mode(SB)

// func Sys_pci_reset_device(handle Handle) Status
TEXT ·Sys_pci_reset_device(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_reset_device(SB)

// func Sys_pci_set_irq_mode(handle Handle, mode uint32, requested_irq_count uint32) Status
TEXT ·Sys_pci_set_irq_mode(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pci_set_irq_mode(SB)

// func Sys_pmt_unpin(handle Handle) Status
TEXT ·Sys_pmt_unpin(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_pmt_unpin(SB)

// func Sys_port_cancel(handle Handle, source Handle, key uint64) Status
TEXT ·Sys_port_cancel(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_port_cancel(SB)

// func Sys_port_create(options uint32, out *Handle) Status
TEXT ·Sys_port_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_port_create(SB)

// func Sys_port_queue(handle Handle, packet *PortPacket) Status
TEXT ·Sys_port_queue(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_port_queue(SB)

// func Sys_port_wait(handle Handle, deadline Time, packet *PortPacket) Status
TEXT ·Sys_port_wait(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_port_wait(SB)

// func Sys_process_create(job Handle, name *byte, name_size uint, options uint32, proc_handle *Handle, vmar_handle *Handle) Status
TEXT ·Sys_process_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_create(SB)

// func Sys_process_create_shared(shared_proc Handle, options uint32, name *byte, name_size uint, proc_handle *Handle, restricted_vmar_handle *Handle) Status
TEXT ·Sys_process_create_shared(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_create_shared(SB)

// func Sys_process_exit(retcode int64)
TEXT ·Sys_process_exit(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_exit(SB)

// func Sys_process_read_memory(handle Handle, vaddr Vaddr, buffer unsafe.Pointer, buffer_size uint, actual *uint) Status
TEXT ·Sys_process_read_memory(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_read_memory(SB)

// func Sys_process_start(handle Handle, thread Handle, entry Vaddr, stack Vaddr, arg1 Handle, arg2 uintptr) Status
TEXT ·Sys_process_start(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_start(SB)

// func Sys_process_write_memory(handle Handle, vaddr Vaddr, buffer unsafe.Pointer, buffer_size uint, actual *uint) Status
TEXT ·Sys_process_write_memory(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_process_write_memory(SB)

// func Sys_profile_create(root_job Handle, options uint32, profile *ProfileInfo, out *Handle) Status
TEXT ·Sys_profile_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_profile_create(SB)

// func Sys_resource_create(parent_rsrc Handle, options uint32, base uint64, size uint, name *byte, name_size uint, resource_out *Handle) Status
TEXT ·Sys_resource_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_resource_create(SB)

// func Sys_restricted_enter(options uint32, vector_table_ptr uintptr, context uintptr) Status
TEXT ·Sys_restricted_enter(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_restricted_enter(SB)

// func Sys_restricted_read_state(buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_restricted_read_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_restricted_read_state(SB)

// func Sys_restricted_write_state(buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_restricted_write_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_restricted_write_state(SB)

// func Sys_smc_call(handle Handle, parameters *SmcParameters, out_smc_result *SmcResult) Status
TEXT ·Sys_smc_call(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_smc_call(SB)

// func Sys_socket_create(options uint32, out0 *Handle, out1 *Handle) Status
TEXT ·Sys_socket_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_socket_create(SB)

// func Sys_socket_read(handle Handle, options uint32, buffer unsafe.Pointer, buffer_size uint, actual *uint) Status
TEXT ·Sys_socket_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_socket_read(SB)

// func Sys_socket_set_disposition(handle Handle, disposition uint32, disposition_peer uint32) Status
TEXT ·Sys_socket_set_disposition(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_socket_set_disposition(SB)

// func Sys_socket_write(handle Handle, options uint32, buffer unsafe.Pointer, buffer_size uint, actual *uint) Status
TEXT ·Sys_socket_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_socket_write(SB)

// func Sys_stream_create(options uint32, vmo Handle, seek Off, out_stream *Handle) Status
TEXT ·Sys_stream_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_create(SB)

// func Sys_stream_readv(handle Handle, options uint32, vectors *Iovec, num_vectors uint, actual *uint) Status
TEXT ·Sys_stream_readv(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_readv(SB)

// func Sys_stream_readv_at(handle Handle, options uint32, offset Off, vectors *Iovec, num_vectors uint, actual *uint) Status
TEXT ·Sys_stream_readv_at(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_readv_at(SB)

// func Sys_stream_seek(handle Handle, whence StreamSeekOrigin, offset int64, out_seek *Off) Status
TEXT ·Sys_stream_seek(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_seek(SB)

// func Sys_stream_writev(handle Handle, options uint32, vectors *Iovec, num_vectors uint, actual *uint) Status
TEXT ·Sys_stream_writev(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_writev(SB)

// func Sys_stream_writev_at(handle Handle, options uint32, offset Off, vectors *Iovec, num_vectors uint, actual *uint) Status
TEXT ·Sys_stream_writev_at(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_stream_writev_at(SB)

// func Sys_syscall_next_1(arg int32) Status
TEXT ·Sys_syscall_next_1(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_next_1(SB)

// func Sys_syscall_test_handle_create(return_value Status, out *Handle) Status
TEXT ·Sys_syscall_test_handle_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_handle_create(SB)

// func Sys_syscall_test_widening_signed_narrow(a int64, b int32, c int16, d int8) int64
TEXT ·Sys_syscall_test_widening_signed_narrow(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_widening_signed_narrow(SB)

// func Sys_syscall_test_widening_signed_wide(a int64, b int32, c int16, d int8) int64
TEXT ·Sys_syscall_test_widening_signed_wide(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_widening_signed_wide(SB)

// func Sys_syscall_test_widening_unsigned_narrow(a uint64, b uint32, c uint16, d uint8) uint64
TEXT ·Sys_syscall_test_widening_unsigned_narrow(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_widening_unsigned_narrow(SB)

// func Sys_syscall_test_widening_unsigned_wide(a uint64, b uint32, c uint16, d uint8) uint64
TEXT ·Sys_syscall_test_widening_unsigned_wide(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_widening_unsigned_wide(SB)

// func Sys_syscall_test_wrapper(a int32, b int32, c int32) Status
TEXT ·Sys_syscall_test_wrapper(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_wrapper(SB)

// func Sys_syscall_test_0() Status
TEXT ·Sys_syscall_test_0(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_0(SB)

// func Sys_syscall_test_1(a int32) Status
TEXT ·Sys_syscall_test_1(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_1(SB)

// func Sys_syscall_test_2(a int32, b int32) Status
TEXT ·Sys_syscall_test_2(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_2(SB)

// func Sys_syscall_test_3(a int32, b int32, c int32) Status
TEXT ·Sys_syscall_test_3(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_3(SB)

// func Sys_syscall_test_4(a int32, b int32, c int32, d int32) Status
TEXT ·Sys_syscall_test_4(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_4(SB)

// func Sys_syscall_test_5(a int32, b int32, c int32, d int32, e int32) Status
TEXT ·Sys_syscall_test_5(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_5(SB)

// func Sys_syscall_test_6(a int32, b int32, c int32, d int32, e int32, f int32) Status
TEXT ·Sys_syscall_test_6(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_6(SB)

// func Sys_syscall_test_7(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32) Status
TEXT ·Sys_syscall_test_7(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_7(SB)

// func Sys_syscall_test_8(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32, h int32) Status
TEXT ·Sys_syscall_test_8(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_syscall_test_8(SB)

// func Sys_system_get_dcache_line_size() uint32
TEXT ·Sys_system_get_dcache_line_size(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_dcache_line_size(SB)

// func Sys_system_get_event(root_job Handle, kind uint32, event *Handle) Status
TEXT ·Sys_system_get_event(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_event(SB)

// func Sys_system_get_features(kind uint32, features *uint32) Status
TEXT ·Sys_system_get_features(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_features(SB)

// func Sys_system_get_num_cpus() uint32
TEXT ·Sys_system_get_num_cpus(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_num_cpus(SB)

// func Sys_system_get_page_size() uint32
TEXT ·Sys_system_get_page_size(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_page_size(SB)

// func Sys_system_get_performance_info(resource Handle, topic uint32, count uint, info unsafe.Pointer, output_count *uint) Status
TEXT ·Sys_system_get_performance_info(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_performance_info(SB)

// func Sys_system_get_physmem() uint64
TEXT ·Sys_system_get_physmem(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_physmem(SB)

// func Sys_system_get_version_string() StringView
TEXT ·Sys_system_get_version_string(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_get_version_string(SB)

// func Sys_system_mexec(resource Handle, kernel_vmo Handle, bootimage_vmo Handle) Status
TEXT ·Sys_system_mexec(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_mexec(SB)

// func Sys_system_mexec_payload_get(resource Handle, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_system_mexec_payload_get(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_mexec_payload_get(SB)

// func Sys_system_powerctl(resource Handle, cmd uint32, arg *SystemPowerctlArg) Status
TEXT ·Sys_system_powerctl(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_powerctl(SB)

// func Sys_system_set_performance_info(resource Handle, topic uint32, info unsafe.Pointer, count uint) Status
TEXT ·Sys_system_set_performance_info(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_system_set_performance_info(SB)

// func Sys_task_create_exception_channel(handle Handle, options uint32, out *Handle) Status
TEXT ·Sys_task_create_exception_channel(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_task_create_exception_channel(SB)

// func Sys_task_kill(handle Handle) Status
TEXT ·Sys_task_kill(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_task_kill(SB)

// func Sys_task_suspend(handle Handle, token *Handle) Status
TEXT ·Sys_task_suspend(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_task_suspend(SB)

// func Sys_task_suspend_token(handle Handle, token *Handle) Status
TEXT ·Sys_task_suspend_token(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_task_suspend_token(SB)

// func Sys_thread_create(process Handle, name *byte, name_size uint, options uint32, out *Handle) Status
TEXT ·Sys_thread_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_create(SB)

// func Sys_thread_exit()
TEXT ·Sys_thread_exit(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_exit(SB)

// func Sys_thread_legacy_yield(options uint32) Status
TEXT ·Sys_thread_legacy_yield(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_legacy_yield(SB)

// func Sys_thread_read_state(handle Handle, kind uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_thread_read_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_read_state(SB)

// func Sys_thread_start(handle Handle, thread_entry Vaddr, stack Vaddr, arg1 uintptr, arg2 uintptr) Status
TEXT ·Sys_thread_start(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_start(SB)

// func Sys_thread_write_state(handle Handle, kind uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_thread_write_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_thread_write_state(SB)

// func Sys_ticks_get() Ticks
TEXT ·Sys_ticks_get(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ticks_get(SB)

// func Sys_ticks_get_via_kernel() Ticks
TEXT ·Sys_ticks_get_via_kernel(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ticks_get_via_kernel(SB)

// func Sys_ticks_per_second() Ticks
TEXT ·Sys_ticks_per_second(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_ticks_per_second(SB)

// func Sys_timer_cancel(handle Handle) Status
TEXT ·Sys_timer_cancel(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_timer_cancel(SB)

// func Sys_timer_create(options uint32, clock_id Clock, out *Handle) Status
TEXT ·Sys_timer_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_timer_create(SB)

// func Sys_timer_set(handle Handle, deadline Time, slack Duration) Status
TEXT ·Sys_timer_set(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_timer_set(SB)

// func Sys_vcpu_create(guest Handle, options uint32, entry Vaddr, out *Handle) Status
TEXT ·Sys_vcpu_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_create(SB)

// func Sys_vcpu_enter(handle Handle, packet *PortPacket) Status
TEXT ·Sys_vcpu_enter(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_enter(SB)

// func Sys_vcpu_interrupt(handle Handle, vector uint32) Status
TEXT ·Sys_vcpu_interrupt(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_interrupt(SB)

// func Sys_vcpu_kick(handle Handle) Status
TEXT ·Sys_vcpu_kick(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_kick(SB)

// func Sys_vcpu_read_state(handle Handle, kind uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_vcpu_read_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_read_state(SB)

// func Sys_vcpu_write_state(handle Handle, kind uint32, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_vcpu_write_state(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vcpu_write_state(SB)

// func Sys_vmar_allocate(parent_vmar Handle, options VmOption, offset uint, size uint, child_vmar *Handle, child_addr *Vaddr) Status
TEXT ·Sys_vmar_allocate(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_allocate(SB)

// func Sys_vmar_destroy(handle Handle) Status
TEXT ·Sys_vmar_destroy(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_destroy(SB)

// func Sys_vmar_map(handle Handle, options VmOption, vmar_offset uint, vmo Handle, vmo_offset uint64, len uint, mapped_addr *Vaddr) Status
TEXT ·Sys_vmar_map(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_map(SB)

// func Sys_vmar_op_range(handle Handle, op uint32, address Vaddr, size uint, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_vmar_op_range(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_op_range(SB)

// func Sys_vmar_protect(handle Handle, options VmOption, addr Vaddr, len uint) Status
TEXT ·Sys_vmar_protect(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_protect(SB)

// func Sys_vmar_unmap(handle Handle, addr Vaddr, len uint) Status
TEXT ·Sys_vmar_unmap(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_unmap(SB)

// func Sys_vmar_unmap_handle_close_thread_exit(vmar_handle Handle, addr Vaddr, size uint, close_handle Handle) Status
TEXT ·Sys_vmar_unmap_handle_close_thread_exit(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmar_unmap_handle_close_thread_exit(SB)

// func Sys_vmo_create(size uint64, options uint32, out *Handle) Status
TEXT ·Sys_vmo_create(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_create(SB)

// func Sys_vmo_create_child(handle Handle, options uint32, offset uint64, size uint64, out *Handle) Status
TEXT ·Sys_vmo_create_child(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_create_child(SB)

// func Sys_vmo_create_contiguous(bti Handle, size uint, alignment_log2 uint32, out *Handle) Status
TEXT ·Sys_vmo_create_contiguous(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_create_contiguous(SB)

// func Sys_vmo_create_physical(resource Handle, paddr Paddr, size uint, out *Handle) Status
TEXT ·Sys_vmo_create_physical(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_create_physical(SB)

// func Sys_vmo_get_size(handle Handle, size *uint64) Status
TEXT ·Sys_vmo_get_size(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_get_size(SB)

// func Sys_vmo_op_range(handle Handle, op uint32, offset uint64, size uint64, buffer unsafe.Pointer, buffer_size uint) Status
TEXT ·Sys_vmo_op_range(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_op_range(SB)

// func Sys_vmo_read(handle Handle, buffer unsafe.Pointer, offset uint64, buffer_size uint) Status
TEXT ·Sys_vmo_read(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_read(SB)

// func Sys_vmo_replace_as_executable(handle Handle, vmex Handle, out *Handle) Status
TEXT ·Sys_vmo_replace_as_executable(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_replace_as_executable(SB)

// func Sys_vmo_set_cache_policy(handle Handle, cache_policy uint32) Status
TEXT ·Sys_vmo_set_cache_policy(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_set_cache_policy(SB)

// func Sys_vmo_set_size(handle Handle, size uint64) Status
TEXT ·Sys_vmo_set_size(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_set_size(SB)

// func Sys_vmo_write(handle Handle, buffer unsafe.Pointer, offset uint64, buffer_size uint) Status
TEXT ·Sys_vmo_write(SB),NOSPLIT,$0
	JMP runtime·vdsoCall_zx_vmo_write(SB)
