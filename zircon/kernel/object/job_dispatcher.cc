// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "object/job_dispatcher.h"

#include <inttypes.h>
#include <lib/counters.h>
#include <platform.h>
#include <zircon/errors.h>
#include <zircon/rights.h>
#include <zircon/syscalls/policy.h>
#include <zircon/types.h>

#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/inline_array.h>
#include <kernel/mutex.h>
#include <ktl/algorithm.h>
#include <object/process_dispatcher.h>
#include <object/root_job_observer.h>

#include <ktl/enforce.h>

KCOUNTER(dispatcher_job_create_count, "dispatcher.job.create")
KCOUNTER(dispatcher_job_destroy_count, "dispatcher.job.destroy")

// The starting max_height value of the root job.
static constexpr uint32_t kRootJobMaxHeight = 32;

static constexpr char kRootJobName[] = "root";

static constexpr size_t kDebugExceptionateInlineCount = 4;

template <>
uint64_t JobDispatcher::ChildCountLocked<JobDispatcher>() const {
  return jobs_.size();
}

template <>
uint64_t JobDispatcher::ChildCountLocked<ProcessDispatcher>() const {
  return procs_.size();
}

// To come up with an order on our recursive locks we take advantage of the fact that our
// max_height reduces from parent to child. As we acquire locks from parent->child we can build an
// increasing counter by inverting the max_height. We add 1 to the counter just so the order value
// of 0 is reserved for the default order when the lock is acquired without an order being
// specified.
uint32_t JobDispatcher::LockOrder() const { return kRootJobMaxHeight - max_height() + 1; }

// Calls the provided |zx_status_t func(fbl::RefPtr<DISPATCHER_TYPE>)|
// function on all live elements of |children|, which must be one of |jobs_|
// or |procs_|. Stops iterating early if |func| returns a value other than
// ZX_OK, returning that value from this method. |lock_| must be held when
// calling this method, and it will still be held while the callback is
// called.
//
// The returned |LiveRefsArray| needs to be destructed when |lock_| is not
// held anymore. The recommended pattern is:
//
//  LiveRefsArray refs;
//  {
//      Guard<Mutex> guard{get_lock()};
//      refs = ForEachChildInLocked(...);
//  }
//
template <typename T, typename L, typename Fn>
JobDispatcher::LiveRefsArray<T> JobDispatcher::ForEachChildInLocked(L& children,
                                                                    zx_status_t* result, Fn func) {
  const uint64_t count = ChildCountLocked<typename L::ValueType>();

  if (!count) {
    *result = ZX_OK;
    return LiveRefsArray<T>();
  }

  fbl::AllocChecker ac;
  LiveRefsArray<T> refs(new (&ac) fbl::RefPtr<T>[count], count);
  if (!ac.check()) {
    *result = ZX_ERR_NO_MEMORY;
    return LiveRefsArray<T>();
  }

  size_t ix = 0;

  *result = TakeEachChildLocked(children, [&ix, &refs, &func](auto&& cref) {
    zx_status_t result = func(cref);
    // As part of our contract with ForEachChildKeepAliveInLocked we must not
    // destroy the |cref| we were given, as it might be the last reference to
    // the object. Therefore we keep the reference alive in the |refs| array
    // and pass the responsibility of releasing them outside the lock to the
    // caller.
    refs[ix++] = ktl::move(cref);
    return result;
  });
  return refs;
}

