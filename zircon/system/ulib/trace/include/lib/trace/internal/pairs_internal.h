// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
// Internal declarations used by the C tracing macros.
// This is not part of the public API: use <trace/event.h> instead.
//

#ifndef LIB_TRACE_INTERNAL_PAIRS_INTERNAL_H_
#define LIB_TRACE_INTERNAL_PAIRS_INTERNAL_H_

// Count the number of pairs of arguments passed to it without evaluating them.
// When the number of arguments is uneven, rounds down.
// Works with 0 to 15 pairs.
#define TRACE_INTERNAL_COUNT_PAIRS(...)                                                            \
  TRACE_INTERNAL_COUNT_PAIRS_(__VA_ARGS__, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, \
                              7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0)
#define TRACE_INTERNAL_COUNT_PAIRS_(_15, _15X, _14, _14X, _13, _13X, _12, _12X, _11, _11X, _10, \
                                    _10X, _9, _9X, _8, _8X, _7, _7X, _6, _6X, _5, _5X, _4, _4X, \
                                    _3, _3X, _2, _2X, _1, _1X, N, ...)                          \
  N

// Applies a function or macro to each pair of arguments.
// Works with 0 to 15 pairs.
//
// |fn| is the function or macro name to apply
// |vn| is the name of a local variable that the caller may be using. It is
// passed on to |fn| in case it needs it, e.g., to uniquify any further local
// variable names that |fn| as a macro may want to create.
//
// |fn| must accept four arguments:
//   - |vn|, described above
//   - |idx|, which is 0,1,2,...,15 and designates the index of the name/value
//     pair in the provided list
//   - |key|, which is the key part of the key/value pair
//   - |value|, which is the value part of the key/value pair
//
// Example:
//     #define MY_FN(vn, idx, a, b)
//     TRACE_INTERNAL_APPLY_PAIRWISE(MY_FN, my_var, "x", 1, "y", 2)

#define TRACE_INTERNAL_APPLY_PAIRWISE(fn, vn, ...)                        \
  TRACE_INTERNAL_APPLY_PAIRWISE_(TRACE_INTERNAL_COUNT_PAIRS(__VA_ARGS__)) \
  (fn, vn, __VA_ARGS__)
#define TRACE_INTERNAL_APPLY_PAIRWISE_(n) TRACE_INTERNAL_APPLY_PAIRWISE__(n)
#define TRACE_INTERNAL_APPLY_PAIRWISE__(n) TRACE_INTERNAL_APPLY_PAIRWISE##n
// clang-format off
#define TRACE_INTERNAL_APPLY_PAIRWISE0(fn, vn, ...)
#define TRACE_INTERNAL_APPLY_PAIRWISE1(fn, vn, k1, v1) \
    fn(vn, 1, k1, v1)
#define TRACE_INTERNAL_APPLY_PAIRWISE2(fn, vn, k1, v1, k2, v2) \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2)
#define TRACE_INTERNAL_APPLY_PAIRWISE3(fn, vn, k1, v1, k2, v2, k3, v3) \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3)
#define TRACE_INTERNAL_APPLY_PAIRWISE4(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4) \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)
#define TRACE_INTERNAL_APPLY_PAIRWISE5(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                       k5, v5)                                 \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)    \
    fn(vn, 5, k5, v5)
#define TRACE_INTERNAL_APPLY_PAIRWISE6(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                       k5, v5, k6, v6)                         \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)    \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6)
#define TRACE_INTERNAL_APPLY_PAIRWISE7(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                       k5, v5, k6, v6, k7, v7)                 \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)    \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7)
#define TRACE_INTERNAL_APPLY_PAIRWISE8(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                       k5, v5, k6, v6, k7, v7, k8, v8)         \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)    \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)
#define TRACE_INTERNAL_APPLY_PAIRWISE9(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                       k5, v5, k6, v6, k7, v7, k8, v8,         \
                                       k9, v9)                                 \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)    \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)    \
    fn(vn, 9, k9, v9)
#define TRACE_INTERNAL_APPLY_PAIRWISE10(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                        k5, v5, k6, v6, k7, v7, k8, v8,         \
                                        k9, v9, k10, v10)                       \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)     \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)     \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10)
#define TRACE_INTERNAL_APPLY_PAIRWISE11(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                        k5, v5, k6, v6, k7, v7, k8, v8,         \
                                        k9, v9, k10, v10, k11, v11)             \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)     \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)     \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10) fn(vn, 11, k11, v11)
#define TRACE_INTERNAL_APPLY_PAIRWISE12(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                        k5, v5, k6, v6, k7, v7, k8, v8,         \
                                        k9, v9, k10, v10, k11, v11, k12, v12)   \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)     \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)     \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10) fn(vn, 11, k11, v11) fn(vn, 12, k12, v12)
#define TRACE_INTERNAL_APPLY_PAIRWISE13(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                        k5, v5, k6, v6, k7, v7, k8, v8,              \
                                        k9, v9, k10, v10, k11, v11, k12, v12,        \
                                        k13, v13)                                    \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)          \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)          \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10) fn(vn, 11, k11, v11) fn(vn, 12, k12, v12) \
    fn(vn, 13, k13, v13)
