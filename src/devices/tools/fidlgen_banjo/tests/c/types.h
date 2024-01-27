// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.types banjo file

#pragma once


#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Forward declarations
typedef struct zx_types zx_types_t;
#define vectors_size UINT32_C(32)
typedef struct vectors vectors_t;
typedef uint8_t u8_enum_t;
#define U8_ENUM_X UINT8_C(23)
typedef uint64_t u64_enum_t;
#define U64_ENUM_X UINT64_C(23)
typedef uint32_t u32_enum_t;
#define U32_ENUM_X UINT32_C(23)
typedef uint16_t u16_enum_t;
#define U16_ENUM_X UINT16_C(23)
typedef union this_is_a_union this_is_a_union_t;
typedef struct unions unions_t;
typedef struct this_is_a_struct this_is_a_struct_t;
typedef union union_types union_types_t;
typedef struct structs structs_t;
#define strings_size UINT32_C(32)
typedef struct strings strings_t;
typedef struct primitive_types primitive_types_t;
typedef struct interfaces interfaces_t;
typedef int8_t i8_enum_t;
#define I8_ENUM_X INT8_C(23)
typedef int64_t i64_enum_t;
#define I64_ENUM_X INT64_C(23)
typedef int32_t i32_enum_t;
#define I32_ENUM_X INT32_C(23)
typedef int16_t i16_enum_t;
#define I16_ENUM_X INT16_C(23)
typedef struct handles handles_t;
typedef struct default_values default_values_t;
typedef uint32_t default_enum_t;
#define DEFAULT_ENUM_X UINT32_C(23)
#define arrays_size UINT32_C(32)
typedef struct arrays arrays_t;
typedef struct this_is_an_interface_protocol this_is_an_interface_protocol_t;
typedef struct this_is_an_interface_protocol_ops this_is_an_interface_protocol_ops_t;

// Declarations
struct zx_types {
    zx_status_t status;
    zx_time_t time;
    zx_duration_t duration;
    zx_ticks_t ticks;
    zx_koid_t koid;
    zx_vaddr_t vaddr;
    zx_paddr_t paddr;
    zx_paddr32_t paddr32;
    zx_gpaddr_t gpaddr;
    zx_off_t offset;
    zx_signals_t signals;
};

