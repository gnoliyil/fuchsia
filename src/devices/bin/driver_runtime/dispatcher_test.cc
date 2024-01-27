// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_runtime/dispatcher.h"

#include <lib/async/cpp/irq.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/sequence_id.h>
#include <lib/async/task.h>
#include <lib/driver/component/cpp/outgoing_directory.h>
#include <lib/fdf/arena.h>
#include <lib/fdf/channel.h>
#include <lib/fdf/cpp/channel_read.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/fdf/cpp/env.h>
#include <lib/fdf/testing.h>
#include <lib/fit/defer.h>
#include <lib/sync/cpp/completion.h>
#include <lib/zx/event.h>
#include <lib/zx/interrupt.h>
#include <zircon/errors.h>

#include <thread>

#include <zxtest/zxtest.h>

#include "lib/async/cpp/task.h"
#include "lib/fdf/dispatcher.h"
#include "lib/zx/time.h"
#include "src/devices/bin/driver_runtime/driver_context.h"
#include "src/devices/bin/driver_runtime/runtime_test_case.h"

class DispatcherTest : public RuntimeTestCase {
 public:
  DispatcherTest() : config_(MakeConfig()), loop_(&config_) {}

  void SetUp() override;
  void TearDown() override;

  // Creates a dispatcher and returns it in |out_dispatcher|.
  // The dispatcher will automatically be destroyed in |TearDown|.
  void CreateDispatcher(uint32_t options, std::string_view name, std::string_view scheduler_role,
                        const void* owner, fdf_dispatcher_t** out_dispatcher);

  // Registers an async read, which on callback will acquire |lock| and read from |read_channel|.
  // If |reply_channel| is not null, it will write an empty message.
  // If |completion| is not null, it will signal before returning from the callback.
  static void RegisterAsyncReadReply(fdf_handle_t read_channel, fdf_dispatcher_t* dispatcher,
                                     fbl::Mutex* lock,
                                     fdf_handle_t reply_channel = ZX_HANDLE_INVALID,
                                     sync_completion_t* completion = nullptr);

  // Registers an async read, which on callback will acquire |lock|, read from |read_channel| and
  // signal |completion|.
  static void RegisterAsyncReadSignal(fdf_handle_t read_channel, fdf_dispatcher_t* dispatcher,
                                      fbl::Mutex* lock, sync_completion_t* completion) {
    return RegisterAsyncReadReply(read_channel, dispatcher, lock, ZX_HANDLE_INVALID, completion);
  }

  // Registers an async read, which on callback will signal |entered_callback| and block
  // until |complete_blocking_read| is signaled.
  static void RegisterAsyncReadBlock(fdf_handle_t ch, fdf_dispatcher_t* dispatcher,
                                     libsync::Completion* entered_callback,
                                     libsync::Completion* complete_blocking_read);

  static void WaitUntilIdle(fdf_dispatcher_t* dispatcher) {
    static_cast<driver_runtime::Dispatcher*>(dispatcher)->WaitUntilIdle();
  }

  static constexpr async_loop_config_t MakeConfig() {
    async_loop_config_t config = kAsyncLoopConfigNoAttachToCurrentThread;
    config.irq_support = true;
    return config;
  }

  fdf_handle_t local_ch_;
  fdf_handle_t remote_ch_;

  fdf_handle_t local_ch2_;
  fdf_handle_t remote_ch2_;

  async_loop_config_t config_;
  async::Loop loop_;
  std::vector<fdf_dispatcher_t*> dispatchers_;
  std::vector<std::unique_ptr<DispatcherShutdownObserver>> observers_;
};

void DispatcherTest::SetUp() {
  ASSERT_EQ(ZX_OK, fdf_channel_create(0, &local_ch_, &remote_ch_));
  ASSERT_EQ(ZX_OK, fdf_channel_create(0, &local_ch2_, &remote_ch2_));

  loop_.StartThread();
}

void DispatcherTest::TearDown() {
  if (local_ch_) {
    fdf_handle_close(local_ch_);
  }
  if (remote_ch_) {
    fdf_handle_close(remote_ch_);
  }
  if (local_ch2_) {
    fdf_handle_close(local_ch2_);
  }
  if (remote_ch2_) {
    fdf_handle_close(remote_ch2_);
  }

  loop_.StartThread();  // Make sure an async loop thread is running for dispatcher destruction.

  for (auto* dispatcher : dispatchers_) {
    fdf_dispatcher_shutdown_async(dispatcher);
  }
  for (auto& observer : observers_) {
    ASSERT_OK(observer->WaitUntilShutdown());
  }
  for (auto* dispatcher : dispatchers_) {
    fdf_dispatcher_destroy(dispatcher);
  }

  loop_.Quit();
  loop_.JoinThreads();
}

void DispatcherTest::CreateDispatcher(uint32_t options, std::string_view name,
                                      std::string_view scheduler_role, const void* owner,
                                      fdf_dispatcher_t** out_dispatcher) {
  auto observer = std::make_unique<DispatcherShutdownObserver>();
  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK,
            driver_runtime::Dispatcher::CreateWithLoop(options, name, scheduler_role, owner, &loop_,
                                                       observer->fdf_observer(), &dispatcher));
  *out_dispatcher = static_cast<fdf_dispatcher_t*>(dispatcher);
  dispatchers_.push_back(*out_dispatcher);
  observers_.push_back(std::move(observer));
}

// static
void DispatcherTest::RegisterAsyncReadReply(fdf_handle_t read_channel, fdf_dispatcher_t* dispatcher,
                                            fbl::Mutex* lock, fdf_handle_t reply_channel,
                                            sync_completion_t* completion) {
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      read_channel, 0 /* options */,
      [=](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);

        {
          fbl::AutoLock auto_lock(lock);

          ASSERT_NO_FATAL_FAILURE(AssertRead(channel_read->channel(), nullptr, 0, nullptr, 0));
          if (reply_channel != ZX_HANDLE_INVALID) {
            ASSERT_EQ(ZX_OK, fdf_channel_write(reply_channel, 0, nullptr, nullptr, 0, nullptr, 0));
          }
        }
        if (completion) {
          sync_completion_signal(completion);
        }
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(dispatcher));
  channel_read.release();  // Deleted on callback.
}

// static
void DispatcherTest::RegisterAsyncReadBlock(fdf_handle_t ch, fdf_dispatcher_t* dispatcher,
                                            libsync::Completion* entered_callback,
                                            libsync::Completion* complete_blocking_read) {
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      ch, 0 /* options */,
      [=](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        entered_callback->Signal();
        ASSERT_OK(complete_blocking_read->Wait(zx::time::infinite()));
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(dispatcher));
  channel_read.release();  // Will be deleted on callback.
}

//
// Synchronous dispatcher tests
//

// Tests that a synchronous dispatcher will call directly into the next driver
// if it is not reentrant.
// This creates 2 drivers and writes a message between them.
TEST_F(DispatcherTest, SyncDispatcherDirectCall) {
  const void* local_driver = CreateFakeDriver();
  const void* remote_driver = CreateFakeDriver();

  // We should bypass the async loop, so quit it now to make sure we don't use it.
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", local_driver, &dispatcher));

  sync_completion_t read_completion;
  ASSERT_NO_FATAL_FAILURE(SignalOnChannelReadable(local_ch_, dispatcher, &read_completion));

  {
    driver_context::PushDriver(remote_driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    // As |local_driver| is not in the thread's call stack,
    // this should call directly into local driver's channel_read callback.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
    ASSERT_OK(sync_completion_wait(&read_completion, ZX_TIME_INFINITE));
  }
}

// Tests that a synchronous dispatcher will queue a request on the async loop if it is reentrant.
// This writes and reads a message from the same driver.
TEST_F(DispatcherTest, SyncDispatcherCallOnLoop) {
  const void* driver = CreateFakeDriver();

  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  sync_completion_t read_completion;
  ASSERT_NO_FATAL_FAILURE(SignalOnChannelReadable(local_ch_, dispatcher, &read_completion));

  {
    // Add the same driver to the thread's call stack.
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    // This should queue the callback to run on an async loop thread.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
    // Check that the callback hasn't been called yet, as we shutdown the async loop.
    ASSERT_FALSE(sync_completion_signaled(&read_completion));
    ASSERT_EQ(1, dispatcher->callback_queue_size_slow());
  }

  loop_.StartThread();
  ASSERT_OK(sync_completion_wait(&read_completion, ZX_TIME_INFINITE));
}

// Tests that a synchronous dispatcher only allows one callback to be running at a time.
// We will register a callback that blocks and one that doesn't. We will then send
// 2 requests, and check that the second callback is not run until the first returns.
TEST_F(DispatcherTest, SyncDispatcherDisallowsParallelCallbacks) {
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  // We shouldn't actually block on a dispatcher that doesn't have ALLOW_SYNC_CALLS set,
  // but this is just for synchronizing the test.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadBlock(local_ch_, dispatcher, &entered_callback, &complete_blocking_read));

  sync_completion_t read_completion;
  ASSERT_NO_FATAL_FAILURE(SignalOnChannelReadable(local_ch2_, dispatcher, &read_completion));

  {
    // This should make the callback run on the async loop, as it would be reentrant.
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  // Write another request. This should also be queued on the async loop.
  std::thread t1 = std::thread([&] {
    // Make the call not reentrant.
    driver_context::PushDriver(CreateFakeDriver());
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch2_, 0, nullptr, nullptr, 0, nullptr, 0));
  });

  // The dispatcher should not call the callback while there is an existing callback running,
  // so we should be able to join with the thread immediately.
  t1.join();
  ASSERT_FALSE(sync_completion_signaled(&read_completion));

  // Complete the first callback.
  complete_blocking_read.Signal();

  // The second callback should complete now.
  ASSERT_OK(sync_completion_wait(&read_completion, ZX_TIME_INFINITE));
}

// Tests that a synchronous dispatcher does not schedule parallel callbacks on the async loop.
TEST_F(DispatcherTest, SyncDispatcherDisallowsParallelCallbacksReentrant) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  constexpr uint32_t kNumThreads = 2;
  constexpr uint32_t kNumClients = 12;

  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  struct ReadClient {
    fdf_handle_t channel;
    libsync::Completion entered_callback;
    libsync::Completion complete_blocking_read;
  };

  std::vector<ReadClient> local(kNumClients);
  std::vector<fdf_handle_t> remote(kNumClients);

  for (uint32_t i = 0; i < kNumClients; i++) {
    ASSERT_OK(fdf_channel_create(0, &local[i].channel, &remote[i]));
    ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadBlock(local[i].channel, dispatcher,
                                                   &local[i].entered_callback,
                                                   &local[i].complete_blocking_read));
  }

  for (uint32_t i = 0; i < kNumClients; i++) {
    // Call is considered reentrant and will be queued on the async loop.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote[i], 0, nullptr, nullptr, 0, nullptr, 0));
  }

  for (uint32_t i = 0; i < kNumThreads; i++) {
    loop_.StartThread();
  }

  ASSERT_OK(local[0].entered_callback.Wait(zx::time::infinite()));
  local[0].complete_blocking_read.Signal();

  // Check that we aren't blocking the second thread by posting a task to another
  // dispatcher.
  fdf_dispatcher_t* dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher2));
  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher2);
  ASSERT_NOT_NULL(async_dispatcher);

  sync_completion_t task_completion;
  ASSERT_OK(async::PostTask(async_dispatcher,
                            [&task_completion] { sync_completion_signal(&task_completion); }));
  ASSERT_OK(sync_completion_wait(&task_completion, ZX_TIME_INFINITE));

  // Allow all the read callbacks to complete.
  for (uint32_t i = 1; i < kNumClients; i++) {
    local[i].complete_blocking_read.Signal();
  }

  for (uint32_t i = 0; i < kNumClients; i++) {
    ASSERT_OK(local[i].entered_callback.Wait(zx::time::infinite()));
  }

  WaitUntilIdle(dispatcher);
  WaitUntilIdle(dispatcher2);

  for (uint32_t i = 0; i < kNumClients; i++) {
    fdf_handle_close(local[i].channel);
    fdf_handle_close(remote[i]);
  }
}

//
// Unsynchronized dispatcher tests
//

