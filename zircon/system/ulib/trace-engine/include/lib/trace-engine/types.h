// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
// Types, constants, and inline functions used to encode and decode trace records.
// This header is used by C and C++ code. For simple support of all choices of
// C/C++ and -O0/-On the inline functions are "static inline".
//

#ifndef LIB_TRACE_ENGINE_TYPES_H_
#define LIB_TRACE_ENGINE_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Timebase recorded into trace files, as returned by zx_ticks_get().
typedef uint64_t trace_ticks_t;

// The ids used to correlate related counters, asynchronous operations, flows, and virtual threads.
typedef uint64_t trace_counter_id_t;
typedef uint64_t trace_async_id_t;
typedef uint64_t trace_flow_id_t;
typedef uint64_t trace_vthread_id_t;

// Specifies the scope of instant events.
typedef enum {
  // The event is only relevant to the thread it occurred on.
  TRACE_SCOPE_THREAD = 0,
  // The event is only relevant to the process in which it occurred.
  TRACE_SCOPE_PROCESS = 1,
  // The event is globally relevant.
  TRACE_SCOPE_GLOBAL = 2,
} trace_scope_t;

// Thread states used to describe context switches.
// Use the |ZX_THREAD_STATE_XXX| values defined in <zircon/syscalls/object.h>.
typedef uint32_t trace_thread_state_t;

// Identifies a particular CPU in a context switch trace record.
typedef uint32_t trace_cpu_number_t;

// Contains a thread's priority in a context switch trace record.
typedef uint32_t trace_thread_priority_t;

// Represents an index into the string table.
typedef uint32_t trace_string_index_t;

// Represents the encoded form of string references.
typedef uint32_t trace_encoded_string_ref_t;
#define TRACE_ENCODED_STRING_REF_EMPTY ((trace_encoded_string_ref_t)0u)
#define TRACE_ENCODED_STRING_REF_INLINE_FLAG ((trace_encoded_string_ref_t)0x8000u)
#define TRACE_ENCODED_STRING_REF_LENGTH_MASK ((trace_encoded_string_ref_t)0x7fffu)
#define TRACE_ENCODED_STRING_REF_MAX_LENGTH ((trace_encoded_string_ref_t)32000)
#define TRACE_ENCODED_STRING_REF_MIN_INDEX ((trace_encoded_string_ref_t)0x1u)
#define TRACE_ENCODED_STRING_REF_MAX_INDEX ((trace_encoded_string_ref_t)0x7fffu)

// Represents an index into the thread table.
typedef uint32_t trace_thread_index_t;

// Represents the encoded form of thread references.
typedef uint32_t trace_encoded_thread_ref_t;
#define TRACE_ENCODED_THREAD_REF_INLINE ((trace_encoded_thread_ref_t)0u)
#define TRACE_ENCODED_THREAD_REF_MIN_INDEX ((trace_encoded_thread_ref_t)0x01)
#define TRACE_ENCODED_THREAD_REF_MAX_INDEX ((trace_encoded_thread_ref_t)0xff)

// A string reference which is either encoded inline or indirectly by string table index.
typedef struct trace_string_ref {
  trace_encoded_string_ref_t encoded_value;
  const char* inline_string;  // only non-null for inline strings
} trace_string_ref_t;

// Makes true if the string ref's content is empty.
static inline bool trace_is_empty_string_ref(const trace_string_ref_t* string_ref) {
  return string_ref->encoded_value == TRACE_ENCODED_STRING_REF_EMPTY;
}

// Returns true if the string ref's content is stored inline (rather than empty or indexed).
static inline bool trace_is_inline_string_ref(const trace_string_ref_t* string_ref) {
  return string_ref->encoded_value & TRACE_ENCODED_STRING_REF_INLINE_FLAG;
}

// Returns true if the string ref's content is stored as an index into the string table.
static inline bool trace_is_indexed_string_ref(const trace_string_ref_t* string_ref) {
  return string_ref->encoded_value >= TRACE_ENCODED_STRING_REF_MIN_INDEX &&
         string_ref->encoded_value <= TRACE_ENCODED_STRING_REF_MAX_INDEX;
}

// Returns the length of an inline string.
// Only valid for inline strings.
static inline size_t trace_inline_string_ref_length(const trace_string_ref_t* string_ref) {
  return string_ref->encoded_value & TRACE_ENCODED_STRING_REF_LENGTH_MASK;
}

// Makes an empty string ref.
static inline trace_string_ref_t trace_make_empty_string_ref(void) {
  trace_string_ref_t ref = {.encoded_value = TRACE_ENCODED_STRING_REF_EMPTY, .inline_string = NULL};
  return ref;
}

