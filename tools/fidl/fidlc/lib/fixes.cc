// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/fixes.h"

namespace fidl {

std::map<const Fix::Kind, const std::string_view> Fix::FIX_KIND_STRINGS = {
    {Fix::Kind::kNoop, "noop"},
    {Fix::Kind::kProtocolModifier, "protocol_modifier"},
    {Fix::Kind::kEmptyStructResponse, "empty_struct_response"},
};

const std::string_view Fix::FixKindName(Kind kind) { return FIX_KIND_STRINGS[kind]; }

}  // namespace fidl