// Tests that an unsynchronized dispatcher allows multiple callbacks to run at the same time.
// We will send requests from multiple threads and check that the expected number of callbacks
// is running.
TEST_F(DispatcherTest, UnsyncDispatcherAllowsParallelCallbacks) {
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, __func__,
                                           "scheduler_role", driver, &dispatcher));

  constexpr uint32_t kNumClients = 10;

  std::vector<fdf_handle_t> local(kNumClients);
  std::vector<fdf_handle_t> remote(kNumClients);

  for (uint32_t i = 0; i < kNumClients; i++) {
    ASSERT_OK(fdf_channel_create(0, &local[i], &remote[i]));
  }

  fbl::Mutex callback_lock;
  uint32_t num_callbacks = 0;
  sync_completion_t completion;

  for (uint32_t i = 0; i < kNumClients; i++) {
    auto channel_read = std::make_unique<fdf::ChannelRead>(
        local[i], 0 /* options */,
        [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
          {
            fbl::AutoLock lock(&callback_lock);
            num_callbacks++;
            if (num_callbacks == kNumClients) {
              sync_completion_signal(&completion);
            }
          }
          // Wait for all threads to ensure we are correctly supporting parallel callbacks.
          ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
          delete channel_read;
        });
    ASSERT_OK(channel_read->Begin(dispatcher));
    channel_read.release();  // Deleted by the callback.
  }

  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < kNumClients; i++) {
    std::thread client = std::thread(
        [&](fdf_handle_t channel) {
          {
            // Ensure the call is not reentrant.
            driver_context::PushDriver(CreateFakeDriver());
            auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
            ASSERT_EQ(ZX_OK, fdf_channel_write(channel, 0, nullptr, nullptr, 0, nullptr, 0));
          }
        },
        remote[i]);
    threads.push_back(std::move(client));
  }

  for (auto& t : threads) {
    t.join();
  }

  for (uint32_t i = 0; i < kNumClients; i++) {
    fdf_handle_close(local[i]);
    fdf_handle_close(remote[i]);
  }
}

// Tests that an unsynchronized dispatcher allows multiple callbacks to run at the same time
// on the async loop.
TEST_F(DispatcherTest, UnsyncDispatcherAllowsParallelCallbacksReentrant) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  constexpr uint32_t kNumThreads = 3;
  constexpr uint32_t kNumClients = 22;

  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, __func__,
                                           "scheduler_role", driver, &dispatcher));

  std::vector<fdf_handle_t> local(kNumClients);
  std::vector<fdf_handle_t> remote(kNumClients);

  for (uint32_t i = 0; i < kNumClients; i++) {
    ASSERT_OK(fdf_channel_create(0, &local[i], &remote[i]));
  }

  fbl::Mutex callback_lock;
  uint32_t num_callbacks = 0;
  sync_completion_t all_threads_running;

  for (uint32_t i = 0; i < kNumClients; i++) {
    auto channel_read = std::make_unique<fdf::ChannelRead>(
        local[i], 0 /* options */,
        [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
          {
            fbl::AutoLock lock(&callback_lock);
            num_callbacks++;
            if (num_callbacks == kNumThreads) {
              sync_completion_signal(&all_threads_running);
            }
          }
          // Wait for all threads to ensure we are correctly supporting parallel callbacks.
          ASSERT_OK(sync_completion_wait(&all_threads_running, ZX_TIME_INFINITE));
          delete channel_read;
        });
    ASSERT_OK(channel_read->Begin(dispatcher));
    channel_read.release();  // Deleted by the callback.
  }

  for (uint32_t i = 0; i < kNumClients; i++) {
    // Call is considered reentrant and will be queued on the async loop.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote[i], 0, nullptr, nullptr, 0, nullptr, 0));
  }

  for (uint32_t i = 0; i < kNumThreads; i++) {
    loop_.StartThread();
  }

  ASSERT_OK(sync_completion_wait(&all_threads_running, ZX_TIME_INFINITE));
  WaitUntilIdle(dispatcher);
  ASSERT_EQ(num_callbacks, kNumClients);

  for (uint32_t i = 0; i < kNumClients; i++) {
    fdf_handle_close(local[i]);
    fdf_handle_close(remote[i]);
  }
}

//
// Blocking dispatcher tests
//

// Tests that a blocking dispatcher will not directly call into the next driver.
TEST_F(DispatcherTest, AllowSyncCallsDoesNotDirectlyCall) {
  const void* blocking_driver = CreateFakeDriver();
  fdf_dispatcher_t* blocking_dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", blocking_driver,
                                           &blocking_dispatcher));

  // Queue a blocking request.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadBlock(remote_ch_, blocking_dispatcher, &entered_callback,
                                                 &complete_blocking_read));

  {
    // Simulate a driver writing a message to the driver with the blocking dispatcher.
    driver_context::PushDriver(CreateFakeDriver());
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    // This is a non reentrant call, but we still shouldn't call into the driver directly.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  // Signal and wait for the blocking read handler to return.
  complete_blocking_read.Signal();

  WaitUntilIdle(blocking_dispatcher);
}

// Tests that a blocking dispatcher does not block the global async loop shared between
// all dispatchers in a process.
// We will register a blocking callback, and ensure we can receive other callbacks
// at the same time.
TEST_F(DispatcherTest, AllowSyncCallsDoesNotBlockGlobalLoop) {
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  const void* blocking_driver = CreateFakeDriver();
  fdf_dispatcher_t* blocking_dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", blocking_driver,
                                           &blocking_dispatcher));

  fdf_handle_t blocking_local_ch, blocking_remote_ch;
  ASSERT_EQ(ZX_OK, fdf_channel_create(0, &blocking_local_ch, &blocking_remote_ch));

  // Queue a blocking read.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadBlock(blocking_remote_ch, blocking_dispatcher,
                                                 &entered_callback, &complete_blocking_read));

  // Write a message for the blocking dispatcher.
  {
    driver_context::PushDriver(blocking_driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    ASSERT_EQ(ZX_OK, fdf_channel_write(blocking_local_ch, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  sync_completion_t read_completion;
  ASSERT_NO_FATAL_FAILURE(SignalOnChannelReadable(remote_ch_, dispatcher, &read_completion));

  {
    // Write a message which will be read on the non-blocking dispatcher.
    // Make the call reentrant so that the request is queued for the async loop.
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(sync_completion_wait(&read_completion, ZX_TIME_INFINITE));
  ASSERT_NO_FATAL_FAILURE(AssertRead(remote_ch_, nullptr, 0, nullptr, 0));

  // Signal and wait for the blocking read handler to return.
  complete_blocking_read.Signal();

  WaitUntilIdle(dispatcher);
  WaitUntilIdle(blocking_dispatcher);

  fdf_handle_close(blocking_local_ch);
  fdf_handle_close(blocking_remote_ch);
}

//
// Additional re-entrancy tests
//

// Tests sending a request to another driver and receiving a reply across a single channel.
TEST_F(DispatcherTest, ReentrancySimpleSendAndReply) {
  // Create a dispatcher for each end of the channel.
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, "", "scheduler_role", driver, &dispatcher));

  const void* driver2 = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, "", "scheduler_role", driver2, &dispatcher2));

  // Lock that is acquired by the first driver whenever it writes or reads from |local_ch_|.
  // We shouldn't need to lock in a synchronous dispatcher, but this is just for testing
  // that the dispatcher handles reentrant calls. If the dispatcher attempts to call
  // reentrantly, this test will deadlock.
  fbl::Mutex driver_lock;
  fbl::Mutex driver2_lock;
  sync_completion_t completion;

  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadSignal(local_ch_, dispatcher, &driver_lock, &completion));
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadReply(remote_ch_, dispatcher2, &driver2_lock, remote_ch_));

  {
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    fbl::AutoLock lock(&driver_lock);
    // This should call directly into the next driver. When the driver writes its reply,
    // the dispatcher should detect that it is reentrant and queue it to be run on the
    // async loop. This will allow |fdf_channel_write| to return and |driver_lock| will
    // be released.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

// Tests sending a request to another driver, who sends a request back into the original driver
// on a different channel.
TEST_F(DispatcherTest, ReentrancyMultipleDriversAndDispatchers) {
  // Driver will own |local_ch_| and |local_ch2_|.
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  // Driver2 will own |remote_ch_| and |remote_ch2_|.
  const void* driver2 = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver2, &dispatcher2));

  // Lock that is acquired by the driver whenever it writes or reads from its channels.
  // We shouldn't need to lock in a synchronous dispatcher, but this is just for testing
  // that the dispatcher handles reentrant calls. If the dispatcher attempts to call
  // reentrantly, this test will deadlock.
  fbl::Mutex driver_lock;
  fbl::Mutex driver2_lock;
  sync_completion_t completion;

  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadSignal(local_ch2_, dispatcher, &driver_lock, &completion));
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadReply(remote_ch_, dispatcher2, &driver2_lock, remote_ch2_));

  {
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    fbl::AutoLock lock(&driver_lock);
    // This should call directly into the next driver. When the driver writes its reply,
    // the dispatcher should detect that it is reentrant and queue it to be run on the
    // async loop. This will allow |fdf_channel_write| to return and |driver_lock| will
    // be released.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

// Tests a driver sending a request to another channel it owns.
TEST_F(DispatcherTest, ReentrancyOneDriverMultipleChannels) {
  const void* driver = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

  // Lock that is acquired by the driver whenever it writes or reads from its channels.
  // We shouldn't need to lock in a synchronous dispatcher, but this is just for testing
  // that the dispatcher handles reentrant calls. If the dispatcher attempts to call
  // reentrantly, this test will deadlock.
  fbl::Mutex driver_lock;
  sync_completion_t completion;

  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadSignal(local_ch2_, dispatcher, &driver_lock, &completion));
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadReply(remote_ch_, dispatcher, &driver_lock, remote_ch2_));

  {
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    fbl::AutoLock lock(&driver_lock);
    // Every call callback in this driver will be reentrant and should be run on the async loop.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

// Tests forwarding a request across many drivers, before calling back into the original driver.
TEST_F(DispatcherTest, ReentrancyManyDrivers) {
  constexpr uint32_t kNumDrivers = 30;

  // Each driver i uses ch_to_prev[i] and ch_to_next[i] to communicate with the driver before and
  // after it, except ch_to_prev[0] and ch_to_next[kNumDrivers-1].
  std::vector<fdf_handle_t> ch_to_prev(kNumDrivers);
  std::vector<fdf_handle_t> ch_to_next(kNumDrivers);

  // Lock that is acquired by the driver whenever it writes or reads from its channels.
  // We shouldn't need to lock in a synchronous dispatcher, but this is just for testing
  // that the dispatcher handles reentrant calls. If the dispatcher attempts to call
  // reentrantly, this test will deadlock.
  std::vector<fbl::Mutex> driver_locks(kNumDrivers);

  for (uint32_t i = 0; i < kNumDrivers; i++) {
    const void* driver = CreateFakeDriver();
    fdf_dispatcher_t* dispatcher;
    ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver, &dispatcher));

    // Get the next driver's channel which is connected to the current driver's channel.
    // The last driver will be connected to the first driver.
    fdf_handle_t* peer = (i == kNumDrivers - 1) ? &ch_to_prev[0] : &ch_to_prev[i + 1];
    ASSERT_OK(fdf_channel_create(0, &ch_to_next[i], peer));
  }

  // Signal once the first driver is called into.
  sync_completion_t completion;
  ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadSignal(ch_to_prev[0],
                                                  static_cast<fdf_dispatcher_t*>(dispatchers_[0]),
                                                  &driver_locks[0], &completion));

  // Each driver will wait for a callback, then write a message to the next driver.
  for (uint32_t i = 1; i < kNumDrivers; i++) {
    ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadReply(ch_to_prev[i],
                                                   static_cast<fdf_dispatcher_t*>(dispatchers_[i]),
                                                   &driver_locks[i], ch_to_next[i]));
  }

  {
    driver_context::PushDriver(dispatchers_[0]->owner());
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    fbl::AutoLock lock(&driver_locks[0]);
    // Write from the first driver.
    // This should call directly into the next |kNumDrivers - 1| drivers.
    ASSERT_EQ(ZX_OK, fdf_channel_write(ch_to_next[0], 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
  for (uint32_t i = 0; i < kNumDrivers; i++) {
    WaitUntilIdle(dispatchers_[i]);
  }
  for (uint32_t i = 0; i < kNumDrivers; i++) {
    fdf_handle_close(ch_to_prev[i]);
    fdf_handle_close(ch_to_next[i]);
  }
}

// Tests writing a request from an unknown driver context.
TEST_F(DispatcherTest, EmptyCallStack) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  sync_completion_t read_completion;
  ASSERT_NO_FATAL_FAILURE(SignalOnChannelReadable(local_ch_, dispatcher, &read_completion));

  {
    // Call without any recorded call stack.
    // This should queue the callback to run on an async loop thread.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
    ASSERT_EQ(1, dispatcher->callback_queue_size_slow());
    ASSERT_FALSE(sync_completion_signaled(&read_completion));
  }

  loop_.StartThread();
  ASSERT_OK(sync_completion_wait(&read_completion, ZX_TIME_INFINITE));
}

//
// Shutdown() tests
//

// Tests shutting down a synchronized dispatcher that has a pending channel read
// that does not have a corresponding channel write.
TEST_F(DispatcherTest, SyncDispatcherShutdownBeforeWrite) {
  libsync::Completion read_complete;
  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK,
            driver_runtime::Dispatcher::CreateWithLoop(0, "", scheduler_role, driver, &loop_,
                                                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));

  // Registered, but not yet ready to run.
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_EQ(status, ZX_ERR_CANCELED);
        read_complete.Signal();
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(fdf_dispatcher.get()));
  channel_read.release();

  fdf_dispatcher.ShutdownAsync();

  ASSERT_OK(read_complete.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
}

