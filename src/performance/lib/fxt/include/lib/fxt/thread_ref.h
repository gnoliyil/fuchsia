// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Given a Writer implementing the Writer interface in writer-internal.h, provide an api
// over the writer to allow serializing fxt to the Writer.
//
// Based heavily on libTrace in zircon/system/ulib/trace to allow compatibility,
// but modified to enable passing in an arbitrary buffering system.

#ifndef SRC_PERFORMANCE_LIB_FXT_INCLUDE_LIB_FXT_THREAD_REF_H_
#define SRC_PERFORMANCE_LIB_FXT_INCLUDE_LIB_FXT_THREAD_REF_H_

#include <zircon/types.h>

#include "record_types.h"

namespace fxt {

// Represents an FXT Thread Reference which is either inline in the record
// body, or an index included in the record header
//
// See also: https://fuchsia.dev/fuchsia-src/reference/tracing/trace-format#thread-references
template <RefType>
class ThreadRef;

template <>
class ThreadRef<RefType::kInline> {
 public:
  ThreadRef(zx_koid_t process, zx_koid_t thread) : process_(Koid(process)), thread_(Koid(thread)) {}
  ThreadRef(Koid process, Koid thread) : process_(process), thread_(thread) {}

  static WordSize PayloadSize() { return WordSize(2); }
  static uint64_t HeaderEntry() { return 0; }

  template <typename Reservation>
  void Write(Reservation& res) const {
    res.WriteWord(process_.koid);
    res.WriteWord(thread_.koid);
  }

 private:
  Koid process_;
  Koid thread_;
};
#if __cplusplus >= 201703L
ThreadRef(zx_koid_t, zx_koid_t)->ThreadRef<RefType::kInline>;
ThreadRef(Koid, Koid)->ThreadRef<RefType::kInline>;
#endif

template <>
class ThreadRef<RefType::kId> {
 public:
  explicit ThreadRef(uint8_t id) : id_(id) {}

  static WordSize PayloadSize() { return WordSize(0); }
  uint64_t HeaderEntry() const { return id_; }

  template <typename Reservation>
  void Write(Reservation& res) const {}

 private:
  uint8_t id_;
};
#if __cplusplus >= 201703L
ThreadRef(uint8_t)->ThreadRef<RefType::kId>;
#endif

}  // namespace fxt

#endif  // SRC_PERFORMANCE_LIB_FXT_INCLUDE_LIB_FXT_THREAD_REF_H_
