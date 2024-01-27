// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_SCHEDULER_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_SCHEDULER_H_

#include <lib/fit/function.h>
#include <lib/fxt/interned_string.h>
#include <lib/relaxed_atomic.h>
#include <lib/zircon-internal/macros.h>
#include <platform.h>
#include <stdint.h>
#include <zircon/syscalls/scheduler.h>
#include <zircon/syscalls/system.h>
#include <zircon/types.h>

#include <fbl/intrusive_pointer_traits.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/wavl_tree_best_node_observer.h>
#include <ffl/fixed.h>
#include <kernel/auto_lock.h>
#include <kernel/scheduler_state.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

// Forward declaration.
struct percpu;

// Ensure this define has a value when not defined globally by the build system.
#ifndef SCHEDULER_TRACING_LEVEL
#define SCHEDULER_TRACING_LEVEL 0
#endif

// Ensure this define has a value when not defined globally by the build system.
#ifndef SCHEDULER_QUEUE_TRACING_ENABLED
#define SCHEDULER_QUEUE_TRACING_ENABLED false
#endif

// Performance scale of a CPU relative to the highest performance CPU in the
// system.
using SchedPerformanceScale = ffl::Fixed<int64_t, 31>;

// Converts a userspace CPU performance scale to a SchedPerformanceScale value.
constexpr SchedPerformanceScale ToSchedPerformanceScale(zx_cpu_performance_scale_t value) {
  const size_t FractionaBits = sizeof(value.fractional_part) * 8;
  return ffl::FromRaw<FractionaBits>(uint64_t{value.integral_part} << FractionaBits |
                                     value.fractional_part);
}

// Converts a SchedPerformanceScale value to a userspace CPU performance scale.
constexpr zx_cpu_performance_scale_t ToUserPerformanceScale(SchedPerformanceScale value) {
  using UserScale = ffl::Fixed<uint64_t, 32>;
  const UserScale user_scale{value};
  const uint64_t integral = user_scale.Integral().raw_value() >> UserScale::Format::FractionalBits;
  const uint64_t fractional = user_scale.Fraction().raw_value();
  return {.integral_part = uint32_t(integral), .fractional_part = uint32_t(fractional)};
}

// Implements fair and deadline scheduling algorithms and manages the associated
// per-CPU state.
class Scheduler {
 public:
  // Default minimum granularity of time slices.
  static constexpr SchedDuration kDefaultMinimumGranularity = SchedMs(1);

  // Default target latency for a scheduling period.
  static constexpr SchedDuration kDefaultTargetLatency = SchedMs(8);

  // The threshold for cross-cluster work stealing. Queues with an estimated
  // runtime below this value are not stolen from if the target and destination
  // CPUs are in different logical clusters. In a performance-balanced system,
  // this tunable value approximates the cost of cross-cluster migration due to
  // cache misses, assuming a task has high cache affinity in its current
  // cluster. This tunable may be increased to limit cross-cluster spill over.
  static constexpr SchedDuration kInterClusterThreshold = SchedMs(2);

  // The threshold for early termination when searching for a CPU to place a
  // task. Queues with an estimated runtime below this value are sufficiently
  // unloaded. In a performance-balanced system, this tunable value approximates
  // the cost of intra-cluster migration due to cache misses, assuming a task
  // has high cache affinity with the last CPU it ran on. This tunable may be
  // increased to limit intra-cluster spill over.
  static constexpr SchedDuration kIntraClusterThreshold = SchedUs(25);

  // The per-CPU deadline utilization limit to attempt to honor when selecting a
  // CPU to place a task. It is up to userspace to ensure that the total set of
  // deadline tasks can honor this limit. Even if userspace ensures the total
  // set of deadline utilizations is within the total available processor
  // resources, when total utilization is high enough it may not be possible to
  // honor this limit due to the bin packing problem.
  static constexpr SchedUtilization kCpuUtilizationLimit{1};

