// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/test.error.methods/cpp/fidl.h>
#include <lib/fidl/cpp/any_error_in.h>

#include <gtest/gtest.h>

namespace {

// Test that the |ErrorsIn| template is wired up correctly: it should contain
// the corresponding domain specific error.

// NoArgsPrimitiveError(struct { should_error bool; }) -> () error int32;
static_assert(
    std::is_base_of<fidl::internal::ErrorsInImpl<int32_t>,
                    fidl::ErrorsIn<test_error_methods::ErrorMethods::NoArgsPrimitiveError>>::value);

// ManyArgsCustomError(struct { should_error bool; })
//     -> (struct { a int32; b int32; c int32; }) error MyError;
static_assert(
    std::is_base_of<fidl::internal::ErrorsInImpl<test_error_methods::MyError>,
                    fidl::ErrorsIn<test_error_methods::ErrorMethods::ManyArgsCustomError>>::value);

using ::test_error_methods::MyError;
using ErrorsInMethod = fidl::ErrorsIn<test_error_methods::ErrorMethods::ManyArgsCustomError>;

TEST(ErrorsInMethod, TransportError) {
  ErrorsInMethod error(fidl::Status::UnknownOrdinal());
  EXPECT_TRUE(error.is_framework_error());
  EXPECT_FALSE(error.is_domain_error());
  EXPECT_EQ(fidl::Reason::kUnexpectedMessage, error.framework_error().reason());
  EXPECT_EQ(
      "FIDL operation failed due to unexpected message, status: "
      "ZX_ERR_NOT_SUPPORTED (-2), detail: unknown ordinal",
      error.FormatDescription());
}

TEST(ErrorsInMethod, DomainError) {
  ErrorsInMethod error(MyError::kBadError);
  EXPECT_FALSE(error.is_framework_error());
  EXPECT_TRUE(error.is_domain_error());
  EXPECT_EQ(MyError::kBadError, error.domain_error());
  EXPECT_EQ("FIDL method domain error: test.error.methods/MyError.BAD_ERROR (value: 1)",
            error.FormatDescription());
}

TEST(ErrorsInMethod, UnknownDomainError) {
  ErrorsInMethod error(static_cast<MyError>(42));
  EXPECT_FALSE(error.is_framework_error());
  EXPECT_TRUE(error.is_domain_error());
  EXPECT_EQ("FIDL method domain error: test.error.methods/MyError.[UNKNOWN] (value: 42)",
            error.FormatDescription());
}

TEST(ErrorsInMethod, SignedNumberedDomainError) {
  fidl::internal::ErrorsInImpl<int32_t> error(-3);
  EXPECT_FALSE(error.is_framework_error());
  EXPECT_TRUE(error.is_domain_error());
  EXPECT_EQ(-3, error.domain_error());
  EXPECT_EQ("FIDL method domain error: int32_t (value: -3)", error.FormatDescription());
}

TEST(ErrorsInMethod, UnsignedNumberedDomainError) {
  fidl::internal::ErrorsInImpl<uint32_t> error(3);
  EXPECT_FALSE(error.is_framework_error());
  EXPECT_TRUE(error.is_domain_error());
  EXPECT_EQ(3u, error.domain_error());
  EXPECT_EQ("FIDL method domain error: uint32_t (value: 3)", error.FormatDescription());
}

}  // namespace
