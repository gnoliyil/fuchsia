// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYS_COMPONENT_CPP_CONSTANTS_H_
#define LIB_SYS_COMPONENT_CPP_CONSTANTS_H_

namespace component {

// The name of the default FIDL Service instance.
constexpr const char kDefaultInstance[] = "default";

// The path referencing the incoming services directory.
constexpr const char kServiceDirectory[] = "svc";

// Path delimiter used by svc library.
constexpr const char kSvcPathDelimiter[] = "/";

}  // namespace component

#endif  // LIB_SYS_COMPONENT_CPP_CONSTANTS_H_