  // The maximum deadline utilization permitted for a single thread. This limit
  // is applied when scaling the utilization of a deadline task to the relative
  // performance of a candidate target processor -- placing a task on a
  // processor that would cause the scaled thread utilization to exceed this
  // value is avoided if possible.
  static constexpr SchedUtilization kThreadUtilizationMax{1};

  // The adjustment rates of the exponential moving averages tracking the
  // expected runtimes of each thread.
  static constexpr ffl::Fixed<int32_t, 2> kExpectedRuntimeAlpha = ffl::FromRatio(1, 4);
  static constexpr ffl::Fixed<int32_t, 0> kExpectedRuntimeBeta = ffl::FromRatio(1, 1);

  Scheduler() = default;
  ~Scheduler() = default;

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Accessors for total weight and number of runnable tasks.
  SchedWeight GetTotalWeight() const TA_EXCL(queue_lock_);
  size_t GetRunnableTasks() const TA_EXCL(queue_lock_);

  // Dumps the state of the run queue to the specified output target.
  void Dump(FILE* output_target = stdout) TA_EXCL(thread_lock, queue_lock_);
  void DumpThreadLocked(FILE* output_target = stdout) TA_REQ(thread_lock) TA_EXCL(queue_lock_);

  // Returns the number of the CPU this scheduler instance is associated with.
  cpu_num_t this_cpu() const { return this_cpu_; }

  // Returns the index of the logical cluster of the CPU this scheduler instance
  // is associated with.
  size_t cluster() const { return cluster_; }

  // Returns the lock-free value of the predicted queue time for the CPU this
  // scheduler instance is associated with.
  SchedDuration predicted_queue_time_ns() const {
    return exported_total_expected_runtime_ns_.load();
  }

  // Returns the lock-free value of the predicted deadline utilization for the
  // CPU this scheduler instance is associated with.
  SchedUtilization predicted_deadline_utilization() const {
    return exported_total_deadline_utilization_.load();
  }

  // Returns the performance scale of the CPU this scheduler instance is
  // associated with.
  SchedPerformanceScale performance_scale() const { return performance_scale_; }

  // Returns the reciprocal performance scale of the CPU this scheduler instance
  // is associated with.
  SchedPerformanceScale performance_scale_reciprocal() const {
    return performance_scale_reciprocal_;
  }

  // Returns a pointer to the currently running thread, if any.
  Thread* active_thread() const TA_REQ(thread_lock) TA_EXCL(queue_lock_) {
    Guard<MonitoredSpinLock, IrqSave> guard{&queue_lock_, SOURCE_TAG};
    return active_thread_;
  }

  // Public entry points.

  static void InitializeThread(Thread* thread, int priority);
  static void InitializeThread(Thread* thread, const zx_sched_deadline_params_t& params);
  static void InitializeFirstThread(Thread* thread);
  static void RemoveFirstThread(Thread* thread);
  static void Block() TA_REQ(thread_lock);
  static void Yield() TA_REQ(thread_lock);
  static void Preempt() TA_REQ(thread_lock);
  static void Reschedule() TA_REQ(thread_lock);
  static void RescheduleInternal() TA_REQ(thread_lock);
  static void Unblock(Thread* thread) TA_REQ(thread_lock);
  static void Unblock(Thread::UnblockList thread_list) TA_REQ(thread_lock);
  static void UnblockIdle(Thread* idle_thread) TA_REQ(thread_lock);

  static void Migrate(Thread* thread) TA_REQ(thread_lock);
  static void MigrateUnpinnedThreads() TA_REQ(thread_lock);

  // TimerTick is called when the preemption timer for a CPU has fired.
  //
  // This function is logically private and should only be called by timer.cc.
  static void TimerTick(SchedTime now);

  // Set the inherited priority of a thread.
  static void InheritPriority(Thread* t, int priority)
      TA_REQ(t->get_lock(), preempt_disabled_token);

