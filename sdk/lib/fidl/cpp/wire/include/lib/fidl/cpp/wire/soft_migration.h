// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_SOFT_MIGRATION_H_
#define LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_SOFT_MIGRATION_H_

#include <type_traits>

#ifdef FIDL_LLCPP_ALLOW_DEPRECATED_RAW_CHANNELS
#define FIDL_DEPRECATED_USE_TYPED_CHANNELS
#else
#define FIDL_DEPRECATED_USE_TYPED_CHANNELS                                                \
  [[deprecated(                                                                           \
      "[fidl][llcpp] This declaration is deprecated because it uses raw |zx::channel|s. " \
      "Consider migrating to a version with typed channels (fxbug.dev/65212). "           \
      "See documentation on the declaration for details.")]]
#endif  // FIDL_LLCPP_ALLOW_DEPRECATED_RAW_CHANNELS

#ifdef FIDL_LLCPP_ALLOW_DEPRECATED_RAW_CHANNELS
#define FIDL_CONDITIONALLY_EXPLICIT_CONVERSION
#else
#define FIDL_CONDITIONALLY_EXPLICIT_CONVERSION explicit
#endif  // FIDL_LLCPP_ALLOW_DEPRECATED_RAW_CHANNELS

#endif  // LIB_FIDL_CPP_WIRE_INCLUDE_LIB_FIDL_CPP_WIRE_SOFT_MIGRATION_H_