#define TRACE_INTERNAL_APPLY_PAIRWISE14(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                        k5, v5, k6, v6, k7, v7, k8, v8,              \
                                        k9, v9, k10, v10, k11, v11, k12, v12,        \
                                        k13, v13, k14, v14)                          \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)          \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)          \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10) fn(vn, 11, k11, v11) fn(vn, 12, k12, v12) \
    fn(vn, 13, k13, v13) fn(vn, 14, k14, v14)
#define TRACE_INTERNAL_APPLY_PAIRWISE15(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                        k5, v5, k6, v6, k7, v7, k8, v8,              \
                                        k9, v9, k10, v10, k11, v11, k12, v12,        \
                                        k13, v13, k14, v14, k15, v15)                \
    fn(vn, 1, k1, v1) fn(vn, 2, k2, v2) fn(vn, 3, k3, v3) fn(vn, 4, k4, v4)          \
    fn(vn, 5, k5, v5) fn(vn, 6, k6, v6) fn(vn, 7, k7, v7) fn(vn, 8, k8, v8)          \
    fn(vn, 9, k9, v9) fn(vn, 10, k10, v10) fn(vn, 11, k11, v11) fn(vn, 12, k12, v12) \
    fn(vn, 13, k13, v13) fn(vn, 14, k14, v14) fn(vn, 15, k15, v15)
// clang-format on

// Applies a function or macro to each pair of arguments to produce a
// comma-separated result.  Works with 0 to 15 pairs.
//
// Example:
//     #define MY_FN(vn, idx, a, b)
//     TRACE_INTERNAL_APPLY_PAIRWISE_CSV(MY_FN, my_var, "x", 1, "y", 2)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV(fn, vn, ...)                        \
  TRACE_INTERNAL_APPLY_PAIRWISE_CSV_(TRACE_INTERNAL_COUNT_PAIRS(__VA_ARGS__)) \
  (fn, vn, __VA_ARGS__)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV_(n) TRACE_INTERNAL_APPLY_PAIRWISE_CSV__(n)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV__(n) TRACE_INTERNAL_APPLY_PAIRWISE_CSV##n
// clang-format off
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV0(fn, vn, ...)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV1(fn, vn, k1, v1) \
    fn(vn, 1, k1, v1)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV2(fn, vn, k1, v1, k2, v2) \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV3(fn, vn, k1, v1, k2, v2, k3, v3) \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV4(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4) \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV5(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                           k5, v5)                                 \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),    \
    fn(vn, 5, k5, v5)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV6(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                           k5, v5, k6, v6)                         \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),    \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV7(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                           k5, v5, k6, v6, k7, v7)                 \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),    \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV8(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                           k5, v5, k6, v6, k7, v7, k8, v8)         \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),    \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV9(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                           k5, v5, k6, v6, k7, v7, k8, v8,         \
                                           k9, v9)                                 \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),    \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),    \
    fn(vn, 9, k9, v9)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV10(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                            k5, v5, k6, v6, k7, v7, k8, v8,         \
                                            k9, v9, k10, v10)                       \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),     \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),     \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV11(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                            k5, v5, k6, v6, k7, v7, k8, v8,         \
                                            k9, v9, k10, v10, k11, v11)             \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),     \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),     \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10), fn(vn, 11, k11, v11)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV12(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4, \
                                            k5, v5, k6, v6, k7, v7, k8, v8,         \
                                            k9, v9, k10, v10, k11, v11, k12, v12)   \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),     \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),     \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10), fn(vn, 11, k11, v11), fn(vn, 12, k12, v12)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV13(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                            k5, v5, k6, v6, k7, v7, k8, v8,              \
                                            k9, v9, k10, v10, k11, v11, k12, v12,        \
                                            k13, v13)                                    \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),          \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),          \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10), fn(vn, 11, k11, v11), fn(vn, 12, k12, v12), \
    fn(vn, 13, k13, v13)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV14(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                            k5, v5, k6, v6, k7, v7, k8, v8,              \
                                            k9, v9, k10, v10, k11, v11, k12, v12,        \
                                            k13, v13, k14, v14)                          \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),          \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),          \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10), fn(vn, 11, k11, v11), fn(vn, 12, k12, v12), \
    fn(vn, 13, k13, v13), fn(vn, 14, k14, v14)
#define TRACE_INTERNAL_APPLY_PAIRWISE_CSV15(fn, vn, k1, v1, k2, v2, k3, v3, k4, v4,      \
                                            k5, v5, k6, v6, k7, v7, k8, v8,              \
                                            k9, v9, k10, v10, k11, v11, k12, v12,        \
                                            k13, v13, k14, v14, k15, v15)                \
    fn(vn, 1, k1, v1), fn(vn, 2, k2, v2), fn(vn, 3, k3, v3), fn(vn, 4, k4, v4),          \
    fn(vn, 5, k5, v5), fn(vn, 6, k6, v6), fn(vn, 7, k7, v7), fn(vn, 8, k8, v8),          \
    fn(vn, 9, k9, v9), fn(vn, 10, k10, v10), fn(vn, 11, k11, v11), fn(vn, 12, k12, v12), \
    fn(vn, 13, k13, v13), fn(vn, 14, k14, v14), fn(vn, 15, k15, v15)
// clang-format on

#endif  // LIB_TRACE_INTERNAL_PAIRS_INTERNAL_H_