// Tests shutting down a synchronized dispatcher that has a pending async wait
// that hasn't been signaled yet.
TEST_F(DispatcherTest, SyncDispatcherShutdownBeforeSignaled) {
  libsync::Completion wait_complete;
  DispatcherShutdownObserver observer;

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK,
            driver_runtime::Dispatcher::CreateWithLoop(0, "", scheduler_role, driver, &loop_,
                                                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));

  // Registered, but not yet signaled.
  async_dispatcher_t* async_dispatcher = dispatcher->GetAsyncDispatcher();
  ASSERT_NOT_NULL(async_dispatcher);

  ASSERT_OK(wait.Begin(async_dispatcher, [&wait_complete, event = std::move(event)](
                                             async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ASSERT_STATUS(status, ZX_ERR_CANCELED);
    wait_complete.Signal();
  }));

  // Shutdown the dispatcher, which should schedule cancellation of the channel read.
  dispatcher->ShutdownAsync();

  ASSERT_OK(wait_complete.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
}

// Tests shutting down an unsynchronized dispatcher.
TEST_F(DispatcherTest, UnsyncDispatcherShutdown) {
  libsync::Completion complete_task;
  libsync::Completion read_complete;

  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  const std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));
  libsync::Completion task_started;
  // Post a task that will block until we signal it.
  ASSERT_OK(async::PostTask(fdf_dispatcher.async_dispatcher(), [&] {
    task_started.Signal();
    ASSERT_OK(complete_task.Wait(zx::time::infinite()));
  }));
  // Ensure the task has been started.
  ASSERT_OK(task_started.Wait(zx::time::infinite()));

  // Register a channel read, which should not be queued until the
  // write happens.
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_EQ(status, ZX_ERR_CANCELED);
        read_complete.Signal();
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(fdf_dispatcher.get()));
  channel_read.release();

  {
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    // This should be considered reentrant and be queued on the async loop.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  fdf_dispatcher.ShutdownAsync();

  // The cancellation should not happen until the task completes.
  ASSERT_FALSE(read_complete.signaled());
  complete_task.Signal();
  ASSERT_OK(read_complete.Wait(zx::time::infinite()));

  ASSERT_OK(observer.WaitUntilShutdown());
}

// Tests shutting down an unsynchronized dispatcher that has a pending channel read
// that does not have a corresponding channel write.
TEST_F(DispatcherTest, UnsyncDispatcherShutdonwBeforeWrite) {
  libsync::Completion read_complete;
  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));

  // Registered, but not yet ready to run.
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_EQ(status, ZX_ERR_CANCELED);
        read_complete.Signal();
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(fdf_dispatcher.get()));
  channel_read.release();

  fdf_dispatcher.ShutdownAsync();

  ASSERT_OK(read_complete.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
}

// Tests shutting down a unsynchronized dispatcher that has a pending async wait
// that hasn't been signaled yet.
TEST_F(DispatcherTest, UnsyncDispatcherShutdownBeforeSignaled) {
  libsync::Completion wait_complete;
  DispatcherShutdownObserver observer;

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));

  // Registered, but not yet signaled.
  async_dispatcher_t* async_dispatcher = dispatcher->GetAsyncDispatcher();
  ASSERT_NOT_NULL(async_dispatcher);

  ASSERT_OK(wait.Begin(async_dispatcher, [&wait_complete, event = std::move(event)](
                                             async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ASSERT_STATUS(status, ZX_ERR_CANCELED);
    wait_complete.Signal();
  }));

  // Shutdown the dispatcher, which should schedule cancellation of the channel read.
  dispatcher->ShutdownAsync();

  ASSERT_OK(wait_complete.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
}

// Tests shutting down an unsynchronized dispatcher from a channel read callback running
// on the async loop.
TEST_F(DispatcherTest, ShutdownDispatcherInAsyncLoopCallback) {
  const void* driver = CreateFakeDriver();
  std::string_view scheduler_role = "scheduler_role";

  DispatcherShutdownObserver dispatcher_observer;

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       dispatcher_observer.fdf_observer(), &dispatcher));

  libsync::Completion completion;
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0 /* options */,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        fdf_dispatcher_shutdown_async(dispatcher);
        completion.Signal();
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(static_cast<fdf_dispatcher_t*>(dispatcher)));
  channel_read.release();  // Deleted on callback.

  {
    // Make the write reentrant so it is scheduled to run on the async loop.
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  ASSERT_OK(completion.Wait(zx::time::infinite()));

  ASSERT_OK(dispatcher_observer.WaitUntilShutdown());
  dispatcher->Destroy();
}

// Tests that attempting to shut down a dispatcher twice from callbacks does not crash.
TEST_F(DispatcherTest, ShutdownDispatcherFromTwoCallbacks) {
  // Stop the async loop, so that the channel reads don't get scheduled
  // until after we shut down the dispatcher.
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  DispatcherShutdownObserver observer;
  const void* driver = CreateFakeDriver();
  std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  libsync::Completion completion;
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0 /* options */,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        fdf_dispatcher_shutdown_async(dispatcher);
        completion.Signal();
      });
  ASSERT_OK(channel_read->Begin(static_cast<fdf_dispatcher_t*>(dispatcher)));

  libsync::Completion completion2;
  auto channel_read2 = std::make_unique<fdf::ChannelRead>(
      remote_ch2_, 0 /* options */,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        fdf_dispatcher_shutdown_async(dispatcher);
        completion2.Signal();
      });
  ASSERT_OK(channel_read2->Begin(static_cast<fdf_dispatcher_t*>(dispatcher)));

  {
    // Make the writes reentrant so they are scheduled to run on the async loop.
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch2_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  loop_.StartThread();

  ASSERT_OK(completion.Wait(zx::time::infinite()));
  ASSERT_OK(completion2.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
  dispatcher->Destroy();
}

// Tests that queueing a ChannelRead while the dispatcher is shutting down fails.
TEST_F(DispatcherTest, ShutdownDispatcherQueueChannelReadCallback) {
  // Stop the async loop, so that the channel read doesn't get scheduled
  // until after we shut down the dispatcher.
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  libsync::Completion read_complete;
  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  fdf::Dispatcher fdf_dispatcher(static_cast<fdf_dispatcher_t*>(dispatcher));

  auto channel_read = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_EQ(status, ZX_ERR_CANCELED);
        // We should not be able to queue the read again.
        ASSERT_EQ(channel_read->Begin(dispatcher), ZX_ERR_UNAVAILABLE);
        read_complete.Signal();
        delete channel_read;
      });
  ASSERT_OK(channel_read->Begin(fdf_dispatcher.get()));
  channel_read.release();  // Deleted on callback.

  {
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    // This should be considered reentrant and be queued on the async loop.
    ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }

  fdf_dispatcher.ShutdownAsync();

  loop_.StartThread();

  ASSERT_OK(read_complete.Wait(zx::time::infinite()));
  ASSERT_OK(observer.WaitUntilShutdown());
}

TEST_F(DispatcherTest, ShutdownCallbackIsNotReentrant) {
  fbl::Mutex driver_lock;

  libsync::Completion completion;
  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) {
    { fbl::AutoLock lock(&driver_lock); }
    completion.Signal();
  };

  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(dispatcher.is_error());

  {
    fbl::AutoLock lock(&driver_lock);
    dispatcher->ShutdownAsync();
  }

  ASSERT_OK(completion.Wait(zx::time::infinite()));
}

TEST_F(DispatcherTest, ChannelPeerWriteDuringShutdown) {
  constexpr uint32_t kNumChannelPairs = 1000;

  libsync::Completion shutdown;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) { shutdown.Signal(); };

  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", shutdown_handler);
  ASSERT_FALSE(dispatcher.is_error());

  // Create a bunch of channels, and register one end with the dispatcher to wait for
  // available channel reads.
  fdf::Channel local[kNumChannelPairs];
  fdf::Channel remote[kNumChannelPairs];
  for (uint32_t i = 0; i < kNumChannelPairs; i++) {
    auto channels_status = fdf::ChannelPair::Create(0);
    ASSERT_OK(channels_status.status_value());
    local[i] = std::move(channels_status->end0);
    remote[i] = std::move(channels_status->end1);

    auto channel_read = std::make_unique<fdf::ChannelRead>(
        local[i].get(), 0 /* options */,
        [=](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
          ASSERT_EQ(ZX_ERR_CANCELED, status);
          delete channel_read;
        });
    ASSERT_OK(channel_read->Begin(dispatcher->get()));
    channel_read.release();  // Will be deleted on callback.
  }

  dispatcher->ShutdownAsync();

  for (uint32_t i = 0; i < kNumChannelPairs; i++) {
    // This will write the packet to the peer channel and attempt to call |QueueRegisteredCallback|
    // on the dispatcher.
    fdf::Arena arena(nullptr);
    ASSERT_EQ(ZX_OK,
              remote[i].Write(0, arena, nullptr, 0, cpp20::span<zx_handle_t>()).status_value());
  }
  ASSERT_OK(shutdown.Wait());
}

//
// async_dispatcher_t
//

// Tests that we can use the fdf_dispatcher_t as an async_dispatcher_t.
TEST_F(DispatcherTest, AsyncDispatcher) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  sync_completion_t completion;
  ASSERT_OK(
      async::PostTask(async_dispatcher, [&completion] { sync_completion_signal(&completion); }));
  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

TEST_F(DispatcherTest, DelayedTask) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  sync_completion_t completion;
  ASSERT_OK(async::PostTaskForTime(
      async_dispatcher, [&completion] { sync_completion_signal(&completion); },
      zx::deadline_after(zx::msec(10))));
  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

TEST_F(DispatcherTest, TasksDoNotCallDirectly) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  libsync::Completion completion;
  ASSERT_OK(async::PostTask(async_dispatcher, [&completion] { completion.Signal(); }));
  ASSERT_FALSE(completion.signaled());

  loop_.StartThread();
  completion.Wait();
}

TEST_F(DispatcherTest, DowncastAsyncDispatcher) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  ASSERT_EQ(fdf_dispatcher_downcast_async_dispatcher(async_dispatcher), dispatcher);
}

TEST_F(DispatcherTest, CancelTask) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  async::TaskClosure task;
  task.set_handler([] { ASSERT_FALSE(true); });
  ASSERT_OK(task.Post(async_dispatcher));

  ASSERT_OK(task.Cancel());  // Task should not be running yet.
}

