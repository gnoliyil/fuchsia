// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_

#include <lib/fit/function.h>

#include <map>
#include <string_view>

namespace fidl {

// A "fix" is a transformation function, which takes some deprecated FIDL files and automatically
// upgrades them to some newer configuration. This could involve changing how things are spelled in
// the syntax, or more complex changes like back-porting support for new features.
class Fix {
 public:
  enum struct Kind {
    kNoop,
    kProtocolModifier,
    kEmptyStructResponse,
  };

  explicit Fix(Kind kind) : kind_(kind) {}

  static const std::string_view FixKindName(Kind kind);

  // TODO(fxbug.dev/114357): This will be the entry point for future fixes, leave empty for now.
  virtual bool Transform() = 0;

 private:
  // TODO(fxbug.dev/114357): Will see use in a future CL.
  [[maybe_unused]] const Kind kind_;
  static std::map<const Fix::Kind, const std::string_view> FIX_KIND_STRINGS;
};

// A fix that does nothing. This is retained both for testing purposes, and to ensure there is
// always at least one "example" |Fix| implementation, even when no active fixes are being
// performed.
class NoopFix : public Fix {
 public:
  NoopFix() : Fix(Fix::Kind::kNoop) {}

  bool Transform() override { return true; }
};

class ProtocolModifierFix : public Fix {
 public:
  ProtocolModifierFix() : Fix(Fix::Kind::kProtocolModifier) {}

  // TODO(fxbug.dev/114357): This will be the entry point for future fixes, leave empty for now.
  bool Transform() override;
};

class EmptyStructResponseFix : public Fix {
 public:
  EmptyStructResponseFix() : Fix(Fix::Kind::kEmptyStructResponse) {}

  // TODO(fxbug.dev/114357): This will be the entry point for future fixes, leave empty for now.
  bool Transform() override;
};

}  // namespace fidl

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_
