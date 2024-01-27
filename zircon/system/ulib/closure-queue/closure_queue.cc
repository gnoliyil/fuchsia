// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/closure-queue/closure_queue.h"

#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <zircon/assert.h>

ClosureQueue::ClosureQueue(async_dispatcher_t* dispatcher, thrd_t dispatcher_thread) {
  SetDispatcher(dispatcher, dispatcher_thread);
}

ClosureQueue::ClosureQueue() {
  // nothing to do here - the SetDispatcher() call takes care of setting up impl_.
}

void ClosureQueue::SetDispatcher(async_dispatcher_t* dispatcher, thrd_t dispatcher_thread) {
  // Max 1 call to SetDispatcher() permitted.
  ZX_DEBUG_ASSERT(!impl_);
  impl_ = ClosureQueue::Impl::Create(dispatcher, dispatcher_thread);
}

ClosureQueue::~ClosureQueue() {
  // Ensure stopped and cleared.
  if (impl_) {
    impl_->StopAndClear();
  }
  // ~impl_ drops refcount on Impl - it'll only be fully deleted after all the
  // tasks queued in Impl::Enqueue() (that run TryRunAll()) have been deleted.
}

void ClosureQueue::Enqueue(fit::closure to_run) {
  ZX_DEBUG_ASSERT(impl_);
  impl_->Enqueue(impl_, std::move(to_run));
}

void ClosureQueue::StopAndClear() {
  ZX_DEBUG_ASSERT(impl_);
  impl_->StopAndClear();
}

bool ClosureQueue::is_stopped() {
  if (!impl_) {
    // Not SetDispatcher()'ed yet, so also not stopped yet.
    return false;
  }
  return impl_->is_stopped();
}

void ClosureQueue::RunOneHere() {
  ZX_DEBUG_ASSERT(impl_);
  impl_->RunOneHere();
}

thrd_t ClosureQueue::dispatcher_thread() {
  ZX_DEBUG_ASSERT(impl_);
  return impl_->dispatcher_thread();
}

std::shared_ptr<ClosureQueue::Impl> ClosureQueue::Impl::Create(async_dispatcher_t* dispatcher,
                                                               thrd_t dispatcher_thread) {
  return std::shared_ptr<ClosureQueue::Impl>(new ClosureQueue::Impl(dispatcher, dispatcher_thread));
}

ClosureQueue::Impl::Impl(async_dispatcher_t* dispatcher, thrd_t dispatcher_thread)
    : dispatcher_(dispatcher), dispatcher_thread_(dispatcher_thread) {
  ZX_DEBUG_ASSERT(dispatcher_);
}

ClosureQueue::Impl::~Impl() {
  // This is set to nullptr in StopAndClear(), which is called in ~ClosureQueue before dropping
  // impl_.
  ZX_DEBUG_ASSERT(!dispatcher_);
}

void ClosureQueue::Impl::Enqueue(std::shared_ptr<Impl> self_shared, fit::closure to_run) {
  std::lock_guard<std::mutex> lock(lock_);
  if (!dispatcher_) {
    // This path avoids LamdaQueue being overly picky about whether StopAndClear() is run before vs.
    // after stopping other threads from calling Enqueue(), but it's of course still up to client
    // code to ensure that ClosureQueue is valid/alive at the time that Enqueue is called on
    // ClosureQueue.  In other words, this path doesn't change the fact that lifetime protection in
    // ClosureQueue is limited to the self_shared that's captured by a posted lambda further down in
    // this method.
    //
    // ~to_run
    return;
  }
  bool was_empty = pending_.empty();
  pending_.emplace(std::move(to_run));
  // We intentionally re-post any time the queue bounced off empty, so that the posted runner task
  // isn't forced to keep re-checking-for/accepting additional tasks, which might tend to starve out
  // other work.
  if (was_empty) {
    pending_not_empty_condition_.notify_all();
    // Posting to a dispatcher under a lock is necessary here because otherwise
    // the dispatcher can already be deleted.
    zx_status_t result = async::PostTask(dispatcher_, [self_shared] {
      // Will just return if StopAndClear() has already run.
      self_shared->TryRunAll();
      // ~self_shared, which may run ~Impl if StopAndClear() has run.
    });
    ZX_ASSERT(result == ZX_OK);
  }
  // ~lock
}

