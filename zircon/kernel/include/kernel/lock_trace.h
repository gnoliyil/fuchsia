// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_LOCK_TRACE_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_LOCK_TRACE_H_

#include <lib/ktrace.h>

#ifndef LOCK_TRACING_ENABLED
#define LOCK_TRACING_ENABLED false
#endif

#define LOCK_TRACE_VARIABLE_NAME(name) FXT_CONCATENATE(name, __COUNTER__)

#define LOCK_TRACE_DURATION(label, args...)           \
  ktrace::Scope LOCK_TRACE_VARIABLE_NAME(duration_) = \
      KTRACE_BEGIN_SCOPE_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#define LOCK_TRACE_DURATION_BEGIN(label, args...) \
  KTRACE_DURATION_BEGIN_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#define LOCK_TRACE_DURATION_END(label, args...) \
  KTRACE_DURATION_END_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#define LOCK_TRACE_FLOW_BEGIN(label, args...) \
  KTRACE_FLOW_BEGIN_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#define LOCK_TRACE_FLOW_STEP(label, args...) \
  KTRACE_FLOW_STEP_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#define LOCK_TRACE_FLOW_END(label, args...) \
  KTRACE_FLOW_END_ENABLE(LOCK_TRACING_ENABLED, "kernel:sched", label, ##args)

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_LOCK_TRACE_H_
