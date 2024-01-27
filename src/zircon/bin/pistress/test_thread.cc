// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_thread.h"

#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/directory.h>
#include <lib/zx/clock.h>
#include <lib/zx/result.h>
#include <lib/zx/thread.h>
#include <lib/zx/time.h>
#include <zircon/threads.h>

#include <algorithm>
#include <memory>
#include <random>

#include <fbl/auto_lock.h>

#include "global_stats.h"
#include "random.h"
#include "sync_obj.h"

TestThread::TestThread(const TestThreadBehavior& behavior, zx::profile profile)
    : behavior_(behavior), profile_(std::move(profile)) {
  ZX_ASSERT(behavior_.path_len_dist.min() <= behavior_.path_len_dist.max());
  ZX_ASSERT(behavior_.path_len_dist.max() <= std::size(sync_objs_));
  ZX_ASSERT(std::size(sync_obj_deck_) == std::size(sync_objs_));

  // Build an array of indices which we will shuffle and use as our sync
  // object "deck" when determining which locks we will obtain during an
  // iteration.
  for (size_t i = 0; i < std::size(sync_obj_deck_); ++i) {
    sync_obj_deck_[i] = i;
  }
}

TestThread::~TestThread() { ZX_ASSERT(!thread_.has_value()); }

zx_status_t TestThread::InitStatics() {
  // Get a handle to the root job.  We will need this in order to create
  // profiles.
  zx::result<zx::job> maybe_root_job = GetRootJob();
  if (maybe_root_job.is_error()) {
    return maybe_root_job.error_value();
  }
  root_job_ = std::move(maybe_root_job.value());

  // Create the proper number of mutexes and cond_vars, then shuffle the array
  // of object pointers so that the acquisition ordering requirements are
  // randomized.
  for (size_t i = 0; i < std::size(sync_objs_); ++i) {
    if (i < kNumMutexes) {
      sync_objs_[i] = std::make_unique<MutexSyncObj>();
    } else {
      sync_objs_[i] = std::make_unique<CondVarSyncObj>();
    }
  }

  Random::Shuffle(sync_objs_);
  return ZX_OK;
}

zx::result<zx::job> TestThread::GetRootJob() {
  auto connect_result = component::Connect<fuchsia_kernel::RootJob>();

  if (connect_result.is_error()) {
    printf("Failed to connect to RootJob Service (%d)\n", connect_result.status_value());
    return zx::error(connect_result.status_value());
  }

  auto response = fidl::WireCall(connect_result.value())->Get();
  if (response.status() != ZX_OK) {
    printf("RootJob service failed to grant root job handle (%d)\n", response.status());
    return zx::error(response.status());
  }

  return zx::ok(std::move(response.value().job));
}

zx_status_t TestThread::AddThread(const TestThreadBehavior& behavior) {
  zx::profile profile;
  zx_status_t status;
  zx_profile_info_t profile_info{};

  if (behavior.profile_type == ProfileType::Fair) {
    profile_info.flags = ZX_PROFILE_INFO_FLAG_PRIORITY;
    profile_info.priority = behavior.priority;
  } else {
    profile_info.flags = ZX_PROFILE_INFO_FLAG_DEADLINE;
    profile_info.deadline_params.capacity = behavior.capacity;
    profile_info.deadline_params.relative_deadline = behavior.deadline;
    profile_info.deadline_params.period = behavior.deadline;
  }

  if (behavior.inheritable == false) {
    profile_info.flags |= ZX_PROFILE_INFO_FLAG_NO_INHERIT;
  }

  status = zx::profile::create(root_job_, 0, &profile_info, &profile);

  if (status != ZX_OK) {
    if (behavior.profile_type == ProfileType::Fair) {
      profile_info.flags = ZX_PROFILE_INFO_FLAG_PRIORITY;
      printf("Failed to create Fair profile with priority %u\n", profile_info.priority);
    } else {
      printf("Failed to create Deadline profile with capacity(%ld) deadline(%ld)\n",
             profile_info.deadline_params.capacity, profile_info.deadline_params.relative_deadline);
    }
    return status;
  }

  threads_.emplace_back(std::unique_ptr<TestThread>(new TestThread(behavior, std::move(profile))));
  thread_dist_ = std::uniform_int_distribution<size_t>{0, threads_.size() - 1};

  return ZX_OK;
}