// Calls the provided |zx_status_t func(fbl::RefPtr<DISPATCHER_TYPE>)|
// function on all live elements of |children|, which must be one of |jobs_|
// or |procs_|. The callback must retain the RefPtr and not destroy it until
// after this methods returns and the lock is dropped. Stops iterating early if
// |func| returns a value other than ZX_OK, returning that value from this
// method.
template <typename T, typename Fn>
zx_status_t JobDispatcher::TakeEachChildLocked(T& children, Fn func) {
  // Convert child raw pointers into RefPtrs. This is tricky and requires
  // special logic on the RefPtr class to handle a ref count that can be
  // zero.
  //
  // The main requirement is that |lock_| is both controlling child
  // list lookup and also making sure that the child destructor cannot
  // make progress when doing so. In other words, when inspecting the
  // |children| list we can be sure that a given child process or child
  // job is either
  //   - alive, with refcount > 0
  //   - in destruction process but blocked, refcount == 0

  for (auto& craw : children) {
    auto cref = ::fbl::MakeRefPtrUpgradeFromRaw(&craw, get_lock());
    if (!cref)
      continue;

    // |cref| might be the last reference at this point. If so,
    // when we drop it in the next iteration the object dtor
    // would be called here with the |get_lock()| held. To avoid that we pass
    // ownership of the refptr to the callback who is required to keep it alive.
    zx_status_t result = func(ktl::move(cref));
    if (result != ZX_OK) {
      return result;
    }
  }

  return ZX_OK;
}

fbl::RefPtr<JobDispatcher> JobDispatcher::CreateRootJob() {
  fbl::AllocChecker ac;
  auto job = fbl::AdoptRef(new (&ac) JobDispatcher(0u, nullptr, JobPolicy::CreateRootPolicy()));
  if (!ac.check()) {
    panic("root-job: failed to allocate\n");
  }
  job->set_name(kRootJobName, sizeof(kRootJobName));
  return job;
}

zx_status_t JobDispatcher::Create(uint32_t flags, const fbl::RefPtr<JobDispatcher>& parent,
                                  KernelHandle<JobDispatcher>* handle, zx_rights_t* rights) {
  if (parent != nullptr && parent->max_height() == 0) {
    // The parent job cannot have children.
    return ZX_ERR_OUT_OF_RANGE;
  }

  fbl::AllocChecker ac;
  KernelHandle new_handle(
      fbl::AdoptRef(new (&ac) JobDispatcher(flags, parent, parent->GetPolicy())));
  if (!ac.check())
    return ZX_ERR_NO_MEMORY;

  if (!parent->AddChildJob(new_handle.dispatcher())) {
    return ZX_ERR_BAD_STATE;
  }

  *rights = default_rights();
  *handle = ktl::move(new_handle);
  return ZX_OK;
}

JobDispatcher::JobDispatcher(uint32_t /*flags*/, fbl::RefPtr<JobDispatcher> parent,
                             JobPolicy policy)
    : SoloDispatcher(ZX_JOB_NO_PROCESSES | ZX_JOB_NO_JOBS | ZX_JOB_NO_CHILDREN),
      parent_(ktl::move(parent)),
      max_height_(parent_ ? parent_->max_height() - 1 : kRootJobMaxHeight),
      state_(State::READY),
      return_code_(0),
      kill_on_oom_(false),
      policy_(policy),
      exceptionate_(ZX_EXCEPTION_CHANNEL_TYPE_JOB) {
  kcounter_add(dispatcher_job_create_count, 1);
}

JobDispatcher::~JobDispatcher() {
  kcounter_add(dispatcher_job_destroy_count, 1);
  bool parent_should_die = RemoveFromJobTreesUnlocked();
  if (parent_should_die) {
    parent_->FinishDeadTransitionUnlocked();
  }
}

zx_koid_t JobDispatcher::get_related_koid() const { return parent_ ? parent_->get_koid() : 0u; }

bool JobDispatcher::AddChildProcess(const fbl::RefPtr<ProcessDispatcher>& process) {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};
  if (state_ != State::READY)
    return false;
  procs_.push_back(process.get());
  UpdateSignalsLocked();
  return true;
}

bool JobDispatcher::AddChildJob(const fbl::RefPtr<JobDispatcher>& job) {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};

  if (state_ != State::READY)
    return false;

  // Put the new job after our next-youngest child, or us if we have none.
  //
  // We try to make older jobs closer to the root (both hierarchically and
  // temporally) show up earlier in enumeration.
  JobDispatcher* neighbor = (jobs_.is_empty() ? this : &jobs_.back());

  // This can only be called once, the job should not already be part
  // of any job tree.
  DEBUG_ASSERT(!fbl::InContainer<JobDispatcher::RawListTag>(*job));
  DEBUG_ASSERT(neighbor != job.get());

  jobs_.push_back(job.get());
  UpdateSignalsLocked();
  return true;
}