void ClosureQueue::Impl::StopAndClear() {
  std::queue<fit::closure> local_pending;
  std::queue<fit::closure> local_pending_on_dispatcher_thread;
  std::lock_guard<std::mutex> lock(lock_);
  if (!dispatcher_) {
    // Idempotent; already stopped and cleared.
    return;
  }
  // We only enforce that the first call to StopAndClear() that actually stops
  // is on the dispatcher_thread_.  It's fine to ~ClosureQueue on a different
  // thread as long as we've previously run StopAndClear() on
  // dispatcher_thread_.
  if (dispatcher_thread_) {
    ZX_DEBUG_ASSERT(thrd_current() == dispatcher_thread_);
  }
  local_pending.swap(pending_);
  local_pending_on_dispatcher_thread.swap(pending_on_dispatcher_thread_);
  dispatcher_ = nullptr;
  // The order of these destructors is intentional, as we don't want to be
  // holding the lock while calling or deleting to_run(s):
  //
  // ~lock
  // ~local_pending_on_dispatcher_thread
  // ~local_pending
}

bool ClosureQueue::Impl::is_stopped() {
  std::lock_guard<std::mutex> lock(lock_);
  return !dispatcher_;
}

// We intentionally only pick up tasks that were in the queue at the start of
// TryRunAll(), and not tasks added while TryRunAll() is running, so that other
// unrelated tasks/work that needs to run on the current thread has a chance to
// run.
void ClosureQueue::Impl::TryRunAll() {
  if (dispatcher_thread_) {
    ZX_DEBUG_ASSERT(thrd_current() == dispatcher_thread_);
  }
  {  // scope lock
    std::lock_guard<std::mutex> lock(lock_);
    // StopAndClear() can only be called on the dispatcher_thread_, so we're
    // actually safe from that method's actions (even without holding lock_),
    // but to make FXL_GUARDED_BY() happy we check dispatcher_ while holding
    // lock_.
    if (!dispatcher_) {
      return;
    }
    ZX_DEBUG_ASSERT(dispatcher_);
    ZX_DEBUG_ASSERT(pending_on_dispatcher_thread_.empty());
    pending_on_dispatcher_thread_.swap(pending_);
    // local_pending can be empty at this point, but only if RunOneHere() was
    // used.
  }  // ~lock
  while (!pending_on_dispatcher_thread_.empty()) {
    fit::closure local_to_run = std::move(pending_on_dispatcher_thread_.front());
    pending_on_dispatcher_thread_.pop();
    local_to_run();
    // local_to_run() may have run StopAndClear().
    if (is_stopped()) {
      // StopAndClear() clears both pending_ and pending_on_dispatcher_thread_.
      ZX_DEBUG_ASSERT(pending_on_dispatcher_thread_.empty());
      break;
    }
  }
}

void ClosureQueue::Impl::RunOneHere() {
  if (dispatcher_thread_) {
    ZX_DEBUG_ASSERT(thrd_current() == dispatcher_thread_);
  }
  fit::closure local_to_run;
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    ZX_DEBUG_ASSERT(dispatcher_);
    while (pending_.empty()) {
      pending_not_empty_condition_.wait(lock);
    }
    local_to_run = std::move(pending_.front());
    pending_.pop();
  }  // ~lock
  local_to_run();
}

thrd_t ClosureQueue::Impl::dispatcher_thread() {
  ZX_DEBUG_ASSERT(dispatcher_thread_);
  return dispatcher_thread_;
}