void TestThread::Shutdown() {
  // Set the global shutdown flag, in addition to all of the CondVarSyncObj
  // shutdown flags.
  shutdown_now_.store(true);
  for (auto& obj : sync_objs_) {
    obj->Shutdown();
  }

  // Join all of our test threads, then destroy them.
  for (auto& t : threads_) {
    t->Join();
  }
  threads_.clear();
}

TestThread& TestThread::random_thread() {
  ZX_ASSERT(threads_.size() > 0);
  return *threads_[Random::Get(thread_dist_)];
}

void TestThread::Start() {
  ZX_ASSERT(!thread_.has_value());
  thread_ = std::thread([](TestThread* thiz) { thiz->Run(); }, this);
}

void TestThread::ChangeProfile() {
  fbl::AutoLock lock{&profile_lock_};
  if (!self_->is_valid()) {
    return;
  }

  // If were borrowing a profile, revert to our base profile.  Otherwise, apply
  // the profile of another thread selected at random.
  zx_status_t status = ZX_ERR_INTERNAL;
  if (profile_borrowed_) {
    status = self_->set_profile(profile_, 0);
    global_stats.profiles_reverted.fetch_add(1u);
  } else {
    status = self_->set_profile(random_thread().profile_, 0);
    global_stats.profiles_changed.fetch_add(1u);
  }

  ZX_ASSERT(status == ZX_OK);
  profile_borrowed_ = !profile_borrowed_;
}

void TestThread::Join() {
  ZX_ASSERT(shutdown_now_.load());
  if (!thread_.has_value()) {
    return;
  }

  thread_->join();
  thread_.reset();
}

void TestThread::HoldLocks(size_t deck_ndx) {
  // Obtain the next sync object in the sequence
  ZX_ASSERT(deck_ndx < path_len_);
  SyncObj& sync_obj = *sync_objs_[sync_obj_deck_[deck_ndx]];

  sync_obj.Acquire(behavior_);

  // Randomly change our profile if needed
  if (Random::RollDice(behavior_.self_profile_change_prob)) {
    ChangeProfile();
  }

  // Choose our linger behavior (intermediate or final) and then linger if we
  // need to.
  size_t next_ndx = deck_ndx + 1;
  const bool intermediate = (next_ndx != path_len_);
  LingerBehavior& lb = intermediate ? behavior_.intermediate_linger : behavior_.final_linger;

  if (Random::RollDice(lb.linger_probability)) {
    zx::duration linger_time{Random::Get(lb.time_dist)};

    if (Random::RollDice(lb.spin_probability)) {
      zx::time deadline = zx::deadline_after(linger_time);
      while (zx::clock::get_monotonic() < deadline) {
      }
      (intermediate ? global_stats.intermediate_spins : global_stats.final_spins).fetch_add(1u);
    } else {
      zx::nanosleep(zx::deadline_after(linger_time));
      (intermediate ? global_stats.intermediate_sleeps : global_stats.final_sleeps).fetch_add(1u);
    }
  }

  // If we have not hit the end, recurse and obtain the next sync object
  if (intermediate) {
    HoldLocks(next_ndx);
  }

  sync_obj.Release();
}

void TestThread::Run() {
  // Set our profile.
  {
    fbl::AutoLock profile_lock{&profile_lock_};
    ZX_ASSERT(!self_->is_valid());

    self_ = zx::unowned_thread{thrd_get_zx_handle(thrd_current())};
    zx_status_t status = self_->set_profile(profile_, 0);
    ZX_ASSERT(status == ZX_OK);

    profile_borrowed_ = false;
  }

  while (!shutdown_now_.load()) {
    // Shuffle the deck
    Random::Shuffle(sync_obj_deck_);

    // Determine how long our path for this pass will be, then sort the top
    // lock_path elements in the deck so that we don't ever have an A/B vs. B/A
    // lock ordering problem.
    path_len_ = Random::Get(behavior_.path_len_dist);
    std::sort(sync_obj_deck_.begin(), sync_obj_deck_.begin() + path_len_);

    // Recursively obtain the sync objects, lingering when we hit the final one, then
    // release.
    HoldLocks();
  }
}