TEST_F(DispatcherTest, CancelDelayedTask) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  async::TaskClosure task;
  task.set_handler([] { ASSERT_FALSE(true); });
  ASSERT_OK(task.PostForTime(async_dispatcher, zx::deadline_after(zx::sec(100))));

  ASSERT_OK(task.Cancel());  // Task should not be running yet.
}

TEST_F(DispatcherTest, CancelTaskNotYetPosted) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  async::TaskClosure task;
  task.set_handler([] { ASSERT_FALSE(true); });

  ASSERT_EQ(task.Cancel(), ZX_ERR_NOT_FOUND);  // Task should not be running yet.
}

TEST_F(DispatcherTest, CancelTaskAlreadyRunning) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  async::TaskClosure task;
  libsync::Completion completion;
  task.set_handler([&] {
    ASSERT_EQ(task.Cancel(), ZX_ERR_NOT_FOUND);  // Task is already running.
    completion.Signal();
  });
  ASSERT_OK(task.Post(async_dispatcher));
  ASSERT_OK(completion.Wait(zx::time::infinite()));
}

TEST_F(DispatcherTest, AsyncWaitOnce) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  sync_completion_t completion;
  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(wait.Begin(async_dispatcher, [&completion, &async_dispatcher](
                                             async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ASSERT_EQ(async_dispatcher, dispatcher);
    ASSERT_OK(status);
    sync_completion_signal(&completion);
  }));
  ASSERT_OK(event.signal(0, ZX_USER_SIGNAL_0));
  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

TEST_F(DispatcherTest, CancelWait) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(wait.Begin(async_dispatcher,
                       [](async_dispatcher_t* dispatcher, async::WaitOnce* wait, zx_status_t status,
                          const zx_packet_signal_t* signal) { ZX_ASSERT(false); }));
  ASSERT_OK(wait.Cancel());
}

TEST_F(DispatcherTest, CancelWaitFromWithinCanceledWait) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", [](fdf_dispatcher_t* dispatcher) {});
  ASSERT_FALSE(dispatcher.is_error());

  async_dispatcher_t* async_dispatcher = dispatcher->async_dispatcher();
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
  async::WaitOnce wait2(event.get(), ZX_USER_SIGNAL_0);

  ASSERT_OK(wait.Begin(async_dispatcher, [&](async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ASSERT_EQ(status, ZX_ERR_CANCELED);
    wait2.Cancel();
  }));

  // We will cancel this wait from wait's handler, so we never expect it to complete.
  ASSERT_OK(wait2.Begin(async_dispatcher, [](async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ZX_ASSERT(false);
  }));

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));

  ASSERT_OK(driver_shutdown_completion.Wait());
}

TEST_F(DispatcherTest, CancelWaitRaceCondition) {
  // Regression test for fxbug.dev/109988, a tricky race condition when cancelling a wait.
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  // Start a second thread as this race condition depends on the dispatcher being multi-threaded.
  loop_.StartThread();

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  // Run the body a bunch of times to increase the chances of hitting the race condition.
  for (int i = 0; i < 100; i++) {
    libsync::Completion completion;
    async::PostTask(async_dispatcher, [&] {
      async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
      ASSERT_OK(
          wait.Begin(async_dispatcher, [](async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                          zx_status_t status, const zx_packet_signal_t* signal) {
            // Since we are going to cancel the wait, the callback should not be invoked.
            ZX_ASSERT(false);
          }));

      // Signal the event, which queues up the wait callback to be invoked.
      event.signal(0, ZX_USER_SIGNAL_0);

      // Cancel should always succeed. This is because the dispatcher is synchronized and should
      // appear to the user as if it is single-threaded. Since the wait is cancelled in the same
      // block as the event is signaled, the code never yields to the dispatcher and it never has a
      // chance to receive the event signal and invoke the callback. However, in our multi-threaded
      // dispatcher, it *is* possible that another thread will receive the signal and queue up the
      // callback to be invoked, so we need to handle this case without failing.
      //
      // In practice, when this test fails it's usually because it hits a debug assert in the
      // underlying async implementation in zircon/system/ulib/async/wait.cc, rather than failing
      // this assert.
      ASSERT_OK(wait.Cancel());
      completion.Signal();
    });

    // Make sure all the async tasks finish before exiting the test.
    ASSERT_OK(completion.Wait());
  }
}

TEST_F(DispatcherTest, GetCurrentDispatcherInWait) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  sync_completion_t completion;
  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(wait.Begin(
      async_dispatcher,
      [&completion, &dispatcher](async_dispatcher_t* async_dispatcher, async::WaitOnce* wait,
                                 zx_status_t status, const zx_packet_signal_t* signal) {
        ASSERT_EQ(fdf_dispatcher_get_current_dispatcher(), dispatcher);
        ASSERT_OK(status);
        sync_completion_signal(&completion);
      }));
  ASSERT_OK(event.signal(0, ZX_USER_SIGNAL_0));
  ASSERT_OK(sync_completion_wait(&completion, ZX_TIME_INFINITE));
}

TEST_F(DispatcherTest, WaitSynchronized) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  // Create a second dispatcher which allows sync calls to force multiple threads.
  fdf_dispatcher_t* unused_dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", CreateFakeDriver(),
                                           &unused_dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event1, event2;
  ASSERT_OK(zx::event::create(0, &event1));
  ASSERT_OK(zx::event::create(0, &event2));

  fbl::Mutex lock1, lock2;
  sync_completion_t completion1, completion2;

  async::WaitOnce wait1(event1.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(wait1.Begin(
      async_dispatcher,
      [&completion1, &lock1, &lock2](async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                     zx_status_t status, const zx_packet_signal_t* signal) {
        // Take note of the order the locks are acquired here.
        {
          fbl::AutoLock al1(&lock1);
          fbl::AutoLock al2(&lock2);
        }
        sync_completion_signal(&completion1);
      }));
  async::WaitOnce wait2(event1.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(wait2.Begin(
      async_dispatcher,
      [&completion2, &lock1, &lock2](async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                     zx_status_t status, const zx_packet_signal_t* signal) {
        // Locks acquired here in opposite order. If these calls are ever made in parallel, then we
        // run into a deadlock. The test should hang and eventually timeout in that case.
        {
          fbl::AutoLock al2(&lock2);
          fbl::AutoLock al1(&lock1);
        }
        sync_completion_signal(&completion2);
      }));

  // While the order of these signals are serialized, the order in which the signals are observed by
  // the waits is not. As a result either of the above waits may trigger first.
  ASSERT_OK(event1.signal(0, ZX_USER_SIGNAL_0));
  ASSERT_OK(event2.signal(0, ZX_USER_SIGNAL_0));
  // The order of observing these completions does not matter.
  ASSERT_OK(sync_completion_wait(&completion2, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&completion1, ZX_TIME_INFINITE));
}

// Tests an irq can be bound and multiple callbacks received.
TEST_F(DispatcherTest, Irq) {
  static constexpr uint32_t kNumCallbacks = 10;

  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  libsync::Completion irq_signal;
  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) {
                   irq_object.ack();
                   ASSERT_EQ(irq_arg, &irq);
                   ASSERT_EQ(ZX_OK, status);
                   irq_signal.Signal();
                 });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));
  ASSERT_EQ(ZX_ERR_ALREADY_EXISTS, irq.Begin(dispatcher));

  for (uint32_t i = 0; i < kNumCallbacks; i++) {
    irq_object.trigger(0, zx::time());
    ASSERT_OK(irq_signal.Wait());
    irq_signal.Reset();
  }

  // Must unbind irq from dispatcher thread.
  libsync::Completion unbind_complete;
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    ASSERT_OK(irq.Cancel());
    ASSERT_EQ(ZX_ERR_NOT_FOUND, irq.Cancel());
    unbind_complete.Signal();
  }));
  ASSERT_OK(unbind_complete.Wait());
}

// Tests that the client will stop receiving callbacks after unbinding the irq.
TEST_F(DispatcherTest, UnbindIrq) {
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) { ASSERT_FALSE(true); });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  // Must unbind irq from dispatcher thread.
  libsync::Completion unbind_complete;
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    ASSERT_OK(irq.Cancel());
    unbind_complete.Signal();
  }));
  ASSERT_OK(unbind_complete.Wait());

  // The irq has been unbound, so this should not call the handler.
  irq_object.trigger(0, zx::time());
}

// Tests that we get cancellation callbacks for irqs that are still bound when shutting down.
TEST_F(DispatcherTest, IrqCancelOnShutdown) {
  libsync::Completion completion;
  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };

  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto fdf_dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(fdf_dispatcher.is_error());

  async_dispatcher_t* dispatcher = fdf_dispatcher->async_dispatcher();
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  libsync::Completion irq_completion;
  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) {
                   ASSERT_EQ(ZX_ERR_CANCELED, status);
                   irq_completion.Signal();
                 });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  // This should unbind the irq and call the handler with ZX_ERR_CANCELED.
  fdf_dispatcher->ShutdownAsync();
  ASSERT_OK(irq_completion.Wait());
  ASSERT_OK(completion.Wait(zx::time::infinite()));
}

// Tests that we get one cancellation callback per irq that is still bound when shutting down.
TEST_F(DispatcherTest, IrqCancelOnShutdownCallbackOnlyOnce) {
  auto shutdown_observer = std::make_unique<DispatcherShutdownObserver>();
  driver_runtime::Dispatcher* runtime_dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       0, __func__, "", CreateFakeDriver(), &loop_,
                       shutdown_observer->fdf_observer(), &runtime_dispatcher));
  fdf_dispatcher_t* fdf_dispatcher = static_cast<fdf_dispatcher_t*>(runtime_dispatcher);

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  // Create a second dispatcher which allows sync calls to force multiple threads.
  fdf_dispatcher_t* fdf_dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", CreateFakeDriver(), &fdf_dispatcher2));

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  libsync::Completion irq_completion;
  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) {
                   ASSERT_FALSE(irq_completion.signaled());  // Make sure it is only called once.
                   ASSERT_EQ(ZX_ERR_CANCELED, status);
                   irq_completion.Signal();
                 });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  // Block the sync dispatcher thread with a task.
  libsync::Completion complete_task;
  ASSERT_OK(async::PostTask(dispatcher, [&] { ASSERT_OK(complete_task.Wait()); }));

  // Trigger the irq to queue a callback request.
  irq_object.trigger(0, zx::time());

  // Make sure the callback request has already been queued by the second global dispatcher thread,
  // by queueing a task after the trigger and waiting for the task's completion.
  libsync::Completion task_complete;
  ASSERT_OK(async::PostTask(fdf_dispatcher_get_async_dispatcher(fdf_dispatcher2),
                            [&] { task_complete.Signal(); }));
  ASSERT_OK(task_complete.Wait());

  // This should remove the in-flight irq, unbind the irq and call the handler with ZX_ERR_CANCELED.
  fdf_dispatcher_shutdown_async(fdf_dispatcher);

  // We can now unblock the first dispatcher.
  complete_task.Signal();

  ASSERT_OK(shutdown_observer->WaitUntilShutdown());
  ASSERT_OK(irq_completion.Wait());
  fdf_dispatcher_destroy(fdf_dispatcher);
}

// Tests that an irq can be unbound after a dispatcher begins shutting down.
TEST_F(DispatcherTest, UnbindIrqAfterDispatcherShutdown) {
  libsync::Completion completion;
  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };

  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto fdf_dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(fdf_dispatcher.is_error());

  async_dispatcher_t* dispatcher = fdf_dispatcher->async_dispatcher();
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) { ASSERT_TRUE(false); });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  ASSERT_OK(async::PostTask(dispatcher, [&] {
    fdf_dispatcher->ShutdownAsync();
    ASSERT_OK(irq.Cancel());
  }));

  ASSERT_OK(completion.Wait(zx::time::infinite()));
}

