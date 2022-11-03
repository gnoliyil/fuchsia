// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "object/process_dispatcher.h"

#include <assert.h>
#include <inttypes.h>
#include <lib/counters.h>
#include <lib/crypto/global_prng.h>
#include <lib/ktrace.h>
#include <string.h>
#include <trace.h>
#include <zircon/listnode.h>
#include <zircon/rights.h>

#include <arch/defines.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <kernel/thread.h>
#include <object/diagnostics.h>
#include <object/handle.h>
#include <object/job_dispatcher.h>
#include <object/thread_dispatcher.h>
#include <object/vm_address_region_dispatcher.h>
#include <object/vm_object_dispatcher.h>
#include <vm/vm.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object.h>

#include "zircon/errors.h"

#define LOCAL_TRACE 0

namespace {

const uint32_t kPolicyIdToPolicyException[] = {
    ZX_EXCP_POLICY_CODE_BAD_HANDLE,             // ZX_POL_BAD_HANDLE,
    ZX_EXCP_POLICY_CODE_WRONG_OBJECT,           // ZX_POL_WRONG_OBJECT
    ZX_EXCP_POLICY_CODE_VMAR_WX,                // ZX_POL_VMAR_WX
    ZX_EXCP_POLICY_CODE_NEW_ANY,                // ZX_POL_NEW_ANY
    ZX_EXCP_POLICY_CODE_NEW_VMO,                // ZX_POL_NEW_VMO
    ZX_EXCP_POLICY_CODE_NEW_CHANNEL,            // ZX_POL_NEW_CHANNEL
    ZX_EXCP_POLICY_CODE_NEW_EVENT,              // ZX_POL_NEW_EVENT
    ZX_EXCP_POLICY_CODE_NEW_EVENTPAIR,          // ZX_POL_NEW_EVENTPAIR
    ZX_EXCP_POLICY_CODE_NEW_PORT,               // ZX_POL_NEW_PORT
    ZX_EXCP_POLICY_CODE_NEW_SOCKET,             // ZX_POL_NEW_SOCKET
    ZX_EXCP_POLICY_CODE_NEW_FIFO,               // ZX_POL_NEW_FIFO
    ZX_EXCP_POLICY_CODE_NEW_TIMER,              // ZX_POL_NEW_TIMER
    ZX_EXCP_POLICY_CODE_NEW_PROCESS,            // ZX_POL_NEW_PROCESS
    ZX_EXCP_POLICY_CODE_NEW_PROFILE,            // ZX_POL_NEW_PROFILE
    ZX_EXCP_POLICY_CODE_NEW_PAGER,              // ZX_POL_NEW_PAGER
    ZX_EXCP_POLICY_CODE_AMBIENT_MARK_VMO_EXEC,  // ZX_POL_AMBIENT_MARK_VMO_EXEC
};

static_assert(std::size(kPolicyIdToPolicyException) == ZX_POL_MAX,
              "must update mapping from policy id to synth_code generated by policy exception");

}  // namespace

KCOUNTER(dispatcher_process_create_count, "dispatcher.process.create")
KCOUNTER(dispatcher_process_destroy_count, "dispatcher.process.destroy")