  // Set the priority of a thread and reset the boost value. This function might reschedule.
  // pri should be 0 <= to <= MAX_PRIORITY.
  static void ChangePriority(Thread* t, int priority) TA_REQ(t->get_lock(), preempt_disabled_token);

  // Set the deadline of a thread. This function might reschedule.
  // This requires: 0 < capacity <= relative_deadline <= period.
  static void ChangeDeadline(Thread* t, const zx_sched_deadline_params_t& params)
      TA_REQ(t->get_lock(), preempt_disabled_token);

  // Releases the lock held by the previous thread and acquires the lock
  // previously held by the current thread when it entered the scheduler. This
  // is called automatically by the scheduler when switching between threads,
  // however, this must be called manually by thread trampolines at some point
  // to release whichever lock was used for scheduler synchronization.
  static void LockHandoff();

  // Return the time at which the current thread should be preempted.
  //
  // May only be called with preemption disabled.
  static zx_time_t GetTargetPreemptionTime() TA_EXCL(thread_lock);

  // Updates the performance scales of the requested CPUs and returns the
  // effective values in place, which may be different than the requested values
  // if they are below the minimum safe values for the respective CPUs.
  //
  // Requires |count| <= num CPUs.
  static void UpdatePerformanceScales(zx_cpu_performance_info_t* info, size_t count)
      TA_EXCL(thread_lock);

  // Gets the performance scales of up to count CPUs. Returns the last values
  // requested by userspace, even if they have not yet taken effect.
  //
  // Requires |count| <= num CPUs.
  static void GetPerformanceScales(zx_cpu_performance_info_t* info, size_t count)
      TA_EXCL(thread_lock);

  // Gets the default performance scales of up to count CPUs. Returns the
  // initial values determined by the system topology, or 1.0 when no topology
  // is available.
  //
  // Requires |count| <= num CPUs.
  static void GetDefaultPerformanceScales(zx_cpu_performance_info_t* info, size_t count)
      TA_EXCL(thread_lock);

 private:
  // Allow percpu to init our cpu number and performance scale.
  friend struct percpu;
  // Load balancer test.
  friend struct LoadBalancerTestAccess;
  // Allow tests to modify our state.
  friend class LoadBalancerTest;

  static void LockHandoffInternal(Thread* thread) TA_NO_THREAD_SAFETY_ANALYSIS;

  // Sets the initial values of the CPU performance scales for this Scheduler
  // instance.
  void InitializePerformanceScale(SchedPerformanceScale scale);

  static inline void RescheduleMask(cpu_mask_t cpus_to_reschedule_mask) TA_REQ(thread_lock);

  static void ChangeWeight(Thread* thread, int priority, cpu_mask_t* cpus_to_reschedule_mask)
      TA_REQ(thread_lock, preempt_disabled_token);
  static void ChangeDeadline(Thread* thread, const SchedDeadlineParams& params,
                             cpu_mask_t* cpus_to_reschedule_mask)
      TA_REQ(thread_lock, preempt_disabled_token);
  static void InheritWeight(Thread* thread, int priority, cpu_mask_t* cpus_to_reschedule_mask)
      TA_REQ(thread_lock, preempt_disabled_token);

  // Specifies how to associate a thread with a Scheduler instance, update
  // metadata, and whether/where to place the thread in a run queue.
  enum class Placement {
    // Selects a place in the queue based on the current insertion time and
    // thread weight or deadline.
    Insertion,

    // Selects a place in the queue based on the original insertion time and
    // the updated (inherited or changed) weight or deadline on the same CPU.
    Adjustment,

    // Selects a place in the queue based on the original insertion time and
    // the updated time slice due to being preempted by another thread.
    Preemption,

    // Selects a place in the queue based on the insertion time in the original
    // queue adjusted for the new queue.
    Migration,