struct vectors {
    const bool* b_0_list;
    size_t b_0_count;
    const int8_t* i8_0_list;
    size_t i8_0_count;
    const int16_t* i16_0_list;
    size_t i16_0_count;
    const int32_t* i32_0_list;
    size_t i32_0_count;
    const int64_t* i64_0_list;
    size_t i64_0_count;
    const uint8_t* u8_0_list;
    size_t u8_0_count;
    const uint16_t* u16_0_list;
    size_t u16_0_count;
    const uint32_t* u32_0_list;
    size_t u32_0_count;
    const uint64_t* u64_0_list;
    size_t u64_0_count;
    const float* f32_0_list;
    size_t f32_0_count;
    const double* f64_0_list;
    size_t f64_0_count;
    const zx_handle_t* handle_0_list;
    size_t handle_0_count;
    const bool* b_1_list;
    size_t b_1_count;
    const int8_t* i8_1_list;
    size_t i8_1_count;
    const int16_t* i16_1_list;
    size_t i16_1_count;
    const int32_t* i32_1_list;
    size_t i32_1_count;
    const int64_t* i64_1_list;
    size_t i64_1_count;
    const uint8_t* u8_1_list;
    size_t u8_1_count;
    const uint16_t* u16_1_list;
    size_t u16_1_count;
    const uint32_t* u32_1_list;
    size_t u32_1_count;
    const uint64_t* u64_1_list;
    size_t u64_1_count;
    const float* f32_1_list;
    size_t f32_1_count;
    const double* f64_1_list;
    size_t f64_1_count;
    const zx_handle_t* handle_1_list;
    size_t handle_1_count;
    const bool* b_sized_0_list;
    size_t b_sized_0_count;
    const int8_t* i8_sized_0_list;
    size_t i8_sized_0_count;
    const int16_t* i16_sized_0_list;
    size_t i16_sized_0_count;
    const int32_t* i32_sized_0_list;
    size_t i32_sized_0_count;
    const int64_t* i64_sized_0_list;
    size_t i64_sized_0_count;
    const uint8_t* u8_sized_0_list;
    size_t u8_sized_0_count;
    const uint16_t* u16_sized_0_list;
    size_t u16_sized_0_count;
    const uint32_t* u32_sized_0_list;
    size_t u32_sized_0_count;
    const uint64_t* u64_sized_0_list;
    size_t u64_sized_0_count;
    const float* f32_sized_0_list;
    size_t f32_sized_0_count;
    const double* f64_sized_0_list;
    size_t f64_sized_0_count;
    const zx_handle_t* handle_sized_0_list;
    size_t handle_sized_0_count;
    const bool* b_sized_1_list;
    size_t b_sized_1_count;
    const int8_t* i8_sized_1_list;
    size_t i8_sized_1_count;
    const int16_t* i16_sized_1_list;
    size_t i16_sized_1_count;
    const int32_t* i32_sized_1_list;
    size_t i32_sized_1_count;
    const int64_t* i64_sized_1_list;
    size_t i64_sized_1_count;
    const uint8_t* u8_sized_1_list;
    size_t u8_sized_1_count;
    const uint16_t* u16_sized_1_list;
    size_t u16_sized_1_count;
    const uint32_t* u32_sized_1_list;
    size_t u32_sized_1_count;
    const uint64_t* u64_sized_1_list;
    size_t u64_sized_1_count;
    const float* f32_sized_1_list;
    size_t f32_sized_1_count;
    const double* f64_sized_1_list;
    size_t f64_sized_1_count;
    const zx_handle_t* handle_sized_1_list;
    size_t handle_sized_1_count;
    const bool* b_sized_2_list;
    size_t b_sized_2_count;
    const int8_t* i8_sized_2_list;
    size_t i8_sized_2_count;
    const int16_t* i16_sized_2_list;
    size_t i16_sized_2_count;
    const int32_t* i32_sized_2_list;
    size_t i32_sized_2_count;
    const int64_t* i64_sized_2_list;
    size_t i64_sized_2_count;
    const uint8_t* u8_sized_2_list;
    size_t u8_sized_2_count;
    const uint16_t* u16_sized_2_list;
    size_t u16_sized_2_count;
    const uint32_t* u32_sized_2_list;
    size_t u32_sized_2_count;
    const uint64_t* u64_sized_2_list;
    size_t u64_sized_2_count;
    const float* f32_sized_2_list;
    size_t f32_sized_2_count;
    const double* f64_sized_2_list;
    size_t f64_sized_2_count;
    const zx_handle_t* handle_sized_2_list;
    size_t handle_sized_2_count;
    const bool* b_nullable_0_list;
    size_t b_nullable_0_count;
    const int8_t* i8_nullable_0_list;
    size_t i8_nullable_0_count;
    const int16_t* i16_nullable_0_list;
    size_t i16_nullable_0_count;
    const int32_t* i32_nullable_0_list;
    size_t i32_nullable_0_count;
    const int64_t* i64_nullable_0_list;
    size_t i64_nullable_0_count;
    const uint8_t* u8_nullable_0_list;
    size_t u8_nullable_0_count;
    const uint16_t* u16_nullable_0_list;
    size_t u16_nullable_0_count;
    const uint32_t* u32_nullable_0_list;
    size_t u32_nullable_0_count;
    const uint64_t* u64_nullable_0_list;
    size_t u64_nullable_0_count;
    const float* f32_nullable_0_list;
    size_t f32_nullable_0_count;
    const double* f64_nullable_0_list;
    size_t f64_nullable_0_count;
    const zx_handle_t* handle_nullable_0_list;
    size_t handle_nullable_0_count;
    const bool* b_nullable_1_list;
    size_t b_nullable_1_count;
    const int8_t* i8_nullable_1_list;
    size_t i8_nullable_1_count;
    const int16_t* i16_nullable_1_list;
    size_t i16_nullable_1_count;
    const int32_t* i32_nullable_1_list;
    size_t i32_nullable_1_count;
    const int64_t* i64_nullable_1_list;
    size_t i64_nullable_1_count;
    const uint8_t* u8_nullable_1_list;
    size_t u8_nullable_1_count;
    const uint16_t* u16_nullable_1_list;
    size_t u16_nullable_1_count;
    const uint32_t* u32_nullable_1_list;
    size_t u32_nullable_1_count;
    const uint64_t* u64_nullable_1_list;
    size_t u64_nullable_1_count;
    const float* f32_nullable_1_list;
    size_t f32_nullable_1_count;
    const double* f64_nullable_1_list;
    size_t f64_nullable_1_count;
    const zx_handle_t* handle_nullable_1_list;
    size_t handle_nullable_1_count;
    const bool* b_nullable_sized_0_list;
    size_t b_nullable_sized_0_count;
    const int8_t* i8_nullable_sized_0_list;
    size_t i8_nullable_sized_0_count;
    const int16_t* i16_nullable_sized_0_list;
    size_t i16_nullable_sized_0_count;
    const int32_t* i32_nullable_sized_0_list;
    size_t i32_nullable_sized_0_count;
    const int64_t* i64_nullable_sized_0_list;
    size_t i64_nullable_sized_0_count;
    const uint8_t* u8_nullable_sized_0_list;
    size_t u8_nullable_sized_0_count;
    const uint16_t* u16_nullable_sized_0_list;
    size_t u16_nullable_sized_0_count;
    const uint32_t* u32_nullable_sized_0_list;
    size_t u32_nullable_sized_0_count;
    const uint64_t* u64_nullable_sized_0_list;
    size_t u64_nullable_sized_0_count;
    const float* f32_nullable_sized_0_list;
    size_t f32_nullable_sized_0_count;
    const double* f64_nullable_sized_0_list;
    size_t f64_nullable_sized_0_count;
    const zx_handle_t* handle_nullable_sized_0_list;
    size_t handle_nullable_sized_0_count;
    const bool* b_nullable_sized_1_list;
    size_t b_nullable_sized_1_count;
    const int8_t* i8_nullable_sized_1_list;
    size_t i8_nullable_sized_1_count;
    const int16_t* i16_nullable_sized_1_list;
    size_t i16_nullable_sized_1_count;
    const int32_t* i32_nullable_sized_1_list;
    size_t i32_nullable_sized_1_count;
    const int64_t* i64_nullable_sized_1_list;
    size_t i64_nullable_sized_1_count;
    const uint8_t* u8_nullable_sized_1_list;
    size_t u8_nullable_sized_1_count;
    const uint16_t* u16_nullable_sized_1_list;
    size_t u16_nullable_sized_1_count;
    const uint32_t* u32_nullable_sized_1_list;
    size_t u32_nullable_sized_1_count;
    const uint64_t* u64_nullable_sized_1_list;
    size_t u64_nullable_sized_1_count;
    const float* f32_nullable_sized_1_list;
    size_t f32_nullable_sized_1_count;
    const double* f64_nullable_sized_1_list;
    size_t f64_nullable_sized_1_count;
    const zx_handle_t* handle_nullable_sized_1_list;
    size_t handle_nullable_sized_1_count;
    const bool* b_nullable_sized_2_list;
    size_t b_nullable_sized_2_count;
    const int8_t* i8_nullable_sized_2_list;
    size_t i8_nullable_sized_2_count;
    const int16_t* i16_nullable_sized_2_list;
    size_t i16_nullable_sized_2_count;
    const int32_t* i32_nullable_sized_2_list;
    size_t i32_nullable_sized_2_count;
    const int64_t* i64_nullable_sized_2_list;
    size_t i64_nullable_sized_2_count;
    const uint8_t* u8_nullable_sized_2_list;
    size_t u8_nullable_sized_2_count;
    const uint16_t* u16_nullable_sized_2_list;
    size_t u16_nullable_sized_2_count;
    const uint32_t* u32_nullable_sized_2_list;
    size_t u32_nullable_sized_2_count;
    const uint64_t* u64_nullable_sized_2_list;
    size_t u64_nullable_sized_2_count;
    const float* f32_nullable_sized_2_list;
    size_t f32_nullable_sized_2_count;
    const double* f64_nullable_sized_2_list;
    size_t f64_nullable_sized_2_count;
    const zx_handle_t* handle_nullable_sized_2_list;
    size_t handle_nullable_sized_2_count;
};

