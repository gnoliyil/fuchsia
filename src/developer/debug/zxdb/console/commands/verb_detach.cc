// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_detach.h"

#include "src/developer/debug/shared/zx_status.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/remote_api.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/common/join_callbacks.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/console/verbs.h"
#include "src/lib/fxl/strings/string_number_conversions.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

const char kDetachShortHelp[] = "detach: Detach from a process.";
const char kDetachHelp[] =
    R"(detach [pid]

  Detaches the debugger from a running process.
  The process will continue running.

Arguments

  pid
      Detach from a process from pid or tell the agent to release an
      uncoordinated process.

      Normally the client and the agent running on Fuchsia are coordinated.
      But there are some cases where the agent will be attached to some
      processes that the client is not aware of. This can happen either when:

      - You are reconnecting to a pre-running agent that was already attached.
      - There are processes waiting on an exception (Just In Time Debugging).

      In both cases, the client is unaware of these processes. Normally upon
      connection zxdb will inform you of these processes and you can query
      those with the "status" command.

      The user can connect to those processes by issuing an attach command or
      it can tell the agent to release them by issuing a detach command. The
      client will first look for any attached processes it is aware of and if
      not it will notify the agent to detach from this "unknown" processes.

Hints

  By default the current process is detached.
  To detach a different process prefix with "process N".

Examples

  detach
      Detaches from the current process.

  detach 1546
      Send a "detach from process 1546" message to the agent. It is not necessary for the client to
      be attached to this process.

  process 4 detach
      Detaches from process context 4.

  process * detach
      Detaches from all currently attached processes.
)";

void DetachFromAllTargets(const Command& cmd, fxl::RefPtr<CommandContext> cmd_context) {
  auto& system = cmd_context->GetConsoleContext()->session()->system();

  struct PackedJoinType {
    fxl::WeakPtr<Target> target;
    Err err;
  };

  auto joiner = fxl::MakeRefCounted<JoinCallbacks<PackedJoinType>>();

  for (auto target : system.GetTargets()) {
    target->Detach(
        [cb = joiner->AddCallback()](fxl::WeakPtr<Target> target, const Err& err) mutable {
          PackedJoinType pack{std::move(target), err};
          cb(pack);
        });
  }

  auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();
  joiner->Ready([out, &system](const std::vector<PackedJoinType>& results) {
    for (const auto& result : results) {
      FX_DCHECK(result.err.ok());
      // Ignore this return value. The last one will always have a warning since one target must
      // always be present. |DeleteTarget| handles that logic for us.
      system.DeleteTarget(result.target.get()).ok();
    }

    // The default, always existing target is an implementation detail that we hide from the user.
    // Tell them we detached from everything even though we didn't.
    out->Append("Detached from ");
    out->Append(std::to_string(results.size()));
    out->Complete(" processes.");
  });

  cmd_context->Output(out);
}

// Returns nullptr if there is no target attached to |process_koid|.
Target* SearchForAttachedTarget(ConsoleContext* context, uint64_t process_koid) {
  if (process_koid == 0)
    return nullptr;

  Target* target = nullptr;
  auto targets = context->session()->system().GetTargets();
  for (auto* target_ptr : targets) {
    auto* process = target_ptr->GetProcess();
    if (!process || process->GetKoid() != process_koid)
      continue;

    // We found a target that matches, mark that one as the one that has to detach.
    target = target_ptr;
    break;
  }

  return target;
}

void SendExplicitDetachMessage(uint64_t process_koid, fxl::RefPtr<CommandContext> cmd_context) {
  debug_ipc::DetachRequest request = {};
  request.koid = process_koid;

  cmd_context->GetConsoleContext()->session()->remote_api()->Detach(
      request, [process_koid, cmd_context](const Err& err, debug_ipc::DetachReply reply) {
        if (err.has_error())
          return cmd_context->ReportError(err);

        if (reply.status.has_error()) {
          return cmd_context->ReportError(Err("Could not detach from process " +
                                              std::to_string(process_koid) + ": " +
                                              reply.status.message()));
        }

        cmd_context->Output(
            fxl::StringPrintf("Successfully detached from %" PRIu64 ".", process_koid));
      });
}

void RunVerbDetach(const Command& cmd, fxl::RefPtr<CommandContext> cmd_context) {
  // Only a process can be detached.
  if (Err err = cmd.ValidateNouns({Noun::kProcess}, true); err.has_error())
    return cmd_context->ReportError(err);

  uint64_t process_koid = 0;

  if (cmd.args().size() == 1) {
    if (cmd.HasNoun(Noun::kProcess)) {
      return cmd_context->ReportError(
          Err(ErrType::kInput, "You can only specify PIDs without context."));
    } else if (cmd.args()[0] == "*") {
      // "detach *".
      DetachFromAllTargets(cmd, cmd_context);
      return;
    }

    process_koid = fxl::StringToNumber<uint64_t>(cmd.args()[0]);
  } else if (cmd.args().size() > 1) {
    return cmd_context->ReportError(Err(ErrType::kInput, "\"detach\" takes at most 1 argument."));
  }

  if (cmd.HasNoun(Noun::kProcess) && cmd.GetNounIndex(Noun::kProcess) == Command::kWildcard) {
    // "pr * detach".
    DetachFromAllTargets(cmd, cmd_context);
    return;
  }

  Target* target = SearchForAttachedTarget(cmd_context->GetConsoleContext(), process_koid);

  // If there is no suitable target and the user specified a pid to detach to, it means we need to
  // send an explicit detach message.
  if (!target && process_koid != 0) {
    SendExplicitDetachMessage(process_koid, cmd_context);
    return;
  }

  // Here we either found an attached target or we use the context one (because the user did not
  // specify a process koid to detach from).
  if (!target)
    target = cmd.target();

  // Only print something when there was an error detaching. The console context will watch for
  // Process destruction and print messages for each one in the success case.
  target->Detach([cmd_context](fxl::WeakPtr<Target> target, const Err& err) mutable {
    // The ConsoleContext displays messages for stopped processes, so don't display messages
    // when successfully detaching.
    ProcessCommandCallback(target, false, err, cmd_context);
  });
}

}  // namespace

VerbRecord GetDetachVerbRecord() {
  return VerbRecord(&RunVerbDetach, {"detach"}, kDetachShortHelp, kDetachHelp,
                    CommandGroup::kProcess);
}

}  // namespace zxdb