void JobDispatcher::RemoveChildProcess(ProcessDispatcher* process) {
  canary_.Assert();

  bool should_die = false;
  {
    Guard<CriticalMutex> guard{get_lock()};
    // The process dispatcher can call us in its destructor, Kill(),
    // or RemoveThread().
    if (!fbl::InContainer<ProcessDispatcher::RawJobListTag>(*process)) {
      return;
    }
    procs_.erase(*process);
    UpdateSignalsLocked();
    should_die = IsReadyForDeadTransitionLocked();

    // Aggregate runtime stats from exiting process.
    aggregated_runtime_stats_.Add(process->GetAggregatedRuntime());
  }

  if (should_die)
    FinishDeadTransitionUnlocked();
}

bool JobDispatcher::RemoveChildJob(JobDispatcher* job) {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};
  if (!fbl::InContainer<JobDispatcher::RawListTag>(*job)) {
    return false;
  }

  jobs_.erase(*job);
  jobs_.size();
  UpdateSignalsLocked();
  return IsReadyForDeadTransitionLocked();
}

JobDispatcher::State JobDispatcher::GetState() const {
  Guard<CriticalMutex> guard{get_lock()};
  return state_;
}

bool JobDispatcher::RemoveFromJobTreesUnlocked() {
  canary_.Assert();

  if (parent_) {
    return parent_->RemoveChildJob(this);
  }
  return false;
}

bool JobDispatcher::IsReadyForDeadTransitionLocked() {
  canary_.Assert();
  return state_ == State::KILLING && jobs_.is_empty() && procs_.is_empty();
}

void JobDispatcher::FinishDeadTransitionUnlocked() {
  canary_.Assert();

  // Dead transition happens in a loop, since at every step we could be causing a parent to need to
  // finish its dead transition, and this loop allows us to avoid an unbounded recursion.
  JobDispatcher* current = this;
  do {
    // Make sure we're killing from the bottom of the tree up or else parent
    // jobs could die before their children.
    //
    // In particular, this means we have to finish dying before leaving the job
    // trees, since the last child leaving the tree can trigger its parent to
    // finish dying.
    DEBUG_ASSERT(!current->parent_ || (current->parent_->GetState() != State::DEAD));
    {
      Guard<CriticalMutex> guard{current->get_lock()};
      current->state_ = State::DEAD;
      current->exceptionate_.Shutdown();
      for (DebugExceptionate& debug_exceptionate : current->debug_exceptionates_) {
        debug_exceptionate.Shutdown();
      }
      current->UpdateStateLocked(0u, ZX_JOB_TERMINATED);
    }

    current = current->RemoveFromJobTreesUnlocked() ? &*current->parent_ : nullptr;
  } while (current);
}

void JobDispatcher::UpdateSignalsLocked() {
  // Clear all signals, and mark the appropriate ones active.
  //
  // The active signals take precedence over the clear signals.
  zx_signals_t clear = (ZX_JOB_NO_JOBS | ZX_JOB_NO_PROCESSES | ZX_JOB_NO_CHILDREN);

  // Removing jobs or processes.
  zx_signals_t set = 0u;
  if (procs_.is_empty()) {
    set |= ZX_JOB_NO_PROCESSES;
  }
  if (jobs_.is_empty()) {
    set |= ZX_JOB_NO_JOBS;
  }
  if (jobs_.is_empty() && procs_.is_empty()) {
    set |= ZX_JOB_NO_CHILDREN;
  }

  UpdateStateLocked(clear, set);
}

JobPolicy JobDispatcher::GetPolicy() const {
  Guard<CriticalMutex> guard{get_lock()};
  return policy_;
}

