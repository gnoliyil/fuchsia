// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// DO NOT EDIT. Generated from FIDL library zx by zither, a Fuchsia platform
// tool.

package runtime

import "unsafe"

const (
	// vdsoArrayMax is the byte-size of a maximally sized array on this architecture.
	// See cmd/compile/internal/amd64/galign.go arch.MAXWIDTH initialization.
	vdsoArrayMax = 1<<50 - 1
)

var vdsoSymbolKeys = []vdsoSymbolKey{
	{"_zx_bti_create", 0x7833987, &vdso_zx_bti_create},
	{"_zx_bti_pin", 0x2aa0e6da, &vdso_zx_bti_pin},
	{"_zx_bti_release_quarantine", 0x441c1c6b, &vdso_zx_bti_release_quarantine},
	{"_zx_cache_flush", 0x319eccca, &vdso_zx_cache_flush},
	{"_zx_channel_call", 0xe70e51c9, &vdso_zx_channel_call},
	{"_zx_channel_call_etc", 0x8639e344, &vdso_zx_channel_call_etc},
	{"_zx_channel_call_etc_finish", 0xd3cdf704, &vdso_zx_channel_call_etc_finish},
	{"_zx_channel_call_etc_noretry", 0xc3a3e7d6, &vdso_zx_channel_call_etc_noretry},
	{"_zx_channel_call_finish", 0x85ce3de9, &vdso_zx_channel_call_finish},
	{"_zx_channel_call_noretry", 0xb5ad0b5b, &vdso_zx_channel_call_noretry},
	{"_zx_channel_create", 0xe5199281, &vdso_zx_channel_create},
	{"_zx_channel_read", 0xe7169b09, &vdso_zx_channel_read},
	{"_zx_channel_read_etc", 0x77c4cc84, &vdso_zx_channel_read_etc},
	{"_zx_channel_write", 0xca4bbc18, &vdso_zx_channel_write},
	{"_zx_channel_write_etc", 0x43b1dd13, &vdso_zx_channel_write_etc},
	{"_zx_clock_create", 0x8d73ce14, &vdso_zx_clock_create},
	{"_zx_clock_get_details", 0xb7227265, &vdso_zx_clock_get_details},
	{"_zx_clock_get_monotonic", 0xb00e6115, &vdso_zx_clock_get_monotonic},
	{"_zx_clock_get_monotonic_via_kernel", 0x3dc12b54, &vdso_zx_clock_get_monotonic_via_kernel},
	{"_zx_clock_read", 0x3676d5dc, &vdso_zx_clock_read},
	{"_zx_clock_update", 0xb74bec03, &vdso_zx_clock_update},
	{"_zx_cprng_add_entropy", 0x1617dd47, &vdso_zx_cprng_add_entropy},
	{"_zx_cprng_draw", 0x12929c5c, &vdso_zx_cprng_draw},
	{"_zx_cprng_draw_once", 0x40248ce0, &vdso_zx_cprng_draw_once},
	{"_zx_deadline_after", 0x6253eb5c, &vdso_zx_deadline_after},
	{"_zx_debug_read", 0x6c062397, &vdso_zx_debug_read},
	{"_zx_debug_send_command", 0xac6e8203, &vdso_zx_debug_send_command},
	{"_zx_debug_write", 0xed2c5666, &vdso_zx_debug_write},
	{"_zx_debuglog_create", 0x2504f1, &vdso_zx_debuglog_create},
	{"_zx_debuglog_read", 0x66c2b179, &vdso_zx_debuglog_read},
	{"_zx_debuglog_write", 0x3f7aa088, &vdso_zx_debuglog_write},
	{"_zx_event_create", 0x4c39490a, &vdso_zx_event_create},
	{"_zx_eventpair_create", 0xe3fd9c16, &vdso_zx_eventpair_create},
	{"_zx_exception_get_process", 0xcddbd761, &vdso_zx_exception_get_process},
	{"_zx_exception_get_thread", 0xb1c70bba, &vdso_zx_exception_get_thread},
	{"_zx_fifo_create", 0xf197cb2c, &vdso_zx_fifo_create},
	{"_zx_fifo_read", 0x3ec8acf4, &vdso_zx_fifo_read},
	{"_zx_fifo_write", 0x18400b63, &vdso_zx_fifo_write},
	{"_zx_framebuffer_get_info", 0xe6c88924, &vdso_zx_framebuffer_get_info},
	{"_zx_framebuffer_set_range", 0x364ad6b1, &vdso_zx_framebuffer_set_range},
	{"_zx_futex_get_owner", 0xf16dec6a, &vdso_zx_futex_get_owner},
	{"_zx_futex_requeue", 0xd509be7c, &vdso_zx_futex_requeue},
	{"_zx_futex_requeue_single_owner", 0x8f9a9e7, &vdso_zx_futex_requeue_single_owner},
	{"_zx_futex_wait", 0xb089e255, &vdso_zx_futex_wait},
	{"_zx_futex_wake", 0xb089e288, &vdso_zx_futex_wake},
	{"_zx_futex_wake_handle_close_thread_exit", 0x49731cb8, &vdso_zx_futex_wake_handle_close_thread_exit},
	{"_zx_futex_wake_single_owner", 0x63970173, &vdso_zx_futex_wake_single_owner},
	{"_zx_guest_create", 0x6f318390, &vdso_zx_guest_create},
	{"_zx_guest_set_trap", 0xffe2547e, &vdso_zx_guest_set_trap},
	{"_zx_handle_close", 0xe769f876, &vdso_zx_handle_close},
	{"_zx_handle_close_many", 0x8a9a3aaa, &vdso_zx_handle_close_many},
	{"_zx_handle_duplicate", 0x3f0a83b, &vdso_zx_handle_duplicate},
	{"_zx_handle_replace", 0xdc2d9edc, &vdso_zx_handle_replace},
	{"_zx_interrupt_ack", 0x3b390f10, &vdso_zx_interrupt_ack},
	{"_zx_interrupt_bind", 0xa25b97be, &vdso_zx_interrupt_bind},
	{"_zx_interrupt_create", 0xaa939795, &vdso_zx_interrupt_create},
	{"_zx_interrupt_destroy", 0x2cb5724b, &vdso_zx_interrupt_destroy},
	{"_zx_interrupt_trigger", 0x19f00875, &vdso_zx_interrupt_trigger},
	{"_zx_interrupt_wait", 0xa266f916, &vdso_zx_interrupt_wait},
	{"_zx_iommu_create", 0x297b6af, &vdso_zx_iommu_create},
	{"_zx_ioports_release", 0xb88e6f05, &vdso_zx_ioports_release},
	{"_zx_ioports_request", 0xb8f1c0ad, &vdso_zx_ioports_request},
	{"_zx_job_create", 0x6b9cbb63, &vdso_zx_job_create},
	{"_zx_job_set_critical", 0x129ab785, &vdso_zx_job_set_critical},
	{"_zx_job_set_policy", 0xa45d60ea, &vdso_zx_job_set_policy},
	{"_zx_ktrace_control", 0x15debecf, &vdso_zx_ktrace_control},
	{"_zx_ktrace_read", 0x7a59dbca, &vdso_zx_ktrace_read},
	{"_zx_ktrace_write", 0xc5f714f9, &vdso_zx_ktrace_write},
	{"_zx_msi_allocate", 0xf5370e42, &vdso_zx_msi_allocate},
	{"_zx_msi_create", 0xbf7b04f1, &vdso_zx_msi_create},
	{"_zx_mtrace_control", 0x8c5f3211, &vdso_zx_mtrace_control},
	{"_zx_nanosleep", 0xe9d6145a, &vdso_zx_nanosleep},
	{"_zx_object_get_child", 0x256ecc2e, &vdso_zx_object_get_child},
	{"_zx_object_get_info", 0x7582ddf6, &vdso_zx_object_get_info},
	{"_zx_object_get_property", 0xd60c8aef, &vdso_zx_object_get_property},
	{"_zx_object_set_profile", 0x7d1d2727, &vdso_zx_object_set_profile},
	{"_zx_object_set_property", 0x2174eb7b, &vdso_zx_object_set_property},
	{"_zx_object_signal", 0x460ab89, &vdso_zx_object_signal},
	{"_zx_object_signal_peer", 0xe90c8694, &vdso_zx_object_signal_peer},
	{"_zx_object_wait_async", 0x61e4dcdd, &vdso_zx_object_wait_async},
	{"_zx_object_wait_many", 0x9e247bd4, &vdso_zx_object_wait_many},
	{"_zx_object_wait_one", 0xed850621, &vdso_zx_object_wait_one},
	{"_zx_pager_create", 0x8d44ed57, &vdso_zx_pager_create},
	{"_zx_pager_create_vmo", 0x564fdd48, &vdso_zx_pager_create_vmo},
	{"_zx_pager_detach_vmo", 0x2072c15d, &vdso_zx_pager_detach_vmo},
	{"_zx_pager_op_range", 0x5e8195ae, &vdso_zx_pager_op_range},
	{"_zx_pager_query_dirty_ranges", 0x1e13a323, &vdso_zx_pager_query_dirty_ranges},
	{"_zx_pager_query_vmo_stats", 0xd3f95338, &vdso_zx_pager_query_vmo_stats},
	{"_zx_pager_supply_pages", 0x69d1fc7f, &vdso_zx_pager_supply_pages},
	{"_zx_pc_firmware_tables", 0x1a05d1fe, &vdso_zx_pc_firmware_tables},
	{"_zx_pci_add_subtract_io_range", 0x5fcd4b03, &vdso_zx_pci_add_subtract_io_range},
	{"_zx_pci_cfg_pio_rw", 0xda224f4f, &vdso_zx_pci_cfg_pio_rw},
	{"_zx_pci_config_read", 0xc765eb61, &vdso_zx_pci_config_read},
	{"_zx_pci_config_write", 0xb4851770, &vdso_zx_pci_config_write},
	{"_zx_pci_enable_bus_master", 0x76091cab, &vdso_zx_pci_enable_bus_master},
	{"_zx_pci_get_bar", 0x14ec9dc4, &vdso_zx_pci_get_bar},
	{"_zx_pci_get_nth_device", 0x32106f08, &vdso_zx_pci_get_nth_device},
	{"_zx_pci_init", 0x4db59c04, &vdso_zx_pci_init},
	{"_zx_pci_map_interrupt", 0x3c5aa3fa, &vdso_zx_pci_map_interrupt},
	{"_zx_pci_query_irq_mode", 0xdf173875, &vdso_zx_pci_query_irq_mode},
	{"_zx_pci_reset_device", 0xc6496142, &vdso_zx_pci_reset_device},
	{"_zx_pci_set_irq_mode", 0xedfcadcb, &vdso_zx_pci_set_irq_mode},
	{"_zx_pmt_unpin", 0x8e954c6f, &vdso_zx_pmt_unpin},
	{"_zx_port_cancel", 0x5166105f, &vdso_zx_port_cancel},
	{"_zx_port_create", 0x5294baed, &vdso_zx_port_create},
	{"_zx_port_queue", 0x8f22883e, &vdso_zx_port_queue},
	{"_zx_port_wait", 0xfc97666e, &vdso_zx_port_wait},
	{"_zx_process_create", 0xa3a21647, &vdso_zx_process_create},
	{"_zx_process_create_shared", 0xcc9e43dd, &vdso_zx_process_create_shared},
	{"_zx_process_exit", 0xc7f8a64d, &vdso_zx_process_exit},
	{"_zx_process_read_memory", 0x883ab627, &vdso_zx_process_read_memory},
	{"_zx_process_start", 0xc80873a1, &vdso_zx_process_start},
	{"_zx_process_write_memory", 0x18162116, &vdso_zx_process_write_memory},
	{"_zx_profile_create", 0x28e1bf39, &vdso_zx_profile_create},
	{"_zx_resource_create", 0x22a0d150, &vdso_zx_resource_create},
	{"_zx_restricted_enter", 0x3dfea9eb, &vdso_zx_restricted_enter},
	{"_zx_restricted_read_state", 0x2b1936e9, &vdso_zx_restricted_read_state},
	{"_zx_restricted_write_state", 0x64c81738, &vdso_zx_restricted_write_state},
	{"_zx_smc_call", 0x63f0533, &vdso_zx_smc_call},
	{"_zx_socket_create", 0xf536e851, &vdso_zx_socket_create},
	{"_zx_socket_read", 0xb5443cd9, &vdso_zx_socket_read},
	{"_zx_socket_set_disposition", 0x63347abd, &vdso_zx_socket_set_disposition},
	{"_zx_socket_write", 0x5e2d97e8, &vdso_zx_socket_write},
	{"_zx_stream_create", 0x5a685d14, &vdso_zx_stream_create},
	{"_zx_stream_readv", 0xae7040d2, &vdso_zx_stream_readv},
	{"_zx_stream_readv_at", 0x7bfd08a6, &vdso_zx_stream_readv_at},
	{"_zx_stream_seek", 0xcefc31c8, &vdso_zx_stream_seek},
	{"_zx_stream_writev", 0x89120a21, &vdso_zx_stream_writev},
	{"_zx_stream_writev_at", 0xcd618395, &vdso_zx_stream_writev_at},
	{"_zx_syscall_next_1", 0xa83386fe, &vdso_zx_syscall_next_1},
	{"_zx_syscall_test_handle_create", 0xb996e56d, &vdso_zx_syscall_test_handle_create},
	{"_zx_syscall_test_widening_signed_narrow", 0x215c6254, &vdso_zx_syscall_test_widening_signed_narrow},
	{"_zx_syscall_test_widening_signed_wide", 0x6b7b27a4, &vdso_zx_syscall_test_widening_signed_wide},
	{"_zx_syscall_test_widening_unsigned_narrow", 0x2b822f37, &vdso_zx_syscall_test_widening_unsigned_narrow},
	{"_zx_syscall_test_widening_unsigned_wide", 0xbaf25fc7, &vdso_zx_syscall_test_widening_unsigned_wide},
	{"_zx_syscall_test_wrapper", 0x4d7af9cf, &vdso_zx_syscall_test_wrapper},
	{"_zx_syscall_test_0", 0xb62fbcde, &vdso_zx_syscall_test_0},
	{"_zx_syscall_test_1", 0xb62fbcdf, &vdso_zx_syscall_test_1},
	{"_zx_syscall_test_2", 0xb62fbce0, &vdso_zx_syscall_test_2},
	{"_zx_syscall_test_3", 0xb62fbce1, &vdso_zx_syscall_test_3},
	{"_zx_syscall_test_4", 0xb62fbce2, &vdso_zx_syscall_test_4},
	{"_zx_syscall_test_5", 0xb62fbce3, &vdso_zx_syscall_test_5},
	{"_zx_syscall_test_6", 0xb62fbce4, &vdso_zx_syscall_test_6},
	{"_zx_syscall_test_7", 0xb62fbce5, &vdso_zx_syscall_test_7},
	{"_zx_syscall_test_8", 0xb62fbce6, &vdso_zx_syscall_test_8},
	{"_zx_system_get_dcache_line_size", 0x2d6d6511, &vdso_zx_system_get_dcache_line_size},
	{"_zx_system_get_event", 0x7a0b68da, &vdso_zx_system_get_event},
	{"_zx_system_get_features", 0x42682df7, &vdso_zx_system_get_features},
	{"_zx_system_get_num_cpus", 0x8e92a0c2, &vdso_zx_system_get_num_cpus},
	{"_zx_system_get_page_size", 0x1495920f, &vdso_zx_system_get_page_size},
	{"_zx_system_get_performance_info", 0x85f5f115, &vdso_zx_system_get_performance_info},
	{"_zx_system_get_physmem", 0x5a0e027b, &vdso_zx_system_get_physmem},
	{"_zx_system_get_version_string", 0xf2daeaf4, &vdso_zx_system_get_version_string},
	{"_zx_system_mexec", 0xd142362b, &vdso_zx_system_mexec},
	{"_zx_system_mexec_payload_get", 0x34bd22b3, &vdso_zx_system_mexec_payload_get},
	{"_zx_system_powerctl", 0x43f6ae09, &vdso_zx_system_powerctl},
	{"_zx_system_set_performance_info", 0x17cc1da1, &vdso_zx_system_set_performance_info},
	{"_zx_task_create_exception_channel", 0x5318f181, &vdso_zx_task_create_exception_channel},
	{"_zx_task_kill", 0x1ae4e313, &vdso_zx_task_kill},
	{"_zx_task_suspend", 0xe13ad509, &vdso_zx_task_suspend},
	{"_zx_task_suspend_token", 0x341e98a9, &vdso_zx_task_suspend_token},
	{"_zx_thread_create", 0x100e8a20, &vdso_zx_thread_create},
	{"_zx_thread_exit", 0xed44fe6, &vdso_zx_thread_exit},
	{"_zx_thread_legacy_yield", 0x1b146d17, &vdso_zx_thread_legacy_yield},
	{"_zx_thread_read_state", 0x82fd0a88, &vdso_zx_thread_read_state},
	{"_zx_thread_start", 0xea59505a, &vdso_zx_thread_start},
	{"_zx_thread_write_state", 0xb9265eb7, &vdso_zx_thread_write_state},
	{"_zx_ticks_get", 0xaeb30a32, &vdso_zx_ticks_get},
	{"_zx_ticks_get_via_kernel", 0x821bc851, &vdso_zx_ticks_get_via_kernel},
	{"_zx_ticks_per_second", 0x6ed47574, &vdso_zx_ticks_per_second},
	{"_zx_timer_cancel", 0x9308c91b, &vdso_zx_timer_cancel},
	{"_zx_timer_create", 0x943773a9, &vdso_zx_timer_create},
	{"_zx_timer_set", 0xa2689081, &vdso_zx_timer_set},
	{"_zx_vcpu_create", 0x458b1fc6, &vdso_zx_vcpu_create},
	{"_zx_vcpu_enter", 0xa5267710, &vdso_zx_vcpu_enter},
	{"_zx_vcpu_interrupt", 0x4ddc6df, &vdso_zx_vcpu_interrupt},
	{"_zx_vcpu_kick", 0xcc64db4, &vdso_zx_vcpu_kick},
	{"_zx_vcpu_read_state", 0xaa78032e, &vdso_zx_vcpu_read_state},
	{"_zx_vcpu_write_state", 0xd0006c1d, &vdso_zx_vcpu_write_state},
	{"_zx_vmar_allocate", 0x4cac85ef, &vdso_zx_vmar_allocate},
	{"_zx_vmar_destroy", 0xc2294134, &vdso_zx_vmar_destroy},
	{"_zx_vmar_map", 0xc7b00448, &vdso_zx_vmar_map},
	{"_zx_vmar_op_range", 0x4e117375, &vdso_zx_vmar_op_range},
	{"_zx_vmar_protect", 0x7bee8f8b, &vdso_zx_vmar_protect},
	{"_zx_vmar_unmap", 0x745a1b6b, &vdso_zx_vmar_unmap},
	{"_zx_vmar_unmap_handle_close_thread_exit", 0x5a372afb, &vdso_zx_vmar_unmap_handle_close_thread_exit},
	{"_zx_vmo_create", 0xb27a765a, &vdso_zx_vmo_create},
	{"_zx_vmo_create_child", 0x72c8b3dd, &vdso_zx_vmo_create_child},
	{"_zx_vmo_create_contiguous", 0x466a8289, &vdso_zx_vmo_create_contiguous},
	{"_zx_vmo_create_physical", 0x659677b6, &vdso_zx_vmo_create_physical},
	{"_zx_vmo_get_size", 0x261c77c0, &vdso_zx_vmo_get_size},
	{"_zx_vmo_op_range", 0xa73d6b71, &vdso_zx_vmo_op_range},
	{"_zx_vmo_read", 0xe70ab4a2, &vdso_zx_vmo_read},
	{"_zx_vmo_replace_as_executable", 0xbd38e576, &vdso_zx_vmo_replace_as_executable},
	{"_zx_vmo_set_cache_policy", 0xe509bad4, &vdso_zx_vmo_set_cache_policy},
	{"_zx_vmo_set_size", 0x3932724c, &vdso_zx_vmo_set_size},
	{"_zx_vmo_write", 0xc8c308d1, &vdso_zx_vmo_write},
}

