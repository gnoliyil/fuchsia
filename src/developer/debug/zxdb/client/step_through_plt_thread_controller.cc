// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/step_through_plt_thread_controller.h"

#include "src/developer/debug/zxdb/client/finish_thread_controller.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/function_step.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/stack.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/symbols/elf_symbol.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"

namespace zxdb {

StepThroughPltThreadController::StepThroughPltThreadController(fit::deferred_callback on_done)
    : ThreadController(std::move(on_done)), weak_factory_(this) {}

void StepThroughPltThreadController::InitWithThread(Thread* thread,
                                                    fit::callback<void(const Err&)> cb) {
  SetThread(thread);

  Stack& stack = thread->GetStack();
  if (stack.empty())
    return cb(Err("Can't step, no frames."));
  const Frame* top_frame = stack[0];

  // Catch returns from the PLT call. This will handle cases where the logic below fails to prevent
  // execution from running away from us.
  //
  // One case where the logic below fails is if the imported ELF function is redirected somewhere
  // else. Libc does this for some functions like memset() to redirect them to platform-specific
  // implementations like __memset_avx2_unaligned(). In this case, a breakpoint on $elf(memset)
  // will never be hit.
  //
  // GDB seems to have more specialized PLT trampoline handling code to actually get through the
  // import function and stop at the proper destination. As of this writing, LLDB has a similar
  // problem as this code does.
  catch_return_ = std::make_unique<FinishThreadController>(stack, 0);
  catch_return_->InitWithThread(thread, [](const Err&) {});

  // Extract the ELF PLT symbol for the current location (the thread should be stopped at a PLT
  // symbol when InitWithThread() is called).
  const Location& cur_loc = top_frame->GetLocation();
  if (!cur_loc.symbol()) {
    FX_NOTREACHED();
    return cb(Err("Expecting a PLT symbol to step through."));
  }
  const ElfSymbol* elf_sym = cur_loc.symbol().Get()->As<ElfSymbol>();
  if (!elf_sym || elf_sym->elf_type() != ElfSymbolType::kPlt) {
    FX_NOTREACHED();
    return cb(Err("Expecting a PLT symbol to step through."));
  }

  const std::string linkage_name = elf_sym->linkage_name();
  plt_address_ = cur_loc.address();

  // The PLT trampoline will have the same name as the destinaion symbols, they'll all be called,
  // for example, "open" and they'll all be a PLT type (so "$plt(open)" in zxdb naming).
  // Currently ELF symbol lookup only takes mangled names, so we need to construct an identifier
  // based on the linkage name.
  //
  // TODO(fxbug.dev/128935) this may fail in some cases because the destination symbol doesn't
  // match. To properly handle this we will need to do a lot of PLT trampoline-specific work.
  // Currently failures end up in the catch_return_ handler which has the effect of stepping out of
  // them.
  Identifier plt_name(IdentifierComponent(SpecialIdentifier::kPlt, linkage_name));
  FX_DCHECK(plt_name.components().size() == 1);  // Expect one component for all ELF symbols.

  // Get the elf symbol name because we don't want to just match PLT entries. Querying for
  // $elf(open) will also match $plt(open) because PLT symbols are a subset of ELF symbols. These
  // extra matches should be harmless: we'll filter out our current PLT symbols and other modules'
  // PLT entries for the same symbol just won't be hit.
  Identifier elf_name(IdentifierComponent(SpecialIdentifier::kElf, linkage_name));

  // We expect the function name to resolve to two locations: the current one (the calling PLT
  // entry) and the destination one. There might be additional ones if there are duplicate symbols
  // (yikes) or other modules importing the same function (normal) but if there is only one it's our
  // calling location and the destination is unresolved.
  //
  // We could pass the function name directly to the "Until" controller but it will also match
  // our current location and will hit when we try to continue.
  //
  // There is some extra logic in the breakpoint that the "until" controller makes about dynamically
  // loaded libraries (like if this PLT thunk actually causes a module to be loaded) that we may
  // want in the future. If that's the case, we may want to just pass the function name to the
  // "until" controller and reach into its breakpoint and disable the current location.
  auto found = thread->GetProcess()->GetSymbols()->ResolveInputLocation(InputLocation(elf_name));

  // Filter out the current IP.
  found.erase(
      std::remove_if(found.begin(), found.end(),
                     [ip = plt_address_](const Location& cur) { return cur.address() == ip; }),
      found.end());

  Log("Got %zu matches for ELF symbol %s, running 'until' there.", found.size(),
      plt_name.components()[0].name().c_str());

  // When no matches were found, the destination can never be hit. Using the "until" controller at
  // this point will be like continuing the program which will lose the current location. In this
  // case, give up. The return controller will catch the result so this will be like "step out".
  if (found.empty()) {
    cb(Err("Could not find destination of PLT trampoline."));
    return;
  }

  // Make the "until" controller run until the resulting address(s). It's important that this
  // forward asynchronous failures back to our callback parameter because the breakpoint set could
  // fail (for example, the code could be in the read-only vDSO) and we don't want execution to just
  // continue in that case.
  std::vector<InputLocation> input_locations;
  for (const auto& loc : found) {
    dest_addrs_.push_back(loc.address());
    input_locations.push_back(InputLocation(loc.address()));
  }

  until_ = std::make_unique<UntilThreadController>(std::move(input_locations));
  until_->InitWithThread(
      thread, [weak_this = weak_factory_.GetWeakPtr(), cb = std::move(cb)](const Err& err) mutable {
        if (err.has_error() && weak_this)
          weak_this->OnUntilControllerInitializationFailed();
        cb(err);
      });
}

ThreadController::ContinueOp StepThroughPltThreadController::GetContinueOp() {
  if (until_)
    return until_->GetContinueOp();

  // Fall back to single-stepping instructions if the until controller failed.
  return ContinueOp::StepInstruction();
}

ThreadController::StopOp StepThroughPltThreadController::OnThreadStop(
    debug_ipc::ExceptionType stop_type,
    const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) {
  if (catch_return_->OnThreadStop(stop_type, hit_breakpoints) == kStopDone) {
    // Caught the return for the PLT call before we thought we stepped through it, stop.
    Log("PLT stepping failed, it just stepped out of the call. Stopping.");
    return StopOp::kStopDone;
  }
  if (until_) {
    // Delegate to thread controller.
    Log("Checking with until controller to see if PLT stepping is complete.");
    return until_->OnThreadStop(stop_type, hit_breakpoints);
  }

  // We're single-stepping through the PLT, check against the addresses.
  if (!dest_addrs_.empty()) {
    Stack& stack = thread()->GetStack();
    if (stack.empty()) {
      Log("Unexpected empty stack");
      return kUnexpected;  // Agent sent bad state, give up trying to step.
    }
    const Frame* top_frame = stack[0];
    uint64_t ip = top_frame->GetAddress();

    for (auto addr : dest_addrs_) {
      if (addr == ip) {
        Log("Matched PLT destination for stepping.");
        return ThreadController::StopOp::kStopDone;
      }
    }

    Log("Continuing to single-step through PLT.");
    return ThreadController::StopOp::kContinue;
  }

  // If there were no address matches, we probably can't step through this. The catch_return_
  // controller will fire when the function returns, so this will will be the equivalent to "step
  // over."
  //
  // If the user wants to step into the PLT when this happens, they'll have to step by instruction.
  // For this use-case, it would be safer to stop here. But the most common time this fires is when
  // stepping over library functions that the user doesn't actually want to step into (this
  // controller is just being used as a sub-controller) and stopping is extremely annoying.
  Log("No destination for PLT step, continuing until return.");
  return ThreadController::StopOp::kContinue;
}

void StepThroughPltThreadController::OnUntilControllerInitializationFailed() {
  // The "until" controller failed to initialize. Most commonly this is because the breakpoint could
  // not be set because the destination memory is read-only (this will happen for syscalls which are
  // in the vDSO). Fall back to single-stepping through the trampoline.
  Log("Until controller failed, falling back to single-stepping through PLT.");
  until_ = nullptr;
}

}  // namespace zxdb