// Tests that when using a SYNCHRONIZED dispatcher, irqs are not delivered in parallel.
TEST_F(DispatcherTest, IrqSynchronized) {
  // Create a dispatcher that we will bind 2 irqs to.
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  // Create a second dispatcher which allows sync calls to force multiple threads.
  fdf_dispatcher_t* fdf_dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", CreateFakeDriver(), &fdf_dispatcher2));

  zx::interrupt irq_object1;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object1));
  zx::interrupt irq_object2;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object2));

  // We will bind 2 irqs to one dispatcher, and trigger them both. The irq handlers will block
  // until a task posted to another dispatcher completes. If the irqs callbacks happen
  // in parallel, the task will not be able to run, and the test will hang.
  libsync::Completion task_completion;
  libsync::Completion irq_completion1, irq_completion2;

  async::Irq irq1(irq_object1.get(), 0,
                  [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                      const zx_packet_interrupt_t* interrupt) {
                    ASSERT_OK(task_completion.Wait());
                    irq_object1.ack();
                    ASSERT_EQ(irq_arg, &irq1);
                    ASSERT_EQ(ZX_OK, status);
                    irq_completion1.Signal();
                  });
  ASSERT_EQ(ZX_OK, irq1.Begin(dispatcher));

  async::Irq irq2(irq_object2.get(), 0,
                  [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                      const zx_packet_interrupt_t* interrupt) {
                    ASSERT_OK(task_completion.Wait());
                    irq_object2.ack();
                    ASSERT_EQ(irq_arg, &irq2);
                    ASSERT_EQ(ZX_OK, status);
                    irq_completion2.Signal();
                  });
  ASSERT_EQ(ZX_OK, irq2.Begin(dispatcher));

  // While the order of these triggers are serialized, the order in which the triggers are observed
  // by the async_irqs is not. As a result either of the above async_irqs may trigger first. If the
  // irqs are not synchronized, both irq handlers will run and block.
  irq_object1.trigger(0, zx::time());
  irq_object2.trigger(0, zx::time());

  // Unblock the irq handler.
  ASSERT_OK(async::PostTask(fdf_dispatcher_get_async_dispatcher(fdf_dispatcher2),
                            [&] { task_completion.Signal(); }));
  ASSERT_OK(task_completion.Wait());

  // The order of observing these completions does not matter.
  ASSERT_OK(irq_completion2.Wait());
  ASSERT_OK(irq_completion1.Wait());

  // Must unbind irqs from dispatcher thread.
  libsync::Completion unbind_complete;
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    ASSERT_OK(irq1.Cancel());
    ASSERT_OK(irq2.Cancel());
    unbind_complete.Signal();
  }));
  ASSERT_OK(unbind_complete.Wait());
}

TEST_F(DispatcherTest, UnbindIrqRemovesPacketFromPort) {
  libsync::Completion completion;
  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };

  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto fdf_dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(fdf_dispatcher.is_error());

  async_dispatcher_t* dispatcher = fdf_dispatcher->async_dispatcher();
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) { ASSERT_TRUE(false); });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  libsync::Completion task_complete;
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    // The irq handler should not be called yet since the dispatcher thread is blocked.
    irq_object.trigger(0, zx::time());
    // This should remove the pending irq packet from the port.
    ASSERT_OK(irq.Cancel());
    task_complete.Signal();
  }));
  ASSERT_OK(task_complete.Wait());

  fdf_dispatcher->ShutdownAsync();
  ASSERT_OK(completion.Wait(zx::time::infinite()));
}

TEST_F(DispatcherTest, UnbindIrqRemovesQueuedIrqs) {
  // Create a dispatcher that we will bind 2 irqs to.
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  // Create a second dispatcher which allows sync calls to force multiple threads.
  fdf_dispatcher_t* fdf_dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                           "scheduler_role", CreateFakeDriver(), &fdf_dispatcher2));

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) { ASSERT_FALSE(true); });
  ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));

  // Block the dispatcher thread.
  libsync::Completion task_started;
  libsync::Completion complete_task;
  libsync::Completion task_complete;
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    task_started.Signal();
    // We will cancel the irq once the test has confirmed that the irq |OnSignal| has happened.
    ASSERT_OK(complete_task.Wait());
    ASSERT_OK(irq.Cancel());
    task_complete.Signal();
  }));
  ASSERT_OK(task_started.Wait());

  irq_object.trigger(0, zx::time());

  // Make sure the irq |OnSignal| has happened on the other |process_shared_dispatcher| thread.
  // Since there are only 2 threads, and 1 is blocked by the task, the other must have already
  // processed the irq.
  libsync::Completion task2_completion;
  ASSERT_OK(async::PostTask(fdf_dispatcher_get_async_dispatcher(fdf_dispatcher2),
                            [&] { task2_completion.Signal(); }));
  ASSERT_OK(task2_completion.Wait());

  complete_task.Signal();
  ASSERT_OK(task_complete.Wait());

  // The task unbound the irq, so any queued irq callback request should be cancelled.
  // If not, the irq handler will be called and assert.
}

namespace driver_runtime {
extern DispatcherCoordinator& GetDispatcherCoordinator();
}

// Tests the potential race condition that occurs when an irq is unbound
// but the port has just read the irq packet from the port.
TEST_F(DispatcherTest, UnbindIrqImmediatelyAfterTriggering) {
  static constexpr uint32_t kNumIrqs = 3000;
  static constexpr uint32_t kNumThreads = 10;

  // TODO(fxbug.dev/102878): this can be replaced by |fdf_env::DriverShutdown| once it works
  // properly.
  libsync::Completion shutdown_completion;
  std::atomic_int num_destructed = 0;
  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) {
    // |fetch_add| returns the value before incrementing.
    if (num_destructed.fetch_add(1) == kNumThreads - 1) {
      shutdown_completion.Signal();
    }
  };

  auto driver = CreateFakeDriver();
  driver_context::PushDriver(driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto fdf_dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(fdf_dispatcher.is_error());

  async_dispatcher_t* dispatcher = fdf_dispatcher->async_dispatcher();
  ASSERT_NOT_NULL(dispatcher);

  // Create a bunch of blocking dispatchers to force new threads.
  fdf::Dispatcher unused_dispatchers[kNumThreads - 1];
  {
    for (uint32_t i = 0; i < kNumThreads - 1; i++) {
      auto fdf_dispatcher =
          fdf::Dispatcher::Create(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, "", destructed_handler);
      ASSERT_FALSE(fdf_dispatcher.is_error());
      unused_dispatchers[i] = *std::move(fdf_dispatcher);
    }
  }

  // Create and unbind a bunch of irqs.
  zx::interrupt irqs[kNumIrqs] = {};
  for (uint32_t i = 0; i < kNumIrqs; i++) {
    // Must unbind irq from dispatcher thread.
    libsync::Completion unbind_complete;
    ASSERT_OK(async::PostTask(dispatcher, [&] {
      ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irqs[i]));

      async::Irq irq(
          irqs[i].get(), 0,
          [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
              const zx_packet_interrupt_t* interrupt) { ASSERT_FALSE(true); });
      ASSERT_EQ(ZX_OK, irq.Begin(dispatcher));
      // This queues the irq packet on the port, which may be read by another thread.
      irqs[i].trigger(0, zx::time());
      ASSERT_OK(irq.Cancel());
      unbind_complete.Signal();
    }));
    ASSERT_OK(unbind_complete.Wait());
  }

  fdf_dispatcher->ShutdownAsync();
  for (uint32_t i = 0; i < kNumThreads - 1; i++) {
    unused_dispatchers[i].ShutdownAsync();
  }
  ASSERT_OK(shutdown_completion.Wait());

  fdf_dispatcher->reset();
  for (uint32_t i = 0; i < kNumThreads - 1; i++) {
    unused_dispatchers[i].reset();
  }

  // Reset the number of threads to 1.
  driver_runtime::GetDispatcherCoordinator().Reset();
}

// Tests that binding irqs to an unsynchronized dispatcher is not allowed.
TEST_F(DispatcherTest, IrqUnsynchronized) {
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, __func__,
                                           "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  async::Irq irq(irq_object.get(), 0,
                 [&](async_dispatcher_t* dispatcher_arg, async::Irq* irq_arg, zx_status_t status,
                     const zx_packet_interrupt_t* interrupt) { ASSERT_TRUE(false); });
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, irq.Begin(dispatcher));
}

void IrqNotCalledHandler(async_dispatcher_t* async, async_irq_t* irq, zx_status_t status,
                         const zx_packet_interrupt_t* packet) {
  ASSERT_TRUE(status == ZX_ERR_CANCELED);
}

// Tests that you cannot unbind an irq from a different dispatcher from which it was bound to.
TEST_F(DispatcherTest, UnbindIrqFromWrongDispatcher) {
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));

  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  ASSERT_NOT_NULL(dispatcher);

  fdf_dispatcher_t* fdf_dispatcher2;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher2));

  async_dispatcher_t* dispatcher2 = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher2);
  ASSERT_NOT_NULL(dispatcher2);

  zx::interrupt irq_object;
  ASSERT_EQ(ZX_OK, zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &irq_object));

  // Use the C API, as the C++ async::Irq will clear the dispatcher on the first call to Cancel.
  async_irq_t irq = {{ASYNC_STATE_INIT}, &IrqNotCalledHandler, irq_object.get()};

  ASSERT_OK(async_bind_irq(dispatcher, &irq));

  libsync::Completion task_complete;
  ASSERT_OK(async::PostTask(dispatcher2, [&] {
    // Cancel the irq from a different dispatcher it was bound to.
    ASSERT_EQ(ZX_ERR_BAD_STATE, async_unbind_irq(dispatcher, &irq));
    task_complete.Signal();
  }));
  ASSERT_OK(task_complete.Wait());

  task_complete.Reset();
  ASSERT_OK(async::PostTask(dispatcher, [&] {
    ASSERT_EQ(ZX_OK, async_unbind_irq(dispatcher, &irq));
    task_complete.Signal();
  }));
  ASSERT_OK(task_complete.Wait());
}

//
// WaitUntilIdle tests
//

TEST_F(DispatcherTest, WaitUntilIdle) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  ASSERT_TRUE(dispatcher->IsIdle());
  WaitUntilIdle(dispatcher);
  ASSERT_TRUE(dispatcher->IsIdle());
}

TEST_F(DispatcherTest, WaitUntilIdleWithDirectCall) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  // We shouldn't actually block on a dispatcher that doesn't have ALLOW_SYNC_CALLS set,
  // but this is just for synchronizing the test.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadBlock(local_ch_, dispatcher, &entered_callback, &complete_blocking_read));

  std::thread t1 = std::thread([&] {
    // Make the call not reentrant, so that the read will run immediately once the write happens.
    driver_context::PushDriver(CreateFakeDriver());
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  });

  // Wait for the read callback to be called, it will block until we signal it to complete.
  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  ASSERT_FALSE(dispatcher->IsIdle());

  // Start a thread that blocks until the dispatcher is idle.
  libsync::Completion wait_started;
  libsync::Completion wait_complete;
  std::thread t2 = std::thread([&] {
    wait_started.Signal();
    WaitUntilIdle(dispatcher);
    ASSERT_TRUE(dispatcher->IsIdle());
    wait_complete.Signal();
  });

  ASSERT_OK(wait_started.Wait(zx::time::infinite()));
  ASSERT_FALSE(wait_complete.signaled());
  ASSERT_FALSE(dispatcher->IsIdle());

  complete_blocking_read.Signal();

  // Dispatcher should be idle now.
  ASSERT_OK(wait_complete.Wait(zx::time::infinite()));

  t1.join();
  t2.join();
}

TEST_F(DispatcherTest, WaitUntilIdleWithAsyncLoop) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  // We shouldn't actually block on a dispatcher that doesn't have ALLOW_SYNC_CALLS set,
  // but this is just for synchronizing the test.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadBlock(local_ch_, dispatcher, &entered_callback, &complete_blocking_read));

  // Call is reentrant, so the read will be queued on the async loop.
  ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  ASSERT_FALSE(dispatcher->IsIdle());

  // Wait for the read callback to be called, it will block until we signal it to complete.
  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  ASSERT_FALSE(dispatcher->IsIdle());

  complete_blocking_read.Signal();
  WaitUntilIdle(dispatcher);
  ASSERT_TRUE(dispatcher->IsIdle());
}

TEST_F(DispatcherTest, WaitUntilIdleCanceledRead) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  auto channel_read = std::make_unique<fdf::ChannelRead>(
      local_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_FALSE(true);  // This callback should never be called.
      });
  ASSERT_OK(channel_read->Begin(dispatcher));

  // Call is reentrant, so the read will be queued on the async loop.
  ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  ASSERT_FALSE(dispatcher->IsIdle());

  ASSERT_OK(channel_read->Cancel());

  loop_.StartThread();

  WaitUntilIdle(dispatcher);
}