bool JobDispatcher::KillJobWithKillOnOOM() {
  // Get list of jobs with kill bit set.
  OOMBitJobArray oom_jobs;
  int count = 0;
  CollectJobsWithOOMBit(&oom_jobs, &count);
  if (count == 0) {
    printf("OOM: no jobs with kill_on_oom found\n");
    return false;
  }

  // Sort |oom_jobs| in descending order by max height.
  ktl::stable_sort(oom_jobs.begin(), oom_jobs.begin() + count,
                   [](const fbl::RefPtr<JobDispatcher>& a, const fbl::RefPtr<JobDispatcher>& b) {
                     return a->max_height() > b->max_height();
                   });

  // Kill lowest to highest until we find something to kill.
  for (int i = count - 1; i >= 0; --i) {
    auto& job = oom_jobs[i];
    if (job->Kill(ZX_TASK_RETCODE_OOM_KILL)) {
      char name[ZX_MAX_NAME_LEN];
      job->get_name(name);
      printf("OOM: killing %" PRIu64 " '%s'\n", job->get_koid(), name);
      return true;
    }
  }

  printf("OOM: no job found to kill\n");
  return false;
}

void JobDispatcher::CollectJobsWithOOMBit(OOMBitJobArray* into, int* count) {
  // As CollectJobsWithOOMBit will recurse we need to give a lock order to the guard.
  Guard<CriticalMutex> guard{AssertOrderedLock, get_lock(), LockOrder()};

  if (kill_on_oom_) {
    if (*count >= static_cast<int>(into->size())) {
      printf("OOM: skipping some jobs, exceeded max count\n");
      return;
    }

    auto cref = ::fbl::MakeRefPtrUpgradeFromRaw(this, get_lock());
    if (!cref)
      return;
    (*into)[*count] = ktl::move(cref);
    *count += 1;
  }

  for (auto& job : jobs_) {
    job.CollectJobsWithOOMBit(into, count);
  }
}

bool JobDispatcher::Kill(int64_t return_code) {
  canary_.Assert();

  JobList jobs_to_kill;
  ProcessList procs_to_kill;

  // Helper that transitions the given job to the KILLING state and adds the children to *_to_kill
  // lists for further cleanup.
  auto kill = [&jobs_to_kill, &procs_to_kill, return_code](JobDispatcher* job) {
    bool should_die = false;
    {
      Guard<CriticalMutex> guard{job->get_lock()};
      if (job->state_ != State::READY)
        return false;

      job->return_code_ = return_code;
      job->state_ = State::KILLING;
      [[maybe_unused]] zx_status_t result;

      // Gather the refs for our children. We can use |TakeEachChildLocked| since we will be
      // recording and keeping alive the RefPtrs in the callback in the *_to_kill lists.
      result =
          job->TakeEachChildLocked(job->jobs_, [&jobs_to_kill](fbl::RefPtr<JobDispatcher>&& job) {
            jobs_to_kill.push_front(ktl::move(job));
            return ZX_OK;
          });
      DEBUG_ASSERT(result == ZX_OK);
      result = job->TakeEachChildLocked(job->procs_,
                                        [&procs_to_kill](fbl::RefPtr<ProcessDispatcher>&& proc) {
                                          procs_to_kill.push_front(ktl::move(proc));
                                          return ZX_OK;
                                        });
      DEBUG_ASSERT(result == ZX_OK);

      should_die = job->IsReadyForDeadTransitionLocked();
    }

    if (should_die)
      job->FinishDeadTransitionUnlocked();
    return true;
  };

  // First transition our 'root' Job that into the KILLING state. If this cannot be done then we
  // consider the Kill to have failed, otherwise it succeeds and we clean all the children up.
  if (!kill(this)) {
    DEBUG_ASSERT(jobs_to_kill.is_empty());
    DEBUG_ASSERT(procs_to_kill.is_empty());
    return false;
  }

  // Since we kill the child jobs first we have a depth-first massacre.
  while (!jobs_to_kill.is_empty()) {
    fbl::RefPtr<JobDispatcher> job = jobs_to_kill.pop_front();
    kill(&*job);
  }

  while (!procs_to_kill.is_empty()) {
    procs_to_kill.pop_front()->Kill(return_code);
  }

  return true;
}

