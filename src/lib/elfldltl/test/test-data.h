// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ELFLDLTL_TEST_TEST_DATA_H_
#define SRC_LIB_ELFLDLTL_TEST_TEST_DATA_H_

#include <stddef.h>
#include <zircon/compiler.h>

#include <string_view>

extern "C" int Return24();

extern "C" const int foo;

extern "C" __EXPORT int BasicSymbol();
extern "C" __EXPORT int NeedsPlt();
extern "C" __EXPORT int NeedsGot();

struct TestData {
  const int* rodata;
  int* data;
  int* bss;
};

constexpr size_t kSmallDataCount = 2;
constexpr size_t kLargeDataCount = 65538;

constexpr std::string_view kRet24 = "elfldltl-test-ret24.so";
constexpr std::string_view kNoXSegment = "elfldltest-no-execute-data-0.so";
constexpr std::string_view kNoXSegmentLargeData = "elfldltest-no-execute-data-65536.so";
constexpr std::string_view kSymbolic = "elfldltl-test-symbolic.so";

#endif  // SRC_LIB_ELFLDLTL_TEST_TEST_DATA_H_