TEST_F(DispatcherTest, WaitUntilIdlePendingWait) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);
  ASSERT_OK(
      wait.Begin(async_dispatcher,
                 [](async_dispatcher_t* async_dispatcher, async::WaitOnce* wait, zx_status_t status,
                    const zx_packet_signal_t* signal) { ASSERT_FALSE(true); }));
  ASSERT_TRUE(dispatcher->IsIdle());
  WaitUntilIdle(dispatcher);
}

TEST_F(DispatcherTest, WaitUntilIdleDelayedTask) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(dispatcher);
  ASSERT_NOT_NULL(async_dispatcher);

  async::TaskClosure task;
  task.set_handler([] { ASSERT_FALSE(true); });
  ASSERT_OK(task.PostForTime(async_dispatcher, zx::deadline_after(zx::sec(100))));

  ASSERT_TRUE(dispatcher->IsIdle());
  WaitUntilIdle(dispatcher);

  ASSERT_OK(task.Cancel());  // Task should not be running yet.
}

TEST_F(DispatcherTest, WaitUntilIdleWithAsyncLoopMultipleThreads) {
  loop_.Quit();
  loop_.JoinThreads();
  loop_.ResetQuit();

  constexpr uint32_t kNumThreads = 2;
  constexpr uint32_t kNumClients = 22;

  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, __func__,
                                           "scheduler_role", CreateFakeDriver(), &dispatcher));

  struct ReadClient {
    fdf::Channel channel;
    libsync::Completion entered_callback;
    libsync::Completion complete_blocking_read;
  };

  std::vector<ReadClient> local(kNumClients);
  std::vector<fdf::Channel> remote(kNumClients);

  for (uint32_t i = 0; i < kNumClients; i++) {
    auto channels = fdf::ChannelPair::Create(0);
    ASSERT_OK(channels.status_value());
    local[i].channel = std::move(channels->end0);
    remote[i] = std::move(channels->end1);
    ASSERT_NO_FATAL_FAILURE(RegisterAsyncReadBlock(local[i].channel.get(), dispatcher,
                                                   &local[i].entered_callback,
                                                   &local[i].complete_blocking_read));
  }

  fdf::Arena arena(nullptr);
  for (uint32_t i = 0; i < kNumClients; i++) {
    // Call is considered reentrant and will be queued on the async loop.
    auto write_status = remote[i].Write(0, arena, nullptr, 0, cpp20::span<zx_handle_t>());
    ASSERT_OK(write_status.status_value());
  }

  for (uint32_t i = 0; i < kNumThreads; i++) {
    loop_.StartThread();
  }

  ASSERT_OK(local[0].entered_callback.Wait(zx::time::infinite()));
  local[0].complete_blocking_read.Signal();

  ASSERT_FALSE(dispatcher->IsIdle());

  // Allow all the read callbacks to complete.
  for (uint32_t i = 1; i < kNumClients; i++) {
    local[i].complete_blocking_read.Signal();
  }

  WaitUntilIdle(dispatcher);

  for (uint32_t i = 0; i < kNumClients; i++) {
    ASSERT_TRUE(local[i].complete_blocking_read.signaled());
  }
}

TEST_F(DispatcherTest, WaitUntilIdleMultipleDispatchers) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  fdf_dispatcher_t* dispatcher2;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher2));

  // We shouldn't actually block on a dispatcher that doesn't have ALLOW_SYNC_CALLS set,
  // but this is just for synchronizing the test.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadBlock(local_ch_, dispatcher, &entered_callback, &complete_blocking_read));

  // Call is reentrant, so the read will be queued on the async loop.
  ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  ASSERT_FALSE(dispatcher->IsIdle());

  // Wait for the read callback to be called, it will block until we signal it to complete.
  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  ASSERT_FALSE(dispatcher->IsIdle());
  ASSERT_TRUE(dispatcher2->IsIdle());
  WaitUntilIdle(dispatcher2);

  complete_blocking_read.Signal();
  WaitUntilIdle(dispatcher);
  ASSERT_TRUE(dispatcher->IsIdle());
}

// Tests shutting down the process async loop while requests are still pending.
TEST_F(DispatcherTest, ShutdownProcessAsyncLoop) {
  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(
                       FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, "", scheduler_role, driver, &loop_,
                       observer.fdf_observer(), &dispatcher));

  libsync::Completion entered_read;
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      local_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        entered_read.Signal();
        // Do not let the read callback complete until the loop has entered a shutdown state.
        while (loop_.GetState() != ASYNC_LOOP_SHUTDOWN) {
        }
      });
  ASSERT_OK(channel_read->Begin(static_cast<fdf_dispatcher_t*>(dispatcher)));

  // Call is reentrant, so the read will be queued on the async loop.
  ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  // This will queue the wait to run |Dispatcher::CompleteShutdown|.
  dispatcher->ShutdownAsync();

  ASSERT_OK(entered_read.Wait(zx::time::infinite()));

  loop_.Shutdown();

  ASSERT_OK(observer.WaitUntilShutdown());
  dispatcher->Destroy();
}

TEST_F(DispatcherTest, SyncDispatcherCancelRequestDuringShutdown) {
  DispatcherShutdownObserver observer;

  const void* driver = CreateFakeDriver();
  constexpr std::string_view scheduler_role = "scheduler_role";

  driver_runtime::Dispatcher* dispatcher;
  ASSERT_EQ(ZX_OK,
            driver_runtime::Dispatcher::CreateWithLoop(0, "", scheduler_role, driver, &loop_,
                                                       observer.fdf_observer(), &dispatcher));

  // Register a channel read that will be canceled by a posted task.
  auto channel_read = std::make_unique<fdf::ChannelRead>(
      local_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_FALSE(true);  // This should never be called.
      });
  ASSERT_OK(channel_read->Begin(static_cast<fdf_dispatcher_t*>(dispatcher)));

  libsync::Completion task_started;
  libsync::Completion dispatcher_shutdown_started;

  ASSERT_OK(async::PostTask(dispatcher->GetAsyncDispatcher(), [&] {
    task_started.Signal();
    ASSERT_OK(dispatcher_shutdown_started.Wait(zx::time::infinite()));
    ASSERT_OK(channel_read->Cancel());
  }));

  ASSERT_OK(task_started.Wait(zx::time::infinite()));

  // |Dispatcher::ShutdownAsync| will move the registered channel read into |shutdown_queue_|.
  dispatcher->ShutdownAsync();
  dispatcher_shutdown_started.Signal();

  ASSERT_OK(observer.WaitUntilShutdown());
  dispatcher->Destroy();
}

//
// Misc tests
//

TEST_F(DispatcherTest, GetCurrentDispatcherNone) {
  ASSERT_NULL(fdf_dispatcher_get_current_dispatcher());
}

TEST_F(DispatcherTest, GetCurrentDispatcher) {
  const void* driver1 = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher1;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver1, &dispatcher1));

  const void* driver2 = CreateFakeDriver();
  fdf_dispatcher_t* dispatcher2;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(0, __func__, "scheduler_role", driver2, &dispatcher2));

  // driver1 will wait on a message from driver2, then reply back.
  auto channel_read1 = std::make_unique<fdf::ChannelRead>(
      local_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        ASSERT_EQ(dispatcher1, fdf_dispatcher_get_current_dispatcher());
        // This reply will be reentrant and queued on the async loop.
        ASSERT_EQ(ZX_OK, fdf_channel_write(local_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
      });
  ASSERT_OK(channel_read1->Begin(dispatcher1));

  libsync::Completion got_reply;
  auto channel_read2 = std::make_unique<fdf::ChannelRead>(
      remote_ch_, 0,
      [&](fdf_dispatcher_t* dispatcher, fdf::ChannelRead* channel_read, zx_status_t status) {
        ASSERT_OK(status);
        ASSERT_EQ(dispatcher2, fdf_dispatcher_get_current_dispatcher());
        got_reply.Signal();
      });
  ASSERT_OK(channel_read2->Begin(dispatcher2));

  // Write from driver 2 to driver1.
  ASSERT_OK(async::PostTask(fdf_dispatcher_get_async_dispatcher(dispatcher2), [&] {
    ASSERT_EQ(dispatcher2, fdf_dispatcher_get_current_dispatcher());
    // Non-reentrant write.
    ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  }));

  ASSERT_OK(got_reply.Wait(zx::time::infinite()));
  WaitUntilIdle(dispatcher2);
}

TEST_F(DispatcherTest, GetCurrentDispatcherShutdownCallback) {
  libsync::Completion shutdown_completion;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) mutable {
    ASSERT_EQ(shutdown_dispatcher, fdf_dispatcher_get_current_dispatcher());
    shutdown_completion.Signal();
  };

  fdf::Dispatcher dispatcher;

  {
    driver_context::PushDriver(CreateFakeDriver());
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    auto dispatcher_with_status = fdf::Dispatcher::Create(0, "", shutdown_handler);
    ASSERT_FALSE(dispatcher_with_status.is_error());
    dispatcher = *std::move(dispatcher_with_status);
  }

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  async::WaitOnce wait(event.get(), ZX_USER_SIGNAL_0);

  // Registered, but not yet signaled.
  async_dispatcher_t* async_dispatcher = dispatcher.async_dispatcher();
  ASSERT_NOT_NULL(async_dispatcher);

  libsync::Completion wait_complete;
  ASSERT_OK(wait.Begin(async_dispatcher, [&wait_complete, event = std::move(event)](
                                             async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
    ASSERT_STATUS(status, ZX_ERR_CANCELED);
    ASSERT_EQ(dispatcher, fdf_dispatcher_get_current_dispatcher());
    wait_complete.Signal();
  }));

  // Shutdown the dispatcher, which should schedule cancellation of the channel read.
  dispatcher.ShutdownAsync();

  ASSERT_OK(wait_complete.Wait(zx::time::infinite()));
  ASSERT_OK(shutdown_completion.Wait());
}

TEST_F(DispatcherTest, HasQueuedTasks) {
  fdf_dispatcher_t* dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &dispatcher));

  ASSERT_FALSE(dispatcher->HasQueuedTasks());

  // We shouldn't actually block on a dispatcher that doesn't have ALLOW_SYNC_CALLS set,
  // but this is just for synchronizing the test.
  libsync::Completion entered_callback;
  libsync::Completion complete_blocking_read;
  ASSERT_NO_FATAL_FAILURE(
      RegisterAsyncReadBlock(local_ch_, dispatcher, &entered_callback, &complete_blocking_read));

  // Call is reentrant, so the read will be queued on the async loop.
  ASSERT_EQ(ZX_OK, fdf_channel_write(remote_ch_, 0, nullptr, nullptr, 0, nullptr, 0));
  ASSERT_FALSE(dispatcher->IsIdle());

  // Wait for the read callback to be called, it will block until we signal it to complete.
  ASSERT_OK(entered_callback.Wait(zx::time::infinite()));

  libsync::Completion entered_task;
  ASSERT_OK(async::PostTask(dispatcher, [&] { entered_task.Signal(); }));
  ASSERT_TRUE(dispatcher->HasQueuedTasks());

  complete_blocking_read.Signal();

  ASSERT_OK(entered_task.Wait());
  ASSERT_FALSE(dispatcher->HasQueuedTasks());

  WaitUntilIdle(dispatcher);
  ASSERT_FALSE(dispatcher->HasQueuedTasks());
}

