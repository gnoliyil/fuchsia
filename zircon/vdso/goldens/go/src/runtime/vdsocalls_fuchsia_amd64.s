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
TEXT runtime·vdsoCall_zx_bti_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL iommu+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ bti_id+8(FP), DX
	MOVQ out+16(FP), CX
	MOVQ vdso_zx_bti_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_bti_pin(handle uint32, options uint32, vmo uint32, offset uint64, size uint64, addrs unsafe.Pointer, num_addrs uint, pmt unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_bti_pin(SB),NOSPLIT,$40-60
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVL vmo+8(FP), DX
	MOVQ offset+16(FP), CX
	MOVQ size+24(FP), R8
	MOVQ addrs+32(FP), R9
	MOVQ num_addrs+40(FP), R12
	MOVQ pmt+48(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_bti_pin(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+56(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_bti_release_quarantine(handle uint32) int32
TEXT runtime·vdsoCall_zx_bti_release_quarantine(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_bti_release_quarantine(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_cache_flush(addr unsafe.Pointer, size uint, options uint32) int32
TEXT runtime·vdsoCall_zx_cache_flush(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ addr+0(FP), DI
	MOVQ size+8(FP), SI
	MOVL options+16(FP), DX
	MOVQ vdso_zx_cache_flush(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ deadline+8(FP), DX
	MOVQ args+16(FP), CX
	MOVQ actual_bytes+24(FP), R8
	MOVQ actual_handles+32(FP), R9
	MOVQ vdso_zx_channel_call(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call_etc(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ deadline+8(FP), DX
	MOVQ args+16(FP), CX
	MOVQ actual_bytes+24(FP), R8
	MOVQ actual_handles+32(FP), R9
	MOVQ vdso_zx_channel_call_etc(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call_etc_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc_finish(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ deadline+0(FP), DI
	MOVQ args+8(FP), SI
	MOVQ actual_bytes+16(FP), DX
	MOVQ actual_handles+24(FP), CX
	MOVQ vdso_zx_channel_call_etc_finish(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call_etc_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_etc_noretry(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ deadline+8(FP), DX
	MOVQ args+16(FP), CX
	MOVQ actual_bytes+24(FP), R8
	MOVQ actual_handles+32(FP), R9
	MOVQ vdso_zx_channel_call_etc_noretry(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call_finish(deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_finish(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ deadline+0(FP), DI
	MOVQ args+8(FP), SI
	MOVQ actual_bytes+16(FP), DX
	MOVQ actual_handles+24(FP), CX
	MOVQ vdso_zx_channel_call_finish(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_call_noretry(handle uint32, options uint32, deadline int64, args unsafe.Pointer, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_call_noretry(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ deadline+8(FP), DX
	MOVQ args+16(FP), CX
	MOVQ actual_bytes+24(FP), R8
	MOVQ actual_handles+32(FP), R9
	MOVQ vdso_zx_channel_call_noretry(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out0+8(FP), SI
	MOVQ out1+16(FP), DX
	MOVQ vdso_zx_channel_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_read(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_read(SB),NOSPLIT,$40-52
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ bytes+8(FP), DX
	MOVQ handles+16(FP), CX
	MOVL num_bytes+24(FP), R8
	MOVL num_handles+28(FP), R9
	MOVQ actual_bytes+32(FP), R12
	MOVQ actual_handles+40(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_channel_read(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+48(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_read_etc(handle uint32, options uint32, bytes unsafe.Pointer, handles unsafe.Pointer, num_bytes uint32, num_handles uint32, actual_bytes unsafe.Pointer, actual_handles unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_channel_read_etc(SB),NOSPLIT,$40-52
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ bytes+8(FP), DX
	MOVQ handles+16(FP), CX
	MOVL num_bytes+24(FP), R8
	MOVL num_handles+28(FP), R9
	MOVQ actual_bytes+32(FP), R12
	MOVQ actual_handles+40(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_channel_read_etc(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+48(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_write(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32
TEXT runtime·vdsoCall_zx_channel_write(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ bytes+8(FP), DX
	MOVL num_bytes+16(FP), CX
	MOVQ handles+24(FP), R8
	MOVL num_handles+32(FP), R9
	MOVQ vdso_zx_channel_write(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_channel_write_etc(handle uint32, options uint32, bytes unsafe.Pointer, num_bytes uint32, handles unsafe.Pointer, num_handles uint32) int32
TEXT runtime·vdsoCall_zx_channel_write_etc(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ bytes+8(FP), DX
	MOVL num_bytes+16(FP), CX
	MOVQ handles+24(FP), R8
	MOVL num_handles+32(FP), R9
	MOVQ vdso_zx_channel_write_etc(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_create(options uint64, args unsafe.Pointer, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ options+0(FP), DI
	MOVQ args+8(FP), SI
	MOVQ out+16(FP), DX
	MOVQ vdso_zx_clock_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_get_details(handle uint32, options uint64, details unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_get_details(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ options+8(FP), SI
	MOVQ details+16(FP), DX
	MOVQ vdso_zx_clock_get_details(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_get_monotonic() int64
TEXT runtime·vdsoCall_zx_clock_get_monotonic(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_clock_get_monotonic(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_get_monotonic_via_kernel() int64
TEXT runtime·vdsoCall_zx_clock_get_monotonic_via_kernel(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_clock_get_monotonic_via_kernel(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_read(handle uint32, now unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_read(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ now+8(FP), SI
	MOVQ vdso_zx_clock_read(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_clock_update(handle uint32, options uint64, args unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_clock_update(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ options+8(FP), SI
	MOVQ args+16(FP), DX
	MOVQ vdso_zx_clock_update(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_cprng_add_entropy(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_cprng_add_entropy(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_cprng_add_entropy(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_cprng_draw(buffer unsafe.Pointer, buffer_size uint)
TEXT runtime·vdsoCall_zx_cprng_draw(SB),NOSPLIT,$8-16
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_cprng_draw(SB), AX
	CALL AX
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_cprng_draw_once(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_cprng_draw_once(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_cprng_draw_once(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_deadline_after(nanoseconds int64) int64
TEXT runtime·vdsoCall_zx_deadline_after(SB),NOSPLIT,$8-16
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ nanoseconds+0(FP), DI
	MOVQ vdso_zx_deadline_after(SB), AX
	CALL AX
	MOVQ AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debug_read(handle uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_debug_read(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ buffer+8(FP), SI
	MOVQ buffer_size+16(FP), DX
	MOVQ actual+24(FP), CX
	MOVQ vdso_zx_debug_read(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debug_send_command(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debug_send_command(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ buffer+8(FP), SI
	MOVQ buffer_size+16(FP), DX
	MOVQ vdso_zx_debug_send_command(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debug_write(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debug_write(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_debug_write(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debuglog_create(resource uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_debuglog_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_debuglog_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debuglog_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debuglog_read(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_debuglog_read(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_debuglog_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_debuglog_write(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_debuglog_write(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_event_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_event_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_event_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_eventpair_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_eventpair_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out0+8(FP), SI
	MOVQ out1+16(FP), DX
	MOVQ vdso_zx_eventpair_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_exception_get_process(handle uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_exception_get_process(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_exception_get_process(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_exception_get_thread(handle uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_exception_get_thread(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_exception_get_thread(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_fifo_create(elem_count uint, elem_size uint, options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_create(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ elem_count+0(FP), DI
	MOVQ elem_size+8(FP), SI
	MOVL options+16(FP), DX
	MOVQ out0+24(FP), CX
	MOVQ out1+32(FP), R8
	MOVQ vdso_zx_fifo_create(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_fifo_read(handle uint32, elem_size uint, data unsafe.Pointer, data_size uint, actual_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_read(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ elem_size+8(FP), SI
	MOVQ data+16(FP), DX
	MOVQ data_size+24(FP), CX
	MOVQ actual_count+32(FP), R8
	MOVQ vdso_zx_fifo_read(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_fifo_write(handle uint32, elem_size uint, data unsafe.Pointer, count uint, actual_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_fifo_write(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ elem_size+8(FP), SI
	MOVQ data+16(FP), DX
	MOVQ count+24(FP), CX
	MOVQ actual_count+32(FP), R8
	MOVQ vdso_zx_fifo_write(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_framebuffer_get_info(resource uint32, format unsafe.Pointer, width unsafe.Pointer, height unsafe.Pointer, stride unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_framebuffer_get_info(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ format+8(FP), SI
	MOVQ width+16(FP), DX
	MOVQ height+24(FP), CX
	MOVQ stride+32(FP), R8
	MOVQ vdso_zx_framebuffer_get_info(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_framebuffer_set_range(resource uint32, vmo uint32, len uint32, format uint32, width uint32, height uint32, stride uint32) int32
TEXT runtime·vdsoCall_zx_framebuffer_set_range(SB),NOSPLIT,$32-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL vmo+4(FP), SI
	MOVL len+8(FP), DX
	MOVL format+12(FP), CX
	MOVL width+16(FP), R8
	MOVL height+20(FP), R9
	MOVL stride+24(FP), R12
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R12
	MOVQ vdso_zx_framebuffer_set_range(SB), AX
	CALL AX
	POPQ R12
	MOVQ BP, SP
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_get_owner(value_ptr unsafe.Pointer, koid unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_futex_get_owner(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVQ koid+8(FP), SI
	MOVQ vdso_zx_futex_get_owner(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_requeue(value_ptr unsafe.Pointer, wake_count uint32, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32
TEXT runtime·vdsoCall_zx_futex_requeue(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVL wake_count+8(FP), SI
	MOVL current_value+12(FP), DX
	MOVQ requeue_ptr+16(FP), CX
	MOVL requeue_count+24(FP), R8
	MOVL new_requeue_owner+28(FP), R9
	MOVQ vdso_zx_futex_requeue(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_requeue_single_owner(value_ptr unsafe.Pointer, current_value int32, requeue_ptr unsafe.Pointer, requeue_count uint32, new_requeue_owner uint32) int32
TEXT runtime·vdsoCall_zx_futex_requeue_single_owner(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVL current_value+8(FP), SI
	MOVQ requeue_ptr+16(FP), DX
	MOVL requeue_count+24(FP), CX
	MOVL new_requeue_owner+28(FP), R8
	MOVQ vdso_zx_futex_requeue_single_owner(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_wait(value_ptr unsafe.Pointer, current_value int32, new_futex_owner uint32, deadline int64) int32
TEXT runtime·vdsoCall_zx_futex_wait(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVL current_value+8(FP), SI
	MOVL new_futex_owner+12(FP), DX
	MOVQ deadline+16(FP), CX
	MOVQ vdso_zx_futex_wait(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_wake(value_ptr unsafe.Pointer, wake_count uint32) int32
TEXT runtime·vdsoCall_zx_futex_wake(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVL wake_count+8(FP), SI
	MOVQ vdso_zx_futex_wake(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_wake_handle_close_thread_exit(value_ptr unsafe.Pointer, wake_count uint32, new_value int32, close_handle uint32)
TEXT runtime·vdsoCall_zx_futex_wake_handle_close_thread_exit(SB),NOSPLIT,$8-24
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVL wake_count+8(FP), SI
	MOVL new_value+12(FP), DX
	MOVL close_handle+16(FP), CX
	MOVQ vdso_zx_futex_wake_handle_close_thread_exit(SB), AX
	CALL AX
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_futex_wake_single_owner(value_ptr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_futex_wake_single_owner(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ value_ptr+0(FP), DI
	MOVQ vdso_zx_futex_wake_single_owner(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_guest_create(resource uint32, options uint32, guest_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_guest_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ guest_handle+8(FP), DX
	MOVQ vmar_handle+16(FP), CX
	MOVQ vdso_zx_guest_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_guest_set_trap(handle uint32, kind uint32, addr uintptr, size uint, port_handle uint32, key uint64) int32
TEXT runtime·vdsoCall_zx_guest_set_trap(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ addr+8(FP), DX
	MOVQ size+16(FP), CX
	MOVL port_handle+24(FP), R8
	MOVQ key+32(FP), R9
	MOVQ vdso_zx_guest_set_trap(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_handle_close(handle uint32) int32
TEXT runtime·vdsoCall_zx_handle_close(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_handle_close(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_handle_close_many(handles unsafe.Pointer, num_handles uint) int32
TEXT runtime·vdsoCall_zx_handle_close_many(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ handles+0(FP), DI
	MOVQ num_handles+8(FP), SI
	MOVQ vdso_zx_handle_close_many(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_handle_duplicate(handle uint32, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_handle_duplicate(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL rights+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_handle_duplicate(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_handle_replace(handle uint32, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_handle_replace(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL rights+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_handle_replace(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_ack(handle uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_ack(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_interrupt_ack(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_bind(handle uint32, port_handle uint32, key uint64, options uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_bind(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL port_handle+4(FP), SI
	MOVQ key+8(FP), DX
	MOVL options+16(FP), CX
	MOVQ vdso_zx_interrupt_bind(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_create(src_obj uint32, src_num uint32, options uint32, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_interrupt_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL src_obj+0(FP), DI
	MOVL src_num+4(FP), SI
	MOVL options+8(FP), DX
	MOVQ out_handle+16(FP), CX
	MOVQ vdso_zx_interrupt_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_destroy(handle uint32) int32
TEXT runtime·vdsoCall_zx_interrupt_destroy(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_interrupt_destroy(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_trigger(handle uint32, options uint32, timestamp int64) int32
TEXT runtime·vdsoCall_zx_interrupt_trigger(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ timestamp+8(FP), DX
	MOVQ vdso_zx_interrupt_trigger(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_interrupt_wait(handle uint32, out_timestamp unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_interrupt_wait(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVQ out_timestamp+8(FP), SI
	MOVQ vdso_zx_interrupt_wait(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_iommu_create(resource uint32, typ uint32, desc unsafe.Pointer, desc_size uint, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_iommu_create(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL typ+4(FP), SI
	MOVQ desc+8(FP), DX
	MOVQ desc_size+16(FP), CX
	MOVQ out+24(FP), R8
	MOVQ vdso_zx_iommu_create(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ioports_release(resource uint32, io_addr uint16, len uint32) int32
TEXT runtime·vdsoCall_zx_ioports_release(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ io_addr+4(FP), SI
	MOVL len+8(FP), DX
	MOVQ vdso_zx_ioports_release(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ioports_request(resource uint32, io_addr uint16, len uint32) int32
TEXT runtime·vdsoCall_zx_ioports_request(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ io_addr+4(FP), SI
	MOVL len+8(FP), DX
	MOVQ vdso_zx_ioports_request(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_job_create(parent_job uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_job_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL parent_job+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_job_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_job_set_critical(job uint32, options uint32, process uint32) int32
TEXT runtime·vdsoCall_zx_job_set_critical(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL job+0(FP), DI
	MOVL options+4(FP), SI
	MOVL process+8(FP), DX
	MOVQ vdso_zx_job_set_critical(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_job_set_policy(handle uint32, options uint32, topic uint32, policy unsafe.Pointer, policy_size uint32) int32
TEXT runtime·vdsoCall_zx_job_set_policy(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVL topic+8(FP), DX
	MOVQ policy+16(FP), CX
	MOVL policy_size+24(FP), R8
	MOVQ vdso_zx_job_set_policy(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ktrace_control(handle uint32, action uint32, options uint32, ptr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_ktrace_control(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL action+4(FP), SI
	MOVL options+8(FP), DX
	MOVQ ptr+16(FP), CX
	MOVQ vdso_zx_ktrace_control(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ktrace_read(handle uint32, data unsafe.Pointer, offset uint32, data_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_ktrace_read(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ data+8(FP), SI
	MOVL offset+16(FP), DX
	MOVQ data_size+24(FP), CX
	MOVQ actual+32(FP), R8
	MOVQ vdso_zx_ktrace_read(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ktrace_write(handle uint32, id uint32, arg0 uint32, arg1 uint32) int32
TEXT runtime·vdsoCall_zx_ktrace_write(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL id+4(FP), SI
	MOVL arg0+8(FP), DX
	MOVL arg1+12(FP), CX
	MOVQ vdso_zx_ktrace_write(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_msi_allocate(handle uint32, count uint32, out_allocation unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_msi_allocate(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL count+4(FP), SI
	MOVQ out_allocation+8(FP), DX
	MOVQ vdso_zx_msi_allocate(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_msi_create(handle uint32, options uint32, msi_id uint32, vmo uint32, vmo_offset uint, out_interrupt unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_msi_create(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVL msi_id+8(FP), DX
	MOVL vmo+12(FP), CX
	MOVQ vmo_offset+16(FP), R8
	MOVQ out_interrupt+24(FP), R9
	MOVQ vdso_zx_msi_create(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_mtrace_control(handle uint32, kind uint32, action uint32, options uint32, ptr unsafe.Pointer, ptr_size uint) int32
TEXT runtime·vdsoCall_zx_mtrace_control(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVL action+8(FP), DX
	MOVL options+12(FP), CX
	MOVQ ptr+16(FP), R8
	MOVQ ptr_size+24(FP), R9
	MOVQ vdso_zx_mtrace_control(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_nanosleep(deadline int64) int32
TEXT runtime·vdsoCall_zx_nanosleep(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ deadline+0(FP), DI
	MOVQ vdso_zx_nanosleep(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_get_child(handle uint32, koid uint64, rights uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_get_child(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ koid+8(FP), SI
	MOVL rights+16(FP), DX
	MOVQ out+24(FP), CX
	MOVQ vdso_zx_object_get_child(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_get_info(handle uint32, topic uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_get_info(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL topic+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ actual+24(FP), R8
	MOVQ avail+32(FP), R9
	MOVQ vdso_zx_object_get_info(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_get_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32
TEXT runtime·vdsoCall_zx_object_get_property(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL property+4(FP), SI
	MOVQ value+8(FP), DX
	MOVQ value_size+16(FP), CX
	MOVQ vdso_zx_object_get_property(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_set_profile(handle uint32, profile uint32, options uint32) int32
TEXT runtime·vdsoCall_zx_object_set_profile(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL profile+4(FP), SI
	MOVL options+8(FP), DX
	MOVQ vdso_zx_object_set_profile(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_set_property(handle uint32, property uint32, value unsafe.Pointer, value_size uint) int32
TEXT runtime·vdsoCall_zx_object_set_property(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL property+4(FP), SI
	MOVQ value+8(FP), DX
	MOVQ value_size+16(FP), CX
	MOVQ vdso_zx_object_set_property(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_signal(handle uint32, clear_mask uint32, set_mask uint32) int32
TEXT runtime·vdsoCall_zx_object_signal(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL clear_mask+4(FP), SI
	MOVL set_mask+8(FP), DX
	MOVQ vdso_zx_object_signal(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_signal_peer(handle uint32, clear_mask uint32, set_mask uint32) int32
TEXT runtime·vdsoCall_zx_object_signal_peer(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL clear_mask+4(FP), SI
	MOVL set_mask+8(FP), DX
	MOVQ vdso_zx_object_signal_peer(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_wait_async(handle uint32, port uint32, key uint64, signals uint32, options uint32) int32
TEXT runtime·vdsoCall_zx_object_wait_async(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL port+4(FP), SI
	MOVQ key+8(FP), DX
	MOVL signals+16(FP), CX
	MOVL options+20(FP), R8
	MOVQ vdso_zx_object_wait_async(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_wait_many(items unsafe.Pointer, num_items uint, deadline int64) int32
TEXT runtime·vdsoCall_zx_object_wait_many(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVQ items+0(FP), DI
	MOVQ num_items+8(FP), SI
	MOVQ deadline+16(FP), DX
	MOVQ vdso_zx_object_wait_many(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_object_wait_one(handle uint32, signals uint32, deadline int64, observed unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_object_wait_one(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL signals+4(FP), SI
	MOVQ deadline+8(FP), DX
	MOVQ observed+16(FP), CX
	MOVQ vdso_zx_object_wait_one(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_pager_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_create_vmo(pager uint32, options uint32, port uint32, key uint64, size uint64, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_create_vmo(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL options+4(FP), SI
	MOVL port+8(FP), DX
	MOVQ key+16(FP), CX
	MOVQ size+24(FP), R8
	MOVQ out+32(FP), R9
	MOVQ vdso_zx_pager_create_vmo(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_detach_vmo(pager uint32, vmo uint32) int32
TEXT runtime·vdsoCall_zx_pager_detach_vmo(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL vmo+4(FP), SI
	MOVQ vdso_zx_pager_detach_vmo(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_op_range(pager uint32, op uint32, pager_vmo uint32, offset uint64, length uint64, data uint64) int32
TEXT runtime·vdsoCall_zx_pager_op_range(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL op+4(FP), SI
	MOVL pager_vmo+8(FP), DX
	MOVQ offset+16(FP), CX
	MOVQ length+24(FP), R8
	MOVQ data+32(FP), R9
	MOVQ vdso_zx_pager_op_range(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_query_dirty_ranges(pager uint32, pager_vmo uint32, offset uint64, length uint64, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer, avail unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pager_query_dirty_ranges(SB),NOSPLIT,$40-60
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL pager_vmo+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ length+16(FP), CX
	MOVQ buffer+24(FP), R8
	MOVQ buffer_size+32(FP), R9
	MOVQ actual+40(FP), R12
	MOVQ avail+48(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_pager_query_dirty_ranges(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+56(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_query_vmo_stats(pager uint32, pager_vmo uint32, options uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_pager_query_vmo_stats(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL pager_vmo+4(FP), SI
	MOVL options+8(FP), DX
	MOVQ buffer+16(FP), CX
	MOVQ buffer_size+24(FP), R8
	MOVQ vdso_zx_pager_query_vmo_stats(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pager_supply_pages(pager uint32, pager_vmo uint32, offset uint64, length uint64, aux_vmo uint32, aux_offset uint64) int32
TEXT runtime·vdsoCall_zx_pager_supply_pages(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL pager+0(FP), DI
	MOVL pager_vmo+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ length+16(FP), CX
	MOVL aux_vmo+24(FP), R8
	MOVQ aux_offset+32(FP), R9
	MOVQ vdso_zx_pager_supply_pages(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pc_firmware_tables(handle uint32, acpi_rsdp unsafe.Pointer, smbios unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pc_firmware_tables(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ acpi_rsdp+8(FP), SI
	MOVQ smbios+16(FP), DX
	MOVQ vdso_zx_pc_firmware_tables(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_add_subtract_io_range(handle uint32, mmio uint32, base uint64, len uint64, add uint32) int32
TEXT runtime·vdsoCall_zx_pci_add_subtract_io_range(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL mmio+4(FP), SI
	MOVQ base+8(FP), DX
	MOVQ len+16(FP), CX
	MOVL add+24(FP), R8
	MOVQ vdso_zx_pci_add_subtract_io_range(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_cfg_pio_rw(handle uint32, bus uint8, dev uint8, funk uint8, offset uint8, val unsafe.Pointer, width uint, write uint32) int32
TEXT runtime·vdsoCall_zx_pci_cfg_pio_rw(SB),NOSPLIT,$40-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ bus+4(FP), SI
	MOVQ dev+5(FP), DX
	MOVQ funk+6(FP), CX
	MOVQ offset+7(FP), R8
	MOVQ val+8(FP), R9
	MOVQ width+16(FP), R12
	MOVL write+24(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_pci_cfg_pio_rw(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_config_read(handle uint32, offset uint16, width uint, out_val unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_config_read(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ offset+4(FP), SI
	MOVQ width+8(FP), DX
	MOVQ out_val+16(FP), CX
	MOVQ vdso_zx_pci_config_read(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_config_write(handle uint32, offset uint16, width uint, val uint32) int32
TEXT runtime·vdsoCall_zx_pci_config_write(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ offset+4(FP), SI
	MOVQ width+8(FP), DX
	MOVL val+16(FP), CX
	MOVQ vdso_zx_pci_config_write(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_enable_bus_master(handle uint32, enable uint32) int32
TEXT runtime·vdsoCall_zx_pci_enable_bus_master(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL enable+4(FP), SI
	MOVQ vdso_zx_pci_enable_bus_master(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_get_bar(handle uint32, bar_num uint32, out_bar unsafe.Pointer, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_get_bar(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL bar_num+4(FP), SI
	MOVQ out_bar+8(FP), DX
	MOVQ out_handle+16(FP), CX
	MOVQ vdso_zx_pci_get_bar(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_get_nth_device(handle uint32, index uint32, out_info unsafe.Pointer, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_get_nth_device(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL index+4(FP), SI
	MOVQ out_info+8(FP), DX
	MOVQ out_handle+16(FP), CX
	MOVQ vdso_zx_pci_get_nth_device(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_init(handle uint32, init_buf unsafe.Pointer, len uint32) int32
TEXT runtime·vdsoCall_zx_pci_init(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ init_buf+8(FP), SI
	MOVL len+16(FP), DX
	MOVQ vdso_zx_pci_init(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_map_interrupt(handle uint32, which_irq int32, out_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_map_interrupt(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL which_irq+4(FP), SI
	MOVQ out_handle+8(FP), DX
	MOVQ vdso_zx_pci_map_interrupt(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_query_irq_mode(handle uint32, mode uint32, out_max_irqs unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_pci_query_irq_mode(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL mode+4(FP), SI
	MOVQ out_max_irqs+8(FP), DX
	MOVQ vdso_zx_pci_query_irq_mode(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_reset_device(handle uint32) int32
TEXT runtime·vdsoCall_zx_pci_reset_device(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_pci_reset_device(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pci_set_irq_mode(handle uint32, mode uint32, requested_irq_count uint32) int32
TEXT runtime·vdsoCall_zx_pci_set_irq_mode(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL mode+4(FP), SI
	MOVL requested_irq_count+8(FP), DX
	MOVQ vdso_zx_pci_set_irq_mode(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_pmt_unpin(handle uint32) int32
TEXT runtime·vdsoCall_zx_pmt_unpin(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_pmt_unpin(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_port_cancel(handle uint32, source uint32, key uint64) int32
TEXT runtime·vdsoCall_zx_port_cancel(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL source+4(FP), SI
	MOVQ key+8(FP), DX
	MOVQ vdso_zx_port_cancel(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_port_create(options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_port_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_port_queue(handle uint32, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_queue(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ packet+8(FP), SI
	MOVQ vdso_zx_port_queue(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_port_wait(handle uint32, deadline int64, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_port_wait(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVQ deadline+8(FP), SI
	MOVQ packet+16(FP), DX
	MOVQ vdso_zx_port_wait(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_create(job uint32, name unsafe.Pointer, name_size uint, options uint32, proc_handle unsafe.Pointer, vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_create(SB),NOSPLIT,$8-52
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL job+0(FP), DI
	MOVQ name+8(FP), SI
	MOVQ name_size+16(FP), DX
	MOVL options+24(FP), CX
	MOVQ proc_handle+32(FP), R8
	MOVQ vmar_handle+40(FP), R9
	MOVQ vdso_zx_process_create(SB), AX
	CALL AX
	MOVL AX, ret+48(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_create_shared(shared_proc uint32, options uint32, name unsafe.Pointer, name_size uint, proc_handle unsafe.Pointer, restricted_vmar_handle unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_create_shared(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL shared_proc+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ name+8(FP), DX
	MOVQ name_size+16(FP), CX
	MOVQ proc_handle+24(FP), R8
	MOVQ restricted_vmar_handle+32(FP), R9
	MOVQ vdso_zx_process_create_shared(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_exit(retcode int64)
TEXT runtime·vdsoCall_zx_process_exit(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ retcode+0(FP), DI
	MOVQ vdso_zx_process_exit(SB), AX
	CALL AX
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_read_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_read_memory(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vaddr+8(FP), SI
	MOVQ buffer+16(FP), DX
	MOVQ buffer_size+24(FP), CX
	MOVQ actual+32(FP), R8
	MOVQ vdso_zx_process_read_memory(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_start(handle uint32, thread uint32, entry uintptr, stack uintptr, arg1 uint32, arg2 uintptr) int32
TEXT runtime·vdsoCall_zx_process_start(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL thread+4(FP), SI
	MOVQ entry+8(FP), DX
	MOVQ stack+16(FP), CX
	MOVL arg1+24(FP), R8
	MOVQ arg2+32(FP), R9
	MOVQ vdso_zx_process_start(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_process_write_memory(handle uint32, vaddr uintptr, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_process_write_memory(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vaddr+8(FP), SI
	MOVQ buffer+16(FP), DX
	MOVQ buffer_size+24(FP), CX
	MOVQ actual+32(FP), R8
	MOVQ vdso_zx_process_write_memory(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_profile_create(root_job uint32, options uint32, profile unsafe.Pointer, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_profile_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL root_job+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ profile+8(FP), DX
	MOVQ out+16(FP), CX
	MOVQ vdso_zx_profile_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_resource_create(parent_rsrc uint32, options uint32, base uint64, size uint, name unsafe.Pointer, name_size uint, resource_out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_resource_create(SB),NOSPLIT,$32-52
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL parent_rsrc+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ base+8(FP), DX
	MOVQ size+16(FP), CX
	MOVQ name+24(FP), R8
	MOVQ name_size+32(FP), R9
	MOVQ resource_out+40(FP), R12
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R12
	MOVQ vdso_zx_resource_create(SB), AX
	CALL AX
	POPQ R12
	MOVQ BP, SP
	MOVL AX, ret+48(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_restricted_enter(options uint32, vector_table_ptr uintptr, context uintptr) int32
TEXT runtime·vdsoCall_zx_restricted_enter(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ vector_table_ptr+8(FP), SI
	MOVQ context+16(FP), DX
	MOVQ vdso_zx_restricted_enter(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_restricted_read_state(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_restricted_read_state(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_restricted_read_state(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_restricted_write_state(buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_restricted_write_state(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ buffer+0(FP), DI
	MOVQ buffer_size+8(FP), SI
	MOVQ vdso_zx_restricted_write_state(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_smc_call(handle uint32, parameters unsafe.Pointer, out_smc_result unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_smc_call(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ parameters+8(FP), SI
	MOVQ out_smc_result+16(FP), DX
	MOVQ vdso_zx_smc_call(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_socket_create(options uint32, out0 unsafe.Pointer, out1 unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ out0+8(FP), SI
	MOVQ out1+16(FP), DX
	MOVQ vdso_zx_socket_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_socket_read(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_read(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ actual+24(FP), R8
	MOVQ vdso_zx_socket_read(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_socket_set_disposition(handle uint32, disposition uint32, disposition_peer uint32) int32
TEXT runtime·vdsoCall_zx_socket_set_disposition(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL disposition+4(FP), SI
	MOVL disposition_peer+8(FP), DX
	MOVQ vdso_zx_socket_set_disposition(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_socket_write(handle uint32, options uint32, buffer unsafe.Pointer, buffer_size uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_socket_write(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ actual+24(FP), R8
	MOVQ vdso_zx_socket_write(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_create(options uint32, vmo uint32, seek uint64, out_stream unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVL vmo+4(FP), SI
	MOVQ seek+8(FP), DX
	MOVQ out_stream+16(FP), CX
	MOVQ vdso_zx_stream_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_readv(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_readv(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ vectors+8(FP), DX
	MOVQ num_vectors+16(FP), CX
	MOVQ actual+24(FP), R8
	MOVQ vdso_zx_stream_readv(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_readv_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_readv_at(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ vectors+16(FP), CX
	MOVQ num_vectors+24(FP), R8
	MOVQ actual+32(FP), R9
	MOVQ vdso_zx_stream_readv_at(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_seek(handle uint32, whence uint32, offset int64, out_seek unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_seek(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL whence+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ out_seek+16(FP), CX
	MOVQ vdso_zx_stream_seek(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_writev(handle uint32, options uint32, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_writev(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ vectors+8(FP), DX
	MOVQ num_vectors+16(FP), CX
	MOVQ actual+24(FP), R8
	MOVQ vdso_zx_stream_writev(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_stream_writev_at(handle uint32, options uint32, offset uint64, vectors unsafe.Pointer, num_vectors uint, actual unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_stream_writev_at(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ vectors+16(FP), CX
	MOVQ num_vectors+24(FP), R8
	MOVQ actual+32(FP), R9
	MOVQ vdso_zx_stream_writev_at(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_next_1(arg int32) int32
TEXT runtime·vdsoCall_zx_syscall_next_1(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL arg+0(FP), DI
	MOVQ vdso_zx_syscall_next_1(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_handle_create(return_value int32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_syscall_test_handle_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL return_value+0(FP), DI
	MOVQ out+8(FP), SI
	MOVQ vdso_zx_syscall_test_handle_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_widening_signed_narrow(a int64, b int32, c int16, d int8) int64
TEXT runtime·vdsoCall_zx_syscall_test_widening_signed_narrow(SB),NOSPLIT,$8-23
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ a+0(FP), DI
	MOVL b+8(FP), SI
	MOVQ c+12(FP), DX
	MOVQ d+14(FP), CX
	MOVQ vdso_zx_syscall_test_widening_signed_narrow(SB), AX
	CALL AX
	MOVQ AX, ret+15(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_widening_signed_wide(a int64, b int32, c int16, d int8) int64
TEXT runtime·vdsoCall_zx_syscall_test_widening_signed_wide(SB),NOSPLIT,$8-23
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ a+0(FP), DI
	MOVL b+8(FP), SI
	MOVQ c+12(FP), DX
	MOVQ d+14(FP), CX
	MOVQ vdso_zx_syscall_test_widening_signed_wide(SB), AX
	CALL AX
	MOVQ AX, ret+15(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_widening_unsigned_narrow(a uint64, b uint32, c uint16, d uint8) uint64
TEXT runtime·vdsoCall_zx_syscall_test_widening_unsigned_narrow(SB),NOSPLIT,$8-23
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ a+0(FP), DI
	MOVL b+8(FP), SI
	MOVQ c+12(FP), DX
	MOVQ d+14(FP), CX
	MOVQ vdso_zx_syscall_test_widening_unsigned_narrow(SB), AX
	CALL AX
	MOVQ AX, ret+15(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_widening_unsigned_wide(a uint64, b uint32, c uint16, d uint8) uint64
TEXT runtime·vdsoCall_zx_syscall_test_widening_unsigned_wide(SB),NOSPLIT,$8-23
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ a+0(FP), DI
	MOVL b+8(FP), SI
	MOVQ c+12(FP), DX
	MOVQ d+14(FP), CX
	MOVQ vdso_zx_syscall_test_widening_unsigned_wide(SB), AX
	CALL AX
	MOVQ AX, ret+15(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_wrapper(a int32, b int32, c int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_wrapper(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVQ vdso_zx_syscall_test_wrapper(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_0() int32
TEXT runtime·vdsoCall_zx_syscall_test_0(SB),NOSPLIT,$8-4
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_syscall_test_0(SB), AX
	CALL AX
	MOVL AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_1(a int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_1(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVQ vdso_zx_syscall_test_1(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_2(a int32, b int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_2(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVQ vdso_zx_syscall_test_2(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_3(a int32, b int32, c int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_3(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVQ vdso_zx_syscall_test_3(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_4(a int32, b int32, c int32, d int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_4(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVL d+12(FP), CX
	MOVQ vdso_zx_syscall_test_4(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_5(a int32, b int32, c int32, d int32, e int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_5(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVL d+12(FP), CX
	MOVL e+16(FP), R8
	MOVQ vdso_zx_syscall_test_5(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_6(a int32, b int32, c int32, d int32, e int32, f int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_6(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVL d+12(FP), CX
	MOVL e+16(FP), R8
	MOVL f+20(FP), R9
	MOVQ vdso_zx_syscall_test_6(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_7(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_7(SB),NOSPLIT,$32-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVL d+12(FP), CX
	MOVL e+16(FP), R8
	MOVL f+20(FP), R9
	MOVL g_+24(FP), R12
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R12
	MOVQ vdso_zx_syscall_test_7(SB), AX
	CALL AX
	POPQ R12
	MOVQ BP, SP
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_syscall_test_8(a int32, b int32, c int32, d int32, e int32, f int32, g_ int32, h int32) int32
TEXT runtime·vdsoCall_zx_syscall_test_8(SB),NOSPLIT,$40-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL a+0(FP), DI
	MOVL b+4(FP), SI
	MOVL c+8(FP), DX
	MOVL d+12(FP), CX
	MOVL e+16(FP), R8
	MOVL f+20(FP), R9
	MOVL g_+24(FP), R12
	MOVL h+28(FP), R13
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R13
	PUSHQ R12
	MOVQ vdso_zx_syscall_test_8(SB), AX
	CALL AX
	POPQ R12
	POPQ R13
	MOVQ BP, SP
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_dcache_line_size() uint32
TEXT runtime·vdsoCall_zx_system_get_dcache_line_size(SB),NOSPLIT,$8-4
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_system_get_dcache_line_size(SB), AX
	CALL AX
	MOVL AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_event(root_job uint32, kind uint32, event unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_event(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL root_job+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ event+8(FP), DX
	MOVQ vdso_zx_system_get_event(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_features(kind uint32, features unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_features(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL kind+0(FP), DI
	MOVQ features+8(FP), SI
	MOVQ vdso_zx_system_get_features(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_num_cpus() uint32
TEXT runtime·vdsoCall_zx_system_get_num_cpus(SB),NOSPLIT,$8-4
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_system_get_num_cpus(SB), AX
	CALL AX
	MOVL AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_page_size() uint32
TEXT runtime·vdsoCall_zx_system_get_page_size(SB),NOSPLIT,$8-4
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_system_get_page_size(SB), AX
	CALL AX
	MOVL AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_performance_info(resource uint32, topic uint32, count uint, info unsafe.Pointer, output_count unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_get_performance_info(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL topic+4(FP), SI
	MOVQ count+8(FP), DX
	MOVQ info+16(FP), CX
	MOVQ output_count+24(FP), R8
	MOVQ vdso_zx_system_get_performance_info(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_physmem() uint64
TEXT runtime·vdsoCall_zx_system_get_physmem(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_system_get_physmem(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_get_version_string() unsafe.Pointer
TEXT runtime·vdsoCall_zx_system_get_version_string(SB),NOSPLIT,$8-16
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_system_get_version_string(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_mexec(resource uint32, kernel_vmo uint32, bootimage_vmo uint32) int32
TEXT runtime·vdsoCall_zx_system_mexec(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL kernel_vmo+4(FP), SI
	MOVL bootimage_vmo+8(FP), DX
	MOVQ vdso_zx_system_mexec(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_mexec_payload_get(resource uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_system_mexec_payload_get(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ buffer+8(FP), SI
	MOVQ buffer_size+16(FP), DX
	MOVQ vdso_zx_system_mexec_payload_get(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_powerctl(resource uint32, cmd uint32, arg unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_system_powerctl(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL cmd+4(FP), SI
	MOVQ arg+8(FP), DX
	MOVQ vdso_zx_system_powerctl(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_system_set_performance_info(resource uint32, topic uint32, info unsafe.Pointer, count uint) int32
TEXT runtime·vdsoCall_zx_system_set_performance_info(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVL topic+4(FP), SI
	MOVQ info+8(FP), DX
	MOVQ count+16(FP), CX
	MOVQ vdso_zx_system_set_performance_info(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_task_create_exception_channel(handle uint32, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_create_exception_channel(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_task_create_exception_channel(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_task_kill(handle uint32) int32
TEXT runtime·vdsoCall_zx_task_kill(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_task_kill(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_task_suspend(handle uint32, token unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_suspend(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ token+8(FP), SI
	MOVQ vdso_zx_task_suspend(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_task_suspend_token(handle uint32, token unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_task_suspend_token(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ token+8(FP), SI
	MOVQ vdso_zx_task_suspend_token(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_create(process uint32, name unsafe.Pointer, name_size uint, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_thread_create(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL process+0(FP), DI
	MOVQ name+8(FP), SI
	MOVQ name_size+16(FP), DX
	MOVL options+24(FP), CX
	MOVQ out+32(FP), R8
	MOVQ vdso_zx_thread_create(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_exit()
TEXT runtime·vdsoCall_zx_thread_exit(SB),NOSPLIT,$8-0
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_thread_exit(SB), AX
	CALL AX
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_legacy_yield(options uint32) int32
TEXT runtime·vdsoCall_zx_thread_legacy_yield(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVQ vdso_zx_thread_legacy_yield(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_thread_read_state(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_thread_read_state(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_start(handle uint32, thread_entry uintptr, stack uintptr, arg1 uintptr, arg2 uintptr) int32
TEXT runtime·vdsoCall_zx_thread_start(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ thread_entry+8(FP), SI
	MOVQ stack+16(FP), DX
	MOVQ arg1+24(FP), CX
	MOVQ arg2+32(FP), R8
	MOVQ vdso_zx_thread_start(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_thread_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_thread_write_state(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_thread_write_state(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ticks_get() int64
TEXT runtime·vdsoCall_zx_ticks_get(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_ticks_get(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ticks_get_via_kernel() int64
TEXT runtime·vdsoCall_zx_ticks_get_via_kernel(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_ticks_get_via_kernel(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_ticks_per_second() int64
TEXT runtime·vdsoCall_zx_ticks_per_second(SB),NOSPLIT,$8-8
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ vdso_zx_ticks_per_second(SB), AX
	CALL AX
	MOVQ AX, ret+0(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_timer_cancel(handle uint32) int32
TEXT runtime·vdsoCall_zx_timer_cancel(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_timer_cancel(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_timer_create(options uint32, clock_id uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_timer_create(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL options+0(FP), DI
	MOVL clock_id+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_timer_create(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_timer_set(handle uint32, deadline int64, slack int64) int32
TEXT runtime·vdsoCall_zx_timer_set(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ deadline+8(FP), SI
	MOVQ slack+16(FP), DX
	MOVQ vdso_zx_timer_set(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_create(guest uint32, options uint32, entry uintptr, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vcpu_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL guest+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ entry+8(FP), DX
	MOVQ out+16(FP), CX
	MOVQ vdso_zx_vcpu_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_enter(handle uint32, packet unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vcpu_enter(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVQ packet+8(FP), SI
	MOVQ vdso_zx_vcpu_enter(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_interrupt(handle uint32, vector uint32) int32
TEXT runtime·vdsoCall_zx_vcpu_interrupt(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL vector+4(FP), SI
	MOVQ vdso_zx_vcpu_interrupt(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_kick(handle uint32) int32
TEXT runtime·vdsoCall_zx_vcpu_kick(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_vcpu_kick(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_read_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vcpu_read_state(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_vcpu_read_state(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vcpu_write_state(handle uint32, kind uint32, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vcpu_write_state(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL kind+4(FP), SI
	MOVQ buffer+8(FP), DX
	MOVQ buffer_size+16(FP), CX
	MOVQ vdso_zx_vcpu_write_state(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_allocate(parent_vmar uint32, options uint32, offset uint, size uint, child_vmar unsafe.Pointer, child_addr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmar_allocate(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL parent_vmar+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ size+16(FP), CX
	MOVQ child_vmar+24(FP), R8
	MOVQ child_addr+32(FP), R9
	MOVQ vdso_zx_vmar_allocate(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_destroy(handle uint32) int32
TEXT runtime·vdsoCall_zx_vmar_destroy(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ vdso_zx_vmar_destroy(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_map(handle uint32, options uint32, vmar_offset uint, vmo uint32, vmo_offset uint64, len uint, mapped_addr unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmar_map(SB),NOSPLIT,$32-52
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ vmar_offset+8(FP), DX
	MOVL vmo+16(FP), CX
	MOVQ vmo_offset+24(FP), R8
	MOVQ len+32(FP), R9
	MOVQ mapped_addr+40(FP), R12
	MOVQ SP, BP   // BP is preserved across vsdo call by the x86-64 ABI
	ANDQ $~15, SP // stack alignment for x86-64 ABI
	PUSHQ R12
	MOVQ vdso_zx_vmar_map(SB), AX
	CALL AX
	POPQ R12
	MOVQ BP, SP
	MOVL AX, ret+48(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_op_range(handle uint32, op uint32, address uintptr, size uint, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmar_op_range(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL op+4(FP), SI
	MOVQ address+8(FP), DX
	MOVQ size+16(FP), CX
	MOVQ buffer+24(FP), R8
	MOVQ buffer_size+32(FP), R9
	MOVQ vdso_zx_vmar_op_range(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_protect(handle uint32, options uint32, addr uintptr, len uint) int32
TEXT runtime·vdsoCall_zx_vmar_protect(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ addr+8(FP), DX
	MOVQ len+16(FP), CX
	MOVQ vdso_zx_vmar_protect(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_unmap(handle uint32, addr uintptr, len uint) int32
TEXT runtime·vdsoCall_zx_vmar_unmap(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ addr+8(FP), SI
	MOVQ len+16(FP), DX
	MOVQ vdso_zx_vmar_unmap(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmar_unmap_handle_close_thread_exit(vmar_handle uint32, addr uintptr, size uint, close_handle uint32) int32
TEXT runtime·vdsoCall_zx_vmar_unmap_handle_close_thread_exit(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL vmar_handle+0(FP), DI
	MOVQ addr+8(FP), SI
	MOVQ size+16(FP), DX
	MOVL close_handle+24(FP), CX
	MOVQ vdso_zx_vmar_unmap_handle_close_thread_exit(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_create(size uint64, options uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create(SB),NOSPLIT,$8-28
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVQ size+0(FP), DI
	MOVL options+8(FP), SI
	MOVQ out+16(FP), DX
	MOVQ vdso_zx_vmo_create(SB), AX
	CALL AX
	MOVL AX, ret+24(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_create_child(handle uint32, options uint32, offset uint64, size uint64, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_child(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL options+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ size+16(FP), CX
	MOVQ out+24(FP), R8
	MOVQ vdso_zx_vmo_create_child(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_create_contiguous(bti uint32, size uint, alignment_log2 uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_contiguous(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL bti+0(FP), DI
	MOVQ size+8(FP), SI
	MOVL alignment_log2+16(FP), DX
	MOVQ out+24(FP), CX
	MOVQ vdso_zx_vmo_create_contiguous(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_create_physical(resource uint32, paddr uintptr, size uint, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_create_physical(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL resource+0(FP), DI
	MOVQ paddr+8(FP), SI
	MOVQ size+16(FP), DX
	MOVQ out+24(FP), CX
	MOVQ vdso_zx_vmo_create_physical(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_get_size(handle uint32, size unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_get_size(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ size+8(FP), SI
	MOVQ vdso_zx_vmo_get_size(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_op_range(handle uint32, op uint32, offset uint64, size uint64, buffer unsafe.Pointer, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_op_range(SB),NOSPLIT,$8-44
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVL op+4(FP), SI
	MOVQ offset+8(FP), DX
	MOVQ size+16(FP), CX
	MOVQ buffer+24(FP), R8
	MOVQ buffer_size+32(FP), R9
	MOVQ vdso_zx_vmo_op_range(SB), AX
	CALL AX
	MOVL AX, ret+40(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_read(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_read(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVQ buffer+8(FP), SI
	MOVQ offset+16(FP), DX
	MOVQ buffer_size+24(FP), CX
	MOVQ vdso_zx_vmo_read(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_replace_as_executable(handle uint32, vmex uint32, out unsafe.Pointer) int32
TEXT runtime·vdsoCall_zx_vmo_replace_as_executable(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL vmex+4(FP), SI
	MOVQ out+8(FP), DX
	MOVQ vdso_zx_vmo_replace_as_executable(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_set_cache_policy(handle uint32, cache_policy uint32) int32
TEXT runtime·vdsoCall_zx_vmo_set_cache_policy(SB),NOSPLIT,$8-12
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVL cache_policy+4(FP), SI
	MOVQ vdso_zx_vmo_set_cache_policy(SB), AX
	CALL AX
	MOVL AX, ret+8(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_set_size(handle uint32, size uint64) int32
TEXT runtime·vdsoCall_zx_vmo_set_size(SB),NOSPLIT,$8-20
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	MOVL handle+0(FP), DI
	MOVQ size+8(FP), SI
	MOVQ vdso_zx_vmo_set_size(SB), AX
	CALL AX
	MOVL AX, ret+16(FP)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET

// func vdsoCall_zx_vmo_write(handle uint32, buffer unsafe.Pointer, offset uint64, buffer_size uint) int32
TEXT runtime·vdsoCall_zx_vmo_write(SB),NOSPLIT,$8-36
	GO_ARGS
	NO_LOCAL_POINTERS
	get_tls(CX)
	MOVQ g(CX), AX
	MOVQ g_m(AX), R14
	PUSHQ R14
	LEAQ ret+0(FP), DX
	MOVQ -8(DX), CX
	MOVQ CX, m_vdsoPC(R14)
	MOVQ DX, m_vdsoSP(R14)
	CALL runtime·entersyscall(SB)
	MOVL handle+0(FP), DI
	MOVQ buffer+8(FP), SI
	MOVQ offset+16(FP), DX
	MOVQ buffer_size+24(FP), CX
	MOVQ vdso_zx_vmo_write(SB), AX
	CALL AX
	MOVL AX, ret+32(FP)
	CALL runtime·exitsyscall(SB)
	POPQ R14
	MOVQ $0, m_vdsoSP(R14)
	RET
