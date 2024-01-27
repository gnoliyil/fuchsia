// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <kernel/attribution.h>
#include <object/executor.h>

void Executor::Init() {
  // Create root job.
  root_job_ = JobDispatcher::CreateRootJob();
  if constexpr (KERNEL_BASED_MEMORY_ATTRIBUTION) {
    // Insert the kernel's attribution object as a child of the root job.
    fbl::RefPtr<AttributionObject> kernel = AttributionObject::GetKernelAttribution();
    kernel->AddToGlobalListWithKoid(root_job_->attribution_objects_end(), ZX_KOID_INVALID);
  }

  // Create handle.
  root_job_handle_ =
      Handle::Make(KernelHandle<JobDispatcher>{root_job_}, JobDispatcher::default_rights());
  ASSERT(root_job_handle_ != nullptr);
}

void Executor::StartRootJobObserver() {
  ASSERT(root_job_observer_.get() == nullptr);
  DEBUG_ASSERT(root_job_.get() != nullptr);

  fbl::AllocChecker ac;
  root_job_observer_ = ktl::make_unique<RootJobObserver>(&ac, root_job_, root_job_handle_.get());
  if (!ac.check()) {
    panic("root-job: failed to allocate observer\n");
  }

  // Initialize the memory watchdog.
  memory_watchdog_.Init(this);
}