//go:cgo_import_dynamic vdso_zx_bti_create zx_bti_create
//go:cgo_import_dynamic vdso_zx_bti_pin zx_bti_pin
//go:cgo_import_dynamic vdso_zx_bti_release_quarantine zx_bti_release_quarantine
//go:cgo_import_dynamic vdso_zx_cache_flush zx_cache_flush
//go:cgo_import_dynamic vdso_zx_channel_call zx_channel_call
//go:cgo_import_dynamic vdso_zx_channel_call_etc zx_channel_call_etc
//go:cgo_import_dynamic vdso_zx_channel_call_etc_finish zx_channel_call_etc_finish
//go:cgo_import_dynamic vdso_zx_channel_call_etc_noretry zx_channel_call_etc_noretry
//go:cgo_import_dynamic vdso_zx_channel_call_finish zx_channel_call_finish
//go:cgo_import_dynamic vdso_zx_channel_call_noretry zx_channel_call_noretry
//go:cgo_import_dynamic vdso_zx_channel_create zx_channel_create
//go:cgo_import_dynamic vdso_zx_channel_read zx_channel_read
//go:cgo_import_dynamic vdso_zx_channel_read_etc zx_channel_read_etc
//go:cgo_import_dynamic vdso_zx_channel_write zx_channel_write
//go:cgo_import_dynamic vdso_zx_channel_write_etc zx_channel_write_etc
//go:cgo_import_dynamic vdso_zx_clock_create zx_clock_create
//go:cgo_import_dynamic vdso_zx_clock_get_details zx_clock_get_details
//go:cgo_import_dynamic vdso_zx_clock_get_monotonic zx_clock_get_monotonic
//go:cgo_import_dynamic vdso_zx_clock_get_monotonic_via_kernel zx_clock_get_monotonic_via_kernel
//go:cgo_import_dynamic vdso_zx_clock_read zx_clock_read
//go:cgo_import_dynamic vdso_zx_clock_update zx_clock_update
//go:cgo_import_dynamic vdso_zx_cprng_add_entropy zx_cprng_add_entropy
//go:cgo_import_dynamic vdso_zx_cprng_draw zx_cprng_draw
//go:cgo_import_dynamic vdso_zx_cprng_draw_once zx_cprng_draw_once
//go:cgo_import_dynamic vdso_zx_deadline_after zx_deadline_after
//go:cgo_import_dynamic vdso_zx_debug_read zx_debug_read
//go:cgo_import_dynamic vdso_zx_debug_send_command zx_debug_send_command
//go:cgo_import_dynamic vdso_zx_debug_write zx_debug_write
//go:cgo_import_dynamic vdso_zx_debuglog_create zx_debuglog_create
//go:cgo_import_dynamic vdso_zx_debuglog_read zx_debuglog_read
//go:cgo_import_dynamic vdso_zx_debuglog_write zx_debuglog_write
//go:cgo_import_dynamic vdso_zx_event_create zx_event_create
//go:cgo_import_dynamic vdso_zx_eventpair_create zx_eventpair_create
//go:cgo_import_dynamic vdso_zx_exception_get_process zx_exception_get_process
//go:cgo_import_dynamic vdso_zx_exception_get_thread zx_exception_get_thread
//go:cgo_import_dynamic vdso_zx_fifo_create zx_fifo_create
//go:cgo_import_dynamic vdso_zx_fifo_read zx_fifo_read
//go:cgo_import_dynamic vdso_zx_fifo_write zx_fifo_write
//go:cgo_import_dynamic vdso_zx_framebuffer_get_info zx_framebuffer_get_info
//go:cgo_import_dynamic vdso_zx_framebuffer_set_range zx_framebuffer_set_range
//go:cgo_import_dynamic vdso_zx_futex_get_owner zx_futex_get_owner
//go:cgo_import_dynamic vdso_zx_futex_requeue zx_futex_requeue
//go:cgo_import_dynamic vdso_zx_futex_requeue_single_owner zx_futex_requeue_single_owner
//go:cgo_import_dynamic vdso_zx_futex_wait zx_futex_wait
//go:cgo_import_dynamic vdso_zx_futex_wake zx_futex_wake
//go:cgo_import_dynamic vdso_zx_futex_wake_handle_close_thread_exit zx_futex_wake_handle_close_thread_exit
//go:cgo_import_dynamic vdso_zx_futex_wake_single_owner zx_futex_wake_single_owner
//go:cgo_import_dynamic vdso_zx_guest_create zx_guest_create
//go:cgo_import_dynamic vdso_zx_guest_set_trap zx_guest_set_trap
//go:cgo_import_dynamic vdso_zx_handle_close zx_handle_close
//go:cgo_import_dynamic vdso_zx_handle_close_many zx_handle_close_many
//go:cgo_import_dynamic vdso_zx_handle_duplicate zx_handle_duplicate
//go:cgo_import_dynamic vdso_zx_handle_replace zx_handle_replace
//go:cgo_import_dynamic vdso_zx_interrupt_ack zx_interrupt_ack
//go:cgo_import_dynamic vdso_zx_interrupt_bind zx_interrupt_bind
//go:cgo_import_dynamic vdso_zx_interrupt_create zx_interrupt_create
//go:cgo_import_dynamic vdso_zx_interrupt_destroy zx_interrupt_destroy
//go:cgo_import_dynamic vdso_zx_interrupt_trigger zx_interrupt_trigger
//go:cgo_import_dynamic vdso_zx_interrupt_wait zx_interrupt_wait
//go:cgo_import_dynamic vdso_zx_iommu_create zx_iommu_create
//go:cgo_import_dynamic vdso_zx_ioports_release zx_ioports_release
//go:cgo_import_dynamic vdso_zx_ioports_request zx_ioports_request
//go:cgo_import_dynamic vdso_zx_job_create zx_job_create
//go:cgo_import_dynamic vdso_zx_job_set_critical zx_job_set_critical
//go:cgo_import_dynamic vdso_zx_job_set_policy zx_job_set_policy
//go:cgo_import_dynamic vdso_zx_ktrace_control zx_ktrace_control
//go:cgo_import_dynamic vdso_zx_ktrace_read zx_ktrace_read
//go:cgo_import_dynamic vdso_zx_ktrace_write zx_ktrace_write
//go:cgo_import_dynamic vdso_zx_msi_allocate zx_msi_allocate
//go:cgo_import_dynamic vdso_zx_msi_create zx_msi_create
//go:cgo_import_dynamic vdso_zx_mtrace_control zx_mtrace_control
//go:cgo_import_dynamic vdso_zx_nanosleep zx_nanosleep
//go:cgo_import_dynamic vdso_zx_object_get_child zx_object_get_child
//go:cgo_import_dynamic vdso_zx_object_get_info zx_object_get_info
//go:cgo_import_dynamic vdso_zx_object_get_property zx_object_get_property
//go:cgo_import_dynamic vdso_zx_object_set_profile zx_object_set_profile
//go:cgo_import_dynamic vdso_zx_object_set_property zx_object_set_property
//go:cgo_import_dynamic vdso_zx_object_signal zx_object_signal
//go:cgo_import_dynamic vdso_zx_object_signal_peer zx_object_signal_peer
//go:cgo_import_dynamic vdso_zx_object_wait_async zx_object_wait_async
//go:cgo_import_dynamic vdso_zx_object_wait_many zx_object_wait_many
//go:cgo_import_dynamic vdso_zx_object_wait_one zx_object_wait_one
//go:cgo_import_dynamic vdso_zx_pager_create zx_pager_create
//go:cgo_import_dynamic vdso_zx_pager_create_vmo zx_pager_create_vmo
//go:cgo_import_dynamic vdso_zx_pager_detach_vmo zx_pager_detach_vmo
//go:cgo_import_dynamic vdso_zx_pager_op_range zx_pager_op_range
//go:cgo_import_dynamic vdso_zx_pager_query_dirty_ranges zx_pager_query_dirty_ranges
//go:cgo_import_dynamic vdso_zx_pager_query_vmo_stats zx_pager_query_vmo_stats
//go:cgo_import_dynamic vdso_zx_pager_supply_pages zx_pager_supply_pages
//go:cgo_import_dynamic vdso_zx_pc_firmware_tables zx_pc_firmware_tables
//go:cgo_import_dynamic vdso_zx_pci_add_subtract_io_range zx_pci_add_subtract_io_range
//go:cgo_import_dynamic vdso_zx_pci_cfg_pio_rw zx_pci_cfg_pio_rw
//go:cgo_import_dynamic vdso_zx_pci_config_read zx_pci_config_read
//go:cgo_import_dynamic vdso_zx_pci_config_write zx_pci_config_write
//go:cgo_import_dynamic vdso_zx_pci_enable_bus_master zx_pci_enable_bus_master
//go:cgo_import_dynamic vdso_zx_pci_get_bar zx_pci_get_bar
//go:cgo_import_dynamic vdso_zx_pci_get_nth_device zx_pci_get_nth_device
//go:cgo_import_dynamic vdso_zx_pci_init zx_pci_init
//go:cgo_import_dynamic vdso_zx_pci_map_interrupt zx_pci_map_interrupt
//go:cgo_import_dynamic vdso_zx_pci_query_irq_mode zx_pci_query_irq_mode
//go:cgo_import_dynamic vdso_zx_pci_reset_device zx_pci_reset_device
//go:cgo_import_dynamic vdso_zx_pci_set_irq_mode zx_pci_set_irq_mode
//go:cgo_import_dynamic vdso_zx_pmt_unpin zx_pmt_unpin
//go:cgo_import_dynamic vdso_zx_port_cancel zx_port_cancel
//go:cgo_import_dynamic vdso_zx_port_create zx_port_create
//go:cgo_import_dynamic vdso_zx_port_queue zx_port_queue
//go:cgo_import_dynamic vdso_zx_port_wait zx_port_wait
//go:cgo_import_dynamic vdso_zx_process_create zx_process_create
//go:cgo_import_dynamic vdso_zx_process_create_shared zx_process_create_shared
//go:cgo_import_dynamic vdso_zx_process_exit zx_process_exit
//go:cgo_import_dynamic vdso_zx_process_read_memory zx_process_read_memory
//go:cgo_import_dynamic vdso_zx_process_start zx_process_start
//go:cgo_import_dynamic vdso_zx_process_write_memory zx_process_write_memory
//go:cgo_import_dynamic vdso_zx_profile_create zx_profile_create
//go:cgo_import_dynamic vdso_zx_resource_create zx_resource_create
//go:cgo_import_dynamic vdso_zx_restricted_enter zx_restricted_enter
//go:cgo_import_dynamic vdso_zx_restricted_read_state zx_restricted_read_state
//go:cgo_import_dynamic vdso_zx_restricted_write_state zx_restricted_write_state
//go:cgo_import_dynamic vdso_zx_smc_call zx_smc_call
//go:cgo_import_dynamic vdso_zx_socket_create zx_socket_create
//go:cgo_import_dynamic vdso_zx_socket_read zx_socket_read
//go:cgo_import_dynamic vdso_zx_socket_set_disposition zx_socket_set_disposition
//go:cgo_import_dynamic vdso_zx_socket_write zx_socket_write
//go:cgo_import_dynamic vdso_zx_stream_create zx_stream_create
//go:cgo_import_dynamic vdso_zx_stream_readv zx_stream_readv
//go:cgo_import_dynamic vdso_zx_stream_readv_at zx_stream_readv_at
//go:cgo_import_dynamic vdso_zx_stream_seek zx_stream_seek
//go:cgo_import_dynamic vdso_zx_stream_writev zx_stream_writev
//go:cgo_import_dynamic vdso_zx_stream_writev_at zx_stream_writev_at
//go:cgo_import_dynamic vdso_zx_syscall_next_1 zx_syscall_next_1
//go:cgo_import_dynamic vdso_zx_syscall_test_handle_create zx_syscall_test_handle_create
//go:cgo_import_dynamic vdso_zx_syscall_test_widening_signed_narrow zx_syscall_test_widening_signed_narrow
//go:cgo_import_dynamic vdso_zx_syscall_test_widening_signed_wide zx_syscall_test_widening_signed_wide
//go:cgo_import_dynamic vdso_zx_syscall_test_widening_unsigned_narrow zx_syscall_test_widening_unsigned_narrow
//go:cgo_import_dynamic vdso_zx_syscall_test_widening_unsigned_wide zx_syscall_test_widening_unsigned_wide
//go:cgo_import_dynamic vdso_zx_syscall_test_wrapper zx_syscall_test_wrapper
//go:cgo_import_dynamic vdso_zx_syscall_test_0 zx_syscall_test_0
//go:cgo_import_dynamic vdso_zx_syscall_test_1 zx_syscall_test_1
//go:cgo_import_dynamic vdso_zx_syscall_test_2 zx_syscall_test_2
//go:cgo_import_dynamic vdso_zx_syscall_test_3 zx_syscall_test_3
//go:cgo_import_dynamic vdso_zx_syscall_test_4 zx_syscall_test_4
//go:cgo_import_dynamic vdso_zx_syscall_test_5 zx_syscall_test_5
//go:cgo_import_dynamic vdso_zx_syscall_test_6 zx_syscall_test_6
//go:cgo_import_dynamic vdso_zx_syscall_test_7 zx_syscall_test_7
//go:cgo_import_dynamic vdso_zx_syscall_test_8 zx_syscall_test_8
//go:cgo_import_dynamic vdso_zx_system_get_dcache_line_size zx_system_get_dcache_line_size
//go:cgo_import_dynamic vdso_zx_system_get_event zx_system_get_event
//go:cgo_import_dynamic vdso_zx_system_get_features zx_system_get_features
//go:cgo_import_dynamic vdso_zx_system_get_num_cpus zx_system_get_num_cpus
//go:cgo_import_dynamic vdso_zx_system_get_page_size zx_system_get_page_size
//go:cgo_import_dynamic vdso_zx_system_get_performance_info zx_system_get_performance_info
//go:cgo_import_dynamic vdso_zx_system_get_physmem zx_system_get_physmem
//go:cgo_import_dynamic vdso_zx_system_get_version_string zx_system_get_version_string
//go:cgo_import_dynamic vdso_zx_system_mexec zx_system_mexec
//go:cgo_import_dynamic vdso_zx_system_mexec_payload_get zx_system_mexec_payload_get
//go:cgo_import_dynamic vdso_zx_system_powerctl zx_system_powerctl
//go:cgo_import_dynamic vdso_zx_system_set_performance_info zx_system_set_performance_info
//go:cgo_import_dynamic vdso_zx_task_create_exception_channel zx_task_create_exception_channel
//go:cgo_import_dynamic vdso_zx_task_kill zx_task_kill
//go:cgo_import_dynamic vdso_zx_task_suspend zx_task_suspend
//go:cgo_import_dynamic vdso_zx_task_suspend_token zx_task_suspend_token
//go:cgo_import_dynamic vdso_zx_thread_create zx_thread_create
//go:cgo_import_dynamic vdso_zx_thread_exit zx_thread_exit
//go:cgo_import_dynamic vdso_zx_thread_legacy_yield zx_thread_legacy_yield
//go:cgo_import_dynamic vdso_zx_thread_read_state zx_thread_read_state
//go:cgo_import_dynamic vdso_zx_thread_start zx_thread_start
//go:cgo_import_dynamic vdso_zx_thread_write_state zx_thread_write_state
//go:cgo_import_dynamic vdso_zx_ticks_get zx_ticks_get
//go:cgo_import_dynamic vdso_zx_ticks_get_via_kernel zx_ticks_get_via_kernel
//go:cgo_import_dynamic vdso_zx_ticks_per_second zx_ticks_per_second
//go:cgo_import_dynamic vdso_zx_timer_cancel zx_timer_cancel
//go:cgo_import_dynamic vdso_zx_timer_create zx_timer_create
//go:cgo_import_dynamic vdso_zx_timer_set zx_timer_set
//go:cgo_import_dynamic vdso_zx_vcpu_create zx_vcpu_create
//go:cgo_import_dynamic vdso_zx_vcpu_enter zx_vcpu_enter
//go:cgo_import_dynamic vdso_zx_vcpu_interrupt zx_vcpu_interrupt
//go:cgo_import_dynamic vdso_zx_vcpu_kick zx_vcpu_kick
//go:cgo_import_dynamic vdso_zx_vcpu_read_state zx_vcpu_read_state
//go:cgo_import_dynamic vdso_zx_vcpu_write_state zx_vcpu_write_state
//go:cgo_import_dynamic vdso_zx_vmar_allocate zx_vmar_allocate
//go:cgo_import_dynamic vdso_zx_vmar_destroy zx_vmar_destroy
//go:cgo_import_dynamic vdso_zx_vmar_map zx_vmar_map
//go:cgo_import_dynamic vdso_zx_vmar_op_range zx_vmar_op_range
//go:cgo_import_dynamic vdso_zx_vmar_protect zx_vmar_protect
//go:cgo_import_dynamic vdso_zx_vmar_unmap zx_vmar_unmap
//go:cgo_import_dynamic vdso_zx_vmar_unmap_handle_close_thread_exit zx_vmar_unmap_handle_close_thread_exit
//go:cgo_import_dynamic vdso_zx_vmo_create zx_vmo_create
//go:cgo_import_dynamic vdso_zx_vmo_create_child zx_vmo_create_child
//go:cgo_import_dynamic vdso_zx_vmo_create_contiguous zx_vmo_create_contiguous
//go:cgo_import_dynamic vdso_zx_vmo_create_physical zx_vmo_create_physical
//go:cgo_import_dynamic vdso_zx_vmo_get_size zx_vmo_get_size
//go:cgo_import_dynamic vdso_zx_vmo_op_range zx_vmo_op_range
//go:cgo_import_dynamic vdso_zx_vmo_read zx_vmo_read
//go:cgo_import_dynamic vdso_zx_vmo_replace_as_executable zx_vmo_replace_as_executable
//go:cgo_import_dynamic vdso_zx_vmo_set_cache_policy zx_vmo_set_cache_policy
//go:cgo_import_dynamic vdso_zx_vmo_set_size zx_vmo_set_size
//go:cgo_import_dynamic vdso_zx_vmo_write zx_vmo_write

