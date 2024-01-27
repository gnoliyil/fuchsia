// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_PIGWEED_BACKENDS_PW_ASYNC_FUCHSIA_PUBLIC_PW_ASYNC_FUCHSIA_FAKE_DISPATCHER_H_
#define THIRD_PARTY_PIGWEED_BACKENDS_PW_ASYNC_FUCHSIA_PUBLIC_PW_ASYNC_FUCHSIA_FAKE_DISPATCHER_H_

#include <lib/async-loop/loop.h>
#include <zircon/assert.h>
#include <zircon/listnode.h>

#include "pw_async/task.h"

namespace pw::async::test::backend {

class NativeFakeDispatcher {
 public:
  explicit NativeFakeDispatcher(Dispatcher& test_dispatcher);
  ~NativeFakeDispatcher() { DestroyLoop(); }

  void RequestStop() { stop_requested_ = true; }

  // Synchronously destroys the loop, runs pending tasks with a cancelled status, and frees the loop
  // memory.
  void DestroyLoop();

  chrono::SystemClock::time_point now();

  void Post(Task& task);
  void PostAfter(Task& task, chrono::SystemClock::duration delay);
  void PostAt(Task& task, chrono::SystemClock::time_point time);

  // No-op
  void PostPeriodic(Task& task, chrono::SystemClock::duration interval);
  // No-op
  void PostPeriodicAfter(Task& task, chrono::SystemClock::duration interval,
                         chrono::SystemClock::duration delay);
  // No-op
  void PostPeriodicAt(Task& task, chrono::SystemClock::duration interval,
                      chrono::SystemClock::time_point start_time);

  bool Cancel(Task& task);

  void RunUntilIdle();
  void RunUntil(chrono::SystemClock::time_point end_time);
  void RunFor(chrono::SystemClock::duration duration);

 private:
  // FakeAsyncLoop is an adapted version of the Zircon async-loop (implemented in
  // zircon/system/ulib/async-loop/loop.c) for testing. It contains adapted copies of a subset of
  // the async-loop methods.
  //
  // In the method copies, 1) code interfacing with Zircon timers has been replaced with a simulated
  // timer system and 2) code related to thread safety/synchronization has been elided.
  class FakeAsyncLoop {
   public:
    explicit FakeAsyncLoop();
    ~FakeAsyncLoop() { Shutdown(); }

    void Shutdown();
    chrono::SystemClock::time_point Now() const;
    zx_status_t PostTask(async_task_t* task);
    zx_status_t CancelTask(async_task_t* task);
    zx_status_t RunUntilIdle();
    zx_status_t Run(zx_time_t deadline, bool once);
    bool Runnable() const { return state_ == ASYNC_LOOP_RUNNABLE; }

   private:
    static inline list_node_t* TaskToNode(async_task_t* task) {
      return reinterpret_cast<list_node_t*>(&task->state);
    }
    static inline async_task_t* NodeToTask(list_node_t* node) {
      return reinterpret_cast<async_task_t*>(reinterpret_cast<char*>(node) -
                                             offsetof(async_task_t, state));
    }

    void InsertTask(async_task_t* task);
    void RestartTimer();
    zx_time_t NextDeadline();
    zx_status_t RunOnce(zx_time_t deadline);
    zx_status_t DispatchTasks();
    void CancelAll();

    // Tracks the current time as viewed by the fake loop.
    zx_time_t now_ = 0;
    // Simulated timer. Stores ZX_TIME_INFINITE when no timer is set.
    zx_time_t next_timer_expiration_ = ZX_TIME_INFINITE;

    async_loop_state_t state_ = ASYNC_LOOP_RUNNABLE;

    // True while the loop is busy dispatching tasks.
    bool dispatching_tasks_ = false;
    // Pending tasks, earliest deadline first.
    list_node_t task_list_;
    // Due tasks, earliest deadline first.
    list_node_t due_list_;
    // True if the simulated timer has been set and has not fired yet.
    bool timer_armed_ = false;
  };

  Dispatcher& dispatcher_;
  FakeAsyncLoop fake_loop_;
  bool stop_requested_ = false;
};

}  // namespace pw::async::test::backend

#endif  // THIRD_PARTY_PIGWEED_BACKENDS_PW_ASYNC_FUCHSIA_PUBLIC_PW_ASYNC_FUCHSIA_FAKE_DISPATCHER_H_
