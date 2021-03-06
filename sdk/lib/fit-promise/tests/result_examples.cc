// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fpromise/result.h>

#include <string>

#include <zxtest/zxtest.h>

namespace {
namespace example1 {
fpromise::result<int, std::string> divide(int dividend, int divisor) {
  if (divisor == 0)
    return fpromise::error<std::string>("divide by zero");
  return fpromise::ok(dividend / divisor);
}

int try_divide(int dividend, int divisor) {
  auto result = divide(dividend, divisor);
  if (result.is_ok()) {
    printf("%d / %d = %d\n", dividend, divisor, result.value());
    return result.value();
  }
  printf("%d / %d: ERROR %s\n", dividend, divisor, result.error().c_str());
  return -999;
}

fpromise::result<> open(std::string secret) {
  printf("guessing \"%s\"\n", secret.c_str());
  if (secret == "sesame") {
    puts("yes!");
    return fpromise::ok();
  }
  puts("no.");
  return fpromise::error();
}

bool guess_combination() { return open("friend") || open("sesame") || open("I give up"); }

TEST(ResultExamples, test) {
  EXPECT_EQ(2, try_divide(5, 2));
  EXPECT_EQ(-999, try_divide(5, 0));
  EXPECT_TRUE(guess_combination());
}
}  // namespace example1
}  // namespace