    // Updates the metadata to account for a stolen thread that was just taken
    // from a different queue. This is distinct from Migration in that the
    // thread is not also enqueued, it is run immediately by the new CPU.
    Association,
  };

  // Returns the current system time as a SchedTime value.
  static SchedTime CurrentTime() { return SchedTime{current_time()}; }

  // Returns the Scheduler instance for the current CPU.
  static Scheduler* Get();

  // Returns the Scheduler instance for the given CPU.
  static Scheduler* Get(cpu_num_t cpu);

  // Returns a CPU to run the given thread on.
  static cpu_num_t FindTargetCpu(Thread* thread);

  // Updates the thread's weight and updates state-dependent bookkeeping.
  static void UpdateWeightCommon(Thread* thread, int original_priority, SchedWeight weight,
                                 cpu_mask_t* cpus_to_reschedule_mask, PropagatePI propagate)
      TA_REQ(thread_lock, preempt_disabled_token);

  // Updates the thread's deadline and updates state-dependent bookkeeping.
  static void UpdateDeadlineCommon(Thread* thread, int original_priority,
                                   const SchedDeadlineParams& params,
                                   cpu_mask_t* cpus_to_reschedule_mask, PropagatePI propagate)
      TA_REQ(thread_lock, preempt_disabled_token);

  using EndTraceCallback = fit::inline_function<void(), sizeof(void*)>;

  // Common logic for reschedule API.
  void RescheduleCommon(SchedTime now, EndTraceCallback end_outer_trace = nullptr);

  // Evaluates the schedule and returns the thread that should execute,
  // updating the run queue as necessary.
  Thread* EvaluateNextThread(SchedTime now, Thread* current_thread, bool timeslice_expired,
                             SchedDuration total_runtime_ns,
                             Guard<MonitoredSpinLock, NoIrqSave>& queue_guard) TA_REQ(queue_lock_);

  // Adds a thread to the run queue tree. The thread must be active on this
  // CPU.
  void QueueThread(Thread* thread, Placement placement, SchedTime now = SchedTime{0},
                   SchedDuration total_runtime_ns = SchedDuration{0}) TA_REQ(queue_lock_);

  // Removes the thread at the head of the first eligible run queue.
  Thread* DequeueThread(SchedTime now, Guard<MonitoredSpinLock, NoIrqSave>& queue_guard)
      TA_REQ(queue_lock_);

  // Removes the thread at the head of the fair run queue and returns it.
  Thread* DequeueFairThread() TA_REQ(queue_lock_);

  // Removes the eligible thread with the earliest deadline in the deadline run
  // queue and returns it.
  Thread* DequeueDeadlineThread(SchedTime eligible_time) TA_REQ(queue_lock_);

  // Returns the eligible thread in the run queue with a deadline earlier than
  // the given deadline, or nullptr if one does not exist.
  Thread* FindEarlierDeadlineThread(SchedTime eligible_time, SchedTime finish_time)
      TA_REQ(queue_lock_);

  // Removes the eligible thread with a deadline earlier than the given deadline
  // and returns it or nullptr if one does not exist.
  Thread* DequeueEarlierDeadlineThread(SchedTime eligible_time, SchedTime finish_time)
      TA_REQ(queue_lock_);

  // Attempts to steal work from other busy CPUs. Returns nullptr if no work was
  // stolen, otherwise returns a pointer to the stolen thread that is now
  // associated with the local Scheduler instance.
  Thread* StealWork(SchedTime now) TA_EXCL(queue_lock_);

  // Returns the time that the next deadline task will become eligible or infinite
  // if there are no ready deadline tasks.
  SchedTime GetNextEligibleTime() TA_REQ(queue_lock_);

  // Calculates the timeslice of the thread based on the current run queue
  // state.
  SchedDuration CalculateTimeslice(Thread* thread) TA_REQ(queue_lock_);

  // Returns the completion time clamped to the start of the earliest deadline
  // thread that will become eligible in that time frame.
  SchedTime ClampToDeadline(SchedTime completion_time) TA_REQ(queue_lock_);

