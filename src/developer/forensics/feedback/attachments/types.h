// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_ATTACHMENTS_TYPES_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_ATTACHMENTS_TYPES_H_

#include <lib/syslog/cpp/macros.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "src/developer/forensics/utils/errors.h"

namespace forensics::feedback {

using AttachmentKey = std::string;
using AttachmentKeys = std::set<AttachmentKey>;

class AttachmentValue {
 public:
  enum class State {
    kComplete,
    kPartial,
    kMissing,
  };

  explicit AttachmentValue(std::string value)
      : state_(State::kComplete),
        value_(std::make_unique<std::string>(std::move(value))),
        error_(std::nullopt) {}
  AttachmentValue(std::string value, enum Error error)
      : state_(State::kPartial),
        value_(std::make_unique<std::string>(std::move(value))),
        error_(error) {}
  AttachmentValue(enum Error error) : state_(State::kMissing), value_(nullptr), error_(error) {}

  bool HasValue() const { return value_ != nullptr; }

  std::string_view Value() const {
    FX_CHECK(HasValue());
    return *value_;
  }

  bool HasError() const { return error_.has_value(); }

  enum Error Error() const {
    FX_CHECK(HasError());
    return error_.value();
  }

  enum State State() const { return state_; }

  // Allow callers to explicitly copy an attachment.
  AttachmentValue Clone() const {
    if (HasValue() && HasError()) {
      return AttachmentValue(*value_, *error_);
    }

    if (HasValue()) {
      return AttachmentValue(*value_);
    }

    return AttachmentValue(*error_);
  }

 private:
  enum State state_;
  std::unique_ptr<std::string> value_;
  std::optional<enum Error> error_;
};

using Attachment = std::pair<AttachmentKey, AttachmentValue>;
using Attachments = std::map<AttachmentKey, AttachmentValue>;

}  // namespace forensics::feedback

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_ATTACHMENTS_TYPES_H_