union this_is_a_union {
    const char* s;
};

struct unions {
    this_is_a_union_t s;
    this_is_a_union_t nullable_u;
};

struct this_is_a_struct {
    const char* s;
};

union union_types {
    bool b;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
    bool b_0[1];
    int8_t i8_0[1];
    int16_t i16_0[1];
    int32_t i32_0[1];
    int64_t i64_0[1];
    uint8_t u8_0[1];
    uint16_t u16_0[1];
    uint32_t u32_0[1];
    uint64_t u64_0[1];
    float f32_0[1];
    double f64_0[1];
    zx_handle_t handle_0[1];
    const char* str;
    this_is_a_struct_t s;
    this_is_a_union_t u;
};

struct structs {
    this_is_a_struct_t s;
    this_is_a_struct_t nullable_s;
};

struct strings {
    const char* s;
    const char* nullable_s;
    char size_0_s[4];
    char size_1_s[32];
    char nullable_size_0_s[4];
    char nullable_size_1_s[32];
};

struct primitive_types {
    bool b;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
};

struct interfaces {
    this_is_an_interface_protocol_t nonnullable_interface;
    this_is_an_interface_protocol_t nullable_interface;
};

struct handles {
    zx_handle_t handle_handle;
    zx_handle_t process_handle;
    zx_handle_t thread_handle;
    zx_handle_t vmo_handle;
    zx_handle_t channel_handle;
    zx_handle_t event_handle;
    zx_handle_t port_handle;
    zx_handle_t interrupt_handle;
    zx_handle_t socket_handle;
    zx_handle_t resource_handle;
    zx_handle_t eventpair_handle;
    zx_handle_t job_handle;
    zx_handle_t vmar_handle;
    zx_handle_t fifo_handle;
    zx_handle_t guest_handle;
    zx_handle_t timer_handle;
    zx_handle_t profile_handle;
    zx_handle_t vcpu_handle;
    zx_handle_t iommu_handle;
    zx_handle_t pager_handle;
    zx_handle_t pmt_handle;
    zx_handle_t clock_handle;
    zx_handle_t nullable_handle_handle;
    zx_handle_t nullable_process_handle;
    zx_handle_t nullable_thread_handle;
    zx_handle_t nullable_vmo_handle;
    zx_handle_t nullable_channel_handle;
    zx_handle_t nullable_event_handle;
    zx_handle_t nullable_port_handle;
    zx_handle_t nullable_interrupt_handle;
    zx_handle_t nullable_socket_handle;
    zx_handle_t nullable_resource_handle;
    zx_handle_t nullable_eventpair_handle;
    zx_handle_t nullable_job_handle;
    zx_handle_t nullable_vmar_handle;
    zx_handle_t nullable_fifo_handle;
    zx_handle_t nullable_guest_handle;
    zx_handle_t nullable_timer_handle;
    zx_handle_t nullable_profile_handle;
    zx_handle_t nullable_vcpu_handle;
    zx_handle_t nullable_iommu_handle;
    zx_handle_t nullable_pager_handle;
    zx_handle_t nullable_pmt_handle;
    zx_handle_t nullable_clock_handle;
};

