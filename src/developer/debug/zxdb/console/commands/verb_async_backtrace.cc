// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_async_backtrace.h"

#include <string>
#include <string_view>

#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/target.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/common/string_util.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/format_location.h"
#include "src/developer/debug/zxdb/console/format_name.h"
#include "src/developer/debug/zxdb/console/format_node_console.h"
#include "src/developer/debug/zxdb/console/string_util.h"
#include "src/developer/debug/zxdb/console/verbs.h"
#include "src/developer/debug/zxdb/expr/expr.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/developer/debug/zxdb/expr/find_name.h"
#include "src/developer/debug/zxdb/expr/found_member.h"
#include "src/developer/debug/zxdb/expr/resolve_collection.h"
#include "src/developer/debug/zxdb/expr/resolve_ptr_ref.h"
#include "src/developer/debug/zxdb/expr/resolve_variant.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"

namespace zxdb {

namespace {

// NOTE: we only support async Rust on Fuchsia for now. Async backtraces in other programming
// languages with different designs will require different approaches.
//
// An asynchronous backtrace is fundamentally different from a synchronous backtrace in that
//
//   * An asynchronous backtrace is a tree of futures rather than a list of frames, as one future
//     could await multiple other futures.
//   * To show a synchronous backtrace, the unwinder walks the stack and uncovers register values
//     of previous frames. Then the debugger symbolizes those frames and could display variables by
//     reading the stack. To show an asynchronous backtrace, the debugger finds the main future,
//     reads its state, and applies the same to its awaitees recursively. Since Rust doesn't keep
//     the stack or register values for non-running futures, the information is limited to the state
//     stored in the memory.
//   * Since any data types in Rust can be futures and the implementation of their poll methods can
//     be arbitrary, it's impossible for us to find all futures and detect every dependency between
//     them. Instead, we only focus on the GenFuture types because they are generated from the async
//     functions and are usually the more interesting part during debugging.
//
// Some limitations of the current implementation are
//
//    * We only support the single-threaded executor.
//    * Only futures reachable from the main future can be printed.
//    * Only debug build is supported. In release build, main_future will be optimized out.
//
// These can be solved by inspecting the active_tasks field of the executor in each thread, instead
// of reading the main_future argument. However, that requires the ability to enumerate a HashMap.

constexpr int kVerbose = 1;
constexpr int kMoreVerbose = 2;

const char kAsyncBacktraceShortHelp[] = "async-backtrace / abt: Display all async tasks.";
const char kAsyncBacktraceHelp[] =
    R"(async-backtrace

  Alias: "abt"

  Print a tree of async tasks from the main future in the current thread.

Arguments

  -v
  --verbose
      Include extra information

  --more-verbose
      Include more extra information

Examples