  // Returns the completion time clamped to the start of the earliest deadline
  // thread that will become eligible in that time frame and also has an earlier
  // deadline than the given finish time.
  SchedTime ClampToEarlierDeadline(SchedTime completion_time, SchedTime finish_time)
      TA_REQ(queue_lock_);

  // Updates the timeslice of the thread based on the current run queue state.
  // Returns the absolute deadline for the next time slice, which may be earlier
  // than the completion of the time slice if other threads could preempt the
  // given thread before the time slice is exhausted.
  SchedTime NextThreadTimeslice(Thread* thread, SchedTime now) TA_REQ(queue_lock_);

  // Updates the scheduling period based on the number of active threads.
  void UpdatePeriod() TA_REQ(queue_lock_);

  // Updates the global virtual timeline.
  void UpdateTimeline(SchedTime now) TA_REQ(queue_lock_);

  // Makes a thread active on this CPU's scheduler and inserts it into the
  // run queue tree.
  void Insert(SchedTime now, Thread* thread, Placement placement = Placement::Insertion)
      TA_REQ(queue_lock_);

  // Removes the thread from this CPU's scheduler. The thread must not be in
  // the run queue tree.
  void Remove(Thread* thread) TA_REQ(queue_lock_);

  // Removes a specific thread from its current RunQueue.  Note that the thread
  // must currently exist in its queue, it is an error otherwise.
  void EraseFromQueue(Thread* thread) TA_REQ(queue_lock_) {
    SchedulerState& state = thread->scheduler_state();
    DEBUG_ASSERT(state.InQueue());
    RunQueue& queue =
        state.discipline() == SchedDiscipline::Fair ? fair_run_queue_ : deadline_run_queue_;
    queue.erase(*thread);
  }

  // Returns true if there is at least one eligible deadline thread in the
  // run queue.
  inline bool IsDeadlineThreadEligible(SchedTime eligible_time) TA_REQ(queue_lock_) {
    return !deadline_run_queue_.is_empty() &&
           deadline_run_queue_.front().scheduler_state().start_time_ <= eligible_time;
  }

  // Updates the total expected runtime estimator and exports the atomic shadow
  // variable for cross-CPU readers.
  inline void UpdateTotalExpectedRuntime(SchedDuration delta_ns) TA_REQ(queue_lock_);

  // Updates to total deadline utilization estimator and exports the atomic
  // shadow variable for cross-CPU readers.
  inline void UpdateTotalDeadlineUtilization(SchedUtilization delta_ns) TA_REQ(queue_lock_);

  // Utilities to scale up or down the given value by the performace scale of the CPU.
  template <typename T>
  inline T ScaleUp(T value) const;
  template <typename T>
  inline T ScaleDown(T value) const;

  // Update trace counters which track the total number of runnable threads for a CPU
  inline void TraceTotalRunnableThreads() const TA_REQ(queue_lock_);

  // Returns a new flow id when flow tracing is enabled, zero otherwise.
  inline static uint64_t NextFlowId();

  // Traits type to adapt the WAVLTree to Thread with node state in the
  // scheduler_state member.
  struct TaskTraits {
    using KeyType = SchedulerState::KeyType;
    static KeyType GetKey(const Thread& thread) { return thread.scheduler_state().key(); }
    static bool LessThan(KeyType a, KeyType b) { return a < b; }
    static bool EqualTo(KeyType a, KeyType b) { return a == b; }
    static auto& node_state(Thread& thread) { return thread.scheduler_state().run_queue_node_; }
  };