//go:linkname vdso_zx_bti_create vdso_zx_bti_create
//go:linkname vdso_zx_bti_pin vdso_zx_bti_pin
//go:linkname vdso_zx_bti_release_quarantine vdso_zx_bti_release_quarantine
//go:linkname vdso_zx_cache_flush vdso_zx_cache_flush
//go:linkname vdso_zx_channel_call vdso_zx_channel_call
//go:linkname vdso_zx_channel_call_etc vdso_zx_channel_call_etc
//go:linkname vdso_zx_channel_call_etc_finish vdso_zx_channel_call_etc_finish
//go:linkname vdso_zx_channel_call_etc_noretry vdso_zx_channel_call_etc_noretry
//go:linkname vdso_zx_channel_call_finish vdso_zx_channel_call_finish
//go:linkname vdso_zx_channel_call_noretry vdso_zx_channel_call_noretry
//go:linkname vdso_zx_channel_create vdso_zx_channel_create
//go:linkname vdso_zx_channel_read vdso_zx_channel_read
//go:linkname vdso_zx_channel_read_etc vdso_zx_channel_read_etc
//go:linkname vdso_zx_channel_write vdso_zx_channel_write
//go:linkname vdso_zx_channel_write_etc vdso_zx_channel_write_etc
//go:linkname vdso_zx_clock_create vdso_zx_clock_create
//go:linkname vdso_zx_clock_get_details vdso_zx_clock_get_details
//go:linkname vdso_zx_clock_get_monotonic vdso_zx_clock_get_monotonic
//go:linkname vdso_zx_clock_get_monotonic_via_kernel vdso_zx_clock_get_monotonic_via_kernel
//go:linkname vdso_zx_clock_read vdso_zx_clock_read
//go:linkname vdso_zx_clock_update vdso_zx_clock_update
//go:linkname vdso_zx_cprng_add_entropy vdso_zx_cprng_add_entropy
//go:linkname vdso_zx_cprng_draw vdso_zx_cprng_draw
//go:linkname vdso_zx_cprng_draw_once vdso_zx_cprng_draw_once
//go:linkname vdso_zx_deadline_after vdso_zx_deadline_after
//go:linkname vdso_zx_debug_read vdso_zx_debug_read
//go:linkname vdso_zx_debug_send_command vdso_zx_debug_send_command
//go:linkname vdso_zx_debug_write vdso_zx_debug_write
//go:linkname vdso_zx_debuglog_create vdso_zx_debuglog_create
//go:linkname vdso_zx_debuglog_read vdso_zx_debuglog_read
//go:linkname vdso_zx_debuglog_write vdso_zx_debuglog_write
//go:linkname vdso_zx_event_create vdso_zx_event_create
//go:linkname vdso_zx_eventpair_create vdso_zx_eventpair_create
//go:linkname vdso_zx_exception_get_process vdso_zx_exception_get_process
//go:linkname vdso_zx_exception_get_thread vdso_zx_exception_get_thread
//go:linkname vdso_zx_fifo_create vdso_zx_fifo_create
//go:linkname vdso_zx_fifo_read vdso_zx_fifo_read
//go:linkname vdso_zx_fifo_write vdso_zx_fifo_write
//go:linkname vdso_zx_framebuffer_get_info vdso_zx_framebuffer_get_info
//go:linkname vdso_zx_framebuffer_set_range vdso_zx_framebuffer_set_range
//go:linkname vdso_zx_futex_get_owner vdso_zx_futex_get_owner
//go:linkname vdso_zx_futex_requeue vdso_zx_futex_requeue
//go:linkname vdso_zx_futex_requeue_single_owner vdso_zx_futex_requeue_single_owner
//go:linkname vdso_zx_futex_wait vdso_zx_futex_wait
//go:linkname vdso_zx_futex_wake vdso_zx_futex_wake
//go:linkname vdso_zx_futex_wake_handle_close_thread_exit vdso_zx_futex_wake_handle_close_thread_exit
//go:linkname vdso_zx_futex_wake_single_owner vdso_zx_futex_wake_single_owner
//go:linkname vdso_zx_guest_create vdso_zx_guest_create
//go:linkname vdso_zx_guest_set_trap vdso_zx_guest_set_trap
//go:linkname vdso_zx_handle_close vdso_zx_handle_close
//go:linkname vdso_zx_handle_close_many vdso_zx_handle_close_many
//go:linkname vdso_zx_handle_duplicate vdso_zx_handle_duplicate
//go:linkname vdso_zx_handle_replace vdso_zx_handle_replace
//go:linkname vdso_zx_interrupt_ack vdso_zx_interrupt_ack
//go:linkname vdso_zx_interrupt_bind vdso_zx_interrupt_bind
//go:linkname vdso_zx_interrupt_create vdso_zx_interrupt_create
//go:linkname vdso_zx_interrupt_destroy vdso_zx_interrupt_destroy
//go:linkname vdso_zx_interrupt_trigger vdso_zx_interrupt_trigger
//go:linkname vdso_zx_interrupt_wait vdso_zx_interrupt_wait
//go:linkname vdso_zx_iommu_create vdso_zx_iommu_create
//go:linkname vdso_zx_ioports_release vdso_zx_ioports_release
//go:linkname vdso_zx_ioports_request vdso_zx_ioports_request
//go:linkname vdso_zx_job_create vdso_zx_job_create
//go:linkname vdso_zx_job_set_critical vdso_zx_job_set_critical
//go:linkname vdso_zx_job_set_policy vdso_zx_job_set_policy
//go:linkname vdso_zx_ktrace_control vdso_zx_ktrace_control
//go:linkname vdso_zx_ktrace_read vdso_zx_ktrace_read
//go:linkname vdso_zx_ktrace_write vdso_zx_ktrace_write
//go:linkname vdso_zx_msi_allocate vdso_zx_msi_allocate
//go:linkname vdso_zx_msi_create vdso_zx_msi_create
//go:linkname vdso_zx_mtrace_control vdso_zx_mtrace_control
//go:linkname vdso_zx_nanosleep vdso_zx_nanosleep
//go:linkname vdso_zx_object_get_child vdso_zx_object_get_child
//go:linkname vdso_zx_object_get_info vdso_zx_object_get_info
//go:linkname vdso_zx_object_get_property vdso_zx_object_get_property
//go:linkname vdso_zx_object_set_profile vdso_zx_object_set_profile
//go:linkname vdso_zx_object_set_property vdso_zx_object_set_property
//go:linkname vdso_zx_object_signal vdso_zx_object_signal
//go:linkname vdso_zx_object_signal_peer vdso_zx_object_signal_peer
//go:linkname vdso_zx_object_wait_async vdso_zx_object_wait_async
//go:linkname vdso_zx_object_wait_many vdso_zx_object_wait_many
//go:linkname vdso_zx_object_wait_one vdso_zx_object_wait_one
//go:linkname vdso_zx_pager_create vdso_zx_pager_create
//go:linkname vdso_zx_pager_create_vmo vdso_zx_pager_create_vmo
//go:linkname vdso_zx_pager_detach_vmo vdso_zx_pager_detach_vmo
//go:linkname vdso_zx_pager_op_range vdso_zx_pager_op_range
//go:linkname vdso_zx_pager_query_dirty_ranges vdso_zx_pager_query_dirty_ranges
//go:linkname vdso_zx_pager_query_vmo_stats vdso_zx_pager_query_vmo_stats
//go:linkname vdso_zx_pager_supply_pages vdso_zx_pager_supply_pages
//go:linkname vdso_zx_pc_firmware_tables vdso_zx_pc_firmware_tables
//go:linkname vdso_zx_pci_add_subtract_io_range vdso_zx_pci_add_subtract_io_range
//go:linkname vdso_zx_pci_cfg_pio_rw vdso_zx_pci_cfg_pio_rw
//go:linkname vdso_zx_pci_config_read vdso_zx_pci_config_read
//go:linkname vdso_zx_pci_config_write vdso_zx_pci_config_write
//go:linkname vdso_zx_pci_enable_bus_master vdso_zx_pci_enable_bus_master
//go:linkname vdso_zx_pci_get_bar vdso_zx_pci_get_bar
//go:linkname vdso_zx_pci_get_nth_device vdso_zx_pci_get_nth_device
//go:linkname vdso_zx_pci_init vdso_zx_pci_init
//go:linkname vdso_zx_pci_map_interrupt vdso_zx_pci_map_interrupt
//go:linkname vdso_zx_pci_query_irq_mode vdso_zx_pci_query_irq_mode
//go:linkname vdso_zx_pci_reset_device vdso_zx_pci_reset_device
//go:linkname vdso_zx_pci_set_irq_mode vdso_zx_pci_set_irq_mode
//go:linkname vdso_zx_pmt_unpin vdso_zx_pmt_unpin
//go:linkname vdso_zx_port_cancel vdso_zx_port_cancel
//go:linkname vdso_zx_port_create vdso_zx_port_create
//go:linkname vdso_zx_port_queue vdso_zx_port_queue
//go:linkname vdso_zx_port_wait vdso_zx_port_wait
//go:linkname vdso_zx_process_create vdso_zx_process_create
//go:linkname vdso_zx_process_create_shared vdso_zx_process_create_shared
//go:linkname vdso_zx_process_exit vdso_zx_process_exit
//go:linkname vdso_zx_process_read_memory vdso_zx_process_read_memory
//go:linkname vdso_zx_process_start vdso_zx_process_start
//go:linkname vdso_zx_process_write_memory vdso_zx_process_write_memory
//go:linkname vdso_zx_profile_create vdso_zx_profile_create
//go:linkname vdso_zx_resource_create vdso_zx_resource_create
//go:linkname vdso_zx_restricted_enter vdso_zx_restricted_enter
//go:linkname vdso_zx_restricted_read_state vdso_zx_restricted_read_state
//go:linkname vdso_zx_restricted_write_state vdso_zx_restricted_write_state
//go:linkname vdso_zx_smc_call vdso_zx_smc_call
//go:linkname vdso_zx_socket_create vdso_zx_socket_create
//go:linkname vdso_zx_socket_read vdso_zx_socket_read
//go:linkname vdso_zx_socket_set_disposition vdso_zx_socket_set_disposition
//go:linkname vdso_zx_socket_write vdso_zx_socket_write
//go:linkname vdso_zx_stream_create vdso_zx_stream_create
//go:linkname vdso_zx_stream_readv vdso_zx_stream_readv
//go:linkname vdso_zx_stream_readv_at vdso_zx_stream_readv_at
//go:linkname vdso_zx_stream_seek vdso_zx_stream_seek
//go:linkname vdso_zx_stream_writev vdso_zx_stream_writev
//go:linkname vdso_zx_stream_writev_at vdso_zx_stream_writev_at
//go:linkname vdso_zx_syscall_next_1 vdso_zx_syscall_next_1
//go:linkname vdso_zx_syscall_test_handle_create vdso_zx_syscall_test_handle_create
//go:linkname vdso_zx_syscall_test_widening_signed_narrow vdso_zx_syscall_test_widening_signed_narrow
//go:linkname vdso_zx_syscall_test_widening_signed_wide vdso_zx_syscall_test_widening_signed_wide
//go:linkname vdso_zx_syscall_test_widening_unsigned_narrow vdso_zx_syscall_test_widening_unsigned_narrow
//go:linkname vdso_zx_syscall_test_widening_unsigned_wide vdso_zx_syscall_test_widening_unsigned_wide
//go:linkname vdso_zx_syscall_test_wrapper vdso_zx_syscall_test_wrapper
//go:linkname vdso_zx_syscall_test_0 vdso_zx_syscall_test_0
//go:linkname vdso_zx_syscall_test_1 vdso_zx_syscall_test_1
//go:linkname vdso_zx_syscall_test_2 vdso_zx_syscall_test_2
//go:linkname vdso_zx_syscall_test_3 vdso_zx_syscall_test_3
//go:linkname vdso_zx_syscall_test_4 vdso_zx_syscall_test_4
//go:linkname vdso_zx_syscall_test_5 vdso_zx_syscall_test_5
//go:linkname vdso_zx_syscall_test_6 vdso_zx_syscall_test_6
//go:linkname vdso_zx_syscall_test_7 vdso_zx_syscall_test_7
//go:linkname vdso_zx_syscall_test_8 vdso_zx_syscall_test_8
//go:linkname vdso_zx_system_get_dcache_line_size vdso_zx_system_get_dcache_line_size
//go:linkname vdso_zx_system_get_event vdso_zx_system_get_event
//go:linkname vdso_zx_system_get_features vdso_zx_system_get_features
//go:linkname vdso_zx_system_get_num_cpus vdso_zx_system_get_num_cpus
//go:linkname vdso_zx_system_get_page_size vdso_zx_system_get_page_size
//go:linkname vdso_zx_system_get_performance_info vdso_zx_system_get_performance_info
//go:linkname vdso_zx_system_get_physmem vdso_zx_system_get_physmem
//go:linkname vdso_zx_system_get_version_string vdso_zx_system_get_version_string
//go:linkname vdso_zx_system_mexec vdso_zx_system_mexec
//go:linkname vdso_zx_system_mexec_payload_get vdso_zx_system_mexec_payload_get
//go:linkname vdso_zx_system_powerctl vdso_zx_system_powerctl
//go:linkname vdso_zx_system_set_performance_info vdso_zx_system_set_performance_info
//go:linkname vdso_zx_task_create_exception_channel vdso_zx_task_create_exception_channel
//go:linkname vdso_zx_task_kill vdso_zx_task_kill
//go:linkname vdso_zx_task_suspend vdso_zx_task_suspend
//go:linkname vdso_zx_task_suspend_token vdso_zx_task_suspend_token
//go:linkname vdso_zx_thread_create vdso_zx_thread_create
//go:linkname vdso_zx_thread_exit vdso_zx_thread_exit
//go:linkname vdso_zx_thread_legacy_yield vdso_zx_thread_legacy_yield
//go:linkname vdso_zx_thread_read_state vdso_zx_thread_read_state
//go:linkname vdso_zx_thread_start vdso_zx_thread_start
//go:linkname vdso_zx_thread_write_state vdso_zx_thread_write_state
//go:linkname vdso_zx_ticks_get vdso_zx_ticks_get
//go:linkname vdso_zx_ticks_get_via_kernel vdso_zx_ticks_get_via_kernel
//go:linkname vdso_zx_ticks_per_second vdso_zx_ticks_per_second
//go:linkname vdso_zx_timer_cancel vdso_zx_timer_cancel
//go:linkname vdso_zx_timer_create vdso_zx_timer_create
//go:linkname vdso_zx_timer_set vdso_zx_timer_set
//go:linkname vdso_zx_vcpu_create vdso_zx_vcpu_create
//go:linkname vdso_zx_vcpu_enter vdso_zx_vcpu_enter
//go:linkname vdso_zx_vcpu_interrupt vdso_zx_vcpu_interrupt
//go:linkname vdso_zx_vcpu_kick vdso_zx_vcpu_kick
//go:linkname vdso_zx_vcpu_read_state vdso_zx_vcpu_read_state
//go:linkname vdso_zx_vcpu_write_state vdso_zx_vcpu_write_state
//go:linkname vdso_zx_vmar_allocate vdso_zx_vmar_allocate
//go:linkname vdso_zx_vmar_destroy vdso_zx_vmar_destroy
//go:linkname vdso_zx_vmar_map vdso_zx_vmar_map
//go:linkname vdso_zx_vmar_op_range vdso_zx_vmar_op_range
//go:linkname vdso_zx_vmar_protect vdso_zx_vmar_protect
//go:linkname vdso_zx_vmar_unmap vdso_zx_vmar_unmap
//go:linkname vdso_zx_vmar_unmap_handle_close_thread_exit vdso_zx_vmar_unmap_handle_close_thread_exit
//go:linkname vdso_zx_vmo_create vdso_zx_vmo_create
//go:linkname vdso_zx_vmo_create_child vdso_zx_vmo_create_child
//go:linkname vdso_zx_vmo_create_contiguous vdso_zx_vmo_create_contiguous
//go:linkname vdso_zx_vmo_create_physical vdso_zx_vmo_create_physical
//go:linkname vdso_zx_vmo_get_size vdso_zx_vmo_get_size
//go:linkname vdso_zx_vmo_op_range vdso_zx_vmo_op_range
//go:linkname vdso_zx_vmo_read vdso_zx_vmo_read
//go:linkname vdso_zx_vmo_replace_as_executable vdso_zx_vmo_replace_as_executable
//go:linkname vdso_zx_vmo_set_cache_policy vdso_zx_vmo_set_cache_policy
//go:linkname vdso_zx_vmo_set_size vdso_zx_vmo_set_size
//go:linkname vdso_zx_vmo_write vdso_zx_vmo_write