  abt
  abt -v
  process 2 abt
)";

struct FormatFutureOptions {
  bool verbose = false;
  ConsoleFormatOptions variable;  // options to format variables
};

fxl::RefPtr<AsyncOutputBuffer> FormatFuture(const ExprValue& future,
                                            const FormatFutureOptions& options,
                                            const fxl::RefPtr<EvalContext>& context, int indent);

// Strip the template part of a type. This bypasses the complexity in ParsedIdentifier and should
// be sufficient.
std::string_view StripTemplate(std::string_view type_name) {
  return type_name.substr(0, type_name.find('<'));
}

fxl::RefPtr<AsyncOutputBuffer> FormatMessage(Syntax syntax, const std::string& str) {
  auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();
  out->Complete(syntax, str + '\n');
  return out;
}

fxl::RefPtr<AsyncOutputBuffer> FormatError(std::string msg, const Err& err = Err()) {
  if (err.has_error())
    msg += ": " + err.msg();
  return FormatMessage(Syntax::kError, msg);
}

fxl::RefPtr<AsyncOutputBuffer> FormatGenFuture(const ExprValue& future,
                                               const FormatFutureOptions& options,
                                               const fxl::RefPtr<EvalContext>& context,
                                               int indent) {
  auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();

  // GenFuture should be a tuple of a single enum.
  ErrOrValue enum_val = ResolveNonstaticMember(context, future, {"__0"});
  if (enum_val.has_error())
    return FormatError("Invalid GenFuture", enum_val.err());

  // Resolve the current value of GenFuture.
  fxl::RefPtr<DataMember> member;
  ErrOrValue varient_val = ResolveSingleVariantValue(context, enum_val.take_value(), &member);
  if (varient_val.has_error())
    return FormatError("Cannot resolve GenFuture", enum_val.err());

  // This should be a struct, e.g., async_rust::main::func::λ::Suspend0.
  ExprValue value = varient_val.take_value();

  // Print the name of the original function, and put the state in a comment.
  Identifier ident = value.type()->GetIdentifier();
  std::string state = ident.components()[ident.components().size() - 1].name();
  ident.components().resize(ident.components().size() - 2);
  out->Append(FormatIdentifier(ident, {}));

  if (!StringStartsWith(state, "Suspend"))
    out->Append(Syntax::kComment, " (" + state + ")");

  if (member->decl_line().is_valid()) {
    out->Append(" " + GetBullet() + " ");
    out->Append(
        FormatFileLine(member->decl_line(), context->GetProcessSymbols()->target_symbols()));
  }
  out->Append("\n");

  // Iterate data members and find the awaitee.
  std::optional<ExprValue> awaitee;
  std::set<std::string> printed;
  if (const Collection* coll = value.type()->As<Collection>()) {
    for (const auto& lazy_member : coll->data_members()) {
      const DataMember* member = lazy_member.Get()->As<DataMember>();
      // Skip compiler-generated data and static data.
      if (!member || member->artificial() || member->is_external())
        continue;

      std::string name = member->GetAssignedName();
      ErrOrValue val = ResolveNonstaticMember(context, value, FoundMember(coll, member));
      if (val.has_error())
        continue;
      if (name == "__awaitee") {
        awaitee = val.take_value();
      } else if (options.verbose && printed.emplace(name).second) {
        // For some reason Rust could repeat the same field twice.
        out->Append(std::string(indent + 2, ' '));
        out->Append(FormatValueForConsole(val.take_value(), options.variable, context, name));
        out->Append("\n");
      }
    }
  }
  if (awaitee) {
    out->Append(std::string(indent, ' ') + "└─ ");
    out->Append(FormatFuture(*awaitee, options, context, indent + 3));
  }
  out->Complete();
  return out;
}

fxl::RefPtr<AsyncOutputBuffer> FormatPollFn(const ExprValue& future,
                                            const FormatFutureOptions& options,
                                            const fxl::RefPtr<EvalContext>& context, int indent) {
  auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();
  bool has_printed_type = false;

  // f should be a lambda.
  ErrOrValue err_or_f = ResolveNonstaticMember(context, future, {"f"});
  if (err_or_f.has_error())
    return FormatError("Cannot read f from PollFn", err_or_f.err());

  ExprValue f = err_or_f.take_value();
  const Collection* f_coll = f.type()->As<Collection>();
  if (!f_coll)
    return FormatError("Wrong type for f in PollFn");

  for (const auto& lazy_member : f_coll->data_members()) {
    const DataMember* member = lazy_member.Get()->As<DataMember>();
    if (!member || member->artificial() || member->is_external())
      continue;
    std::string name = member->GetAssignedName();
    if (!has_printed_type) {
      if (StringStartsWith(name, "_ref")) {
        out->Append("select!\n", TextForegroundColor::kCyan);
      } else if (StringStartsWith(name, "_fut")) {
        out->Append("join!\n", TextForegroundColor::kCyan);
      }
      has_printed_type = true;
    }
    ErrOrValue member_val = ResolveNonstaticMember(context, f, FoundMember(f_coll, member));
    // Each member_val should be a future.
    if (member_val) {
      out->Append(std::string(indent, ' ') + "└─ ");
      out->Append(FormatFuture(member_val.take_value(), options, context, indent + 3));
    }
  }

  out->Complete();
  return out;
}

fxl::RefPtr<AsyncOutputBuffer> FormatFuse(const ExprValue& fuse, const FormatFutureOptions& options,
                                          const fxl::RefPtr<EvalContext>& context, int indent) {
  ErrOrValue inner = ResolveNonstaticMember(context, fuse, {"inner"});
  if (inner.has_error())
    return FormatError("Invalid Fuse (1)", inner.err());
  // |inner| should be an option.
  ErrOrValue some = ResolveSingleVariantValue(context, inner.value());
  if (some.has_error())
    return FormatError("Invalid Fuse (2)", some.err());
  if (some.value().type()->GetAssignedName() == "None")
    return FormatMessage(Syntax::kComment, "(terminated)");
  ErrOrValue future = ResolveNonstaticMember(context, some.value(), {"__0"});
  if (future.has_error())
    return FormatError("Invalid Fuse (3)", future.err());
  return FormatFuture(future.value(), options, context, indent);
}

fxl::RefPtr<AsyncOutputBuffer> FormatMaybeDone(const ExprValue& maybe_done,
                                               const FormatFutureOptions& options,
                                               const fxl::RefPtr<EvalContext>& context,
                                               int indent) {
  ErrOrValue some = ResolveSingleVariantValue(context, maybe_done);
  if (some.has_error())
    return FormatError("Invalid MaybeDone (1)", some.err());
  if (some.value().type()->GetAssignedName() == "Future") {
    ErrOrValue future = ResolveNonstaticMember(context, some.value(), {"__0"});
    if (future.has_error())
      return FormatError("Invalid MaybeDone (2)", future.err());
    return FormatFuture(future.value(), options, context, indent);
  }
  return FormatMessage(Syntax::kComment, "(" + some.value().type()->GetAssignedName() + ")");
}

fxl::RefPtr<AsyncOutputBuffer> FormatPin(const ExprValue& pin, const FormatFutureOptions& options,
                                         const fxl::RefPtr<EvalContext>& context, int indent) {
  ErrOrValue pointer = ResolveNonstaticMember(context, pin, {"pointer"});
  if (pointer.has_error())
    return FormatError("Invalid Pin", pointer.err());
  // Let FormatFuture to resolve pointer.
  return FormatFuture(pointer.value(), options, context, indent);
}

fxl::RefPtr<AsyncOutputBuffer> FormatFuture(const ExprValue& future,
                                            const FormatFutureOptions& options,
                                            const fxl::RefPtr<EvalContext>& context, int indent) {
  // Resolve pointers first.
  if (future.type()->As<ModifiedType>()) {
    auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();
    ResolvePointer(context, future, [=](ErrOrValue val) {
      if (val.has_error()) {
        out->Complete(FormatError("Fail to resolve pointer", val.err()));
      } else {
        out->Complete(FormatFuture(val.value(), options, context, indent));
      }
    });
    return out;
  }

  std::string_view type = StripTemplate(future.type()->GetFullName());
  if (type == "core::future::from_generator::GenFuture")
    return FormatGenFuture(future, options, context, indent);
  if (type == "futures_util::future::poll_fn::PollFn")
    return FormatPollFn(future, options, context, indent);
  if (type == "futures_util::future::future::fuse::Fuse")
    return FormatFuse(future, options, context, indent);
  if (type == "futures_util::future::maybe_done::MaybeDone")
    return FormatMaybeDone(future, options, context, indent);
  if (type == "core::pin::Pin")
    return FormatPin(future, options, context, indent);

  // General formatter.
  auto out = fxl::MakeRefCounted<AsyncOutputBuffer>();
  if (options.verbose) {
    out->Append(FormatValueForConsole(future, options.variable, context));
    out->Complete("\n");
  } else {
    out->Complete(std::string(type) + "\n", TextForegroundColor::kGray);
  }
  return out;
}

void OnStackReady(Stack& stack, fxl::RefPtr<CommandContext> cmd_context,
                  const FormatFutureOptions& options) {
  // Step 2: locate main_future.
  if (stack.empty()) {
    cmd_context->ReportError(
        Err("Cannot sync frames. Please ensure the thread is either suspended "
            "or blocked in an exception. Use \"pause\" to suspend it."));
    return;
  }
  for (size_t i = 0; i < stack.size(); i++) {
    if (!stack[i]->GetLocation().has_symbols())
      continue;
    if (StripTemplate(stack[i]->GetLocation().symbol().Get()->GetFullName()) ==
        "fuchsia_async::runtime::fuchsia::executor::local::LocalExecutor::run_singlethreaded") {
      auto eval_context = stack[i]->GetEvalContext();
      EvalExpression(
          "main_future", eval_context, false,
          [options, cmd_context, eval_context](const ErrOrValue& value) {
            if (value.has_error()) {
              cmd_context->ReportError(Err(
                  "Cannot locate main_future: %s. Note that only debug build is supported for now.",
                  value.err().msg().c_str()));
            } else {
              cmd_context->Output(FormatFuture(value.value(), options, eval_context, 0));
            }
          });
      return;
    }
  }
  cmd_context->ReportError(
      Err("Cannot locate the async executor on the stack. "
          "Note that only single-threaded executor is supported for now."));
}

void RunVerbAsyncBacktrace(const Command& cmd, fxl::RefPtr<CommandContext> cmd_context) {
  if (Err err = cmd.ValidateNouns({Noun::kProcess, Noun::kThread}); err.has_error())
    return cmd_context->ReportError(err);

  if (!cmd.thread())
    return cmd_context->ReportError(Err("There is no thread to show backtrace."));

  FormatFutureOptions options;
  if (cmd.HasSwitch(kMoreVerbose)) {
    options.verbose = true;
    options.variable.verbosity = ConsoleFormatOptions::Verbosity::kMedium;
    options.variable.wrapping = ConsoleFormatOptions::Wrapping::kSmart;
    options.variable.pointer_expand_depth = 3;
    options.variable.max_depth = 6;
  } else if (cmd.HasSwitch(kVerbose)) {
    options.verbose = true;
    options.variable.verbosity = ConsoleFormatOptions::Verbosity::kMinimal;
    options.variable.pointer_expand_depth = 1;
    options.variable.max_depth = 3;
  }

  // Step 1: obtain the (synchronous) stack.
  if (cmd.thread()->GetStack().has_all_frames()) {
    OnStackReady(cmd.thread()->GetStack(), std::move(cmd_context), options);
  } else {
    cmd.thread()->GetStack().SyncFrames([thread = cmd.thread(),
                                         cmd_context = std::move(cmd_context),
                                         options](const Err& err) mutable {
      if (err.has_error()) {
        cmd_context->ReportError(err);
      } else {
        OnStackReady(thread->GetStack(), std::move(cmd_context), options);
      }
    });
  }
}

}  // namespace

VerbRecord GetAsyncBacktraceVerbRecord() {
  VerbRecord abt(&RunVerbAsyncBacktrace, {"async-backtrace", "abt"}, kAsyncBacktraceShortHelp,
                 kAsyncBacktraceHelp, CommandGroup::kQuery);
  abt.switches.emplace_back(kVerbose, false, "verbose", 'v');
  abt.switches.emplace_back(kMoreVerbose, false, "more-verbose", 0);
  return abt;
}

}  // namespace zxdb
