// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/reboot_log/graceful_reboot_reason.h"

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>

#include <string>

#include <fbl/unique_fd.h>

#include "src/lib/files/file_descriptor.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace forensics {
namespace feedback {
namespace {

constexpr char kReasonNotSet[] = "NOT SET";
constexpr char kReasonNone[] = "NONE";
constexpr char kReasonUserRequest[] = "USER REQUEST";
constexpr char kReasonSystemUpdate[] = "SYSTEM UPDATE";
constexpr char kReasonRetrySystemUpdate[] = "RETRY SYSTEM UPDATE";
constexpr char kReasonHighTemperature[] = "HIGH TEMPERATURE";
constexpr char kReasonSessionFailure[] = "SESSION FAILURE";
constexpr char kReasonSysmgrFailure[] = "SYSMGR FAILURE";
constexpr char kReasonCriticalComponentFailure[] = "CRITICAL COMPONENT FAILURE";
constexpr char kReasonFdr[] = "FACTORY DATA RESET";
constexpr char kReasonZbiSwap[] = "ZBI SWAP";
constexpr char kOutOfMemory[] = "OUT OF MEMORY";
constexpr char kReasonNotSupported[] = "NOT SUPPORTED";
constexpr char kReasonNotParseable[] = "NOT PARSEABLE";

}  // namespace

std::string ToString(const GracefulRebootReason reason) {
  switch (reason) {
    case GracefulRebootReason::kNotSet:
      return kReasonNotSet;
    case GracefulRebootReason::kNone:
      return kReasonNone;
    case GracefulRebootReason::kUserRequest:
      return kReasonUserRequest;
    case GracefulRebootReason::kSystemUpdate:
      return kReasonSystemUpdate;
    case GracefulRebootReason::kRetrySystemUpdate:
      return kReasonRetrySystemUpdate;
    case GracefulRebootReason::kHighTemperature:
      return kReasonHighTemperature;
    case GracefulRebootReason::kSessionFailure:
      return kReasonSessionFailure;
    case GracefulRebootReason::kSysmgrFailure:
      return kReasonSysmgrFailure;
    case GracefulRebootReason::kCriticalComponentFailure:
      return kReasonCriticalComponentFailure;
    case GracefulRebootReason::kFdr:
      return kReasonFdr;
    case GracefulRebootReason::kZbiSwap:
      return kReasonZbiSwap;
    case GracefulRebootReason::kOutOfMemory:
      return kOutOfMemory;
    case GracefulRebootReason::kNotSupported:
      return kReasonNotSupported;
    case GracefulRebootReason::kNotParseable:
      return kReasonNotParseable;
  }

  return kReasonNotSet;
}

std::string ToFileContent(const GracefulRebootReason reason) {
  switch (reason) {
    case GracefulRebootReason::kUserRequest:
    case GracefulRebootReason::kSystemUpdate:
    case GracefulRebootReason::kRetrySystemUpdate:
    case GracefulRebootReason::kHighTemperature:
    case GracefulRebootReason::kSessionFailure:
    case GracefulRebootReason::kSysmgrFailure:
    case GracefulRebootReason::kCriticalComponentFailure:
    case GracefulRebootReason::kFdr:
    case GracefulRebootReason::kZbiSwap:
    case GracefulRebootReason::kOutOfMemory:
    case GracefulRebootReason::kNotSupported:
      return ToString(reason);
    case GracefulRebootReason::kNotSet:
    case GracefulRebootReason::kNone:
    case GracefulRebootReason::kNotParseable:
      FX_LOGS(ERROR) << "Invalid persisted graceful reboot reason: " << ToString(reason);
      return kReasonNotSupported;
  }

  return kReasonNotSupported;
}

GracefulRebootReason FromFileContent(const std::string reason) {
  if (reason == kReasonUserRequest) {
    return GracefulRebootReason::kUserRequest;
  } else if (reason == kReasonSystemUpdate) {
    return GracefulRebootReason::kSystemUpdate;
  } else if (reason == kReasonRetrySystemUpdate) {
    return GracefulRebootReason::kRetrySystemUpdate;
  } else if (reason == kReasonHighTemperature) {
    return GracefulRebootReason::kHighTemperature;
  } else if (reason == kReasonSessionFailure) {
    return GracefulRebootReason::kSessionFailure;
  } else if (reason == kReasonSysmgrFailure) {
    return GracefulRebootReason::kSysmgrFailure;
  } else if (reason == kReasonCriticalComponentFailure) {
    return GracefulRebootReason::kCriticalComponentFailure;
  } else if (reason == kReasonFdr) {
    return GracefulRebootReason::kFdr;
  } else if (reason == kReasonZbiSwap) {
    return GracefulRebootReason::kZbiSwap;
  } else if (reason == kReasonNotSupported) {
    return GracefulRebootReason::kNotSupported;
  } else if (reason == kOutOfMemory) {
    return GracefulRebootReason::kOutOfMemory;
  }

  FX_LOGS(ERROR) << "Invalid persisted graceful reboot reason: " << reason;
  return GracefulRebootReason::kNotParseable;
}

GracefulRebootReason ToGracefulRebootReason(
    const fuchsia::hardware::power::statecontrol::RebootReason reason) {
  using fuchsia::hardware::power::statecontrol::RebootReason;

  switch (reason) {
    case RebootReason::USER_REQUEST:
      return GracefulRebootReason::kUserRequest;
    case RebootReason::SYSTEM_UPDATE:
      return GracefulRebootReason::kSystemUpdate;
    case RebootReason::RETRY_SYSTEM_UPDATE:
      return GracefulRebootReason::kRetrySystemUpdate;
    case RebootReason::HIGH_TEMPERATURE:
      return GracefulRebootReason::kHighTemperature;
    case RebootReason::SESSION_FAILURE:
      return GracefulRebootReason::kSessionFailure;
    case RebootReason::SYSMGR_FAILURE:
      return GracefulRebootReason::kSysmgrFailure;
    case RebootReason::CRITICAL_COMPONENT_FAILURE:
      return GracefulRebootReason::kCriticalComponentFailure;
    case RebootReason::FACTORY_DATA_RESET:
      return GracefulRebootReason::kFdr;
    case RebootReason::ZBI_SWAP:
      return GracefulRebootReason::kZbiSwap;
    case RebootReason::OUT_OF_MEMORY:
      return GracefulRebootReason::kOutOfMemory;
    default:
      return GracefulRebootReason::kNotSupported;
  }
}

void WriteGracefulRebootReason(GracefulRebootReason reason, cobalt::Logger* cobalt,
                               const std::string& path) {
  const size_t timer_id = cobalt->StartTimer();

  fbl::unique_fd fd(open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR));
  if (!fd.is_valid()) {
    FX_LOGS(INFO) << "Failed to open reboot reason file: " << path;
    return;
  }

  if (const std::string content = ToFileContent(reason);
      fxl::WriteFileDescriptor(fd.get(), content.data(), content.size())) {
    cobalt->LogElapsedTime(cobalt::RebootReasonWriteResult::kSuccess, timer_id);
  } else {
    cobalt->LogElapsedTime(cobalt::RebootReasonWriteResult::kFailure, timer_id);
    FX_LOGS(ERROR) << "Failed to write reboot reason '" << content << "' to " << path;
  }

  // Force the flush as we want to persist the content asap and we don't have more content to
  // write.
  fsync(fd.get());
}

}  // namespace feedback
}  // namespace forensics
