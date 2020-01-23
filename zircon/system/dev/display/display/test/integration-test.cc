// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-testing/test_loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <zircon/pixelformat.h>
#include <zircon/types.h>

#include <fbl/auto_lock.h>
#include <zxtest/zxtest.h>

#include "base.h"
#include "fidl_client.h"

namespace display {

class IntegrationTest : public TestBase {};

TEST_F(IntegrationTest, ClientsCanBail) {
  TestFidlClient client;
  ASSERT_TRUE(client.CreateChannel(display_fidl()->get(), false));
  ASSERT_TRUE(client.Bind(dispatcher()));
}

TEST_F(IntegrationTest, MustUseUniqueEvenIDs) {
  TestFidlClient client;
  ASSERT_TRUE(client.CreateChannel(display_fidl()->get(), false));
  ASSERT_TRUE(client.Bind(dispatcher()));
  zx::event event_a, event_b, event_c;
  ASSERT_OK(zx::event::create(0, &event_a));
  ASSERT_OK(zx::event::create(0, &event_b));
  ASSERT_OK(zx::event::create(0, &event_c));
  fbl::AutoLock lock(client.mtx());
  EXPECT_OK(client.dc_->ImportEvent(std::move(event_a), 123).status());
  // ImportEvent is one way. Expect the next call to fail.
  EXPECT_OK(client.dc_->ImportEvent(std::move(event_b), 123).status());
  // This test passes if it closes without deadlocking.
  // TODO: Use LLCPP epitaphs when available to detect ZX_ERR_PEER_CLOSED.
}

}  // namespace display