// Makes an inline or empty string ref from a string with given size.
// The |string| does not need to be null-terminated because its length is provided.
// The |string| must not be null if length is non-zero.
// The |string| is truncated if longer than |TRACE_ENCODED_STRING_REF_MAX_LENGTH|.
static inline trace_string_ref_t trace_make_inline_string_ref(const char* string, size_t length) {
  if (!length)
    return trace_make_empty_string_ref();

  ZX_DEBUG_ASSERT(string != NULL);
  if (length > TRACE_ENCODED_STRING_REF_MAX_LENGTH)
    length = TRACE_ENCODED_STRING_REF_MAX_LENGTH;
  trace_string_ref_t ref = {
      .encoded_value = TRACE_ENCODED_STRING_REF_INLINE_FLAG | (trace_encoded_string_ref_t)length,
      .inline_string = string};
  return ref;
}

// Makes an inline or empty string ref from a null-terminated string.
// The |string| is truncated if longer than |TRACE_ENCODED_STRING_REF_MAX_LENGTH|.
static inline trace_string_ref_t trace_make_inline_c_string_ref(const char* string) {
  return trace_make_inline_string_ref(string, string ? strlen(string) : 0u);
}

// Makes an indexed string ref.
// The |index| must be >= |TRACE_ENCODED_STRING_REF_MIN_INDEX|
// and <= |TRACE_ENCODED_STRING_REF_MAX_INDEX|.
static inline trace_string_ref_t trace_make_indexed_string_ref(trace_string_index_t index) {
  ZX_DEBUG_ASSERT(index >= TRACE_ENCODED_STRING_REF_MIN_INDEX &&
                  index <= TRACE_ENCODED_STRING_REF_MAX_INDEX);
  trace_string_ref_t ref = {.encoded_value = index, .inline_string = NULL};
  return ref;
}

// A thread reference which is either encoded inline or indirectly by thread table index.
typedef struct trace_thread_ref {
  trace_encoded_thread_ref_t encoded_value;
  zx_koid_t inline_process_koid;
  zx_koid_t inline_thread_koid;
} trace_thread_ref_t;

// Returns true if the thread ref's value is unknown.
static inline bool trace_is_unknown_thread_ref(const trace_thread_ref_t* thread_ref) {
  return thread_ref->encoded_value == TRACE_ENCODED_THREAD_REF_INLINE &&
         thread_ref->inline_process_koid == ZX_KOID_INVALID &&
         thread_ref->inline_thread_koid == ZX_KOID_INVALID;
}

// Returns true if the thread ref's content is stored as an index into the thread table.
static inline bool trace_is_indexed_thread_ref(const trace_thread_ref_t* thread_ref) {
  return thread_ref->encoded_value >= TRACE_ENCODED_THREAD_REF_MIN_INDEX &&
         thread_ref->encoded_value <= TRACE_ENCODED_THREAD_REF_MAX_INDEX;
}

// Returns true if the thread ref's value is stored inline (rather than unknown or indexed).
static inline bool trace_is_inline_thread_ref(const trace_thread_ref_t* thread_ref) {
  return thread_ref->encoded_value == TRACE_ENCODED_THREAD_REF_INLINE &&
         (thread_ref->inline_process_koid != ZX_KOID_INVALID ||
          thread_ref->inline_thread_koid != ZX_KOID_INVALID);
}

// Makes a thread ref representing an unknown thread.
// TODO(fxbug.dev/30974): Reserve thread ref index 0 for unknown threads,
// use thread ref index 255 for inline threads.
static inline trace_thread_ref_t trace_make_unknown_thread_ref(void) {
  trace_thread_ref_t ref = {.encoded_value = TRACE_ENCODED_THREAD_REF_INLINE,
                            .inline_process_koid = ZX_KOID_INVALID,
                            .inline_thread_koid = ZX_KOID_INVALID};
  return ref;
}

// Makes a thread ref with an inline value.
// The process and thread koids must not both be invalid.
static inline trace_thread_ref_t trace_make_inline_thread_ref(zx_koid_t process_koid,
                                                              zx_koid_t thread_koid) {
  ZX_DEBUG_ASSERT(process_koid != ZX_KOID_INVALID || thread_koid != ZX_KOID_INVALID);
  trace_thread_ref_t ref = {.encoded_value = TRACE_ENCODED_THREAD_REF_INLINE,
                            .inline_process_koid = process_koid,
                            .inline_thread_koid = thread_koid};
  return ref;
}