  // Observer that maintains the subtree invariant min_finish_time as nodes are
  // added to and removed from the run queue.
  struct SubtreeMinTraits {
    static SchedTime GetValue(const Thread& node) { return node.scheduler_state().finish_time_; }
    static SchedTime GetSubtreeBest(const Thread& node) {
      return node.scheduler_state().min_finish_time_;
    }
    static bool Compare(SchedTime a, SchedTime b) { return a < b; }
    static void AssignBest(Thread& node, SchedTime val) {
      node.scheduler_state().min_finish_time_ = val;
    }
    static void ResetBest(Thread& target) {}
  };

  using SubtreeMinObserver = fbl::WAVLTreeBestNodeObserver<SubtreeMinTraits>;

  // Alias of the WAVLTree type for the run queue.
  using RunQueue = fbl::WAVLTree<TaskTraits::KeyType, Thread*, TaskTraits, fbl::DefaultObjectTag,
                                 TaskTraits, SubtreeMinObserver>;

  // Finds the next eligible thread in the given run queue.
  Thread* FindEarliestEligibleThread(RunQueue* run_queue, SchedTime eligible_time)
      TA_REQ(queue_lock_);

  // Finds the next eligible thread in the given run queue that also passes the
  // given predicate.
  template <typename Predicate>
  Thread* FindEarliestEligibleThread(RunQueue* run_queue, SchedTime eligible_time,
                                     Predicate&& predicate) TA_REQ(queue_lock_);

  // Emits queue event tracers for trace-based scheduler performance analysis.
  inline void TraceThreadQueueEvent(const fxt::InternedString& name, Thread* thread) const
      TA_REQ(queue_lock_);

  // Protects run queues and associated metadata for this Scheduler instance.
  // The queue lock is the bottom most lock in the system for the CPU it is
  // associated with: no other locks may be nested within a queue lock. A queue
  // lock may be acquired across CPUs, however, the no nesting rule still
  // applies.
  mutable DECLARE_SPINLOCK_WITH_TYPE(Scheduler, MonitoredSpinLock,
                                     lockdep::LockFlagsLeaf) queue_lock_;

  // Alias of the queue lock wrapper type.
  using QueueLock = decltype(queue_lock_);

  // Simplified static assertion on the wrapped queue lock type to avoid the more verbose template
  // parameters required by lockdep::AssertHeld.
  static void AssertHeld(const QueueLock& lock) TA_ASSERT(lock) TA_ASSERT(lock.lock()) {
    lock.lock().AssertHeld();
  }

  // The run queue of fair scheduled threads ready to run, but not currently running.
  TA_GUARDED(queue_lock_)
  RunQueue fair_run_queue_;

  // The run queue of deadline scheduled threads ready to run, but not currently running.
  TA_GUARDED(queue_lock_)
  RunQueue deadline_run_queue_;

  // Pointer to the thread actively running on this CPU.
  TA_GUARDED(queue_lock_)
  Thread* active_thread_{nullptr};

  // Monotonically increasing counter to break ties when queuing tasks with
  // the same key. This has the effect of placing newly queued tasks behind
  // already queued tasks with the same key. This is also necessary to
  // guarantee uniqueness of the key as required by the WAVLTree container.
  TA_GUARDED(queue_lock_)
  uint64_t generation_count_{0};

  // Count of the fair threads running on this CPU, including threads in the run
  // queue and the currently running thread. Does not include the idle thread.
  TA_GUARDED(queue_lock_)
  int32_t runnable_fair_task_count_{0};

  // Count of the deadline threads running on this CPU, including threads in the
  // run queue and the currently running thread. Does not include the idle
  // thread.
  TA_GUARDED(queue_lock_)
  int32_t runnable_deadline_task_count_{0};

  // Total weights of threads running on this CPU, including threads in the
  // run queue and the currently running thread. Does not include the idle
  // thread.
  TA_GUARDED(queue_lock_)
  SchedWeight weight_total_{0};

  // The value of |weight_total_| when the current thread was scheduled.
  // Provides a reference for determining whether the total weights changed
  // since the last reschedule.
  SchedWeight scheduled_weight_total_{0};