// Tests shutting down all the dispatchers owned by a driver.
TEST_F(DispatcherTest, ShutdownAllDriverDispatchers) {
  const void* fake_driver = CreateFakeDriver();
  const void* fake_driver2 = CreateFakeDriver();
  const std::string_view scheduler_role = "scheduler_role";

  constexpr uint32_t kNumDispatchers = 3;
  DispatcherShutdownObserver observers[kNumDispatchers];
  driver_runtime::Dispatcher* dispatchers[kNumDispatchers];

  for (uint32_t i = 0; i < kNumDispatchers; i++) {
    const void* driver = i == 0 ? fake_driver : fake_driver2;
    ASSERT_EQ(ZX_OK, driver_runtime::Dispatcher::CreateWithLoop(0, "", scheduler_role, driver,
                                                                &loop_, observers[i].fdf_observer(),
                                                                &dispatchers[i]));
  }

  // Shutdown the second driver, dispatchers[1] and dispatchers[2] should be shutdown.
  fdf_env::DriverShutdown driver2_shutdown;
  libsync::Completion driver2_shutdown_completion;
  ASSERT_OK(driver2_shutdown.Begin(fake_driver2, [&](const void* driver) {
    ASSERT_EQ(fake_driver2, driver);
    driver2_shutdown_completion.Signal();
  }));

  ASSERT_OK(observers[1].WaitUntilShutdown());
  ASSERT_OK(observers[2].WaitUntilShutdown());
  ASSERT_OK(driver2_shutdown_completion.Wait());

  // Shutdown the first driver, dispatchers[0] should be shutdown.
  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver2_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));

  ASSERT_OK(observers[0].WaitUntilShutdown());
  ASSERT_OK(driver_shutdown_completion.Wait());

  for (uint32_t i = 0; i < kNumDispatchers; i++) {
    dispatchers[i]->Destroy();
  }
}

TEST_F(DispatcherTest, DriverDestroysDispatcherShutdownByDriverHost) {
  zx::result<fdf::Dispatcher> dispatcher;

  libsync::Completion completion;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) mutable {
    ASSERT_EQ(shutdown_dispatcher, dispatcher->get());
    dispatcher->reset();
    completion.Signal();
  };

  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  dispatcher = fdf::Dispatcher::Create(0, "", shutdown_handler);
  ASSERT_FALSE(dispatcher.is_error());

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));

  ASSERT_OK(completion.Wait());
  ASSERT_OK(driver_shutdown_completion.Wait());
}

TEST_F(DispatcherTest, CannotCreateNewDispatcherDuringDriverShutdown) {
  libsync::Completion completion;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) { completion.Signal(); };

  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", shutdown_handler);
  ASSERT_FALSE(dispatcher.is_error());

  libsync::Completion task_started;
  libsync::Completion driver_shutting_down;
  ASSERT_OK(async::PostTask(dispatcher->async_dispatcher(), [&] {
    task_started.Signal();
    ASSERT_OK(driver_shutting_down.Wait(zx::time::infinite()));
    auto dispatcher = fdf::Dispatcher::Create(0, "", [](fdf_dispatcher_t* dispatcher) {});
    // Creating a new dispatcher should fail, as the driver is currently shutting down.
    ASSERT_TRUE(dispatcher.is_error());
  }));
  ASSERT_OK(task_started.Wait(zx::time::infinite()));

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));

  driver_shutting_down.Signal();

  ASSERT_OK(completion.Wait(zx::time::infinite()));
  ASSERT_OK(driver_shutdown_completion.Wait());
}

// Tests shutting down all dispatchers for a driver, but the dispatchers are already in a shutdown
// state.
TEST_F(DispatcherTest, ShutdownAllDispatchersAlreadyShutdown) {
  libsync::Completion completion;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) { completion.Signal(); };

  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", shutdown_handler);
  ASSERT_FALSE(dispatcher.is_error());

  dispatcher->ShutdownAsync();
  ASSERT_OK(completion.Wait(zx::time::infinite()));

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));
  ASSERT_OK(driver_shutdown_completion.Wait());
}

// Tests shutting down all dispatchers for a driver, but the dispatcher is in the shutdown observer
// callback.
TEST_F(DispatcherTest, ShutdownAllDispatchersCurrentlyInShutdownCallback) {
  libsync::Completion entered_shutdown_handler;
  libsync::Completion complete_shutdown_handler;
  auto shutdown_handler = [&](fdf_dispatcher_t* shutdown_dispatcher) {
    entered_shutdown_handler.Signal();
    ASSERT_OK(complete_shutdown_handler.Wait());
  };

  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(0, "", shutdown_handler);
  ASSERT_FALSE(dispatcher.is_error());

  dispatcher->ShutdownAsync();
  ASSERT_OK(entered_shutdown_handler.Wait());

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));

  // The dispatcher is still in the dispatcher shutdown handler.
  ASSERT_FALSE(driver_shutdown_completion.signaled());
  complete_shutdown_handler.Signal();
  ASSERT_OK(driver_shutdown_completion.Wait());
}

TEST_F(DispatcherTest, DestroyAllDispatchers) {
  // Create drivers which leak their dispatchers.
  auto fake_driver = CreateFakeDriver();
  {
    driver_context::PushDriver(fake_driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    auto dispatcher = fdf::Dispatcher::Create(0, "", [](fdf_dispatcher_t* dispatcher) {});
    ASSERT_FALSE(dispatcher.is_error());
    dispatcher->release();
  }

  auto fake_driver2 = CreateFakeDriver();
  {
    driver_context::PushDriver(fake_driver2);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });
    auto dispatcher2 = fdf::Dispatcher::Create(0, "", [](fdf_dispatcher_t* dispatcher) {});
    ASSERT_FALSE(dispatcher2.is_error());
    dispatcher2->release();
  }

  // Driver host shuts down all drivers.
  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));
  ASSERT_OK(driver_shutdown_completion.Wait());
  driver_shutdown_completion.Reset();

  ASSERT_OK(driver_shutdown.Begin(fake_driver2, [&](const void* driver) {
    ASSERT_EQ(fake_driver2, driver);
    driver_shutdown_completion.Signal();
  }));
  ASSERT_OK(driver_shutdown_completion.Wait());

  // This will stop memory from leaking.
  fdf_env_destroy_all_dispatchers();
}

TEST_F(DispatcherTest, WaitUntilDispatchersDestroyed) {
  // No dispatchers, should immediately return.
  fdf_testing_wait_until_all_dispatchers_destroyed();

  constexpr uint32_t kNumDispatchers = 4;
  fdf_dispatcher_t* dispatchers[kNumDispatchers];

  for (uint32_t i = 0; i < kNumDispatchers; i++) {
    auto fake_driver = CreateFakeDriver();
    driver_context::PushDriver(fake_driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    auto dispatcher = fdf::Dispatcher::Create(
        0, "", [&](fdf_dispatcher_t* dispatcher) { fdf_dispatcher_destroy(dispatcher); });
    ASSERT_FALSE(dispatcher.is_error());
    dispatchers[i] = dispatcher->release();  // Destroyed in shutdown handler.
  }

  libsync::Completion thread_started;
  std::atomic_bool wait_complete = false;
  std::thread thread = std::thread([&]() {
    thread_started.Signal();
    fdf_testing_wait_until_all_dispatchers_destroyed();
    wait_complete = true;
  });

  ASSERT_OK(thread_started.Wait());
  for (uint32_t i = 0; i < kNumDispatchers; i++) {
    // Not all dispatchers have been destroyed yet.
    ASSERT_FALSE(wait_complete);
    dispatchers[i]->ShutdownAsync();
  }
  thread.join();
  ASSERT_TRUE(wait_complete);
}

// Tests waiting for all dispatchers to be destroyed when a driver shutdown
// observer is also registered.
TEST_F(DispatcherTest, WaitUntilDispatchersDestroyedHasDriverShutdownObserver) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(
      0, "", [&](fdf_dispatcher_t* dispatcher) { fdf_dispatcher_destroy(dispatcher); });
  ASSERT_FALSE(dispatcher.is_error());
  dispatcher->release();  // Destroyed in the shutdown handler.

  libsync::Completion thread_started;
  std::atomic_bool wait_complete = false;
  std::thread thread = std::thread([&]() {
    thread_started.Signal();
    fdf_testing_wait_until_all_dispatchers_destroyed();
    wait_complete = true;
  });

  ASSERT_OK(thread_started.Wait());

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_completion.Signal();
  }));
  ASSERT_OK(driver_shutdown_completion.Wait());

  thread.join();
  ASSERT_TRUE(wait_complete);
}

TEST_F(DispatcherTest, WaitUntilDispatchersDestroyedDuringDriverShutdownHandler) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  auto dispatcher = fdf::Dispatcher::Create(
      0, "", [&](fdf_dispatcher_t* dispatcher) { fdf_dispatcher_destroy(dispatcher); });
  ASSERT_FALSE(dispatcher.is_error());
  dispatcher->release();  // Destroyed in shutdown handler.

  // Block in the driver shutdown handler until we signal.
  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion driver_shutdown_started;
  libsync::Completion complete_driver_shutdown;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    ASSERT_EQ(fake_driver, driver);
    driver_shutdown_started.Signal();
    ASSERT_OK(complete_driver_shutdown.Wait());
  }));

  ASSERT_OK(driver_shutdown_started.Wait());

  // Start waiting for all dispatchers to be destroyed. This should not complete
  // until the shutdown handler completes.
  libsync::Completion thread_started;
  std::atomic_bool wait_complete = false;
  std::thread thread = std::thread([&]() {
    thread_started.Signal();
    fdf_testing_wait_until_all_dispatchers_destroyed();
    wait_complete = true;
  });

  ASSERT_OK(thread_started.Wait());

  // Shutdown handler has not returned yet.
  ASSERT_FALSE(wait_complete);
  complete_driver_shutdown.Signal();

  thread.join();
  ASSERT_TRUE(wait_complete);
}

TEST_F(DispatcherTest, GetSequenceIdSynchronizedDispatcher) {
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));
  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);

  fdf_dispatcher_t* fdf_dispatcher2;
  ASSERT_NO_FATAL_FAILURE(
      CreateDispatcher(0, __func__, "scheduler_role", CreateFakeDriver(), &fdf_dispatcher2));
  async_dispatcher_t* async_dispatcher2 = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher2);

  async_sequence_id_t dispatcher_id;
  async_sequence_id_t dispatcher2_id;

  // Get the sequence id for the first dispatcher.
  libsync::Completion task_completion;
  ASSERT_OK(async::PostTask(async_dispatcher, [&] {
    const char* error = nullptr;
    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              async_get_sequence_id(async_dispatcher2, &dispatcher_id, &error));
    ASSERT_NOT_NULL(error);
    ASSERT_SUBSTR(error, "multiple driver dispatchers detected");
    error = nullptr;
    ASSERT_OK(async_get_sequence_id(async_dispatcher, &dispatcher_id, &error));
    ASSERT_NULL(error);
    task_completion.Signal();
  }));
  ASSERT_OK(task_completion.Wait());

  // Get the sequence id for the second dispatcher.
  task_completion.Reset();
  ASSERT_OK(async::PostTask(async_dispatcher2, [&] {
    const char* error = nullptr;
    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              async_get_sequence_id(async_dispatcher, &dispatcher2_id, &error));
    ASSERT_NOT_NULL(error);
    ASSERT_SUBSTR(error, "multiple driver dispatchers detected");
    error = nullptr;
    ASSERT_OK(async_get_sequence_id(async_dispatcher2, &dispatcher2_id, &error));
    ASSERT_NULL(error);
    task_completion.Signal();
  }));
  ASSERT_OK(task_completion.Wait());

  ASSERT_NE(dispatcher_id.value, dispatcher2_id.value);

  // Get the sequence id again for the first dispatcher.
  task_completion.Reset();
  ASSERT_OK(async::PostTask(async_dispatcher, [&] {
    async_sequence_id_t id;
    const char* error = nullptr;
    ASSERT_EQ(ZX_ERR_INVALID_ARGS, async_get_sequence_id(async_dispatcher2, &id, &error));
    ASSERT_NOT_NULL(error);
    ASSERT_SUBSTR(error, "multiple driver dispatchers detected");
    error = nullptr;
    ASSERT_OK(async_get_sequence_id(async_dispatcher, &id, &error));
    ASSERT_NULL(error);
    ASSERT_EQ(id.value, dispatcher_id.value);
    task_completion.Signal();
  }));
  ASSERT_OK(task_completion.Wait());

  // Get the sequence id from a non-managed thread.
  async_sequence_id_t id;
  const char* error = nullptr;
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, async_get_sequence_id(async_dispatcher, &id, &error));
  ASSERT_NOT_NULL(error);
  ASSERT_SUBSTR(error, "not managed");
  error = nullptr;
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, async_get_sequence_id(async_dispatcher2, &id, &error));
  ASSERT_NOT_NULL(error);
  ASSERT_SUBSTR(error, "not managed");
}