// Makes an indexed thread ref.
// The index must be >= |TRACE_ENCODED_THREAD_REF_MIN_INDEX|
// and <= |TRACE_ENCODED_THREAD_REF_MAX_INDEX|.
static inline trace_thread_ref_t trace_make_indexed_thread_ref(trace_thread_index_t index) {
  ZX_DEBUG_ASSERT(index >= TRACE_ENCODED_THREAD_REF_MIN_INDEX &&
                  index <= TRACE_ENCODED_THREAD_REF_MAX_INDEX);
  trace_thread_ref_t ref = {.encoded_value = (trace_encoded_thread_ref_t)index,
                            .inline_process_koid = ZX_KOID_INVALID,
                            .inline_thread_koid = ZX_KOID_INVALID};
  return ref;
}

// The maximum length of a trace record that is allowed inline in the
// trace buffer. The limit is currently set to 256KB.
#define TRACE_ENCODED_INLINE_LARGE_RECORD_MAX_SIZE ((size_t)(1ull << 18))

typedef enum {
  TRACE_BLOB_FORMAT_EVENT = 0,
  TRACE_BLOB_FORMAT_ATTACHMENT = 1,
} trace_blob_format_t;

// Enumerates all known argument types.
typedef enum {
  TRACE_ARG_NULL = 0,
  TRACE_ARG_INT32 = 1,
  TRACE_ARG_UINT32 = 2,
  TRACE_ARG_INT64 = 3,
  TRACE_ARG_UINT64 = 4,
  TRACE_ARG_DOUBLE = 5,
  TRACE_ARG_STRING = 6,
  TRACE_ARG_POINTER = 7,
  TRACE_ARG_KOID = 8,
  TRACE_ARG_BOOL = 9,
} trace_arg_type_t;

// A typed argument value.
typedef struct {
  trace_arg_type_t type;
  union {
    int32_t int32_value;
    uint32_t uint32_value;
    int64_t int64_value;
    uint64_t uint64_value;
    double double_value;
    trace_string_ref_t string_value_ref;
    uintptr_t pointer_value;
    zx_koid_t koid_value;
    bool bool_value;
    uintptr_t reserved_for_future_expansion[2];
  };
} trace_arg_value_t;

// Makes a null argument value.
static inline trace_arg_value_t trace_make_null_arg_value(void) {
  trace_arg_value_t arg_value = {TRACE_ARG_NULL, {}};
  return arg_value;
}

// Makes a signed 32-bit integer argument value.
static inline trace_arg_value_t trace_make_int32_arg_value(int32_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_INT32, {.int32_value = value}};
  return arg_value;
}

// Makes an unsigned 32-bit integer argument value.
static inline trace_arg_value_t trace_make_uint32_arg_value(uint32_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_UINT32, {.uint32_value = value}};
  return arg_value;
}

// Makes a signed 64-bit integer argument value.
static inline trace_arg_value_t trace_make_int64_arg_value(int64_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_INT64, {.int64_value = value}};
  return arg_value;
}

// Makes an unsigned 64-bit integer argument value.
static inline trace_arg_value_t trace_make_uint64_arg_value(uint64_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_UINT64, {.uint64_value = value}};
  return arg_value;
}

// Makes a double-precision floating point argument value.
static inline trace_arg_value_t trace_make_double_arg_value(double value) {
  trace_arg_value_t arg_value = {TRACE_ARG_DOUBLE, {.double_value = value}};
  return arg_value;
}

// Makes a string argument value.
static inline trace_arg_value_t trace_make_string_arg_value(trace_string_ref_t value_ref) {
  trace_arg_value_t arg_value = {TRACE_ARG_STRING, {.string_value_ref = value_ref}};
  return arg_value;
}

// Makes a pointer argument value.
static inline trace_arg_value_t trace_make_pointer_arg_value(uintptr_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_POINTER, {.pointer_value = value}};
  return arg_value;
}

// Makes a koid argument value.
static inline trace_arg_value_t trace_make_koid_arg_value(zx_koid_t value) {
  trace_arg_value_t arg_value = {TRACE_ARG_KOID, {.koid_value = value}};
  return arg_value;
}

// Makes a boolean argument value.
static inline trace_arg_value_t trace_make_bool_arg_value(bool value) {
  trace_arg_value_t arg_value = {TRACE_ARG_BOOL, {.bool_value = value}};
  return arg_value;
}

// A named argument and value.
// Often packed into an array to form an argument list when writing records.
typedef struct {
  trace_string_ref_t name_ref;
  trace_arg_value_t value;
} trace_arg_t;

