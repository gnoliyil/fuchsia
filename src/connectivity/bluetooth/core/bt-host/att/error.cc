// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "error.h"

#include "pw_string/format.h"

namespace bt {
namespace {

constexpr const char* ErrorToString(att::ErrorCode ecode) {
  switch (ecode) {
    case att::ErrorCode::kInvalidHandle:
      return "invalid handle";
    case att::ErrorCode::kReadNotPermitted:
      return "read not permitted";
    case att::ErrorCode::kWriteNotPermitted:
      return "write not permitted";
    case att::ErrorCode::kInvalidPDU:
      return "invalid PDU";
    case att::ErrorCode::kInsufficientAuthentication:
      return "insuff. authentication";
    case att::ErrorCode::kRequestNotSupported:
      return "request not supported";
    case att::ErrorCode::kInvalidOffset:
      return "invalid offset";
    case att::ErrorCode::kInsufficientAuthorization:
      return "insuff. authorization";
    case att::ErrorCode::kPrepareQueueFull:
      return "prepare queue full";
    case att::ErrorCode::kAttributeNotFound:
      return "attribute not found";
    case att::ErrorCode::kAttributeNotLong:
      return "attribute not long";
    case att::ErrorCode::kInsufficientEncryptionKeySize:
      return "insuff. encryption key size";
    case att::ErrorCode::kInvalidAttributeValueLength:
      return "invalid attribute value length";
    case att::ErrorCode::kUnlikelyError:
      return "unlikely error";
    case att::ErrorCode::kInsufficientEncryption:
      return "insuff. encryption";
    case att::ErrorCode::kUnsupportedGroupType:
      return "unsupported group type";
    case att::ErrorCode::kInsufficientResources:
      return "insuff. resources";
    default:
      break;
  }

  return "(unknown)";
}

}  // namespace

std::string ProtocolErrorTraits<att::ErrorCode>::ToString(att::ErrorCode ecode) {
  constexpr size_t out_size = sizeof("invalid attribute value length (ATT 0x0d)");
  char out[out_size] = "";
  pw::StatusWithSize status =
      pw::string::Format({out, sizeof(out)}, "%s (ATT %#.2hhx)", ErrorToString(ecode),
                         static_cast<unsigned char>(ecode));
  BT_DEBUG_ASSERT(status.ok());
  return out;
}

}  // namespace bt
