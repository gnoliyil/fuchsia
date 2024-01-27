// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>
#include <zircon/syscalls/object.h>

#include <cinttypes>
#include <climits>
#include <memory>
#include <utility>

#include <zxtest/zxtest.h>

#include "helper.h"

namespace object_info_test {
namespace {

constexpr auto job_provider = []() -> const zx::job& {
  static const zx::unowned_job job = zx::job::default_job();
  return *job;
};

constexpr auto process_provider = []() -> const zx::process& {
  static const zx::unowned_process process = zx::process::self();
  return *process;
};

constexpr auto thread_provider = []() -> const zx::thread& {
  static const zx::unowned_thread thread = zx::thread::self();
  return *thread;
};

constexpr auto vmar_provider = []() -> const zx::vmar& {
  const static zx::unowned_vmar vmar = zx::vmar::root_self();
  return *vmar;
};

TEST(VmarGetInfoTest, InfoHandleBasicOnSelfSuceeds) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckSelfInfoSuceeds<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider())));
}

TEST(VmarGetInfoTest, InfoHandleBasicInvalidHandleFails) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckInvalidHandleFails<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicNullAvailSucceeds) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckNullAvailSuceeds<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicNullActualSucceeds) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckNullActualSuceeds<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicNullActualAndAvailSucceeds) {
  ASSERT_NO_FATAL_FAILURE((CheckNullActualAndAvailSuceeds<zx_info_handle_basic_t>(
      ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicInvalidBufferPointerFails) {
  ASSERT_NO_FATAL_FAILURE((
      CheckInvalidBufferPointerFails<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicBadActualIsInvalidArg) {
  ASSERT_NO_FATAL_FAILURE(
      (BadActualIsInvalidArgs<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicBadAvailIsInvalidArg) {
  ASSERT_NO_FATAL_FAILURE(
      (BadAvailIsInvalidArgs<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoHandleBasicZeroSizedBufferFails) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckZeroSizeBufferFails<zx_info_handle_basic_t>(ZX_INFO_HANDLE_BASIC, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarOnSelfFails) {
  ASSERT_NO_FATAL_FAILURE((CheckSelfInfoSuceeds<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider())));
}

TEST(VmarGetInfoTest, InfoVmarInvalidHandleFails) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckInvalidHandleFails<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarNullAvailSucceeds) {
  ASSERT_NO_FATAL_FAILURE((CheckNullAvailSuceeds<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarNullActualSucceeds) {
  ASSERT_NO_FATAL_FAILURE((CheckNullActualSuceeds<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarNullActualAndAvailSucceeds) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckNullActualAndAvailSuceeds<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarInvalidBufferPointerFails) {
  ASSERT_NO_FATAL_FAILURE(
      (CheckInvalidBufferPointerFails<zx_info_vmar_t>(ZX_INFO_VMAR, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarBadActualIsInvalidArg) {
  ASSERT_NO_FATAL_FAILURE((BadActualIsInvalidArgs<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarBadAvailIsInvalidArg) {
  ASSERT_NO_FATAL_FAILURE((BadAvailIsInvalidArgs<zx_info_vmar_t>(ZX_INFO_VMAR, 1, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarZeroSizedBufferFails) {
  ASSERT_NO_FATAL_FAILURE((CheckZeroSizeBufferFails<zx_info_vmar_t>(ZX_INFO_VMAR, vmar_provider)));
}

TEST(VmarGetInfoTest, InfoVmarJobHandleIsBadHandle) {
  ASSERT_NO_FATAL_FAILURE(
      CheckWrongHandleTypeFails<zx_info_vmar_t>(ZX_INFO_VMAR, 32, job_provider));
}

TEST(VmarGetInfoTest, InfoVmarProcessHandleIsBadHandle) {
  ASSERT_NO_FATAL_FAILURE(
      CheckWrongHandleTypeFails<zx_info_vmar_t>(ZX_INFO_VMAR, 32, process_provider));
}

TEST(VmarGetInfoTest, InfoVmarThreadHandleIsBadHandle) {
  ASSERT_NO_FATAL_FAILURE(
      CheckWrongHandleTypeFails<zx_info_vmar_t>(ZX_INFO_VMAR, 32, thread_provider));
}

}  // namespace
}  // namespace object_info_test
