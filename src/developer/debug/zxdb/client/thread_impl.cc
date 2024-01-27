// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/thread_impl.h"

#include <inttypes.h>
#include <lib/syslog/cpp/macros.h>

#include <iostream>
#include <limits>

#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/developer/debug/zxdb/client/breakpoint.h"
#include "src/developer/debug/zxdb/client/breakpoint_observer.h"
#include "src/developer/debug/zxdb/client/frame_impl.h"
#include "src/developer/debug/zxdb/client/process_impl.h"
#include "src/developer/debug/zxdb/client/remote_api.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/client/target_impl.h"
#include "src/developer/debug/zxdb/client/thread_controller.h"
#include "src/developer/debug/zxdb/common/join_callbacks.h"
#include "src/developer/debug/zxdb/expr/cast.h"
#include "src/developer/debug/zxdb/expr/expr.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"

namespace zxdb {

namespace {

// Maximum number of times we'll allow thread controller to return "kFuture" before we declare
// they're in an infinite loop. Normally there will only be one "future" return before calling
// ResumeFromAsyncThreadController() so if we get many it probably indicates a thread controller is
// continuing to return the "future" from the same state and it's stuck (this is an easy mistake).
constexpr int kMaxNestedFutureCompletion = 16;

}  // namespace

ThreadImpl::ThreadImpl(ProcessImpl* process, const debug_ipc::ThreadRecord& record)
    : Thread(process->session()),
      process_(process),
      koid_(record.id.thread),
      stack_(this),
      weak_factory_(this) {
  SetMetadata(record);
  settings_.set_fallback(&process_->target()->settings());

  // Should be at the bottom of this function. This will enable notifications now that
  // initialization is complete.
  allow_notifications_ = true;
}

ThreadImpl::~ThreadImpl() = default;

Process* ThreadImpl::GetProcess() const { return process_; }

uint64_t ThreadImpl::GetKoid() const { return koid_; }

const std::string& ThreadImpl::GetName() const { return name_; }

std::optional<debug_ipc::ThreadRecord::State> ThreadImpl::GetState() const { return state_; }
debug_ipc::ThreadRecord::BlockedReason ThreadImpl::GetBlockedReason() const {
  return blocked_reason_;
}

void ThreadImpl::Pause(fit::callback<void()> on_paused) {
  // The frames may have been requested when the thread was running which will have marked them
  // "empty but complete." When a pause happens the frames will become available so we want
  // subsequent requests to request them.
  ClearState();

  debug_ipc::PauseRequest request;
  request.ids.push_back({.process = process_->GetKoid(), .thread = koid_});

  session()->remote_api()->Pause(
      request, [weak_thread = weak_factory_.GetWeakPtr(), on_paused = std::move(on_paused)](
                   const Err& err, debug_ipc::PauseReply reply) mutable {
        if (!err.has_error() && weak_thread) {
          // Save the new metadata.
          if (reply.threads.size() == 1 && reply.threads[0].id.thread == weak_thread->koid_) {
            weak_thread->SetMetadata(reply.threads[0]);
          } else {
            // If the client thread still exists, the agent's record of that thread should have
            // existed at the time the message was sent so there should be no reason the update
            // doesn't match.
            FX_NOTREACHED();
          }
        }
        on_paused();
      });
}

void ThreadImpl::Continue(bool forward_exception) {
  debug_ipc::ResumeRequest request;
  request.ids.push_back({.process = process_->GetKoid(), .thread = koid_});

  if (controllers_.empty()) {
    request.how = forward_exception ? debug_ipc::ResumeRequest::How::kForwardAndContinue
                                    : debug_ipc::ResumeRequest::How::kResolveAndContinue;
  } else {
    // When there are thread controllers, ask the most recent one for how to continue.
    //
    // Theoretically we're running with all controllers at once and we want to stop at the first one
    // that triggers, which means we want to compute the most restrictive intersection of all of
    // them.
    //
    // This is annoying to implement and it's difficult to construct a situation where this would be
    // required. The controller that doesn't involve breakpoints is "step in range" and generally
    // ranges refer to code lines that will align. Things like "until" are implemented with
    // breakpoints so can overlap arbitrarily with other operations with no problem.
    //
    // A case where this might show up:
    //  1. Do "step into" which steps through a range of instructions.
    //  2. In the middle of that range is a breakpoint that's hit.
    //  3. The user does "finish." We'll ask the finish controller what to do and it will say
    //     "continue" and the range from step 1 is lost.
    // However, in this case probably does want to end up one stack frame back rather than several
    // instructions after the breakpoint due to the original "step into" command, so even when
    // "wrong" this current behavior isn't necessarily bad.
    controllers_.back()->Log("Continuing with this controller as primary.");
    ThreadController::ContinueOp op = controllers_.back()->GetContinueOp();
    if (op.synthetic_stop_) {
      // Synthetic stop. Skip notifying the backend and broadcast a stop notification for the
      // current state.
      controllers_.back()->Log("Synthetic stop.");
      debug::MessageLoop::Current()->PostTask(FROM_HERE, [thread = weak_factory_.GetWeakPtr()]() {
        if (thread) {
          StopInfo info;
          info.exception_type = debug_ipc::ExceptionType::kSynthetic;
          thread->OnException(info);
        }
      });
      return;
    } else {
      // Dispatch the continuation message.
      request.how = op.how;
      request.range_begin = op.range.begin();
      request.range_end = op.range.end();
    }
  }

  ClearState();
  session()->remote_api()->Resume(request, [](const Err& err, debug_ipc::ResumeReply) {});
}

void ThreadImpl::ContinueWith(std::unique_ptr<ThreadController> controller,
                              fit::callback<void(const Err&)> on_continue) {
  ThreadController* controller_ptr = controller.get();

  // Add it first so that its presence will be noted by anything its initialization function does.
  controllers_.push_back(std::move(controller));

  controller_ptr->InitWithThread(
      this, [this, controller_ptr, on_continue = std::move(on_continue)](const Err& err) mutable {
        if (err.has_error()) {
          controller_ptr->Log("InitWithThread failed: %s", err.msg().c_str());
          NotifyControllerDone(controller_ptr);  // Remove the controller.
        } else {
          controller_ptr->Log("Initialized, continuing...");
          Continue(false);
        }
        on_continue(err);
      });
}

void ThreadImpl::AddPostStopTask(PostStopTask task) {
  // This function must only be called from a ThreadController::OnThreadStop() handler.
  FX_DCHECK(handling_on_stop_);
  post_stop_tasks_.push_back(std::move(task));
}

void ThreadImpl::CancelAllThreadControllers() {
  controllers_.clear();
  if (nested_stop_future_completion_) {
    // We're waiting on an async thread controller to complete but just cleared them all. Reissue
    // the exception to clean up the async state and issue stop notifications.
    OnException(async_stop_info_);
  }
}

void ThreadImpl::ResumeFromAsyncThreadController(std::optional<debug_ipc::ExceptionType> type) {
  bool debug_stepping = settings().GetBool(ClientSettings::Thread::kDebugStepping);
  if (debug_stepping)
    printf("↓↓↓↓↓↓↓↓↓↓ Resuming from async thread controller.\r\n");

  if (nested_stop_future_completion_ == 0) {
    // Not waiting on an async thread controller to finish. This could be a programming error but it
    // could also be that somebody called CancelAllThreadControllers() out from under us.
    if (debug_stepping)
      printf("No async stepping in progress, giving up.\r\n");
    return;
  }

  if (type)
    async_stop_info_.exception_type = *type;

  OnException(async_stop_info_);
}

void ThreadImpl::JumpTo(uint64_t new_address, fit::callback<void(const Err&)> cb) {
  // The register to set.
  debug_ipc::WriteRegistersRequest request;
  request.id = {.process = process_->GetKoid(), .thread = koid_};
  request.registers.emplace_back(
      GetSpecialRegisterID(session()->arch(), debug::SpecialRegisterType::kIP), new_address);

  // The "jump" command updates the thread's location so we need to recompute the stack. So once the
  // jump is complete we re-request the thread's status.
  //
  // This could be made faster by requesting status immediately after sending the update so we don't
  // have to wait for two round-trips, but that complicates the callback logic and this feature is
  // not performance- sensitive.
  //
  // Another approach is to make the register request message able to optionally request a stack
  // backtrace and include that in the reply.
  session()->remote_api()->WriteRegisters(
      request, [thread = weak_factory_.GetWeakPtr(), cb = std::move(cb)](
                   const Err& err, debug_ipc::WriteRegistersReply reply) mutable {
        if (err.has_error()) {
          cb(err);  // Transport error.
        } else if (reply.status.has_error()) {
          cb(Err("Could not set thread instruction pointer: " + reply.status.message()));
        } else if (!thread) {
          cb(Err("Thread destroyed."));
        } else {
          // Success, update the current stack before issuing the callback.
          thread->SyncFramesForStack(std::move(cb));
        }
      });
}

void ThreadImpl::NotifyControllerDone(ThreadController* controller) {
  controller->Log("Controller done, removing.");

  // We expect to have few controllers so brute-force is sufficient.
  for (auto cur = controllers_.begin(); cur != controllers_.end(); ++cur) {
    if (cur->get() == controller) {
      controllers_.erase(cur);
      return;
    }
  }
  FX_NOTREACHED();  // Notification for unknown controller.
}

void ThreadImpl::StepInstructions(uint64_t count) {
  debug_ipc::ResumeRequest request;
  request.ids.push_back({.process = process_->GetKoid(), .thread = koid_});
  request.how = debug_ipc::ResumeRequest::How::kStepInstruction;
  request.count = count;
  session()->remote_api()->Resume(request, [](const Err& err, debug_ipc::ResumeReply) {});
}

const Stack& ThreadImpl::GetStack() const { return stack_; }

Stack& ThreadImpl::GetStack() { return stack_; }

void ThreadImpl::SetMetadata(const debug_ipc::ThreadRecord& record) {
  FX_DCHECK(koid_ == record.id.thread);

  name_ = record.name;
  state_ = record.state;
  blocked_reason_ = record.blocked_reason;

  stack_.SetFrames(record.stack_amount, record.frames);
}

void ThreadImpl::OnException(const StopInfo& info) {
  if (settings().GetBool(ClientSettings::Thread::kDebugStepping)) {
    printf("----------\r\nGot %s exception @ 0x%" PRIx64 " in %s\r\n",
           debug_ipc::ExceptionTypeToString(info.exception_type), stack_[0]->GetAddress(),
           ThreadController::FrameFunctionNameForLog(stack_[0]).c_str());
  }

  if (stack_.empty()) {
    // Threads can stop with no stack if the thread is killed while processing an exception. If
    // this happens (or any other error that might cause an empty stack), declare all thread
    // controllers done since they can't meaningfully continue or process this state, and forcing
    // them all to separately check for an empty stack is error-prone.
    controllers_.clear();
  }

  // Debug tracking for proper usage from OnThreadStop handlers.
  handling_on_stop_ = true;

  // When any controller says "stop" it takes precedence and the thread will stop no matter what
  // any other controllers say.
  bool should_stop = false;

  // Set when any controller says "continue". If no controller says "stop" we need to differentiate
  // the case where there are no controllers or all controllers say "unexpected" (thread should
  // stop), from where one or more said "continue" (thread should continue, any "unexpected" votes
  // are ignored).
  bool have_continue = false;

  auto controller_iter = controllers_.begin();
  while (controller_iter != controllers_.end()) {
    ThreadController* controller = controller_iter->get();
    switch (controller->OnThreadStop(info.exception_type, info.hit_breakpoints)) {
      case ThreadController::kContinue:
        // Try the next controller.
        controller->Log("Reported continue on exception.");
        have_continue = true;
        controller_iter++;
        break;
      case ThreadController::kStopDone:
        // Once a controller tells us to stop, we assume the controller no longer applies and delete
        // it.
        //
        // Need to continue with checking all controllers even though we know we should stop at this
        // point. Multiple controllers should say "stop" at the same time and we need to be able to
        // delete all that no longer apply (say you did "finish", hit a breakpoint, and then
        // "finish" again, both finish commands would be active and you would want them both to be
        // completed when the current frame actually finishes).
        controller->Log("Reported stop on exception, stopping and removing it.");
        controller_iter = controllers_.erase(controller_iter);
        should_stop = true;
        break;
      case ThreadController::kUnexpected:
        // An unexpected exception means the controller is still active but doesn't know what to do
        // with this exception.
        controller->Log("Reported unexpected exception.");
        controller_iter++;
        break;
      case ThreadController::kFuture:
        controller->Log("Returned kFuture, waiting for async completion.");

        nested_stop_future_completion_++;
        if (nested_stop_future_completion_ >= kMaxNestedFutureCompletion) {
          // The thread controllers issued too many sequential "future" stop completions. It's easy
          // to accidentally get into an infinite loop by continuing to return kFuture from the same
          // stop type. This code detects that case and gives up.
          controller->Log(
              "Hit limit for nested 'future' thread controllers. Clearing state and stopping.");
          controllers_.clear();
          should_stop = true;
        } else {
          // Normal good case. Don't do anything and wait for the controller to call
          // ResumeFromAsyncThreadController() to continue.
          //
          // In this case we keep handling_on_stop_ true because we can continue to accumulate
          // post-stop tasks.
          async_stop_info_ = info;
          return;
        }
    }
  }

  handling_on_stop_ = false;
  nested_stop_future_completion_ = 0;

  if (!have_continue && !controllers_.empty()) {
    // No controller voted to continue (maybe all active controllers reported "unexpected"). Such
    // cases should stop.
    should_stop = true;
  }

  // This joiner is responsible for collecting all of the conditional breakpoint evaluation results
  // at this location. Only a single conditional breakpoint needs to evaluate to true to trigger a
  // stop. Unconditional breakpoints will always stop, and if there are only unconditional
  // breakpoints at this location, then this evaluation will happen synchronously.
  auto conditional_breakpoints_callback = fxl::MakeRefCounted<JoinCallbacks<bool>>();

  StopInfo external_info = info;

  // The existence of any non-internal, unconditional breakpoints being hit means the thread should
  // always stop. Breakpoints that have conditional expressions will be evaluated and stop only if
  // the condition is true. This check happens after notifying the controllers so if a controller
  // triggers, it's counted as a "hit" (otherwise, doing "run until" to a line with a normal
  // breakpoint on it would keep the "run until" operation active even after it was hit).
  //
  // Also, filter out internal breakpoints in the notification sent to the observers.
  for (auto it = external_info.hit_breakpoints.begin(); it != external_info.hit_breakpoints.end();
       /* nothing */) {
    if (*it && !it->get()->IsInternal()) {
      auto bp = it->get();

      if (const auto& cond = bp->GetSettings().condition; !cond.empty()) {
        ResolveConditionalBreakpoint(cond, bp, conditional_breakpoints_callback->AddCallback());
      } else {
        // Non-internal, unconditional breakpoints should always stop. Conditional breakpoints will
        // evaluate their expressions (there could be multiple, but only one has to vote to stop)
        // and then decide to stop or not.
        should_stop = true;
      }
      ++it;
    } else {
      // Remove deleted weak pointers and internal breakpoints.
      it = external_info.hit_breakpoints.erase(it);
    }
  }

  // If there are any conditional breakpoints that need to evaluate an expression, the callback(s)
  // will be issued asynchronously and collected into a vector. In the case no asynchronous
  // evaluations need to occur, this will return immediately and issue the callback above.
  conditional_breakpoints_callback->Ready([weak_this = weak_factory_.GetWeakPtr(), should_stop,
                                           external_info](std::vector<bool> results) mutable {
    if (weak_this) {
      // Non-debug exceptions (most likely a crash is happening) also mean the thread should
      // always stop (check this after running the controllers for the same reason as the
      // breakpoint check above).
      if (external_info.exception_type != debug_ipc::ExceptionType::kNone &&
          !debug_ipc::IsDebug(external_info.exception_type))
        should_stop = true;

      if (std::any_of(results.cbegin(), results.cend(), [](bool result) { return result; }))
        should_stop = true;

      // If there are no conditional breakpoints installed here, and there are no running
      // controllers, we should stop. This case catches things like __builtin_debugtrap() or
      // "int 3" on x86_64 architectures.
      if (!should_stop && results.empty() && weak_this->controllers_.empty())
        should_stop = true;

      // Execute the chain of post-stop tasks (may be asynchronous) and then dispatch the stop
      // notification or continue operation.
      weak_this->RunNextPostStopTaskOrNotify(external_info, should_stop);
    }
  });
}

void ThreadImpl::ResolveConditionalBreakpoint(const std::string& cond, Breakpoint* bp,
                                              fit::callback<void(bool)> cb) {
  // Update the evaluation context to the current stack frame and schedule the expression
  // evaluation.
  auto ctx = GetStack()[0]->GetEvalContext();
  EvalExpression(cond, ctx, true, [this, ctx, bp, cb = std::move(cb)](ErrOrValue val) mutable {
    std::string_view msg;

    if (val.ok()) {
      if (auto cast_result = CastNumericExprValueToBool(ctx, val.value()); cast_result.ok()) {
        // This is the normal good case. The expression resolved to a value that successfully
        // casted to a boolean. We're done.
        cb(cast_result.value());
        return;
      } else {
        // The expression evaluated successfully, but couldn't be cast to a bool.
        msg = cast_result.err().msg();
      }
    } else {
      // The expression couldn't be evaluated.
      msg = val.err().msg();
    }

    // If we get here, the conditional expression failed to evaluate somehow. Show a warning message
    // if the expression resolution fails for some reason. Note: we still want to stop execution
    // here so the user can fix whatever went wrong.
    Err err(
        "Hit conditional breakpoint, but expression evaluation failed:\n%s\nThis location "
        "could have been optimized out, or this variable might not exist in the current scope."
        "\nCheck the expression with `bp <id> get condition`, or modify the expression with "
        "`bp <id> set condition <expr>`.",
        msg.data());

    for (auto& observer : session()->breakpoint_observers()) {
      observer.OnBreakpointUpdateFailure(bp, err);
    }

    cb(true);
  });
}

void ThreadImpl::SyncFramesForStack(fit::callback<void(const Err&)> callback) {
  debug_ipc::ThreadStatusRequest request;
  request.id = {.process = process_->GetKoid(), .thread = koid_};

  session()->remote_api()->ThreadStatus(
      request, [callback = std::move(callback), thread = weak_factory_.GetWeakPtr()](
                   const Err& err, debug_ipc::ThreadStatusReply reply) mutable {
        if (err.has_error()) {
          callback(err);
          return;
        }

        if (!thread) {
          callback(Err("Thread destroyed."));
          return;
        }

        thread->SetMetadata(reply.record);
        callback(Err());
      });
}

std::unique_ptr<Frame> ThreadImpl::MakeFrameForStack(const debug_ipc::StackFrame& input,
                                                     Location location) {
  return std::make_unique<FrameImpl>(this, input, std::move(location));
}

Location ThreadImpl::GetSymbolizedLocationForAddress(uint64_t address) {
  auto vect = GetProcess()->GetSymbols()->ResolveInputLocation(InputLocation(address));

  // Symbolizing an address should always give exactly one result.
  FX_DCHECK(vect.size() == 1u);
  return vect[0];
}

void ThreadImpl::DidUpdateStackFrames() {
  if (allow_notifications_) {
    for (auto& observer : session()->thread_observers()) {
      observer.DidUpdateStackFrames(this);
    }
  }
}

void ThreadImpl::ClearState() {
  state_ = std::nullopt;
  blocked_reason_ = debug_ipc::ThreadRecord::BlockedReason::kNotBlocked;
  stack_.ClearFrames();
}

void ThreadImpl::RunNextPostStopTaskOrNotify(const StopInfo& info, bool should_stop) {
  // It's possible that the user is typing "pause" or "continue" during any asynchronous tasks
  // so the thread state doesn't match what we thought it was. Even though we haven't sent the
  // notifications, things still could have happened.
  //
  // Therefore, we don't do anything if the thread has started running from underneath us (it
  // should always be stopped when the thread controllers are notified unless the user has done
  // something), and cancel other pending stop tasks.
  //
  // The other half of the race condition is user has requested a manual stop while we were
  // processing these post-stop tasks and we shouldn't continue it. This needs extra logic to
  // detect.
  //
  // TODO(fxbug.dev/80418) don't automatically continue if the user has stopped the thread.
  if (state_ == debug_ipc::ThreadRecord::State::kRunning) {
    post_stop_tasks_.clear();
    return;
  }

  bool debug_stepping = settings().GetBool(ClientSettings::Thread::kDebugStepping);

  if (post_stop_tasks_.empty()) {
    // No post-stop tasks left to run, dispatch the stop notification or continue.
    if (should_stop) {
      // Stay stopped and notify the observers.
      if (debug_stepping)
        printf(" → Dispatching stop notification.\r\n");
      if (allow_notifications_) {
        for (auto& observer : session()->thread_observers()) {
          observer.OnThreadStopped(this, info);
        }
      }
    } else {
      // Controllers all say to continue.
      if (debug_stepping)
        printf(" → Sending continue request.\r\n");
      Continue(false);
    }
  } else {
    // Run the next post-stop task.
    PostStopTask task = std::move(post_stop_tasks_.front());
    post_stop_tasks_.pop_front();
    task(fit::defer_callback([weak_this = weak_factory_.GetWeakPtr(), info, should_stop]() mutable {
      if (weak_this)
        weak_this->RunNextPostStopTaskOrNotify(info, should_stop);
    }));
  }
}

}  // namespace zxdb
