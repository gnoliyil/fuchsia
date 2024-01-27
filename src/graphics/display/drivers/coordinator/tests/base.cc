// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/coordinator/tests/base.h"

#include <lib/async/cpp/task.h>
#include <lib/zx/clock.h>

#include <memory>

#include "src/devices/sysmem/drivers/sysmem/device.h"
#include "src/devices/testing/mock-ddk/mock-device.h"
#include "src/graphics/display/drivers/coordinator/controller.h"
#include "src/graphics/display/drivers/fake/fake-display.h"

namespace display {

void TestBase::SetUp() {
  std::shared_ptr<zx_device> mock_root = MockDevice::FakeRootParent();
  loop_.StartThread("display::TestBase::loop_", &loop_thrd_);
  auto sysmem =
      std::make_unique<GenericSysmemDeviceWrapper<sysmem_driver::Device>>(mock_root.get());
  tree_ = std::make_unique<MockDisplayDeviceTree>(std::move(mock_root), std::move(sysmem),
                                                  /*start_vsync=*/false);
}

void TestBase::TearDown() {
  tree_->AsyncShutdown();
  async::PostTask(loop_.dispatcher(), [this]() { loop_.Quit(); });

  // Wait for loop_.Quit() to execute.
  loop_.JoinThreads();

  tree_.reset();
}

bool TestBase::RunLoopWithTimeoutOrUntil(fit::function<bool()>&& condition, zx::duration timeout,
                                         zx::duration step) {
  ZX_ASSERT(step != zx::duration::infinite());
  const zx::time timeout_deadline = zx::deadline_after(timeout) + step;

  // We can't create a task on the loop that will block, so this task reschedules itself every
  // |step| until |timeout|.
  auto done = new sync_completion_t;
  auto task = new async::Task();
  auto result =
      std::make_shared<std::atomic<bool>>(false);  // Used by this thread and the looping task.
  task->set_handler([c = std::move(condition), result, done, step](
                        async_dispatcher_t* dispatcher, async::Task* self, zx_status_t status) {
    if (sync_completion_signaled(done)) {
      // The client either timed out or noticed the condition signaled.
      delete done;
      delete self;
      return;
    }
    if (c()) {
      result->store(true);
    }
    zx::nanosleep(zx::deadline_after(step));
    if (self->Post(dispatcher) != ZX_OK) {
      zxlogf(INFO, "Deleted task due to dispatcher shutdown");
      delete done;
      delete self;
    }
  });
  if (task->Post(loop_.dispatcher()) != ZX_OK) {
    delete done;
    delete task;
    return false;
  }
  while (zx::clock::get_monotonic() < timeout_deadline) {
    if (result->load()) {
      sync_completion_signal(done);
      return true;
    }
    zx::nanosleep(zx::deadline_after(step));
  }

  sync_completion_signal(done);
  return result->load();
}

const fidl::WireSyncClient<fuchsia_sysmem2::DriverConnector>& TestBase::sysmem_fidl() {
  return tree_->sysmem_client();
}
const fidl::WireSyncClient<fuchsia_hardware_display::Provider>& TestBase::display_fidl() {
  return tree_->display_client();
}

}  // namespace display