// Makes an argument with name and value.
static inline trace_arg_t trace_make_arg(trace_string_ref_t name_ref, trace_arg_value_t value) {
  trace_arg_t arg = {.name_ref = name_ref, .value = value};
  return arg;
}

// The trace format specified maximum number of args for a record.
#define TRACE_MAX_ARGS ((size_t)15u)

// BlobType enumerates all known trace blob types.
typedef enum {
  TRACE_BLOB_TYPE_DATA = 1,
  TRACE_BLOB_TYPE_LAST_BRANCH = 2,
  TRACE_BLOB_TYPE_PERFETTO = 3,
} trace_blob_type_t;

// The maximum size of a blob.
// This is slightly less than the actual maximum ((= 0xfff * 8) - header size)
// to allow room for reasonably sized blob names should they get inlined.
#define TRACE_MAX_BLOB_SIZE ((size_t)32000u)

// The buffering mode.
typedef enum {
  // Keep filling the trace buffer until it is full and then stop tracing.
  TRACE_BUFFERING_MODE_ONESHOT = 0,
  // When the buffer fills start overwriting records from the beginning.
  TRACE_BUFFERING_MODE_CIRCULAR = 1,
  // When the buffer reaches a critical point notify the trace manager to
  // save the trace thus far. Essentially this is an implementation of
  // double buffering, though the underlying details are unspecified.
  TRACE_BUFFERING_MODE_STREAMING = 2,
} trace_buffering_mode_t;

__END_CDECLS

#ifdef __cplusplus

namespace trace {

// Enumerates all known record types.
enum class RecordType {
  kMetadata = 0,
  kInitialization = 1,
  kString = 2,
  kThread = 3,
  kEvent = 4,
  kBlob = 5,
  kKernelObject = 7,
  kScheduler = 8,
  kLog = 9,

  // The kLargeRecord uses a 32-bit size field.
  kLargeRecord = 15,
};

enum class LargeRecordType {
  kBlob = 0,
};

// MetadataType enumerates all known trace metadata types.
enum class MetadataType {
  kProviderInfo = 1,
  kProviderSection = 2,
  kProviderEvent = 3,
  kTraceInfo = 4,
};

// Enumerates all provider events.
enum class ProviderEventType {
  kBufferOverflow = 0,
};

// Enumerates all known trace info types.
enum class TraceInfoType {
  kMagicNumber = 0,
};

// The four byte value present in a magic number record.
constexpr uint32_t kMagicValue = 0x16547846;

// Enumerates all known argument types.
enum class ArgumentType {
  kNull = TRACE_ARG_NULL,
  kInt32 = TRACE_ARG_INT32,
  kUint32 = TRACE_ARG_UINT32,
  kInt64 = TRACE_ARG_INT64,
  kUint64 = TRACE_ARG_UINT64,
  kDouble = TRACE_ARG_DOUBLE,
  kString = TRACE_ARG_STRING,
  kPointer = TRACE_ARG_POINTER,
  kKoid = TRACE_ARG_KOID,
  kBool = TRACE_ARG_BOOL,
};

// EventType enumerates all known trace event types.
enum class EventType {
  kInstant = 0,
  kCounter = 1,
  kDurationBegin = 2,
  kDurationEnd = 3,
  kDurationComplete = 4,
  kAsyncBegin = 5,
  kAsyncInstant = 6,
  kAsyncEnd = 7,
  kFlowBegin = 8,
  kFlowStep = 9,
  kFlowEnd = 10,
};

// SchedulerEventType enumerates all known scheduler event types.
enum class SchedulerEventType {
  kLegacyContextSwitch = 0,
  kContextSwitch = 1,
  kThreadWakeup = 2,
};

// Specifies the scope of instant events.
enum class EventScope {
  kThread = TRACE_SCOPE_THREAD,
  kProcess = TRACE_SCOPE_PROCESS,
  kGlobal = TRACE_SCOPE_GLOBAL,
};

// Trace provider id in a trace session.
using ProviderId = uint32_t;

// Thread states used to describe context switches.
enum class ThreadState {
  kNew = ZX_THREAD_STATE_NEW,
  kRunning = ZX_THREAD_STATE_RUNNING,
  kSuspended = ZX_THREAD_STATE_SUSPENDED,
  kBlocked = ZX_THREAD_STATE_BLOCKED,
  kDying = ZX_THREAD_STATE_DYING,
  kDead = ZX_THREAD_STATE_DEAD,
};

using ArgumentHeader = uint64_t;
using RecordHeader = uint64_t;

}  // namespace trace

#endif  // __cplusplus

#endif  // LIB_TRACE_ENGINE_TYPES_H_