struct default_values {
    bool b1;
    bool b2;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    const char* s;
};

struct arrays {
    bool b_0[1];
    int8_t i8_0[1];
    int16_t i16_0[1];
    int32_t i32_0[1];
    int64_t i64_0[1];
    uint8_t u8_0[1];
    uint16_t u16_0[1];
    uint32_t u32_0[1];
    uint64_t u64_0[1];
    float f32_0[1];
    double f64_0[1];
    zx_handle_t handle_0[1];
    bool b_1[32];
    int8_t i8_1[32];
    int16_t i16_1[32];
    int32_t i32_1[32];
    int64_t i64_1[32];
    uint8_t u8_1[32];
    uint16_t u16_1[32];
    uint32_t u32_1[32];
    uint64_t u64_1[32];
    float f32_1[32];
    double f64_1[32];
    zx_handle_t handle_1[32];
    bool b_2[32][4];
    int8_t i8_2[32][4];
    int16_t i16_2[32][4];
    int32_t i32_2[32][4];
    int64_t i64_2[32][4];
    uint8_t u8_2[32][4];
    uint16_t u16_2[32][4];
    uint32_t u32_2[32][4];
    uint64_t u64_2[32][4];
    float f32_2[32][4];
    double f64_2[32][4];
    zx_handle_t handle_2[32][4];
};

struct this_is_an_interface_protocol_ops {
    void (*copy)(void* ctx, const char* s, uint32_t count, char* out_s, size_t s_capacity);
};


struct this_is_an_interface_protocol {
    const this_is_an_interface_protocol_ops_t* ops;
    void* ctx;
};


// Helpers
static inline void this_is_an_interface_copy(const this_is_an_interface_protocol_t* proto, const char* s, uint32_t count, char* out_s, size_t s_capacity) {
    proto->ops->copy(proto->ctx, s, count, out_s, s_capacity);
}


__END_CDECLS