void JobDispatcher::CriticalProcessKill(fbl::RefPtr<ProcessDispatcher> dead_process) {
  char proc_name[ZX_MAX_NAME_LEN];
  dead_process->get_name(proc_name);

  char job_name[ZX_MAX_NAME_LEN];
  get_name(job_name);

  printf("critical-process: process '%s' (%" PRIu64 ") died, killing job '%s' (%" PRIu64 ")\n",
         proc_name, dead_process->get_koid(), job_name, get_koid());

  if (GetRootJobDispatcher().get() == this) {
    RootJobObserver::CriticalProcessKill(ktl::move(dead_process));
  }

  Kill(ZX_TASK_RETCODE_CRITICAL_PROCESS_KILL);
}

bool JobDispatcher::CanSetPolicy() TA_REQ(get_lock()) {
  // Can't set policy when there are active processes or jobs. This constraint ensures that a
  // process's policy cannot change over its lifetime.  Because a process's policy cannot change,
  // the risk of TOCTOU bugs is reduced and we are free to apply policy at the ProcessDispatcher
  // without having to walk up the tree to its containing job.
  if (!procs_.is_empty() || !jobs_.is_empty()) {
    return false;
  }
  return true;
}

zx_status_t JobDispatcher::SetBasicPolicy(uint32_t mode, const zx_policy_basic_v1_t* in_policy,
                                          size_t policy_count) {
  fbl::AllocChecker ac;
  fbl::InlineArray<zx_policy_basic_v2_t, kPolicyBasicInlineCount> policy(&ac, policy_count);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  for (size_t ix = 0; ix != policy.size(); ++ix) {
    policy[ix].condition = in_policy[ix].condition;
    policy[ix].action = in_policy[ix].policy;
    policy[ix].flags = ZX_POL_OVERRIDE_DENY;
  }

  return SetBasicPolicy(mode, policy.get(), policy.size());
}

zx_status_t JobDispatcher::SetBasicPolicy(uint32_t mode, const zx_policy_basic_v2_t* in_policy,
                                          size_t policy_count) {
  Guard<CriticalMutex> guard{get_lock()};

  if (!CanSetPolicy()) {
    return ZX_ERR_BAD_STATE;
  }
  return policy_.AddBasicPolicy(mode, in_policy, policy_count);
}

