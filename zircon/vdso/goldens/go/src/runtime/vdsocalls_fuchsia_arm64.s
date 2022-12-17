// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// DO NOT EDIT. Generated from FIDL library zx by zither, a Fuchsia platform
// tool.

#include "go_asm.h"
#include "go_tls.h"
#include "textflag.h"
#include "funcdata.h"


// func vdsoCall_zx_bti_create(iommu uint32, options uint32, bti_id uint64, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_bti_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW iommu+0(FP), R0
	MOVW options+4(FP), R1
	MOVD bti_id+8(FP), R2
	MOVD out+16(FP), R3
	BL vdso_zx_bti_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_bti_pin(handle uint32, options uint32, vmo uint32, offset uint64, size uint64, addrs unsafe.Pointer, num_addrs uint, pmt unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_bti_pin(SB),NOSPLIT,$0-60
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVW vmo+8(FP), R2
	MOVD offset+16(FP), R3
	MOVD size+24(FP), R4
	MOVD addrs+32(FP), R5
	MOVD num_addrs+40(FP), R6
	MOVD pmt+48(FP), R7
	BL vdso_zx_bti_pin(SB)
	MOVW R0, ret+56(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_bti_release_quarantine(handle uint32) int32
TEXT runtime·vdsoCall_zx_bti_release_quarantine(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_bti_release_quarantine(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_cache_flush(addr unsafe.Pointer, size uint, options uint32) int32
TEXT runtime·vdsoCall_zx_cache_flush(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD addr+0(FP), R0
	MOVD size+8(FP), R1
	MOVW options+16(FP), R2
	BL vdso_zx_cache_flush(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD deadline+8(FP), R2
	MOVD args+16(FP), R3
	MOVD actual_bytes+24(FP), R4
	MOVD actual_handles+32(FP), R5
	BL vdso_zx_channel_call(SB)
	MOVW R0, ret+40(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call_etc(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD deadline+8(FP), R2
	MOVD args+16(FP), R3
	MOVD actual_bytes+24(FP), R4
	MOVD actual_handles+32(FP), R5
	BL vdso_zx_channel_call_etc(SB)
	MOVW R0, ret+40(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call_etc_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc_finish(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD deadline+0(FP), R0
	MOVD args+8(FP), R1
	MOVD actual_bytes+16(FP), R2
	MOVD actual_handles+24(FP), R3
	BL vdso_zx_channel_call_etc_finish(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call_etc_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc_noretry(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD deadline+8(FP), R2
	MOVD args+16(FP), R3
	MOVD actual_bytes+24(FP), R4
	MOVD actual_handles+32(FP), R5
	BL vdso_zx_channel_call_etc_noretry(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_finish(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD deadline+0(FP), R0
	MOVD args+8(FP), R1
	MOVD actual_bytes+16(FP), R2
	MOVD actual_handles+24(FP), R3
	BL vdso_zx_channel_call_finish(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_call_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_noretry(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD deadline+8(FP), R2
	MOVD args+16(FP), R3
	MOVD actual_bytes+24(FP), R4
	MOVD actual_handles+32(FP), R5
	BL vdso_zx_channel_call_noretry(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out0+8(FP), R1
	MOVD out1+16(FP), R2
	BL vdso_zx_channel_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_read(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_read(SB),NOSPLIT,$0-52
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD bytes+8(FP), R2
	MOVD handles+16(FP), R3
	MOVW num_bytes+24(FP), R4
	MOVW num_handles+28(FP), R5
	MOVD actual_bytes+32(FP), R6
	MOVD actual_handles+40(FP), R7
	BL vdso_zx_channel_read(SB)
	MOVW R0, ret+48(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_read_etc(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_read_etc(SB),NOSPLIT,$0-52
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD bytes+8(FP), R2
	MOVD handles+16(FP), R3
	MOVW num_bytes+24(FP), R4
	MOVW num_handles+28(FP), R5
	MOVD actual_bytes+32(FP), R6
	MOVD actual_handles+40(FP), R7
	BL vdso_zx_channel_read_etc(SB)
	MOVW R0, ret+48(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_write(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32
TEXT runtime·vdsoCall_zx_channel_write(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD bytes+8(FP), R2
	MOVW num_bytes+16(FP), R3
	MOVD handles+24(FP), R4
	MOVW num_handles+32(FP), R5
	BL vdso_zx_channel_write(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_channel_write_etc(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32
TEXT runtime·vdsoCall_zx_channel_write_etc(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD bytes+8(FP), R2
	MOVW num_bytes+16(FP), R3
	MOVD handles+24(FP), R4
	MOVW num_handles+32(FP), R5
	BL vdso_zx_channel_write_etc(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_create(options uint64, args unsafe.Pointer, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD options+0(FP), R0
	MOVD args+8(FP), R1
	MOVD out+16(FP), R2
	BL vdso_zx_clock_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_get_details(handle uint32, options uint64, details unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_get_details(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD options+8(FP), R1
	MOVD details+16(FP), R2
	BL vdso_zx_clock_get_details(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_get_monotonic() int64
TEXT runtime·vdsoCall_zx_clock_get_monotonic(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_clock_get_monotonic(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_get_monotonic_via_kernel() int64
TEXT runtime·vdsoCall_zx_clock_get_monotonic_via_kernel(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_clock_get_monotonic_via_kernel(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_read(handle uint32, now unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_read(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD now+8(FP), R1
	BL vdso_zx_clock_read(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_clock_update(handle uint32, options uint64, args unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_update(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD options+8(FP), R1
	MOVD args+16(FP), R2
	BL vdso_zx_clock_update(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_cprng_add_entropy(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_cprng_add_entropy(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_cprng_add_entropy(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_cprng_draw(buffer unsafe.Pointer, buffer_size uint)
TEXT runtime·vdsoCall_zx_cprng_draw(SB),NOSPLIT,$0-16
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_cprng_draw(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_cprng_draw_once(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_cprng_draw_once(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_cprng_draw_once(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_deadline_after(nanoseconds int64) int64
TEXT runtime·vdsoCall_zx_deadline_after(SB),NOSPLIT,$0-16
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD nanoseconds+0(FP), R0
	BL vdso_zx_deadline_after(SB)
	MOVD R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debug_read(handle uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_debug_read(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD buffer+8(FP), R1
	MOVD buffer_size+16(FP), R2
	MOVD actual+24(FP), R3
	BL vdso_zx_debug_read(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debug_send_command(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debug_send_command(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD buffer+8(FP), R1
	MOVD buffer_size+16(FP), R2
	BL vdso_zx_debug_send_command(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debug_write(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debug_write(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_debug_write(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debuglog_create(resource uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_debuglog_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW options+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_debuglog_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debuglog_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debuglog_read(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_debuglog_read(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_debuglog_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debuglog_write(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_debuglog_write(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_event_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_event_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_event_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_eventpair_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_eventpair_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out0+8(FP), R1
	MOVD out1+16(FP), R2
	BL vdso_zx_eventpair_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_exception_get_process(handle uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_exception_get_process(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_exception_get_process(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_exception_get_thread(handle uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_exception_get_thread(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_exception_get_thread(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_fifo_create(elem_count uint, elem_size uint, options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_create(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD elem_count+0(FP), R0
	MOVD elem_size+8(FP), R1
	MOVW options+16(FP), R2
	MOVD out0+24(FP), R3
	MOVD out1+32(FP), R4
	BL vdso_zx_fifo_create(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_fifo_read(handle uint32, elem_size uint, data unsafe.Pointer, data_size uint, actual_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_read(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD elem_size+8(FP), R1
	MOVD data+16(FP), R2
	MOVD data_size+24(FP), R3
	MOVD actual_count+32(FP), R4
	BL vdso_zx_fifo_read(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_fifo_write(handle uint32, elem_size uint, data unsafe.Pointer, count uint, actual_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_write(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD elem_size+8(FP), R1
	MOVD data+16(FP), R2
	MOVD count+24(FP), R3
	MOVD actual_count+32(FP), R4
	BL vdso_zx_fifo_write(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_framebuffer_get_info(resource uint32, format unsafe.Pointer, width unsafe.Pointer, height unsafe.Pointer, stride unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_framebuffer_get_info(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD format+8(FP), R1
	MOVD width+16(FP), R2
	MOVD height+24(FP), R3
	MOVD stride+32(FP), R4
	BL vdso_zx_framebuffer_get_info(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_framebuffer_set_range(resource uint32, vmo uint32, len uint32, format uint32, width uint32, height uint32, stride uint32) int32
TEXT runtime·vdsoCall_zx_framebuffer_set_range(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW vmo+4(FP), R1
	MOVW len+8(FP), R2
	MOVW format+12(FP), R3
	MOVW width+16(FP), R4
	MOVW height+20(FP), R5
	MOVW stride+24(FP), R6
	BL vdso_zx_framebuffer_set_range(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_get_owner(value_ptr unsafe.Pointer, koid unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_futex_get_owner(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVD koid+8(FP), R1
	BL vdso_zx_futex_get_owner(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_requeue(value_ptr unsafe.Pointer, wake_count uint32, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32
TEXT runtime·vdsoCall_zx_futex_requeue(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVW wake_count+8(FP), R1
	MOVW current_value+12(FP), R2
	MOVD requeue_ptr+16(FP), R3
	MOVW requeue_count+24(FP), R4
	MOVW new_requeue_owner+28(FP), R5
	BL vdso_zx_futex_requeue(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_requeue_single_owner(value_ptr unsafe.Pointer, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32
TEXT runtime·vdsoCall_zx_futex_requeue_single_owner(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVW current_value+8(FP), R1
	MOVD requeue_ptr+16(FP), R2
	MOVW requeue_count+24(FP), R3
	MOVW new_requeue_owner+28(FP), R4
	BL vdso_zx_futex_requeue_single_owner(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_wait(value_ptr unsafe.Pointer, current_value int32, new_futex_owner uint32, deadline int64) int32
TEXT runtime·vdsoCall_zx_futex_wait(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVW current_value+8(FP), R1
	MOVW new_futex_owner+12(FP), R2
	MOVD deadline+16(FP), R3
	BL vdso_zx_futex_wait(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_wake(value_ptr unsafe.Pointer, wake_count uint32) int32
TEXT runtime·vdsoCall_zx_futex_wake(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVW wake_count+8(FP), R1
	BL vdso_zx_futex_wake(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_wake_handle_close_thread_exit(value_ptr unsafe.Pointer, wake_count uint32, new_value int32, close_handle uint32)
TEXT runtime·vdsoCall_zx_futex_wake_handle_close_thread_exit(SB),NOSPLIT,$0-24
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	MOVW wake_count+8(FP), R1
	MOVW new_value+12(FP), R2
	MOVW close_handle+16(FP), R3
	BL vdso_zx_futex_wake_handle_close_thread_exit(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_futex_wake_single_owner(value_ptr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_futex_wake_single_owner(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD value_ptr+0(FP), R0
	BL vdso_zx_futex_wake_single_owner(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_guest_create(resource uint32, options uint32, guest_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_guest_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW options+4(FP), R1
	MOVD guest_handle+8(FP), R2
	MOVD vmar_handle+16(FP), R3
	BL vdso_zx_guest_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_guest_set_trap(handle uint32, kind uint32, addr uintptr, size uint, port_handle uint32, key uint64) int32
TEXT runtime·vdsoCall_zx_guest_set_trap(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD addr+8(FP), R2
	MOVD size+16(FP), R3
	MOVW port_handle+24(FP), R4
	MOVD key+32(FP), R5
	BL vdso_zx_guest_set_trap(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_handle_close(handle uint32) int32
TEXT runtime·vdsoCall_zx_handle_close(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_handle_close(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_handle_close_many(handles unsafe.Pointer, num_handles uint) int32
TEXT runtime·vdsoCall_zx_handle_close_many(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD handles+0(FP), R0
	MOVD num_handles+8(FP), R1
	BL vdso_zx_handle_close_many(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_handle_duplicate(handle uint32, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_handle_duplicate(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW rights+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_handle_duplicate(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_handle_replace(handle uint32, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_handle_replace(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW rights+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_handle_replace(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_ack(handle uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_ack(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_interrupt_ack(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_bind(handle uint32, port_handle uint32, key uint64, options uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_bind(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW port_handle+4(FP), R1
	MOVD key+8(FP), R2
	MOVW options+16(FP), R3
	BL vdso_zx_interrupt_bind(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_create(src_obj uint32, src_num uint32, options uint32, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_interrupt_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW src_obj+0(FP), R0
	MOVW src_num+4(FP), R1
	MOVW options+8(FP), R2
	MOVD out_handle+16(FP), R3
	BL vdso_zx_interrupt_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_destroy(handle uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_destroy(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_interrupt_destroy(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_trigger(handle uint32, options uint32, timestamp int64) int32
TEXT runtime·vdsoCall_zx_interrupt_trigger(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD timestamp+8(FP), R2
	BL vdso_zx_interrupt_trigger(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_interrupt_wait(handle uint32, out_timestamp unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_interrupt_wait(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVD out_timestamp+8(FP), R1
	BL vdso_zx_interrupt_wait(SB)
	MOVW R0, ret+16(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_iommu_create(resource uint32, typ uint32, desc unsafe.Pointer, desc_size uint, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_iommu_create(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW typ+4(FP), R1
	MOVD desc+8(FP), R2
	MOVD desc_size+16(FP), R3
	MOVD out+24(FP), R4
	BL vdso_zx_iommu_create(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ioports_release(resource uint32, io_addr uint16, len uint32) int32
TEXT runtime·vdsoCall_zx_ioports_release(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD io_addr+4(FP), R1
	MOVW len+8(FP), R2
	BL vdso_zx_ioports_release(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ioports_request(resource uint32, io_addr uint16, len uint32) int32
TEXT runtime·vdsoCall_zx_ioports_request(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD io_addr+4(FP), R1
	MOVW len+8(FP), R2
	BL vdso_zx_ioports_request(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_job_create(parent_job uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_job_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW parent_job+0(FP), R0
	MOVW options+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_job_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_job_set_critical(job uint32, options uint32, process uint32) int32
TEXT runtime·vdsoCall_zx_job_set_critical(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW job+0(FP), R0
	MOVW options+4(FP), R1
	MOVW process+8(FP), R2
	BL vdso_zx_job_set_critical(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_job_set_policy(handle uint32, options uint32, topic uint32, policy unsafe.Pointer, policy_size uint32) int32
TEXT runtime·vdsoCall_zx_job_set_policy(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVW topic+8(FP), R2
	MOVD policy+16(FP), R3
	MOVW policy_size+24(FP), R4
	BL vdso_zx_job_set_policy(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ktrace_control(handle uint32, action uint32, options uint32, ptr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_ktrace_control(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW action+4(FP), R1
	MOVW options+8(FP), R2
	MOVD ptr+16(FP), R3
	BL vdso_zx_ktrace_control(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ktrace_read(handle uint32, data unsafe.Pointer, offset uint32, data_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_ktrace_read(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD data+8(FP), R1
	MOVW offset+16(FP), R2
	MOVD data_size+24(FP), R3
	MOVD actual+32(FP), R4
	BL vdso_zx_ktrace_read(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ktrace_write(handle uint32, id uint32, arg0 uint32, arg1 uint32) int32
TEXT runtime·vdsoCall_zx_ktrace_write(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW id+4(FP), R1
	MOVW arg0+8(FP), R2
	MOVW arg1+12(FP), R3
	BL vdso_zx_ktrace_write(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_msi_allocate(handle uint32, count uint32, out_allocation unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_msi_allocate(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW count+4(FP), R1
	MOVD out_allocation+8(FP), R2
	BL vdso_zx_msi_allocate(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_msi_create(handle uint32, options uint32, msi_id uint32, vmo uint32, vmo_offset uint, out_interrupt unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_msi_create(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVW msi_id+8(FP), R2
	MOVW vmo+12(FP), R3
	MOVD vmo_offset+16(FP), R4
	MOVD out_interrupt+24(FP), R5
	BL vdso_zx_msi_create(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_mtrace_control(handle uint32, kind uint32, action uint32, options uint32, ptr unsafe.Pointer, ptr_size uint) int32
TEXT runtime·vdsoCall_zx_mtrace_control(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVW action+8(FP), R2
	MOVW options+12(FP), R3
	MOVD ptr+16(FP), R4
	MOVD ptr_size+24(FP), R5
	BL vdso_zx_mtrace_control(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_nanosleep(deadline int64) int32
TEXT runtime·vdsoCall_zx_nanosleep(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD deadline+0(FP), R0
	BL vdso_zx_nanosleep(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_get_child(handle uint32, koid uint64, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_get_child(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD koid+8(FP), R1
	MOVW rights+16(FP), R2
	MOVD out+24(FP), R3
	BL vdso_zx_object_get_child(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_get_info(handle uint32, topic uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_get_info(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW topic+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	MOVD actual+24(FP), R4
	MOVD avail+32(FP), R5
	BL vdso_zx_object_get_info(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_get_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32
TEXT runtime·vdsoCall_zx_object_get_property(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW property+4(FP), R1
	MOVD value+8(FP), R2
	MOVD value_size+16(FP), R3
	BL vdso_zx_object_get_property(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_set_profile(handle uint32, profile uint32, options uint32) int32
TEXT runtime·vdsoCall_zx_object_set_profile(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW profile+4(FP), R1
	MOVW options+8(FP), R2
	BL vdso_zx_object_set_profile(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_set_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32
TEXT runtime·vdsoCall_zx_object_set_property(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW property+4(FP), R1
	MOVD value+8(FP), R2
	MOVD value_size+16(FP), R3
	BL vdso_zx_object_set_property(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_signal(handle uint32, clear_mask uint32, set_mask uint32) int32
TEXT runtime·vdsoCall_zx_object_signal(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW clear_mask+4(FP), R1
	MOVW set_mask+8(FP), R2
	BL vdso_zx_object_signal(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_signal_peer(handle uint32, clear_mask uint32, set_mask uint32) int32
TEXT runtime·vdsoCall_zx_object_signal_peer(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW clear_mask+4(FP), R1
	MOVW set_mask+8(FP), R2
	BL vdso_zx_object_signal_peer(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_wait_async(handle uint32, port uint32, key uint64, signals uint32, options uint32) int32
TEXT runtime·vdsoCall_zx_object_wait_async(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW port+4(FP), R1
	MOVD key+8(FP), R2
	MOVW signals+16(FP), R3
	MOVW options+20(FP), R4
	BL vdso_zx_object_wait_async(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_wait_many(items unsafe.Pointer, num_items uint, deadline int64) int32
TEXT runtime·vdsoCall_zx_object_wait_many(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVD items+0(FP), R0
	MOVD num_items+8(FP), R1
	MOVD deadline+16(FP), R2
	BL vdso_zx_object_wait_many(SB)
	MOVW R0, ret+24(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_object_wait_one(handle uint32, signals uint32, deadline int64, observed unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_wait_one(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW signals+4(FP), R1
	MOVD deadline+8(FP), R2
	MOVD observed+16(FP), R3
	BL vdso_zx_object_wait_one(SB)
	MOVW R0, ret+24(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_pager_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_create_vmo(pager uint32, options uint32, port uint32, key uint64, size uint64, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_create_vmo(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW options+4(FP), R1
	MOVW port+8(FP), R2
	MOVD key+16(FP), R3
	MOVD size+24(FP), R4
	MOVD out+32(FP), R5
	BL vdso_zx_pager_create_vmo(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_detach_vmo(pager uint32, vmo uint32) int32
TEXT runtime·vdsoCall_zx_pager_detach_vmo(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW vmo+4(FP), R1
	BL vdso_zx_pager_detach_vmo(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_op_range(pager uint32, op uint32, pager_vmo uint32, offset uint64, length uint64, data uint64) int32
TEXT runtime·vdsoCall_zx_pager_op_range(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW op+4(FP), R1
	MOVW pager_vmo+8(FP), R2
	MOVD offset+16(FP), R3
	MOVD length+24(FP), R4
	MOVD data+32(FP), R5
	BL vdso_zx_pager_op_range(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_query_dirty_ranges(pager uint32, pager_vmo uint32, offset uint64, length uint64, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_query_dirty_ranges(SB),NOSPLIT,$0-60
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW pager_vmo+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD length+16(FP), R3
	MOVD buffer+24(FP), R4
	MOVD buffer_size+32(FP), R5
	MOVD actual+40(FP), R6
	MOVD avail+48(FP), R7
	BL vdso_zx_pager_query_dirty_ranges(SB)
	MOVW R0, ret+56(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_query_vmo_stats(pager uint32, pager_vmo uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_pager_query_vmo_stats(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW pager_vmo+4(FP), R1
	MOVW options+8(FP), R2
	MOVD buffer+16(FP), R3
	MOVD buffer_size+24(FP), R4
	BL vdso_zx_pager_query_vmo_stats(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pager_supply_pages(pager uint32, pager_vmo uint32, offset uint64, length uint64, aux_vmo uint32, aux_offset uint64) int32
TEXT runtime·vdsoCall_zx_pager_supply_pages(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW pager+0(FP), R0
	MOVW pager_vmo+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD length+16(FP), R3
	MOVW aux_vmo+24(FP), R4
	MOVD aux_offset+32(FP), R5
	BL vdso_zx_pager_supply_pages(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pc_firmware_tables(handle uint32, acpi_rsdp unsafe.Pointer, smbios unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pc_firmware_tables(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD acpi_rsdp+8(FP), R1
	MOVD smbios+16(FP), R2
	BL vdso_zx_pc_firmware_tables(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_add_subtract_io_range(handle uint32, mmio uint32, base uint64, len uint64, add uint32) int32
TEXT runtime·vdsoCall_zx_pci_add_subtract_io_range(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW mmio+4(FP), R1
	MOVD base+8(FP), R2
	MOVD len+16(FP), R3
	MOVW add+24(FP), R4
	BL vdso_zx_pci_add_subtract_io_range(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_cfg_pio_rw(handle uint32, bus uint8, dev uint8, funk uint8, offset uint8, val unsafe.Pointer, width uint, write uint32) int32
TEXT runtime·vdsoCall_zx_pci_cfg_pio_rw(SB),NOSPLIT,$0-68
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD bus+8(FP), R1
	MOVD dev+16(FP), R2
	MOVD funk+24(FP), R3
	MOVD offset+32(FP), R4
	MOVD val+40(FP), R5
	MOVD width+48(FP), R6
	MOVW write+56(FP), R7
	BL vdso_zx_pci_cfg_pio_rw(SB)
	MOVW R0, ret+64(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_config_read(handle uint32, offset uint16, width uint, out_val unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_config_read(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD offset+4(FP), R1
	MOVD width+8(FP), R2
	MOVD out_val+16(FP), R3
	BL vdso_zx_pci_config_read(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_config_write(handle uint32, offset uint16, width uint, val uint32) int32
TEXT runtime·vdsoCall_zx_pci_config_write(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD offset+4(FP), R1
	MOVD width+8(FP), R2
	MOVW val+16(FP), R3
	BL vdso_zx_pci_config_write(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_enable_bus_master(handle uint32, enable uint32) int32
TEXT runtime·vdsoCall_zx_pci_enable_bus_master(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW enable+4(FP), R1
	BL vdso_zx_pci_enable_bus_master(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_get_bar(handle uint32, bar_num uint32, out_bar unsafe.Pointer, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_get_bar(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW bar_num+4(FP), R1
	MOVD out_bar+8(FP), R2
	MOVD out_handle+16(FP), R3
	BL vdso_zx_pci_get_bar(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_get_nth_device(handle uint32, index uint32, out_info unsafe.Pointer, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_get_nth_device(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW index+4(FP), R1
	MOVD out_info+8(FP), R2
	MOVD out_handle+16(FP), R3
	BL vdso_zx_pci_get_nth_device(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_init(handle uint32, init_buf unsafe.Pointer, len uint32) int32
TEXT runtime·vdsoCall_zx_pci_init(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD init_buf+8(FP), R1
	MOVW len+16(FP), R2
	BL vdso_zx_pci_init(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_map_interrupt(handle uint32, which_irq int32, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_map_interrupt(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW which_irq+4(FP), R1
	MOVD out_handle+8(FP), R2
	BL vdso_zx_pci_map_interrupt(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_query_irq_mode(handle uint32, mode uint32, out_max_irqs unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_query_irq_mode(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW mode+4(FP), R1
	MOVD out_max_irqs+8(FP), R2
	BL vdso_zx_pci_query_irq_mode(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_reset_device(handle uint32) int32
TEXT runtime·vdsoCall_zx_pci_reset_device(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_pci_reset_device(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pci_set_irq_mode(handle uint32, mode uint32, requested_irq_count uint32) int32
TEXT runtime·vdsoCall_zx_pci_set_irq_mode(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW mode+4(FP), R1
	MOVW requested_irq_count+8(FP), R2
	BL vdso_zx_pci_set_irq_mode(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_pmt_unpin(handle uint32) int32
TEXT runtime·vdsoCall_zx_pmt_unpin(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_pmt_unpin(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_port_cancel(handle uint32, source uint32, key uint64) int32
TEXT runtime·vdsoCall_zx_port_cancel(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW source+4(FP), R1
	MOVD key+8(FP), R2
	BL vdso_zx_port_cancel(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_port_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_port_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_port_queue(handle uint32, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_queue(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD packet+8(FP), R1
	BL vdso_zx_port_queue(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_port_wait(handle uint32, deadline int64, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_wait(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVD deadline+8(FP), R1
	MOVD packet+16(FP), R2
	BL vdso_zx_port_wait(SB)
	MOVW R0, ret+24(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_create(job uint32, name unsafe.Pointer, name_size uint, options uint32, proc_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_create(SB),NOSPLIT,$0-52
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW job+0(FP), R0
	MOVD name+8(FP), R1
	MOVD name_size+16(FP), R2
	MOVW options+24(FP), R3
	MOVD proc_handle+32(FP), R4
	MOVD vmar_handle+40(FP), R5
	BL vdso_zx_process_create(SB)
	MOVW R0, ret+48(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_create_shared(shared_proc uint32, options uint32, name unsafe.Pointer, name_size uint, proc_handle unsafe.Pointer, restricted_vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_create_shared(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW shared_proc+0(FP), R0
	MOVW options+4(FP), R1
	MOVD name+8(FP), R2
	MOVD name_size+16(FP), R3
	MOVD proc_handle+24(FP), R4
	MOVD restricted_vmar_handle+32(FP), R5
	BL vdso_zx_process_create_shared(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_exit(retcode int64)
TEXT runtime·vdsoCall_zx_process_exit(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD retcode+0(FP), R0
	BL vdso_zx_process_exit(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_read_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_read_memory(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD vaddr+8(FP), R1
	MOVD buffer+16(FP), R2
	MOVD buffer_size+24(FP), R3
	MOVD actual+32(FP), R4
	BL vdso_zx_process_read_memory(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_start(handle uint32, thread uint32, entry uintptr, stack uintptr, arg1 uint32, arg2 uintptr) int32
TEXT runtime·vdsoCall_zx_process_start(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW thread+4(FP), R1
	MOVD entry+8(FP), R2
	MOVD stack+16(FP), R3
	MOVW arg1+24(FP), R4
	MOVD arg2+32(FP), R5
	BL vdso_zx_process_start(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_process_write_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_write_memory(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD vaddr+8(FP), R1
	MOVD buffer+16(FP), R2
	MOVD buffer_size+24(FP), R3
	MOVD actual+32(FP), R4
	BL vdso_zx_process_write_memory(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_profile_create(root_job uint32, options uint32, profile unsafe.Pointer, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_profile_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW root_job+0(FP), R0
	MOVW options+4(FP), R1
	MOVD profile+8(FP), R2
	MOVD out+16(FP), R3
	BL vdso_zx_profile_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_resource_create(parent_rsrc uint32, options uint32, base uint64, size uint, name unsafe.Pointer, name_size uint, resource_out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_resource_create(SB),NOSPLIT,$0-52
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW parent_rsrc+0(FP), R0
	MOVW options+4(FP), R1
	MOVD base+8(FP), R2
	MOVD size+16(FP), R3
	MOVD name+24(FP), R4
	MOVD name_size+32(FP), R5
	MOVD resource_out+40(FP), R6
	BL vdso_zx_resource_create(SB)
	MOVW R0, ret+48(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_restricted_enter(options uint32, vector_table_ptr uintptr, context uintptr) int32
TEXT runtime·vdsoCall_zx_restricted_enter(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD vector_table_ptr+8(FP), R1
	MOVD context+16(FP), R2
	BL vdso_zx_restricted_enter(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_restricted_read_state(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_restricted_read_state(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_restricted_read_state(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_restricted_write_state(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_restricted_write_state(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD buffer+0(FP), R0
	MOVD buffer_size+8(FP), R1
	BL vdso_zx_restricted_write_state(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_smc_call(handle uint32, parameters unsafe.Pointer, out_smc_result unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_smc_call(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD parameters+8(FP), R1
	MOVD out_smc_result+16(FP), R2
	BL vdso_zx_smc_call(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_socket_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVD out0+8(FP), R1
	MOVD out1+16(FP), R2
	BL vdso_zx_socket_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_socket_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_read(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	MOVD actual+24(FP), R4
	BL vdso_zx_socket_read(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_socket_set_disposition(handle uint32, disposition uint32, disposition_peer uint32) int32
TEXT runtime·vdsoCall_zx_socket_set_disposition(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW disposition+4(FP), R1
	MOVW disposition_peer+8(FP), R2
	BL vdso_zx_socket_set_disposition(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_socket_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_write(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	MOVD actual+24(FP), R4
	BL vdso_zx_socket_write(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_create(options uint32, vmo uint32, seek uint64, out_stream unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVW vmo+4(FP), R1
	MOVD seek+8(FP), R2
	MOVD out_stream+16(FP), R3
	BL vdso_zx_stream_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_readv(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_readv(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD vectors+8(FP), R2
	MOVD num_vectors+16(FP), R3
	MOVD actual+24(FP), R4
	BL vdso_zx_stream_readv(SB)
	MOVW R0, ret+32(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_readv_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_readv_at(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD vectors+16(FP), R3
	MOVD num_vectors+24(FP), R4
	MOVD actual+32(FP), R5
	BL vdso_zx_stream_readv_at(SB)
	MOVW R0, ret+40(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_seek(handle uint32, whence uint32, offset int64, out_seek unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_seek(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW whence+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD out_seek+16(FP), R3
	BL vdso_zx_stream_seek(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_writev(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_writev(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD vectors+8(FP), R2
	MOVD num_vectors+16(FP), R3
	MOVD actual+24(FP), R4
	BL vdso_zx_stream_writev(SB)
	MOVW R0, ret+32(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_stream_writev_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_writev_at(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD vectors+16(FP), R3
	MOVD num_vectors+24(FP), R4
	MOVD actual+32(FP), R5
	BL vdso_zx_stream_writev_at(SB)
	MOVW R0, ret+40(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_next_1(arg int32) int32
TEXT runtime·vdsoCall_zx_syscall_next_1(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW arg+0(FP), R0
	BL vdso_zx_syscall_next_1(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_handle_create(return_value int32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_syscall_test_handle_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW return_value+0(FP), R0
	MOVD out+8(FP), R1
	BL vdso_zx_syscall_test_handle_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_widening_signed_narrow(a int64, b int32, c int16, d int8) int64
TEXT runtime·vdsoCall_zx_syscall_test_widening_signed_narrow(SB),NOSPLIT,$0-32
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD a+0(FP), R0
	MOVW b+8(FP), R1
	MOVD c+12(FP), R2
	MOVD d+16(FP), R3
	BL vdso_zx_syscall_test_widening_signed_narrow(SB)
	MOVD R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_widening_signed_wide(a int64, b int32, c int16, d int8) int64
TEXT runtime·vdsoCall_zx_syscall_test_widening_signed_wide(SB),NOSPLIT,$0-32
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD a+0(FP), R0
	MOVW b+8(FP), R1
	MOVD c+12(FP), R2
	MOVD d+16(FP), R3
	BL vdso_zx_syscall_test_widening_signed_wide(SB)
	MOVD R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_widening_unsigned_narrow(a uint64, b uint32, c uint16, d uint8) uint64
TEXT runtime·vdsoCall_zx_syscall_test_widening_unsigned_narrow(SB),NOSPLIT,$0-32
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD a+0(FP), R0
	MOVW b+8(FP), R1
	MOVD c+12(FP), R2
	MOVD d+16(FP), R3
	BL vdso_zx_syscall_test_widening_unsigned_narrow(SB)
	MOVD R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_widening_unsigned_wide(a uint64, b uint32, c uint16, d uint8) uint64
TEXT runtime·vdsoCall_zx_syscall_test_widening_unsigned_wide(SB),NOSPLIT,$0-32
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD a+0(FP), R0
	MOVW b+8(FP), R1
	MOVD c+12(FP), R2
	MOVD d+16(FP), R3
	BL vdso_zx_syscall_test_widening_unsigned_wide(SB)
	MOVD R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_wrapper(a int32, b int32, c int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_wrapper(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	BL vdso_zx_syscall_test_wrapper(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_0() int32
TEXT runtime·vdsoCall_zx_syscall_test_0(SB),NOSPLIT,$0-4
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_syscall_test_0(SB)
	MOVW R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_1(a int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_1(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	BL vdso_zx_syscall_test_1(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_2(a int32, b int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_2(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	BL vdso_zx_syscall_test_2(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_3(a int32, b int32, c int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_3(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	BL vdso_zx_syscall_test_3(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_4(a int32, b int32, c int32, d int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_4(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	MOVW d+12(FP), R3
	BL vdso_zx_syscall_test_4(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_5(a int32, b int32, c int32, d int32, e int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_5(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	MOVW d+12(FP), R3
	MOVW e+16(FP), R4
	BL vdso_zx_syscall_test_5(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_6(a int32, b int32, c int32, d int32, e int32, f int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_6(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	MOVW d+12(FP), R3
	MOVW e+16(FP), R4
	MOVW f+20(FP), R5
	BL vdso_zx_syscall_test_6(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_7(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_7(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	MOVW d+12(FP), R3
	MOVW e+16(FP), R4
	MOVW f+20(FP), R5
	MOVW g_+24(FP), R6
	BL vdso_zx_syscall_test_7(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_syscall_test_8(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32, h int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_8(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW a+0(FP), R0
	MOVW b+4(FP), R1
	MOVW c+8(FP), R2
	MOVW d+12(FP), R3
	MOVW e+16(FP), R4
	MOVW f+20(FP), R5
	MOVW g_+24(FP), R6
	MOVW h+28(FP), R7
	BL vdso_zx_syscall_test_8(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_dcache_line_size() uint32
TEXT runtime·vdsoCall_zx_system_get_dcache_line_size(SB),NOSPLIT,$0-4
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_system_get_dcache_line_size(SB)
	MOVW R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_event(root_job uint32, kind uint32, event unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_event(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW root_job+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD event+8(FP), R2
	BL vdso_zx_system_get_event(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_features(kind uint32, features unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_features(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW kind+0(FP), R0
	MOVD features+8(FP), R1
	BL vdso_zx_system_get_features(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_num_cpus() uint32
TEXT runtime·vdsoCall_zx_system_get_num_cpus(SB),NOSPLIT,$0-4
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_system_get_num_cpus(SB)
	MOVW R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_page_size() uint32
TEXT runtime·vdsoCall_zx_system_get_page_size(SB),NOSPLIT,$0-4
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_system_get_page_size(SB)
	MOVW R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_performance_info(resource uint32, topic uint32, count uint, info unsafe.Pointer, output_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_performance_info(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW topic+4(FP), R1
	MOVD count+8(FP), R2
	MOVD info+16(FP), R3
	MOVD output_count+24(FP), R4
	BL vdso_zx_system_get_performance_info(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_physmem() uint64
TEXT runtime·vdsoCall_zx_system_get_physmem(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_system_get_physmem(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_get_version_string() unsafe.Pointer
TEXT runtime·vdsoCall_zx_system_get_version_string(SB),NOSPLIT,$0-16
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_system_get_version_string(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_mexec(resource uint32, kernel_vmo uint32, bootimage_vmo uint32) int32
TEXT runtime·vdsoCall_zx_system_mexec(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW kernel_vmo+4(FP), R1
	MOVW bootimage_vmo+8(FP), R2
	BL vdso_zx_system_mexec(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_mexec_payload_get(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_system_mexec_payload_get(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD buffer+8(FP), R1
	MOVD buffer_size+16(FP), R2
	BL vdso_zx_system_mexec_payload_get(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_powerctl(resource uint32, cmd uint32, arg unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_powerctl(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW cmd+4(FP), R1
	MOVD arg+8(FP), R2
	BL vdso_zx_system_powerctl(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_system_set_performance_info(resource uint32, topic uint32, info unsafe.Pointer, count uint) int32
TEXT runtime·vdsoCall_zx_system_set_performance_info(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVW topic+4(FP), R1
	MOVD info+8(FP), R2
	MOVD count+16(FP), R3
	BL vdso_zx_system_set_performance_info(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_task_create_exception_channel(handle uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_create_exception_channel(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_task_create_exception_channel(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_task_kill(handle uint32) int32
TEXT runtime·vdsoCall_zx_task_kill(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_task_kill(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_task_suspend(handle uint32, token unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_suspend(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD token+8(FP), R1
	BL vdso_zx_task_suspend(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_task_suspend_token(handle uint32, token unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_suspend_token(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD token+8(FP), R1
	BL vdso_zx_task_suspend_token(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_create(process uint32, name unsafe.Pointer, name_size uint, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_thread_create(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW process+0(FP), R0
	MOVD name+8(FP), R1
	MOVD name_size+16(FP), R2
	MOVW options+24(FP), R3
	MOVD out+32(FP), R4
	BL vdso_zx_thread_create(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_exit()
TEXT runtime·vdsoCall_zx_thread_exit(SB),NOSPLIT,$0-0
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_thread_exit(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_legacy_yield(options uint32) int32
TEXT runtime·vdsoCall_zx_thread_legacy_yield(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	BL vdso_zx_thread_legacy_yield(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_thread_read_state(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_thread_read_state(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_start(handle uint32, thread_entry uintptr, stack uintptr, arg1 uintptr, arg2 uintptr) int32
TEXT runtime·vdsoCall_zx_thread_start(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD thread_entry+8(FP), R1
	MOVD stack+16(FP), R2
	MOVD arg1+24(FP), R3
	MOVD arg2+32(FP), R4
	BL vdso_zx_thread_start(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_thread_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_thread_write_state(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_thread_write_state(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ticks_get() int64
TEXT runtime·vdsoCall_zx_ticks_get(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_ticks_get(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ticks_get_via_kernel() int64
TEXT runtime·vdsoCall_zx_ticks_get_via_kernel(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_ticks_get_via_kernel(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_ticks_per_second() int64
TEXT runtime·vdsoCall_zx_ticks_per_second(SB),NOSPLIT,$0-8
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	BL vdso_zx_ticks_per_second(SB)
	MOVD R0, ret+0(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_timer_cancel(handle uint32) int32
TEXT runtime·vdsoCall_zx_timer_cancel(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_timer_cancel(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_timer_create(options uint32, clock_id uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_timer_create(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW options+0(FP), R0
	MOVW clock_id+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_timer_create(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_timer_set(handle uint32, deadline int64, slack int64) int32
TEXT runtime·vdsoCall_zx_timer_set(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD deadline+8(FP), R1
	MOVD slack+16(FP), R2
	BL vdso_zx_timer_set(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_create(guest uint32, options uint32, entry uintptr, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vcpu_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW guest+0(FP), R0
	MOVW options+4(FP), R1
	MOVD entry+8(FP), R2
	MOVD out+16(FP), R3
	BL vdso_zx_vcpu_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_enter(handle uint32, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vcpu_enter(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVD packet+8(FP), R1
	BL vdso_zx_vcpu_enter(SB)
	MOVW R0, ret+16(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_interrupt(handle uint32, vector uint32) int32
TEXT runtime·vdsoCall_zx_vcpu_interrupt(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW vector+4(FP), R1
	BL vdso_zx_vcpu_interrupt(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_kick(handle uint32) int32
TEXT runtime·vdsoCall_zx_vcpu_kick(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_vcpu_kick(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vcpu_read_state(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_vcpu_read_state(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vcpu_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vcpu_write_state(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW kind+4(FP), R1
	MOVD buffer+8(FP), R2
	MOVD buffer_size+16(FP), R3
	BL vdso_zx_vcpu_write_state(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_allocate(parent_vmar uint32, options uint32, offset uint, size uint, child_vmar unsafe.Pointer, child_addr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmar_allocate(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW parent_vmar+0(FP), R0
	MOVW options+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD size+16(FP), R3
	MOVD child_vmar+24(FP), R4
	MOVD child_addr+32(FP), R5
	BL vdso_zx_vmar_allocate(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_destroy(handle uint32) int32
TEXT runtime·vdsoCall_zx_vmar_destroy(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	BL vdso_zx_vmar_destroy(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_map(handle uint32, options uint32, vmar_offset uint, vmo uint32, vmo_offset uint64, len uint, mapped_addr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmar_map(SB),NOSPLIT,$0-52
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD vmar_offset+8(FP), R2
	MOVW vmo+16(FP), R3
	MOVD vmo_offset+24(FP), R4
	MOVD len+32(FP), R5
	MOVD mapped_addr+40(FP), R6
	BL vdso_zx_vmar_map(SB)
	MOVW R0, ret+48(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_op_range(handle uint32, op uint32, address uintptr, size uint, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmar_op_range(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW op+4(FP), R1
	MOVD address+8(FP), R2
	MOVD size+16(FP), R3
	MOVD buffer+24(FP), R4
	MOVD buffer_size+32(FP), R5
	BL vdso_zx_vmar_op_range(SB)
	MOVW R0, ret+40(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_protect(handle uint32, options uint32, addr uintptr, len uint) int32
TEXT runtime·vdsoCall_zx_vmar_protect(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD addr+8(FP), R2
	MOVD len+16(FP), R3
	BL vdso_zx_vmar_protect(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_unmap(handle uint32, addr uintptr, len uint) int32
TEXT runtime·vdsoCall_zx_vmar_unmap(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD addr+8(FP), R1
	MOVD len+16(FP), R2
	BL vdso_zx_vmar_unmap(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmar_unmap_handle_close_thread_exit(vmar_handle uint32, addr uintptr, size uint, close_handle uint32) int32
TEXT runtime·vdsoCall_zx_vmar_unmap_handle_close_thread_exit(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW vmar_handle+0(FP), R0
	MOVD addr+8(FP), R1
	MOVD size+16(FP), R2
	MOVW close_handle+24(FP), R3
	BL vdso_zx_vmar_unmap_handle_close_thread_exit(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_create(size uint64, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create(SB),NOSPLIT,$0-28
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVD size+0(FP), R0
	MOVW options+8(FP), R1
	MOVD out+16(FP), R2
	BL vdso_zx_vmo_create(SB)
	MOVW R0, ret+24(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_create_child(handle uint32, options uint32, offset uint64, size uint64, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_child(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW options+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD size+16(FP), R3
	MOVD out+24(FP), R4
	BL vdso_zx_vmo_create_child(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_create_contiguous(bti uint32, size uint, alignment_log2 uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_contiguous(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW bti+0(FP), R0
	MOVD size+8(FP), R1
	MOVW alignment_log2+16(FP), R2
	MOVD out+24(FP), R3
	BL vdso_zx_vmo_create_contiguous(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_create_physical(resource uint32, paddr uintptr, size uint, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_physical(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW resource+0(FP), R0
	MOVD paddr+8(FP), R1
	MOVD size+16(FP), R2
	MOVD out+24(FP), R3
	BL vdso_zx_vmo_create_physical(SB)
	MOVW R0, ret+32(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_get_size(handle uint32, size unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_get_size(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD size+8(FP), R1
	BL vdso_zx_vmo_get_size(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_op_range(handle uint32, op uint32, offset uint64, size uint64, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_op_range(SB),NOSPLIT,$0-44
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVW op+4(FP), R1
	MOVD offset+8(FP), R2
	MOVD size+16(FP), R3
	MOVD buffer+24(FP), R4
	MOVD buffer_size+32(FP), R5
	BL vdso_zx_vmo_op_range(SB)
	MOVW R0, ret+40(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_read(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_read(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVD buffer+8(FP), R1
	MOVD offset+16(FP), R2
	MOVD buffer_size+24(FP), R3
	BL vdso_zx_vmo_read(SB)
	MOVW R0, ret+32(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_replace_as_executable(handle uint32, vmex uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_replace_as_executable(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW vmex+4(FP), R1
	MOVD out+8(FP), R2
	BL vdso_zx_vmo_replace_as_executable(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_set_cache_policy(handle uint32, cache_policy uint32) int32
TEXT runtime·vdsoCall_zx_vmo_set_cache_policy(SB),NOSPLIT,$0-12
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVW cache_policy+4(FP), R1
	BL vdso_zx_vmo_set_cache_policy(SB)
	MOVW R0, ret+8(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_set_size(handle uint32, size uint64) int32
TEXT runtime·vdsoCall_zx_vmo_set_size(SB),NOSPLIT,$0-20
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	MOVW handle+0(FP), R0
	MOVD size+8(FP), R1
	BL vdso_zx_vmo_set_size(SB)
	MOVW R0, ret+16(FP)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET

// func vdsoCall_zx_vmo_write(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_write(SB),NOSPLIT,$0-36
	GO_ARGS
	NO_LOCAL_POINTERS
	MOVD g_m(g), R21
	MOVD LR, m_vdsoPC(R21)
	DMB $0xe
	MOVD $ret-8(FP), R20 // caller's SP
	MOVD R20, m_vdsoSP(R21)
	CALL runtime·entersyscall(SB)
	MOVW handle+0(FP), R0
	MOVD buffer+8(FP), R1
	MOVD offset+16(FP), R2
	MOVD buffer_size+24(FP), R3
	BL vdso_zx_vmo_write(SB)
	MOVW R0, ret+32(FP)
	BL runtime·exitsyscall(SB)
	MOVD g_m(g), R21
	MOVD $0, m_vdsoSP(R21)
	RET
