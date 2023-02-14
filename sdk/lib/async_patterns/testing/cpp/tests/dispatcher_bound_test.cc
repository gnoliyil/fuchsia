// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>

#include <memory>

#include <gtest/gtest.h>

namespace {

TEST(BlockOn, Int) {
  async::Loop remote_loop{&kAsyncLoopConfigNeverAttachToThread};
  remote_loop.StartThread();
  class Object {
   public:
    int Get() { return 42; }
    int Add(int a, int b) { return a + b; }
  };
  async_patterns::TestDispatcherBound<Object> object{remote_loop.dispatcher()};
  int result = object.SyncCall(&Object::Get);
  EXPECT_EQ(result, 42);
  int sum = object.SyncCall(&Object::Add, 1, 2);
  EXPECT_EQ(sum, 3);
}

TEST(BlockOn, MoveOnly) {
  async::Loop remote_loop{&kAsyncLoopConfigNeverAttachToThread};
  remote_loop.StartThread();
  class Object {
   public:
    std::unique_ptr<int> PassThrough(std::unique_ptr<int> a) { return a; }
  };
  async_patterns::TestDispatcherBound<Object> object{remote_loop.dispatcher()};
  std::unique_ptr<int> result = object.SyncCall(&Object::PassThrough, std::make_unique<int>(1));
  EXPECT_TRUE(result);
  EXPECT_EQ(*result, 1);
}

}  // namespace