//go:noescape
//go:nosplit
func vdsoCall_zx_bti_create(iommu uint32, options uint32, bti_id uint64, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_bti_pin(handle uint32, options uint32, vmo uint32, offset uint64, size uint64, addrs unsafe.Pointer, num_addrs uint, pmt unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_bti_release_quarantine(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_cache_flush(addr unsafe.Pointer, size uint, options uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call_etc(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call_etc_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call_etc_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_call_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_read(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_read_etc(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_write(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_channel_write_etc(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_create(options uint64, args unsafe.Pointer, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_get_details(handle uint32, options uint64, details unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_get_monotonic() int64

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_get_monotonic_via_kernel() int64

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_read(handle uint32, now unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_clock_update(handle uint32, options uint64, args unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_cprng_add_entropy(buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_cprng_draw(buffer unsafe.Pointer, buffer_size uint)

//go:noescape
//go:nosplit
func vdsoCall_zx_cprng_draw_once(buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_deadline_after(nanoseconds int64) int64

//go:noescape
//go:nosplit
func vdsoCall_zx_debug_read(handle uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_debug_send_command(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_debug_write(buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_debuglog_create(resource uint32, options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_debuglog_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_debuglog_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_event_create(options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_eventpair_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_exception_get_process(handle uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_exception_get_thread(handle uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_fifo_create(elem_count uint, elem_size uint, options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_fifo_read(handle uint32, elem_size uint, data unsafe.Pointer, data_size uint, actual_count unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_fifo_write(handle uint32, elem_size uint, data unsafe.Pointer, count uint, actual_count unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_framebuffer_get_info(resource uint32, format unsafe.Pointer, width unsafe.Pointer, height unsafe.Pointer, stride unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_framebuffer_set_range(resource uint32, vmo uint32, len uint32, format uint32, width uint32, height uint32, stride uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_get_owner(value_ptr unsafe.Pointer, koid unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_requeue(value_ptr unsafe.Pointer, wake_count uint32, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_requeue_single_owner(value_ptr unsafe.Pointer, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_wait(value_ptr unsafe.Pointer, current_value int32, new_futex_owner uint32, deadline int64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_wake(value_ptr unsafe.Pointer, wake_count uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_wake_handle_close_thread_exit(value_ptr unsafe.Pointer, wake_count uint32, new_value int32, close_handle uint32)

//go:noescape
//go:nosplit
func vdsoCall_zx_futex_wake_single_owner(value_ptr unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_guest_create(resource uint32, options uint32, guest_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_guest_set_trap(handle uint32, kind uint32, addr uintptr, size uint, port_handle uint32, key uint64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_handle_close(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_handle_close_many(handles unsafe.Pointer, num_handles uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_handle_duplicate(handle uint32, rights uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_handle_replace(handle uint32, rights uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_ack(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_bind(handle uint32, port_handle uint32, key uint64, options uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_create(src_obj uint32, src_num uint32, options uint32, out_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_destroy(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_trigger(handle uint32, options uint32, timestamp int64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_interrupt_wait(handle uint32, out_timestamp unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_iommu_create(resource uint32, typ uint32, desc unsafe.Pointer, desc_size uint, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ioports_release(resource uint32, io_addr uint16, len uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ioports_request(resource uint32, io_addr uint16, len uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_job_create(parent_job uint32, options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_job_set_critical(job uint32, options uint32, process uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_job_set_policy(handle uint32, options uint32, topic uint32, policy unsafe.Pointer, policy_size uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ktrace_control(handle uint32, action uint32, options uint32, ptr unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ktrace_read(handle uint32, data unsafe.Pointer, offset uint32, data_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ktrace_write(handle uint32, id uint32, arg0 uint32, arg1 uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_msi_allocate(handle uint32, count uint32, out_allocation unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_msi_create(handle uint32, options uint32, msi_id uint32, vmo uint32, vmo_offset uint, out_interrupt unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_mtrace_control(handle uint32, kind uint32, action uint32, options uint32, ptr unsafe.Pointer, ptr_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_nanosleep(deadline int64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_get_child(handle uint32, koid uint64, rights uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_get_info(handle uint32, topic uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_get_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_set_profile(handle uint32, profile uint32, options uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_set_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_signal(handle uint32, clear_mask uint32, set_mask uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_signal_peer(handle uint32, clear_mask uint32, set_mask uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_wait_async(handle uint32, port uint32, key uint64, signals uint32, options uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_wait_many(items unsafe.Pointer, num_items uint, deadline int64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_object_wait_one(handle uint32, signals uint32, deadline int64, observed unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_create(options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_create_vmo(pager uint32, options uint32, port uint32, key uint64, size uint64, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_detach_vmo(pager uint32, vmo uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_op_range(pager uint32, op uint32, pager_vmo uint32, offset uint64, length uint64, data uint64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_query_dirty_ranges(pager uint32, pager_vmo uint32, offset uint64, length uint64, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_query_vmo_stats(pager uint32, pager_vmo uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pager_supply_pages(pager uint32, pager_vmo uint32, offset uint64, length uint64, aux_vmo uint32, aux_offset uint64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pc_firmware_tables(handle uint32, acpi_rsdp unsafe.Pointer, smbios unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_add_subtract_io_range(handle uint32, mmio uint32, base uint64, len uint64, add uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_cfg_pio_rw(handle uint32, bus uint8, dev uint8, funk uint8, offset uint8, val unsafe.Pointer, width uint, write uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_config_read(handle uint32, offset uint16, width uint, out_val unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_config_write(handle uint32, offset uint16, width uint, val uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_enable_bus_master(handle uint32, enable uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_get_bar(handle uint32, bar_num uint32, out_bar unsafe.Pointer, out_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_get_nth_device(handle uint32, index uint32, out_info unsafe.Pointer, out_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_init(handle uint32, init_buf unsafe.Pointer, len uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_map_interrupt(handle uint32, which_irq int32, out_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_query_irq_mode(handle uint32, mode uint32, out_max_irqs unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_reset_device(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pci_set_irq_mode(handle uint32, mode uint32, requested_irq_count uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_pmt_unpin(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_port_cancel(handle uint32, source uint32, key uint64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_port_create(options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_port_queue(handle uint32, packet unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_port_wait(handle uint32, deadline int64, packet unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_process_create(job uint32, name unsafe.Pointer, name_size uint, options uint32, proc_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_process_create_shared(shared_proc uint32, options uint32, name unsafe.Pointer, name_size uint, proc_handle unsafe.Pointer, restricted_vmar_handle unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_process_exit(retcode int64)

//go:noescape
//go:nosplit
func vdsoCall_zx_process_read_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_process_start(handle uint32, thread uint32, entry uintptr, stack uintptr, arg1 uint32, arg2 uintptr) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_process_write_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_profile_create(root_job uint32, options uint32, profile unsafe.Pointer, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_resource_create(parent_rsrc uint32, options uint32, base uint64, size uint, name unsafe.Pointer, name_size uint, resource_out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_restricted_enter(options uint32, vector_table_ptr uintptr, context uintptr) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_restricted_read_state(buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_restricted_write_state(buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_smc_call(handle uint32, parameters unsafe.Pointer, out_smc_result unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_socket_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_socket_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_socket_set_disposition(handle uint32, disposition uint32, disposition_peer uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_socket_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_create(options uint32, vmo uint32, seek uint64, out_stream unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_readv(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_readv_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_seek(handle uint32, whence uint32, offset int64, out_seek unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_writev(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_stream_writev_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_next_1(arg int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_handle_create(return_value int32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_widening_signed_narrow(a int64, b int32, c int16, d int8) int64

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_widening_signed_wide(a int64, b int32, c int16, d int8) int64

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_widening_unsigned_narrow(a uint64, b uint32, c uint16, d uint8) uint64

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_widening_unsigned_wide(a uint64, b uint32, c uint16, d uint8) uint64

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_wrapper(a int32, b int32, c int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_0() int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_1(a int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_2(a int32, b int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_3(a int32, b int32, c int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_4(a int32, b int32, c int32, d int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_5(a int32, b int32, c int32, d int32, e int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_6(a int32, b int32, c int32, d int32, e int32, f int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_7(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_syscall_test_8(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32, h int32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_dcache_line_size() uint32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_event(root_job uint32, kind uint32, event unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_features(kind uint32, features unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_num_cpus() uint32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_page_size() uint32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_performance_info(resource uint32, topic uint32, count uint, info unsafe.Pointer, output_count unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_physmem() uint64

//go:noescape
//go:nosplit
func vdsoCall_zx_system_get_version_string() unsafe.Pointer

//go:noescape
//go:nosplit
func vdsoCall_zx_system_mexec(resource uint32, kernel_vmo uint32, bootimage_vmo uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_mexec_payload_get(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_powerctl(resource uint32, cmd uint32, arg unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_system_set_performance_info(resource uint32, topic uint32, info unsafe.Pointer, count uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_task_create_exception_channel(handle uint32, options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_task_kill(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_task_suspend(handle uint32, token unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_task_suspend_token(handle uint32, token unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_create(process uint32, name unsafe.Pointer, name_size uint, options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_exit()

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_legacy_yield(options uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_start(handle uint32, thread_entry uintptr, stack uintptr, arg1 uintptr, arg2 uintptr) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_thread_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_ticks_get() int64

//go:noescape
//go:nosplit
func vdsoCall_zx_ticks_get_via_kernel() int64

//go:noescape
//go:nosplit
func vdsoCall_zx_ticks_per_second() int64

//go:noescape
//go:nosplit
func vdsoCall_zx_timer_cancel(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_timer_create(options uint32, clock_id uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_timer_set(handle uint32, deadline int64, slack int64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_create(guest uint32, options uint32, entry uintptr, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_enter(handle uint32, packet unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_interrupt(handle uint32, vector uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_kick(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vcpu_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_allocate(parent_vmar uint32, options uint32, offset uint, size uint, child_vmar unsafe.Pointer, child_addr unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_destroy(handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_map(handle uint32, options uint32, vmar_offset uint, vmo uint32, vmo_offset uint64, len uint, mapped_addr unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_op_range(handle uint32, op uint32, address uintptr, size uint, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_protect(handle uint32, options uint32, addr uintptr, len uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_unmap(handle uint32, addr uintptr, len uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmar_unmap_handle_close_thread_exit(vmar_handle uint32, addr uintptr, size uint, close_handle uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_create(size uint64, options uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_create_child(handle uint32, options uint32, offset uint64, size uint64, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_create_contiguous(bti uint32, size uint, alignment_log2 uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_create_physical(resource uint32, paddr uintptr, size uint, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_get_size(handle uint32, size unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_op_range(handle uint32, op uint32, offset uint64, size uint64, buffer unsafe.Pointer, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_read(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_replace_as_executable(handle uint32, vmex uint32, out unsafe.Pointer) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_set_cache_policy(handle uint32, cache_policy uint32) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_set_size(handle uint32, size uint64) int32

//go:noescape
//go:nosplit
func vdsoCall_zx_vmo_write(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32

var (
	vdso_zx_bti_create                            uintptr
	vdso_zx_bti_pin                               uintptr
	vdso_zx_bti_release_quarantine                uintptr
	vdso_zx_cache_flush                           uintptr
	vdso_zx_channel_call                          uintptr
	vdso_zx_channel_call_etc                      uintptr
	vdso_zx_channel_call_etc_finish               uintptr
	vdso_zx_channel_call_etc_noretry              uintptr
	vdso_zx_channel_call_finish                   uintptr
	vdso_zx_channel_call_noretry                  uintptr
	vdso_zx_channel_create                        uintptr
	vdso_zx_channel_read                          uintptr
	vdso_zx_channel_read_etc                      uintptr
	vdso_zx_channel_write                         uintptr
	vdso_zx_channel_write_etc                     uintptr
	vdso_zx_clock_create                          uintptr
	vdso_zx_clock_get_details                     uintptr
	vdso_zx_clock_get_monotonic                   uintptr
	vdso_zx_clock_get_monotonic_via_kernel        uintptr
	vdso_zx_clock_read                            uintptr
	vdso_zx_clock_update                          uintptr
	vdso_zx_cprng_add_entropy                     uintptr
	vdso_zx_cprng_draw                            uintptr
	vdso_zx_cprng_draw_once                       uintptr
	vdso_zx_deadline_after                        uintptr
	vdso_zx_debug_read                            uintptr
	vdso_zx_debug_send_command                    uintptr
	vdso_zx_debug_write                           uintptr
	vdso_zx_debuglog_create                       uintptr
	vdso_zx_debuglog_read                         uintptr
	vdso_zx_debuglog_write                        uintptr
	vdso_zx_event_create                          uintptr
	vdso_zx_eventpair_create                      uintptr
	vdso_zx_exception_get_process                 uintptr
	vdso_zx_exception_get_thread                  uintptr
	vdso_zx_fifo_create                           uintptr
	vdso_zx_fifo_read                             uintptr
	vdso_zx_fifo_write                            uintptr
	vdso_zx_framebuffer_get_info                  uintptr
	vdso_zx_framebuffer_set_range                 uintptr
	vdso_zx_futex_get_owner                       uintptr
	vdso_zx_futex_requeue                         uintptr
	vdso_zx_futex_requeue_single_owner            uintptr
	vdso_zx_futex_wait                            uintptr
	vdso_zx_futex_wake                            uintptr
	vdso_zx_futex_wake_handle_close_thread_exit   uintptr
	vdso_zx_futex_wake_single_owner               uintptr
	vdso_zx_guest_create                          uintptr
	vdso_zx_guest_set_trap                        uintptr
	vdso_zx_handle_close                          uintptr
	vdso_zx_handle_close_many                     uintptr
	vdso_zx_handle_duplicate                      uintptr
	vdso_zx_handle_replace                        uintptr
	vdso_zx_interrupt_ack                         uintptr
	vdso_zx_interrupt_bind                        uintptr
	vdso_zx_interrupt_create                      uintptr
	vdso_zx_interrupt_destroy                     uintptr
	vdso_zx_interrupt_trigger                     uintptr
	vdso_zx_interrupt_wait                        uintptr
	vdso_zx_iommu_create                          uintptr
	vdso_zx_ioports_release                       uintptr
	vdso_zx_ioports_request                       uintptr
	vdso_zx_job_create                            uintptr
	vdso_zx_job_set_critical                      uintptr
	vdso_zx_job_set_policy                        uintptr
	vdso_zx_ktrace_control                        uintptr
	vdso_zx_ktrace_read                           uintptr
	vdso_zx_ktrace_write                          uintptr
	vdso_zx_msi_allocate                          uintptr
	vdso_zx_msi_create                            uintptr
	vdso_zx_mtrace_control                        uintptr
	vdso_zx_nanosleep                             uintptr
	vdso_zx_object_get_child                      uintptr
	vdso_zx_object_get_info                       uintptr
	vdso_zx_object_get_property                   uintptr
	vdso_zx_object_set_profile                    uintptr
	vdso_zx_object_set_property                   uintptr
	vdso_zx_object_signal                         uintptr
	vdso_zx_object_signal_peer                    uintptr
	vdso_zx_object_wait_async                     uintptr
	vdso_zx_object_wait_many                      uintptr
	vdso_zx_object_wait_one                       uintptr
	vdso_zx_pager_create                          uintptr
	vdso_zx_pager_create_vmo                      uintptr
	vdso_zx_pager_detach_vmo                      uintptr
	vdso_zx_pager_op_range                        uintptr
	vdso_zx_pager_query_dirty_ranges              uintptr
	vdso_zx_pager_query_vmo_stats                 uintptr
	vdso_zx_pager_supply_pages                    uintptr
	vdso_zx_pc_firmware_tables                    uintptr
	vdso_zx_pci_add_subtract_io_range             uintptr
	vdso_zx_pci_cfg_pio_rw                        uintptr
	vdso_zx_pci_config_read                       uintptr
	vdso_zx_pci_config_write                      uintptr
	vdso_zx_pci_enable_bus_master                 uintptr
	vdso_zx_pci_get_bar                           uintptr
	vdso_zx_pci_get_nth_device                    uintptr
	vdso_zx_pci_init                              uintptr
	vdso_zx_pci_map_interrupt                     uintptr
	vdso_zx_pci_query_irq_mode                    uintptr
	vdso_zx_pci_reset_device                      uintptr
	vdso_zx_pci_set_irq_mode                      uintptr
	vdso_zx_pmt_unpin                             uintptr
	vdso_zx_port_cancel                           uintptr
	vdso_zx_port_create                           uintptr
	vdso_zx_port_queue                            uintptr
	vdso_zx_port_wait                             uintptr
	vdso_zx_process_create                        uintptr
	vdso_zx_process_create_shared                 uintptr
	vdso_zx_process_exit                          uintptr
	vdso_zx_process_read_memory                   uintptr
	vdso_zx_process_start                         uintptr
	vdso_zx_process_write_memory                  uintptr
	vdso_zx_profile_create                        uintptr
	vdso_zx_resource_create                       uintptr
	vdso_zx_restricted_enter                      uintptr
	vdso_zx_restricted_read_state                 uintptr
	vdso_zx_restricted_write_state                uintptr
	vdso_zx_smc_call                              uintptr
	vdso_zx_socket_create                         uintptr
	vdso_zx_socket_read                           uintptr
	vdso_zx_socket_set_disposition                uintptr
	vdso_zx_socket_write                          uintptr
	vdso_zx_stream_create                         uintptr
	vdso_zx_stream_readv                          uintptr
	vdso_zx_stream_readv_at                       uintptr
	vdso_zx_stream_seek                           uintptr
	vdso_zx_stream_writev                         uintptr
	vdso_zx_stream_writev_at                      uintptr
	vdso_zx_syscall_next_1                        uintptr
	vdso_zx_syscall_test_handle_create            uintptr
	vdso_zx_syscall_test_widening_signed_narrow   uintptr
	vdso_zx_syscall_test_widening_signed_wide     uintptr
	vdso_zx_syscall_test_widening_unsigned_narrow uintptr
	vdso_zx_syscall_test_widening_unsigned_wide   uintptr
	vdso_zx_syscall_test_wrapper                  uintptr
	vdso_zx_syscall_test_0                        uintptr
	vdso_zx_syscall_test_1                        uintptr
	vdso_zx_syscall_test_2                        uintptr
	vdso_zx_syscall_test_3                        uintptr
	vdso_zx_syscall_test_4                        uintptr
	vdso_zx_syscall_test_5                        uintptr
	vdso_zx_syscall_test_6                        uintptr
	vdso_zx_syscall_test_7                        uintptr
	vdso_zx_syscall_test_8                        uintptr
	vdso_zx_system_get_dcache_line_size           uintptr
	vdso_zx_system_get_event                      uintptr
	vdso_zx_system_get_features                   uintptr
	vdso_zx_system_get_num_cpus                   uintptr
	vdso_zx_system_get_page_size                  uintptr
	vdso_zx_system_get_performance_info           uintptr
	vdso_zx_system_get_physmem                    uintptr
	vdso_zx_system_get_version_string             uintptr
	vdso_zx_system_mexec                          uintptr
	vdso_zx_system_mexec_payload_get              uintptr
	vdso_zx_system_powerctl                       uintptr
	vdso_zx_system_set_performance_info           uintptr
	vdso_zx_task_create_exception_channel         uintptr
	vdso_zx_task_kill                             uintptr
	vdso_zx_task_suspend                          uintptr
	vdso_zx_task_suspend_token                    uintptr
	vdso_zx_thread_create                         uintptr
	vdso_zx_thread_exit                           uintptr
	vdso_zx_thread_legacy_yield                   uintptr
	vdso_zx_thread_read_state                     uintptr
	vdso_zx_thread_start                          uintptr
	vdso_zx_thread_write_state                    uintptr
	vdso_zx_ticks_get                             uintptr
	vdso_zx_ticks_get_via_kernel                  uintptr
	vdso_zx_ticks_per_second                      uintptr
	vdso_zx_timer_cancel                          uintptr
	vdso_zx_timer_create                          uintptr
	vdso_zx_timer_set                             uintptr
	vdso_zx_vcpu_create                           uintptr
	vdso_zx_vcpu_enter                            uintptr
	vdso_zx_vcpu_interrupt                        uintptr
	vdso_zx_vcpu_kick                             uintptr
	vdso_zx_vcpu_read_state                       uintptr
	vdso_zx_vcpu_write_state                      uintptr
	vdso_zx_vmar_allocate                         uintptr
	vdso_zx_vmar_destroy                          uintptr
	vdso_zx_vmar_map                              uintptr
	vdso_zx_vmar_op_range                         uintptr
	vdso_zx_vmar_protect                          uintptr
	vdso_zx_vmar_unmap                            uintptr
	vdso_zx_vmar_unmap_handle_close_thread_exit   uintptr
	vdso_zx_vmo_create                            uintptr
	vdso_zx_vmo_create_child                      uintptr
	vdso_zx_vmo_create_contiguous                 uintptr
	vdso_zx_vmo_create_physical                   uintptr
	vdso_zx_vmo_get_size                          uintptr
	vdso_zx_vmo_op_range                          uintptr
	vdso_zx_vmo_read                              uintptr
	vdso_zx_vmo_replace_as_executable             uintptr
	vdso_zx_vmo_set_cache_policy                  uintptr
	vdso_zx_vmo_set_size                          uintptr
	vdso_zx_vmo_write                             uintptr
)