zx_status_t ProcessDispatcher::Create(fbl::RefPtr<JobDispatcher> job, ktl::string_view name,
                                      uint32_t flags, KernelHandle<ProcessDispatcher>* handle,
                                      zx_rights_t* rights,
                                      KernelHandle<VmAddressRegionDispatcher>* root_vmar_handle,
                                      zx_rights_t* root_vmar_rights) {
  fbl::AllocChecker ac;
  fbl::RefPtr<ShareableProcessState> shareable_state =
      fbl::AdoptRef(new (&ac) ShareableProcessState);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  KernelHandle new_handle(
      fbl::AdoptRef(new (&ac) ProcessDispatcher(shareable_state, job, name, flags)));
  if (!ac.check()) {
    // shareable_state was created successfully, and thus has a process count of 1 that needs to be
    // decremented here.
    shareable_state->DecrementShareCount();
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t result;
  if (flags == ZX_PROCESS_SHARED) {
    result = new_handle.dispatcher()->Initialize(SharedAspaceType::New);
  } else {
    result = new_handle.dispatcher()->Initialize();
  }
  if (result != ZX_OK)
    return result;

  // Create a dispatcher for the root VMAR.
  KernelHandle<VmAddressRegionDispatcher> new_vmar_handle;
  result = VmAddressRegionDispatcher::Create(new_handle.dispatcher()->normal_aspace()->RootVmar(),
                                             ARCH_MMU_FLAG_PERM_USER, &new_vmar_handle,
                                             root_vmar_rights);
  if (result != ZX_OK)
    return result;

  // Only now that the process has been fully created and initialized can we register it with its
  // parent job. We don't want anyone to see it in a partially initalized state.
  if (!job->AddChildProcess(new_handle.dispatcher())) {
    return ZX_ERR_BAD_STATE;
  }

  *rights = default_rights();
  *handle = ktl::move(new_handle);
  *root_vmar_handle = ktl::move(new_vmar_handle);

  return ZX_OK;
}

zx_status_t ProcessDispatcher::CreateShared(
    fbl::RefPtr<ProcessDispatcher> shared_proc, ktl::string_view name, uint32_t flags,
    KernelHandle<ProcessDispatcher>* handle, zx_rights_t* rights,
    KernelHandle<VmAddressRegionDispatcher>* restricted_vmar_handle,
    zx_rights_t* restricted_vmar_rights) {
  fbl::AllocChecker ac;
  fbl::RefPtr<ShareableProcessState> shareable_state;
  if (!shared_proc->normal_aspace() || !shared_proc->restricted_aspace()) {
    // The shared_proc process must have a normal and restricted aspace.
    return ZX_ERR_INVALID_ARGS;
  }

  shareable_state = shared_proc->shared_state_;
  // Increment the share count on the shared state before passing it to the ProcessDispatcher
  // constructor. If the increment fails, the shared state has already been destroyed.
  if (!shareable_state->IncrementShareCount()) {
    return ZX_ERR_BAD_STATE;
  }

  KernelHandle new_process(
      fbl::AdoptRef(new (&ac) ProcessDispatcher(shareable_state, shared_proc->job(), name, flags)));
  if (!ac.check()) {
    // shareable_state was created successfully, and thus has a process count of 1 that needs to be
    // decremented here.
    shareable_state->DecrementShareCount();
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t result = new_process.dispatcher()->Initialize(SharedAspaceType::Shared);
  if (result != ZX_OK) {
    return result;
  }

  // Create a dispatcher for the restricted VMAR.
  KernelHandle<VmAddressRegionDispatcher> new_restricted_vmar_handle;
  fbl::RefPtr<VmAddressRegion> restricted_vmar =
      new_process.dispatcher()->restricted_aspace()->RootVmar();
  result = VmAddressRegionDispatcher::Create(restricted_vmar, ARCH_MMU_FLAG_PERM_USER,
                                             &new_restricted_vmar_handle, restricted_vmar_rights);
  if (result != ZX_OK) {
    return result;
  }

  // Only now that the process has been fully created and initialized can we register it with its
  // parent job. We don't want anyone to see it in a partially initialized state.
  if (!shared_proc->job()->AddChildProcess(new_process.dispatcher())) {
    return ZX_ERR_BAD_STATE;
  }

  *rights = default_rights();
  *handle = ktl::move(new_process);
  *restricted_vmar_handle = ktl::move(new_restricted_vmar_handle);

  return ZX_OK;
}

ProcessDispatcher::ProcessDispatcher(fbl::RefPtr<ShareableProcessState> shared_state,
                                     fbl::RefPtr<JobDispatcher> job, ktl::string_view name,
                                     uint32_t flags)
    : shared_state_(ktl::move(shared_state)),
      job_(ktl::move(job)),
      policy_(job_->GetPolicy()),
      exceptionate_(ZX_EXCEPTION_CHANNEL_TYPE_PROCESS),
      debug_exceptionate_(ZX_EXCEPTION_CHANNEL_TYPE_DEBUGGER),
      name_(name.data(), name.length()) {
  LTRACE_ENTRY_OBJ;

  kcounter_add(dispatcher_process_create_count, 1);
}

ProcessDispatcher::~ProcessDispatcher() {
  LTRACE_ENTRY_OBJ;

  DEBUG_ASSERT(state_ == State::INITIAL || state_ == State::DEAD);

  // Assert that the -> DEAD transition cleaned up what it should have.
  DEBUG_ASSERT(!restricted_aspace_ || restricted_aspace_->is_destroyed());

  kcounter_add(dispatcher_process_destroy_count, 1);

  // Remove ourselves from the parent job's raw ref to us. Note that this might
  // have been called when transitioning State::DEAD. The Job can handle double calls.
  job_->RemoveChildProcess(this);

  LTRACE_EXIT_OBJ;
}

void ProcessDispatcher::on_zero_handles() {
  // If the process is in the initial state and the last handle is closed
  // we never detach from the parent job, so run the shutdown sequence for
  // that case.
  {
    Guard<CriticalMutex> guard{get_lock()};
    if (state_ != State::INITIAL) {
      // Initalized proceses are kept alive by their threads, see
      // RemoveThread() for the details.
      return;
    }
    SetStateLocked(State::DEAD);
  }

  FinishDeadTransition();
}

void ProcessDispatcher::get_name(char (&out_name)[ZX_MAX_NAME_LEN]) const {
  name_.get(ZX_MAX_NAME_LEN, out_name);
}

zx_status_t ProcessDispatcher::set_name(const char* name, size_t len) {
  return name_.set(name, len);
}

zx_status_t ProcessDispatcher::Initialize() {
  LTRACE_ENTRY_OBJ;

  Guard<CriticalMutex> guard{get_lock()};

  DEBUG_ASSERT(state_ == State::INITIAL);

  char aspace_name[ZX_MAX_NAME_LEN];
  snprintf(aspace_name, sizeof(aspace_name), "proc:%" PRIu64, get_koid());

  if (!shared_state_->Initialize(USER_ASPACE_BASE, USER_ASPACE_SIZE, aspace_name)) {
    return ZX_ERR_NO_MEMORY;
  }

  return ZX_OK;
}

zx_status_t ProcessDispatcher::Initialize(SharedAspaceType type) {
  LTRACE_ENTRY_OBJ;

  Guard<CriticalMutex> guard{get_lock()};

  DEBUG_ASSERT(state_ == State::INITIAL);

  char aspace_name[ZX_MAX_NAME_LEN];
  snprintf(aspace_name, sizeof(aspace_name), "proc:%" PRIu64, get_koid());

  if (type == SharedAspaceType::New) {
    static constexpr vaddr_t top_of_private = USER_ASPACE_BASE + PAGE_ALIGN(USER_ASPACE_SIZE / 2);
    static constexpr vaddr_t size_of_shared = USER_ASPACE_BASE + USER_ASPACE_SIZE - top_of_private;
    DEBUG_ASSERT(IS_PAGE_ALIGNED(top_of_private));
    if (!shared_state_->Initialize(top_of_private, size_of_shared, aspace_name)) {
      return ZX_ERR_NO_MEMORY;
    }
  }

  snprintf(aspace_name, sizeof(aspace_name), "proc(restricted):%" PRIu64, get_koid());
  restricted_aspace_ = VmAspace::Create(USER_ASPACE_BASE, PAGE_ALIGN(USER_ASPACE_SIZE / 2),
                                        VmAspace::Type::User, aspace_name);
  if (!restricted_aspace_) {
    return ZX_ERR_NO_MEMORY;
  }

  return ZX_OK;
}

void ProcessDispatcher::Exit(int64_t retcode) {
  LTRACE_ENTRY_OBJ;

  DEBUG_ASSERT(ProcessDispatcher::GetCurrent() == this);

  {
    Guard<CriticalMutex> guard{get_lock()};

    // check that we're in the RUNNING state or we're racing with something
    // else that has already pushed us until the DYING state
    DEBUG_ASSERT_MSG(state_ == State::RUNNING || state_ == State::DYING, "state is %s",
                     StateToString(state_));

    // Set the exit status if there isn't already an exit in progress.
    if (state_ != State::DYING) {
      DEBUG_ASSERT(retcode_ == 0);
      retcode_ = retcode;
      if (critical_to_job_ && critical_to_job_ == GetRootJobDispatcher()) {
        char pname[ZX_MAX_NAME_LEN];
        get_name(pname);
        printf("KERN: process '%s' (%lu) critical to root job exited %ld\n", pname, get_koid(),
               retcode);
      }
    }

    // enter the dying state, which should kill all threads
    SetStateLocked(State::DYING);
  }

  ThreadDispatcher::ExitCurrent();

  __UNREACHABLE;
}

void ProcessDispatcher::Kill(int64_t retcode) {
  LTRACE_ENTRY_OBJ;

  // fxbug.dev/30829: Call RemoveChildProcess outside of |get_lock()|.
  bool became_dead = false;

  {
    Guard<CriticalMutex> guard{get_lock()};

    // we're already dead
    if (state_ == State::DEAD)
      return;

    if (state_ != State::DYING) {
      DEBUG_ASSERT(retcode_ == 0);
      retcode_ = retcode;
      if (critical_to_job_ && critical_to_job_ == GetRootJobDispatcher()) {
        char pname[ZX_MAX_NAME_LEN];
        get_name(pname);
        printf("KERN: process '%s' (%lu) critical to root job killed with %ld\n", pname, get_koid(),
               retcode);
      }
    }

    // if we have no threads, enter the dead state directly
    if (thread_list_.is_empty()) {
      SetStateLocked(State::DEAD);
      became_dead = true;
    } else {
      // enter the dying state, which should trigger a thread kill.
      // the last thread exiting will transition us to DEAD
      SetStateLocked(State::DYING);
    }
  }

  if (became_dead)
    FinishDeadTransition();
}

zx_status_t ProcessDispatcher::Suspend() {
  canary_.Assert();

  LTRACE_ENTRY_OBJ;

  Guard<CriticalMutex> guard{get_lock()};

  // If we're dying don't try to suspend.
  if (state_ == State::DYING || state_ == State::DEAD)
    return ZX_ERR_BAD_STATE;

  DEBUG_ASSERT(suspend_count_ >= 0);
  suspend_count_++;
  if (suspend_count_ == 1) {
    for (auto& thread : thread_list_) {
      // Thread suspend can only fail if the thread is already dying, which is fine here
      // since it will be removed from this process shortly, so continue to suspend whether
      // the thread suspend succeeds or fails.
      zx_status_t status = thread.Suspend();
      DEBUG_ASSERT(status == ZX_OK || thread.IsDyingOrDead());
    }
  }

  return ZX_OK;
}

void ProcessDispatcher::Resume() {
  canary_.Assert();

  LTRACE_ENTRY_OBJ;

  Guard<CriticalMutex> guard{get_lock()};

  // If we're in the process of dying don't try to resume, just let it continue to clean up.
  if (state_ == State::DYING || state_ == State::DEAD)
    return;

  DEBUG_ASSERT(suspend_count_ > 0);
  suspend_count_--;
  if (suspend_count_ == 0) {
    for (auto& thread : thread_list_) {
      thread.Resume();
    }
  }
}

void ProcessDispatcher::KillAllThreadsLocked() {
  LTRACE_ENTRY_OBJ;

  for (auto& thread : thread_list_) {
    LTRACEF("killing thread %p\n", &thread);
    thread.Kill();
  }
}

zx_status_t ProcessDispatcher::AddInitializedThread(ThreadDispatcher* t, bool ensure_initial_thread,
                                                    const ThreadDispatcher::EntryState& entry) {
  LTRACE_ENTRY_OBJ;

  Guard<CriticalMutex> guard{get_lock()};
  const bool initial_thread = thread_list_.is_empty();
  if (ensure_initial_thread && !initial_thread) {
    return ZX_ERR_BAD_STATE;
  }

  if (initial_thread) {
    if (state_ != State::INITIAL)
      return ZX_ERR_BAD_STATE;
    t->set_is_initial_thread(true);
  } else {
    // We must not add a thread when in the DYING or DEAD states.
    // Also, we want to ensure that this is not the first thread.
    if (state_ != State::RUNNING)
      return ZX_ERR_BAD_STATE;
  }

  // Now that we know our state is okay we can attempt to start the thread running. This is okay
  // since as long as the thread doesn't refuse to start running then we cannot fail from here
  // and so we will update our thread_list_ and state before we drop the lock, making this
  // whole process atomic to any observers.
  zx_status_t result = t->MakeRunnable(entry, suspend_count_ > 0);
  if (result != ZX_OK) {
    t->set_is_initial_thread(false);
    return result;
  }

  // add the thread to our list
  thread_list_.push_back(t);

  DEBUG_ASSERT(t->process() == this);

  if (initial_thread) {
    DEBUG_ASSERT_MSG(start_time_ == 0, "start_time_ %ld", start_time_);
    start_time_ = current_time();
    SetStateLocked(State::RUNNING);
  }

  return ZX_OK;
}

// This is called within thread T's context when it is exiting.

void ProcessDispatcher::RemoveThread(ThreadDispatcher* t) {
  LTRACE_ENTRY_OBJ;

  // fxbug.dev/30829: Call RemoveChildProcess outside of |get_lock()|.
  bool became_dead = false;

  {
    // we're going to check for state and possibly transition below
    Guard<CriticalMutex> guard{get_lock()};

    // remove the thread from our list
    DEBUG_ASSERT(t != nullptr);
    thread_list_.erase(*t);

    // if this was the last thread, transition directly to DEAD state
    if (thread_list_.is_empty()) {
      LTRACEF("last thread left the process %p, entering DEAD state\n", this);
      SetStateLocked(State::DEAD);
      became_dead = true;
    }

    TaskRuntimeStats child_runtime;
    if (t->GetRuntimeStats(&child_runtime) == ZX_OK) {
      aggregated_runtime_stats_.Add(child_runtime);
    }
  }

  if (became_dead)
    FinishDeadTransition();
}

zx_koid_t ProcessDispatcher::get_related_koid() const { return job_->get_koid(); }

ProcessDispatcher::State ProcessDispatcher::state() const {
  Guard<CriticalMutex> guard{get_lock()};
  return state_;
}

fbl::RefPtr<JobDispatcher> ProcessDispatcher::job() { return job_; }

void ProcessDispatcher::SetStateLocked(State s) {
  LTRACEF("process %p: state %u (%s)\n", this, static_cast<unsigned int>(s), StateToString(s));

  DEBUG_ASSERT(get_lock()->lock().IsHeld());

  // look for some invalid state transitions
  if (state_ == State::DEAD && s != State::DEAD) {
    panic("ProcessDispatcher::SetStateLocked invalid state transition from DEAD to !DEAD\n");
    return;
  }

  // transitions to your own state are okay
  if (s == state_)
    return;

  state_ = s;

  if (s == State::DYING) {
    // send kill to all of our threads
    KillAllThreadsLocked();
  }
}

// Finish processing of the transition to State::DEAD. Some things need to be done
// outside of holding |get_lock()|. Beware this is called from several places
// including on_zero_handles().
void ProcessDispatcher::FinishDeadTransition() {
  DEBUG_ASSERT(!completely_dead_);
  completely_dead_ = true;

  // It doesn't matter whether the lock is held or not while shutting down
  // the exceptionates, this is just the most convenient place to do it.
  exceptionate_.Shutdown();
  debug_exceptionate_.Shutdown();

  // clean up shared state, including the handle table
  LTRACEF_LEVEL(2, "removing shared state reference from proc %p\n", this);
  shared_state_->DecrementShareCount();

  // Tear down the restricted address space. It may not exist if Initialize() failed or if this
  // process was not created with one.
  if (restricted_aspace_) {
    zx_status_t result = restricted_aspace_->Destroy();
    ASSERT_MSG(result == ZX_OK, "%d\n", result);
  }

  // signal waiter
  LTRACEF_LEVEL(2, "signaling waiters\n");
  UpdateState(0u, ZX_TASK_TERMINATED);

  // Call job_->RemoveChildProcess(this) outside of |get_lock()|. Otherwise
  // we risk a deadlock as we have |get_lock()| and RemoveChildProcess grabs
  // the job's |lock_|, whereas JobDispatcher::EnumerateChildren obtains the
  // locks in the opposite order. We want to keep lock acquisition order
  // consistent, and JobDispatcher::EnumerateChildren's order makes
  // sense. We don't need |get_lock()| when calling RemoveChildProcess
  // here. fxbug.dev/30829
  job_->RemoveChildProcess(this);

  // If we are critical to a job, we need to take action. Similar to the above
  // comment, we avoid performing the actual call into the job whilst still
  // holding the lock.
  fbl::RefPtr<JobDispatcher> kill_job;
  {
    Guard<CriticalMutex> guard{get_lock()};
    if (critical_to_job_ != nullptr) {
      // Check if we accept any return code, or require it be non-zero.
      if (!retcode_nonzero_ || retcode_ != 0) {
        kill_job = critical_to_job_;
      }
    }
  }
  if (kill_job) {
    kill_job->CriticalProcessKill(fbl::RefPtr<ProcessDispatcher>(this));
  }
}

bool ProcessDispatcher::CriticalToRootJob() const {
  Guard<CriticalMutex> guard{get_lock()};
  return critical_to_job_ == GetRootJobDispatcher();
}

void ProcessDispatcher::GetInfo(zx_info_process_t* info) const {
  canary_.Assert();

  State state;
  zx_time_t start_time;
  int64_t return_code;
  zx_info_process_flags_t flags = 0u;
  // retcode_ depends on the state: make sure they're consistent.
  {
    Guard<CriticalMutex> guard{get_lock()};
    state = state_;
    start_time = start_time_;
    return_code = retcode_;
    // TODO: Protect with rights if necessary.
    if (debug_exceptionate_.HasValidChannel()) {
      flags |= ZX_INFO_PROCESS_FLAG_DEBUGGER_ATTACHED;
    }
  }

  switch (state) {
    case State::DEAD:
    case State::DYING:
      flags |= ZX_INFO_PROCESS_FLAG_EXITED;
      __FALLTHROUGH;
    case State::RUNNING:
      flags |= ZX_INFO_PROCESS_FLAG_STARTED;
      break;
    case State::INITIAL:
    default:
      break;
  }

  *info = zx_info_process_t{
      .return_code = return_code,
      .start_time = start_time,
      .flags = flags,
      .padding1 = {},
  };
}

zx_status_t ProcessDispatcher::GetStats(zx_info_task_stats_t* stats) const {
  DEBUG_ASSERT(stats != nullptr);
  Guard<CriticalMutex> guard{get_lock()};
  if (state_ == State::DEAD) {
    return ZX_ERR_BAD_STATE;
  }
  VmAspace::vm_usage_t usage;
  zx_status_t s = shared_state_->aspace()->GetMemoryUsage(&usage);
  if (s != ZX_OK) {
    return s;
  }

  if (restricted_aspace_) {
    VmAspace::vm_usage_t restricted_usage;
    zx_status_t sr = restricted_aspace_->GetMemoryUsage(&restricted_usage);
    usage.mapped_pages += restricted_usage.mapped_pages;
    usage.private_pages += restricted_usage.private_pages;
    usage.shared_pages += restricted_usage.shared_pages;
    usage.scaled_shared_bytes += restricted_usage.scaled_shared_bytes;
    if (sr != ZX_OK) {
      return sr;
    }
  }

  stats->mem_mapped_bytes = usage.mapped_pages * PAGE_SIZE;
  stats->mem_private_bytes = usage.private_pages * PAGE_SIZE;
  stats->mem_shared_bytes = usage.shared_pages * PAGE_SIZE;
  stats->mem_scaled_shared_bytes = usage.scaled_shared_bytes;
  return ZX_OK;
}

zx_status_t ProcessDispatcher::AccumulateRuntimeTo(zx_info_task_runtime_t* info) const {
  Guard<CriticalMutex> guard{get_lock()};
  aggregated_runtime_stats_.AccumulateRuntimeTo(info);
  for (const auto& thread : thread_list_) {
    zx_status_t err = thread.AccumulateRuntimeTo(info);
    if (err != ZX_OK) {
      return err;
    }
  }

  return ZX_OK;
}

zx_status_t ProcessDispatcher::GetAspaceMaps(user_out_ptr<zx_info_maps_t> maps, size_t max,
                                             size_t* actual_out, size_t* available_out) const {
  *actual_out = 0;
  *available_out = 0;

  // Do not check the state_ since we need to call GetVmAspaceMaps without the dispatcher lock held,
  // and so any check will become stale anyway. Should the process be dead, or transition to the
  // dead state during the operation, then the associated aspace will also be destroyed, which will
  // be noticed and result in a ZX_ERR_BAD_STATE being returned from GetVmAspaceMaps.
  size_t actual = 0;
  size_t available = 0;
  zx_status_t status = GetVmAspaceMaps(shared_state_->aspace(), maps, max, &actual, &available);
  DEBUG_ASSERT(max >= actual);
  if (status != ZX_OK) {
    return status;
  }

  size_t actual2 = 0;
  size_t available2 = 0;
  if (restricted_aspace_) {
    user_out_ptr<zx_info_maps_t> maps_offset = maps ? maps.element_offset(actual) : maps;
    status = GetVmAspaceMaps(restricted_aspace_, maps_offset, max - actual, &actual2, &available2);
  }

  *actual_out = actual + actual2;
  *available_out = available + available2;

  return status;
}

zx_status_t ProcessDispatcher::GetVmos(VmoInfoWriter& vmos, size_t max, size_t* actual_out,
                                       size_t* available_out) {
  {
    Guard<CriticalMutex> guard{get_lock()};
    if (state_ != State::RUNNING) {
      return ZX_ERR_BAD_STATE;
    }
  }

  size_t actual = 0;
  size_t available = 0;
  zx_status_t s = GetProcessVmos(this, vmos, max, &actual, &available);
  if (s != ZX_OK) {
    return s;
  }

  size_t actual2 = 0;
  size_t available2 = 0;
  DEBUG_ASSERT(max >= actual);
  vmos.AddOffset(actual);
  s = GetVmAspaceVmos(shared_state_->aspace(), vmos, max - actual, &actual2, &available2);
  if (s != ZX_OK) {
    return s;
  }

  size_t actual3 = 0;
  size_t available3 = 0;
  DEBUG_ASSERT(max >= actual + actual2);
  vmos.AddOffset(actual2);
  if (restricted_aspace_) {
    s = GetVmAspaceVmos(restricted_aspace_, vmos, max - (actual + actual2), &actual3, &available3);
    if (s != ZX_OK) {
      return s;
    }
  }

  *actual_out = actual + actual2 + actual3;
  *available_out = available + available2 + available3;
  return ZX_OK;
}

zx_status_t ProcessDispatcher::GetThreads(fbl::Array<zx_koid_t>* out_threads) const {
  Guard<CriticalMutex> guard{get_lock()};
  size_t n = thread_list_.size_slow();
  fbl::Array<zx_koid_t> threads;
  fbl::AllocChecker ac;
  threads.reset(new (&ac) zx_koid_t[n], n);
  if (!ac.check())
    return ZX_ERR_NO_MEMORY;
  size_t i = 0;
  for (auto& thread : thread_list_) {
    threads[i] = thread.get_koid();
    ++i;
  }
  DEBUG_ASSERT(i == n);
  *out_threads = ktl::move(threads);
  return ZX_OK;
}

zx_status_t ProcessDispatcher::SetCriticalToJob(fbl::RefPtr<JobDispatcher> critical_to_job,
                                                bool retcode_nonzero) {
  Guard<CriticalMutex> guard{get_lock()};

  if (critical_to_job_) {
    // The process is already critical to a job.
    return ZX_ERR_ALREADY_BOUND;
  }

  auto job_copy = job_;
  for (auto& job = job_copy; job; job = job->parent()) {
    if (job == critical_to_job) {
      critical_to_job_ = critical_to_job;
      break;
    }
  }

  if (!critical_to_job_) {
    // The provided job is not the parent of this process, or an ancestor.
    return ZX_ERR_INVALID_ARGS;
  }

  retcode_nonzero_ = retcode_nonzero;
  return ZX_OK;
}

Exceptionate* ProcessDispatcher::exceptionate() {
  canary_.Assert();
  return &exceptionate_;
}

Exceptionate* ProcessDispatcher::debug_exceptionate() {
  canary_.Assert();
  return &debug_exceptionate_;
}

uint32_t ProcessDispatcher::ThreadCount() const {
  canary_.Assert();

  Guard<CriticalMutex> guard{get_lock()};
  return static_cast<uint32_t>(thread_list_.size_slow());
}

VmObject::AttributionCounts ProcessDispatcher::PageCount() const {
  canary_.Assert();

  VmObject::AttributionCounts page_counts;
  Guard<CriticalMutex> guard{get_lock()};
  if (state_ != State::RUNNING) {
    return page_counts;
  }

  auto root_vmar = shared_state_->aspace()->RootVmar();
  if (root_vmar) {
    page_counts += root_vmar->AllocatedPages();
  }

  if (restricted_aspace_ != nullptr) {
    root_vmar = restricted_aspace_->RootVmar();
    if (root_vmar) {
      page_counts += root_vmar->AllocatedPages();
    };
  }
  return page_counts;
}

class FindProcessByKoid final : public JobEnumerator {
 public:
  FindProcessByKoid(zx_koid_t koid) : koid_(koid) {}
  FindProcessByKoid(const FindProcessByKoid&) = delete;

  // To be called after enumeration.
  fbl::RefPtr<ProcessDispatcher> get_pd() { return pd_; }

 private:
  bool OnProcess(ProcessDispatcher* process) final {
    if (process->get_koid() == koid_) {
      pd_ = fbl::RefPtr(process);
      // Stop the enumeration.
      return false;
    }
    // Keep looking.
    return true;
  }

  const zx_koid_t koid_;
  fbl::RefPtr<ProcessDispatcher> pd_ = nullptr;
};

// static
fbl::RefPtr<ProcessDispatcher> ProcessDispatcher::LookupProcessById(zx_koid_t koid) {
  FindProcessByKoid finder(koid);
  GetRootJobDispatcher()->EnumerateChildrenRecursive(&finder);
  return finder.get_pd();
}

fbl::RefPtr<ThreadDispatcher> ProcessDispatcher::LookupThreadById(zx_koid_t koid) {
  LTRACE_ENTRY_OBJ;
  Guard<CriticalMutex> guard{get_lock()};

  auto iter =
      thread_list_.find_if([koid](const ThreadDispatcher& t) { return t.get_koid() == koid; });
  return fbl::RefPtr(iter.CopyPointer());
}

uintptr_t ProcessDispatcher::get_debug_addr() const {
  Guard<CriticalMutex> guard{get_lock()};
  return debug_addr_;
}

zx_status_t ProcessDispatcher::set_debug_addr(uintptr_t addr) {
  Guard<CriticalMutex> guard{get_lock()};
  debug_addr_ = addr;
  return ZX_OK;
}

uintptr_t ProcessDispatcher::get_dyn_break_on_load() const {
  Guard<CriticalMutex> guard{get_lock()};
  return dyn_break_on_load_;
}

zx_status_t ProcessDispatcher::set_dyn_break_on_load(uintptr_t break_on_load) {
  Guard<CriticalMutex> guard{get_lock()};
  dyn_break_on_load_ = break_on_load;
  return ZX_OK;
}

zx_status_t ProcessDispatcher::EnforceBasicPolicy(uint32_t condition) {
  const auto action = policy_.QueryBasicPolicy(condition);
  switch (action) {
    case ZX_POL_ACTION_ALLOW:
      // Not calling IncrementCounter here because this is the common case (fast path).
      return ZX_OK;
    case ZX_POL_ACTION_DENY:
      JobPolicy::IncrementCounter(action, condition);
      return ZX_ERR_ACCESS_DENIED;
    case ZX_POL_ACTION_ALLOW_EXCEPTION:
      Thread::Current::SignalPolicyException(kPolicyIdToPolicyException[condition], 0u);
      JobPolicy::IncrementCounter(action, condition);
      return ZX_OK;
    case ZX_POL_ACTION_DENY_EXCEPTION:
      Thread::Current::SignalPolicyException(kPolicyIdToPolicyException[condition], 0u);
      JobPolicy::IncrementCounter(action, condition);
      return ZX_ERR_ACCESS_DENIED;
    case ZX_POL_ACTION_KILL:
      Kill(ZX_TASK_RETCODE_POLICY_KILL);
      JobPolicy::IncrementCounter(action, condition);
      // Because we've killed, this return value will never make it out to usermode. However,
      // callers of this method will see and act on it.
      return ZX_ERR_ACCESS_DENIED;
  };
  panic("unexpected policy action %u\n", action);
}

TimerSlack ProcessDispatcher::GetTimerSlackPolicy() const { return policy_.GetTimerSlack(); }

TaskRuntimeStats ProcessDispatcher::GetAggregatedRuntime() const {
  Guard<CriticalMutex> guard{get_lock()};
  return aggregated_runtime_stats_;
}

uintptr_t ProcessDispatcher::cache_vdso_code_address() {
  Guard<CriticalMutex> guard{get_lock()};
  vdso_code_address_ = normal_aspace()->vdso_code_address();
  return vdso_code_address_;
}

const char* StateToString(ProcessDispatcher::State state) {
  switch (state) {
    case ProcessDispatcher::State::INITIAL:
      return "initial";
    case ProcessDispatcher::State::RUNNING:
      return "running";
    case ProcessDispatcher::State::DYING:
      return "dying";
    case ProcessDispatcher::State::DEAD:
      return "dead";
  }
  return "unknown";
}

void ProcessDispatcher::OnProcessStartForJobDebugger(ThreadDispatcher* t,
                                                     const arch_exception_context_t* context) {
  auto job = job_;
  while (job) {
    if (job->ForEachDebuExceptionate([t, context](Exceptionate* exceptionate) {
          t->HandleSingleShotException(exceptionate, ZX_EXCP_PROCESS_STARTING, *context);
        }) != ZX_OK) {
      printf("KERN: failed to allocate memory to notify process starts in %lu\n", get_koid());
      break;
    }

    job = job->parent();
  }
}

fbl::RefPtr<VmAspace> ProcessDispatcher::aspace_at(vaddr_t va) {
  if (!restricted_aspace_) {
    // If there is no restricted aspace associated with the process, shortcut and return the normal
    // aspace. This ensures a valid VmAspace pointer is returned.
    return shared_state_->aspace();
  }

  const vaddr_t begin = restricted_aspace_->base();
  const vaddr_t end = begin + restricted_aspace_->size();

  if (va >= begin && va < end) {
    return restricted_aspace_;
  }

  return shared_state_->aspace();
}
