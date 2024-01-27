// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ASYNC_LOOP_CPP_LOOP_H_
#define LIB_ASYNC_LOOP_CPP_LOOP_H_

#include <lib/async-loop/loop.h>
#include <lib/zx/time.h>
#include <stdbool.h>
#include <stddef.h>
#include <threads.h>
#include <zircon/compiler.h>

namespace async {

/// C++ wrapper for an asynchronous dispatch loop.
///
/// This class is thread-safe.
class Loop {
 public:
  /// Creates a message loop.
  ///
  /// All operations on the message loop are thread-safe (except destroy).
  ///
  /// Note that it's ok to run the loop on a different thread from the one on which it was created.
  ///
  /// |config| provides configuration for the message loop.  Must not be null.
  ///
  /// See also:
  ///   |kAsyncLoopConfigNeverAttachToThread|
  ///   |kAsyncLoopConfigAttachToCurrentThread|
  ///   |kAsyncLoopConfigNoAttachToCurrentThread|
  explicit Loop(const async_loop_config_t* config);

  Loop(const Loop&) = delete;
  Loop(Loop&&) = delete;
  Loop& operator=(const Loop&) = delete;
  Loop& operator=(Loop&&) = delete;

  /// Destroys the message loop.  Implicitly calls |Shutdown()|.
  ~Loop();

  /// Gets the underlying message loop structure.
  async_loop_t* loop() const { return loop_; }

  /// Gets the loop's asynchronous dispatch interface.
  async_dispatcher_t* dispatcher() const { return async_loop_get_dispatcher(loop_); }

  /// Shuts down the message loop, notifies handlers which asked to handle shutdown. The message
  /// loop must not currently be running on any threads other than those started by |StartThread()|
  /// which this function will join.
  ///
  /// Any tasks still queued when |Shutdown()| is called will be immediately completed with the
  /// ZX_ERR_CANCELLED status. Callbacks will called on one of the loop's worker threads (i.e., a
  /// thread created by |StartThread()|) if such a thread exists. If no worker threads have been
  /// created, callbacks will be called by the current thread.
  ///
  /// Does nothing if already shutting down.
  void Shutdown();

  /// Runs the message loop on the current thread. This function can be called on multiple threads
  /// to setup a multi-threaded dispatcher.
  ///
  /// Dispatches events until the |deadline| expires or the loop is quitted. Use |ZX_TIME_INFINITE|
  /// to dispatch events indefinitely.
  ///
  /// If |once| is true, performs a single unit of work then returns.
  ///
  /// Returns |ZX_OK| if the dispatcher returns after one cycle.
  /// Returns |ZX_ERR_TIMED_OUT| if the deadline expired.
  /// Returns |ZX_ERR_CANCELED| if the loop quitted.
  /// Returns |ZX_ERR_BAD_STATE| if the loop was shut down with |Shutdown()|.
  zx_status_t Run(zx::time deadline = zx::time::infinite(), bool once = false);

  /// Runs the message loop on the current thread. Dispatches events until there are none remaining,
  /// and then returns without waiting. This is useful for unit testing, because the behavior
  /// doesn't depend on time.
  ///
  /// WARNING: Calling RunUntilIdle() with a dispatcher that runs on a separate thread or a
  /// multi-threaded dispatcher (common outside single-threaded tests) can have subtle unexpected
  /// consequences:
  ///   1) This may return before newly queued work is completed, because the work may be dispatched
  ///      to a different thread. The calling thread will not wait on that work to complete, only
  ///      for it to be removed from the pending work queue (roughly speaking).
  ///   2) Work that was previously queued, prior to calling RunUntilIdle(), is also not guaranteed
  ///      to have completed when RunUntilIdle() finishes.
  ///
  /// Returns |ZX_OK| if the dispatcher reaches an idle state.
  /// Returns |ZX_ERR_CANCELED| if the loop quitted.
  /// Returns |ZX_ERR_BAD_STATE| if the loop was shut down with |Shutdown()|.
  zx_status_t RunUntilIdle();

  /// Quits the message loop.
  ///
  /// Active invocations of |Run()| and threads started using |StartThread()| will eventually
  /// terminate upon completion of their current unit of work.
  ///
  /// Subsequent calls to |Run()| or |StartThread()| will return immediately until |ResetQuit()| is
  /// called.
  void Quit();

  /// Resets the quit state of the message loop so that it can be restarted using |Run()| or
  /// |StartThread()|.
  ///
  /// This function must only be called when the message loop is not running. The caller must ensure
  /// all active invocations of |Run()| and threads started using |StartThread()| have terminated
  /// before resetting the quit state.
  ///
  /// Returns |ZX_OK| if the loop's state was |ASYNC_LOOP_RUNNABLE| or |ASYNC_LOOP_QUIT|.
  /// Returns |ZX_ERR_BAD_STATE| if the loop's state was |ASYNC_LOOP_SHUTDOWN| or if the message
  /// loop is currently active on one or more threads.
  zx_status_t ResetQuit();

  /// Returns the current state of the message loop.
  async_loop_state_t GetState() const;

  /// Starts a message loop running on a new thread. The thread will run until the loop quits.
  ///
  /// |name| is the desired name for the new thread, may be NULL.
  /// If |out_thread| is not NULL, it is set to the new thread identifier.
  ///
  /// Returns |ZX_OK| on success.
  /// Returns |ZX_ERR_BAD_STATE| if the loop was shut down with |async_loop_shutdown()|.
  /// Returns |ZX_ERR_NO_MEMORY| if allocation or thread creation failed.
  zx_status_t StartThread(const char* name = nullptr, thrd_t* out_thread = nullptr);

  /// Blocks until all dispatch threads started with |StartThread()| have terminated.
  void JoinThreads();

 private:
  async_loop_t* loop_;
};

}  // namespace async

#endif  // LIB_ASYNC_LOOP_CPP_LOOP_H_