TEST_F(DispatcherTest, GetSequenceIdUnsynchronizedDispatcher) {
  fdf_dispatcher_t* fdf_dispatcher;
  ASSERT_NO_FATAL_FAILURE(CreateDispatcher(FDF_DISPATCHER_OPTION_UNSYNCHRONIZED, __func__,
                                           "scheduler_role", CreateFakeDriver(), &fdf_dispatcher));
  async_dispatcher_t* async_dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);

  // Get the sequence id for the unsynchronized dispatcher.
  libsync::Completion task_completion;
  ASSERT_OK(async::PostTask(async_dispatcher, [&] {
    async_sequence_id_t id;
    const char* error = nullptr;
    ASSERT_EQ(ZX_ERR_WRONG_TYPE, async_get_sequence_id(async_dispatcher, &id, &error));
    ASSERT_NOT_NULL(error);
    ASSERT_SUBSTR(error, "UNSYNCHRONIZED");
    task_completion.Signal();
  }));
  ASSERT_OK(task_completion.Wait());

  // Get the sequence id from a non-managed thread.
  async_sequence_id_t id;
  const char* error = nullptr;
  ASSERT_EQ(ZX_ERR_WRONG_TYPE, async_get_sequence_id(async_dispatcher, &id, &error));
  ASSERT_NOT_NULL(error);
  ASSERT_SUBSTR(error, "UNSYNCHRONIZED");
}

//
// Error handling
//

// Tests that you cannot create an unsynchronized blocking dispatcher.
TEST_F(DispatcherTest, CreateUnsynchronizedAllowSyncCallsFails) {
  driver_context::PushDriver(CreateFakeDriver());
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  DispatcherShutdownObserver observer(false /* require_callback */);
  driver_runtime::Dispatcher* dispatcher;
  uint32_t options = FDF_DISPATCHER_OPTION_UNSYNCHRONIZED | FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS;
  ASSERT_NE(ZX_OK, fdf_dispatcher::Create(options, __func__, "scheduler_role",
                                          observer.fdf_observer(), &dispatcher));
}

// Tests that you cannot create a dispatcher on a thread not managed by the driver runtime.
TEST_F(DispatcherTest, CreateDispatcherOnNonRuntimeThreadFails) {
  DispatcherShutdownObserver observer(false /* require_callback */);
  driver_runtime::Dispatcher* dispatcher;
  ASSERT_NE(ZX_OK, fdf_dispatcher::Create(0, __func__, "scheduler_role", observer.fdf_observer(),
                                          &dispatcher));
}

// Tests that we don't spawn more threads than we need.
TEST_F(DispatcherTest, ExtraThreadIsReused) {
  {
    void* driver = reinterpret_cast<void*>(uintptr_t(1));
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 1);

    // Create first dispatcher
    driver_runtime::Dispatcher* dispatcher;
    DispatcherShutdownObserver observer;
    ASSERT_OK(fdf_dispatcher::Create(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                     "scheduler_role", observer.fdf_observer(), &dispatcher));
    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 2);

    dispatcher->ShutdownAsync();
    ASSERT_OK(observer.WaitUntilShutdown());
    dispatcher->Destroy();

    // Create second dispatcher
    DispatcherShutdownObserver observer2;
    ASSERT_OK(fdf_dispatcher::Create(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                     "scheduler_role", observer2.fdf_observer(), &dispatcher));
    // Note that we are still at 2 threads.
    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 2);

    dispatcher->ShutdownAsync();
    ASSERT_OK(observer2.WaitUntilShutdown());
    dispatcher->Destroy();

    // Ideally we would be back down 1 thread at this point, but that is challenging. A future
    // change may rememdy this.
    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 2);
  }

  driver_runtime::GetDispatcherCoordinator().Reset();
  ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 1);
}

TEST_F(DispatcherTest, MaximumTenThreads) {
  {
    void* driver = reinterpret_cast<void*>(uintptr_t(1));
    driver_context::PushDriver(driver);
    auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 1);

    constexpr uint32_t kNumDispatchers = 11;

    std::array<driver_runtime::Dispatcher*, kNumDispatchers> dispatchers;
    std::array<DispatcherShutdownObserver, kNumDispatchers> observers;
    for (uint32_t i = 0; i < kNumDispatchers; i++) {
      ASSERT_OK(fdf_dispatcher::Create(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, __func__,
                                       "scheduler_role", observers[i].fdf_observer(),
                                       &dispatchers[i]));
      ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), std::min(i + 2, 10u));
    }

    ASSERT_EQ(driver_runtime::GetDispatcherCoordinator().num_threads(), 10);

    for (uint32_t i = 0; i < kNumDispatchers; i++) {
      dispatchers[i]->ShutdownAsync();
      ASSERT_OK(observers[i].WaitUntilShutdown());
      dispatchers[i]->Destroy();
    }
  }

  driver_runtime::GetDispatcherCoordinator().Reset();
}

// Tests shutting down and destroying multiple dispatchers concurrently.
TEST_F(DispatcherTest, ConcurrentDispatcherDestroy) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  // Synchronize the dispatcher shutdown handlers to return at the same time,
  // so that |DispatcherCoordinator::NotifyShutdown| is more likely to happen concurrently.
  fbl::Mutex lock;
  bool dispatcher_shutdown = false;
  fbl::ConditionVariable all_dispatchers_shutdown;

  auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) {
    fdf_dispatcher_destroy(dispatcher);

    fbl::AutoLock al(&lock);
    // IF the other dispatcher has shutdown, we should signal them to wake up.
    if (dispatcher_shutdown) {
      all_dispatchers_shutdown.Broadcast();
    } else {
      // Block until the other dispatcher completes shutdown.
      dispatcher_shutdown = true;
      all_dispatchers_shutdown.Wait(&lock);
    }
  };

  auto dispatcher = fdf::Dispatcher::Create(0, "", destructed_handler);
  ASSERT_FALSE(dispatcher.is_error());

  auto dispatcher2 =
      fdf::Dispatcher::Create(FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS, "", destructed_handler);
  ASSERT_FALSE(dispatcher2.is_error());

  // The dispatchers will be destroyed in their shutdown handlers.
  dispatcher->release();
  dispatcher2->release();

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion completion;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) { completion.Signal(); }));
  ASSERT_OK(completion.Wait());

  // Wait for the driver to be removed from the dispatcher coordinator's |driver_state_| map as
  // |Reset| expects it to be empty.
  fdf_testing_wait_until_all_dispatchers_destroyed();

  // Reset the number of threads to 1.
  driver_runtime::GetDispatcherCoordinator().Reset();
}

// Tests that the sequence id retrieved in the driver shutdown callback
// matches that of the initial dispatcher.
TEST_F(DispatcherTest, ShutdownCallbackSequenceId) {
  auto fake_driver = CreateFakeDriver();

  async_sequence_id_t initial_dispatcher_id;

  auto dispatcher = fdf_env::DispatcherBuilder::CreateWithOwner(
      fake_driver, 0, "dispatcher", [](fdf_dispatcher_t* dispatcher) {});

  // We will create a second dispatcher while running on the initial dispatcher.
  fdf::Dispatcher additional_dispatcher;

  libsync::Completion completion;
  ASSERT_OK(async::PostTask(dispatcher->async_dispatcher(), [&] {
    // This needs to be retrieved when running on the dispatcher thread.
    const char* error = nullptr;
    ASSERT_OK(async_get_sequence_id(dispatcher->get(), &initial_dispatcher_id, &error));
    ASSERT_NULL(error);

    auto result = fdf::Dispatcher::Create(0, "", [&](fdf_dispatcher_t* dispatcher) {});
    ASSERT_FALSE(result.is_error());
    additional_dispatcher = std::move(*result);

    completion.Signal();
  }));

  ASSERT_OK(completion.Wait());

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion shutdown;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    async_sequence_id_t shutdown_id;
    const char* error = nullptr;
    ASSERT_OK(async_get_sequence_id(dispatcher->get(), &shutdown_id, &error));
    ASSERT_NULL(error);
    ASSERT_EQ(shutdown_id.value, initial_dispatcher_id.value);
    shutdown.Signal();
  }));

  ASSERT_OK(shutdown.Wait());
}

// Tests that the outgoing directory can be destructed on driver shutdown.
TEST_F(DispatcherTest, OutgoingDirectoryDestructionOnShutdown) {
  auto fake_driver = CreateFakeDriver();

  std::shared_ptr<fdf::OutgoingDirectory> outgoing;

  auto dispatcher = fdf_env::DispatcherBuilder::CreateWithOwner(
      fake_driver, 0, "dispatcher", [](fdf_dispatcher_t* dispatcher) {});

  // We will create a second dispatcher while running on the initial dispatcher.
  fdf::Dispatcher additional_dispatcher;

  libsync::Completion completion;
  ASSERT_OK(async::PostTask(dispatcher->async_dispatcher(), [&] {
    outgoing =
        std::make_shared<fdf::OutgoingDirectory>(fdf::OutgoingDirectory::Create(dispatcher->get()));

    auto result = fdf::Dispatcher::Create(0, "", [&](fdf_dispatcher_t* dispatcher) {});
    ASSERT_FALSE(result.is_error());
    additional_dispatcher = std::move(*result);

    completion.Signal();
  }));

  ASSERT_OK(completion.Wait());

  fdf_env::DriverShutdown driver_shutdown;
  libsync::Completion shutdown;
  ASSERT_OK(driver_shutdown.Begin(fake_driver, [&](const void* driver) {
    // The outgoing directory destructor will check that we are running on the
    // initial dispatcher's thread.
    outgoing.reset();
    shutdown.Signal();
  }));

  ASSERT_OK(shutdown.Wait());
}

TEST_F(DispatcherTest, SynchronizedDispatcherWrapper) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  {
    libsync::Completion completion;
    auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };
    auto dispatcher = fdf::SynchronizedDispatcher::Create({}, "", destructed_handler);
    ASSERT_FALSE(dispatcher.is_error());
    auto options = dispatcher->options();
    ASSERT_TRUE(options.has_value());
    ASSERT_EQ(*options, FDF_DISPATCHER_OPTION_SYNCHRONIZED);

    fdf::SynchronizedDispatcher dispatcher2 = *std::move(dispatcher);
    dispatcher2.ShutdownAsync();
    ASSERT_OK(completion.Wait());
  }
  {
    libsync::Completion completion;
    auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };
    auto blocking_dispatcher = fdf::SynchronizedDispatcher::Create(
        fdf::SynchronizedDispatcher::Options::kAllowSyncCalls, "", destructed_handler);
    ASSERT_FALSE(blocking_dispatcher.is_error());
    auto options = blocking_dispatcher->options();
    ASSERT_TRUE(options.has_value());
    ASSERT_EQ(*options,
              FDF_DISPATCHER_OPTION_SYNCHRONIZED | FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS);
    blocking_dispatcher->ShutdownAsync();
    ASSERT_OK(completion.Wait());
  }
  // Reset the number of threads to 1.
  driver_runtime::GetDispatcherCoordinator().Reset();
}

TEST_F(DispatcherTest, UnsynchronizedDispatcherWrapper) {
  auto fake_driver = CreateFakeDriver();
  driver_context::PushDriver(fake_driver);
  auto pop_driver = fit::defer([]() { driver_context::PopDriver(); });

  {
    libsync::Completion completion;
    auto destructed_handler = [&](fdf_dispatcher_t* dispatcher) { completion.Signal(); };
    auto dispatcher = fdf::UnsynchronizedDispatcher::Create({}, "", destructed_handler);
    ASSERT_FALSE(dispatcher.is_error());
    auto options = dispatcher->options();
    ASSERT_TRUE(options.has_value());
    ASSERT_EQ(*options, FDF_DISPATCHER_OPTION_UNSYNCHRONIZED);

    fdf::UnsynchronizedDispatcher dispatcher2 = *std::move(dispatcher);
    dispatcher2.ShutdownAsync();
    ASSERT_OK(completion.Wait());
  }
}