zx_status_t JobDispatcher::SetTimerSlackPolicy(const zx_policy_timer_slack& policy) {
  Guard<CriticalMutex> guard{get_lock()};

  if (!CanSetPolicy()) {
    return ZX_ERR_BAD_STATE;
  }

  // Is the policy valid?
  if (policy.min_slack < 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  slack_mode new_mode;
  switch (policy.default_mode) {
    case ZX_TIMER_SLACK_CENTER:
      new_mode = TIMER_SLACK_CENTER;
      break;
    case ZX_TIMER_SLACK_EARLY:
      new_mode = TIMER_SLACK_EARLY;
      break;
    case ZX_TIMER_SLACK_LATE:
      new_mode = TIMER_SLACK_LATE;
      break;
    default:
      return ZX_ERR_INVALID_ARGS;
  };

  const TimerSlack old_slack = policy_.GetTimerSlack();
  const zx_duration_t new_amount = ktl::max(old_slack.amount(), policy.min_slack);
  const TimerSlack new_slack(new_amount, new_mode);

  policy_.SetTimerSlack(new_slack);

  return ZX_OK;
}

bool JobDispatcher::EnumerateChildren(JobEnumerator* je) {
  canary_.Assert();

  LiveRefsArray<JobDispatcher> jobs_refs;
  LiveRefsArray<ProcessDispatcher> proc_refs;

  zx_status_t result = ZX_OK;

  {
    Guard<CriticalMutex> guard{get_lock()};

    proc_refs = ForEachChildInLocked<ProcessDispatcher>(
        procs_, &result, [&](const fbl::RefPtr<ProcessDispatcher>& proc) { return ZX_OK; });
    if (result != ZX_OK) {
      return false;
    }

    jobs_refs = ForEachChildInLocked<JobDispatcher>(
        jobs_, &result, [&](const fbl::RefPtr<JobDispatcher>& job) { return ZX_OK; });
  }

  // With the processes and jobs collected into their respective LiveRefsArrays, we can now invoke
  // the JobEnumerator callbacks on them. We perform this here, outside the lock, instead of
  // directly in the ForEachChildInLocked callbacks so that the callbacks are permitted to perform
  // user copies, or generally do actions that are not permitted whilst locks are held.
  for (auto& process : proc_refs) {
    if (process) {
      if (!je->OnProcess(&*process)) {
        return false;
      }
    }
  }
  for (auto& job : jobs_refs) {
    if (job) {
      if (!je->OnJob(&*job)) {
        return false;
      }
    }
  }

  return result == ZX_OK;
}

bool JobDispatcher::EnumerateChildrenRecursive(JobEnumerator* je) {
  canary_.Assert();

  LiveRefsArray<JobDispatcher> jobs_refs;
  LiveRefsArray<ProcessDispatcher> proc_refs;

  zx_status_t result = ZX_OK;

  {
    // As EnumerateChildren will recurse we need to give a lock order to the guard.
    Guard<CriticalMutex> guard{AssertOrderedLock, get_lock(), LockOrder()};

    proc_refs = ForEachChildInLocked<ProcessDispatcher>(
        procs_, &result, [&](const fbl::RefPtr<ProcessDispatcher>& proc) {
          return je->OnProcess(proc.get()) ? ZX_OK : ZX_ERR_STOP;
        });
    if (result != ZX_OK) {
      return false;
    }

    jobs_refs = ForEachChildInLocked<JobDispatcher>(
        jobs_, &result, [&](const fbl::RefPtr<JobDispatcher>& job) {
          if (!je->OnJob(job.get())) {
            return ZX_ERR_STOP;
          }
          // TODO(kulakowski): This recursive call can overflow the stack.
          return job->EnumerateChildrenRecursive(je) ? ZX_OK : ZX_ERR_STOP;
        });
  }

  return result == ZX_OK;
}

fbl::RefPtr<ProcessDispatcher> JobDispatcher::LookupProcessById(zx_koid_t koid) {
  canary_.Assert();

  LiveRefsArray<ProcessDispatcher> proc_refs;

  fbl::RefPtr<ProcessDispatcher> found_proc;
  {
    Guard<CriticalMutex> guard{get_lock()};
    zx_status_t result;

    proc_refs = ForEachChildInLocked<ProcessDispatcher>(procs_, &result,
                                                        [&](fbl::RefPtr<ProcessDispatcher> proc) {
                                                          if (proc->get_koid() == koid) {
                                                            found_proc = ktl::move(proc);
                                                            return ZX_ERR_STOP;
                                                          }
                                                          return ZX_OK;
                                                        });
  }
  return found_proc;  // Null if not found.
}

fbl::RefPtr<JobDispatcher> JobDispatcher::LookupJobById(zx_koid_t koid) {
  canary_.Assert();

  LiveRefsArray<JobDispatcher> jobs_refs;

  fbl::RefPtr<JobDispatcher> found_job;
  {
    Guard<CriticalMutex> guard{get_lock()};
    zx_status_t result;

    jobs_refs =
        ForEachChildInLocked<JobDispatcher>(jobs_, &result, [&](fbl::RefPtr<JobDispatcher> job) {
          if (job->get_koid() == koid) {
            found_job = ktl::move(job);
            return ZX_ERR_STOP;
          }
          return ZX_OK;
        });
  }
  return found_job;  // Null if not found.
}

void JobDispatcher::get_name(char (&out_name)[ZX_MAX_NAME_LEN]) const {
  canary_.Assert();

  name_.get(ZX_MAX_NAME_LEN, out_name);
}

zx_status_t JobDispatcher::set_name(const char* name, size_t len) {
  canary_.Assert();

  return name_.set(name, len);
}

Exceptionate* JobDispatcher::exceptionate() {
  canary_.Assert();
  return &exceptionate_;
}

zx_status_t JobDispatcher::ForEachDebugExceptionate(
    fit::inline_function<void(Exceptionate*)> func) {
  Guard<CriticalMutex> guard{get_lock()};
  // Remove disconnected exceptionates and get the count.
  size_t count = 0;
  for (auto it = debug_exceptionates_.begin(); it.IsValid();) {
    if (!it->HasValidChannel()) {
      debug_exceptionates_.erase(it++);
    } else {
      it++;
      count++;
    }
  }
  if (count == 0) {
    return ZX_OK;
  }
  fbl::AllocChecker ac;
  fbl::InlineArray<fbl::RefPtr<DebugExceptionate>, kDebugExceptionateInlineCount> snapshot(&ac,
                                                                                           count);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Perform the snapshot.
  ktl::transform(
      debug_exceptionates_.begin(), debug_exceptionates_.end(), snapshot.get(),
      [](DebugExceptionate& debug_exceptionate) { return fbl::RefPtr(&debug_exceptionate); });
  guard.Release();
  ktl::for_each(snapshot.get(), snapshot.get() + count,
                [func = ktl::move(func)](const fbl::RefPtr<DebugExceptionate>& exceptionate) {
                  func(exceptionate.get());
                });
  return ZX_OK;
}

zx_status_t JobDispatcher::CreateDebugExceptionate(KernelHandle<ChannelDispatcher> channel_handle,
                                                   zx_rights_t thread_rights,
                                                   zx_rights_t process_rights) {
  Guard<CriticalMutex> guard{get_lock()};
  // Remove disconnected exceptionates.
  size_t count = 0;
  for (auto it = debug_exceptionates_.begin(); it.IsValid();) {
    if (!it->HasValidChannel()) {
      debug_exceptionates_.erase(it++);
    } else {
      it++;
      count++;
    }
  }
  if (count >= ZX_EXCEPTION_CHANNEL_JOB_DEBUGGER_MAX_COUNT) {
    return ZX_ERR_ALREADY_BOUND;
  }
  fbl::AllocChecker ac;
  auto exceptionate =
      fbl::MakeRefCountedChecked<DebugExceptionate>(&ac, ZX_EXCEPTION_CHANNEL_TYPE_JOB_DEBUGGER);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status =
      exceptionate->SetChannel(ktl::move(channel_handle), thread_rights, process_rights);
  if (status != ZX_OK) {
    return status;
  }
  debug_exceptionates_.push_back(ktl::move(exceptionate));
  return ZX_OK;
}

void JobDispatcher::set_kill_on_oom(bool value) {
  Guard<CriticalMutex> guard{get_lock()};
  kill_on_oom_ = value;
}

bool JobDispatcher::get_kill_on_oom() const {
  Guard<CriticalMutex> guard{AssertOrderedLock, get_lock(), LockOrder()};
  return kill_on_oom_;
}

void JobDispatcher::GetInfo(zx_info_job_t* info) const {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};
  info->return_code = return_code_;
  info->exited = (state_ == State::DEAD) || (state_ == State::KILLING);
  info->kill_on_oom = kill_on_oom_;
  info->debugger_attached = !debug_exceptionates_.is_empty();
}

zx_status_t JobDispatcher::AccumulateRuntimeTo(zx_info_task_runtime_t* info) const {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};
  aggregated_runtime_stats_.AccumulateRuntimeTo(info);

  // At this point, the process in question may be in its destructor waiting to acquire the lock and
  // remove itself from this job, but its aggregated runtime is not yet part of this job's data.
  //
  // AccumulateRuntimeTo must be safe to be called even when the process is in its destructor.
  for (const auto& proc : procs_) {
    zx_status_t err = proc.AccumulateRuntimeTo(info);
    if (err != ZX_OK) {
      return err;
    }
  }
  return ZX_OK;
}