  // The global virtual time of this run queue.
  TA_GUARDED(queue_lock_)
  SchedTime virtual_time_{0};

  // The system time since the last update to the global virtual time.
  TA_GUARDED(queue_lock_)
  SchedTime last_update_time_ns_{0};

  // The system time that the current time slice started.
  SchedTime start_of_current_time_slice_ns_{0};

  // The system time that the current thread should be preempted. Initialized to
  // ZX_TIME_INFINITE to pass the assertion now < target_preemption_time_ns_ (or
  // else the current time slice is expired) on the first entry into the
  // scheduler.
  SchedTime target_preemption_time_ns_{ZX_TIME_INFINITE};

  // The sum of the expected runtimes of all active threads on this CPU. This
  // value is an estimate of the average queuimg time for this CPU, given the
  // current set of active threads.
  TA_GUARDED(queue_lock_)
  SchedDuration total_expected_runtime_ns_{0};

  // The sum of the worst case utilization of all active deadline threads on
  // this CPU.
  TA_GUARDED(queue_lock_)
  SchedUtilization total_deadline_utilization_{0};

  // Scheduling period in which every runnable task executes once in units of
  // minimum granularity.
  TA_GUARDED(queue_lock_)
  SchedDuration scheduling_period_grans_{kDefaultTargetLatency / kDefaultMinimumGranularity};

  // The smallest timeslice a thread is allocated in a single round.
  TA_GUARDED(queue_lock_)
  SchedDuration minimum_granularity_ns_{kDefaultMinimumGranularity};

  // The target scheduling period. The scheduling period is set to this value
  // when the number of tasks is low enough for the sum of all timeslices to
  // fit within this duration. This has the effect of increasing the size of
  // the timeslices under nominal load to reduce scheduling overhead.
  TA_GUARDED(queue_lock_)
  SchedDuration target_latency_grans_{kDefaultTargetLatency / kDefaultMinimumGranularity};

  // Performance scale of this CPU relative to the highest performance CPU. This
  // value is initially determined from the system topology, when available, and
  // by userspace performance/thermal management at runtime.
  SchedPerformanceScale performance_scale_{1};
  SchedPerformanceScale performance_scale_reciprocal_{1};

  // Performance scale requested by userspace. The operational performance scale
  // is updated to this value (possibly adjusted for the minimum allowed value)
  // on the next reschedule, after the current thread's accounting is updated.
  SchedPerformanceScale pending_user_performance_scale_{1};

  // Default performance scale, determined from the system topology, when
  // available.
  SchedPerformanceScale default_performance_scale_{1};

  // The CPU this scheduler instance is associated with.
  // NOTE: This member is not initialized to prevent clobbering the value set
  // by sched_early_init(), which is called before the global ctors that
  // initialize the rest of the members of this class.
  // TODO(eieio): Figure out a better long-term solution to determine which
  // CPU is associated with each instance of this class. This is needed by
  // non-static methods that are called from arbitrary CPUs, namely Insert().
  cpu_num_t this_cpu_;

  // The index of the logical cluster this CPU belongs to. CPUs with the same
  // logical cluster index have the best chance of good cache affinity with
  // respect to load distribution decisions.
  size_t cluster_{0};

  // Values exported for lock-free access across CPUs. These are mirrors of the
  // members of the same name without the exported_ prefix. This avoids
  // unnecessary atomic loads when updating the values using arithmetic
  // operations on the local CPU. These values are atomically readonly to other
  // CPUs.
  // TODO(eieio): Look at cache line alignment for these members to optimize
  // cache performance.
  RelaxedAtomic<SchedDuration> exported_total_expected_runtime_ns_{SchedNs(0)};
  RelaxedAtomic<SchedUtilization> exported_total_deadline_utilization_{SchedUtilization{0}};

  // Flow id counter for sched_latency flow events.
  inline static RelaxedAtomic<uint64_t> next_flow_id_{1};
};

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_SCHEDULER_H_
