// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/frame_impl.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/developer/debug/zxdb/client/client_eval_context_impl.h"
#include "src/developer/debug/zxdb/client/frame_symbol_data_provider.h"
#include "src/developer/debug/zxdb/client/process_impl.h"
#include "src/developer/debug/zxdb/client/remote_api.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/client/thread_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_expr_eval.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/input_location.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/developer/debug/zxdb/symbols/variable_location.h"

namespace zxdb {

using debug::RegisterCategory;

FrameImpl::FrameImpl(Thread* thread, const debug_ipc::StackFrame& stack_frame, Location location)
    : Frame(thread->session()),
      thread_(thread),
      sp_(stack_frame.sp),
      cfa_(stack_frame.cfa),
      location_(std::move(location)),
      weak_factory_(this) {
  registers_[static_cast<size_t>(RegisterCategory::kGeneral)] = stack_frame.regs;
}

FrameImpl::~FrameImpl() = default;

Thread* FrameImpl::GetThread() const { return thread_; }

bool FrameImpl::IsInline() const { return false; }

const Frame* FrameImpl::GetPhysicalFrame() const { return this; }

const Location& FrameImpl::GetLocation() const { return location_; }

uint64_t FrameImpl::GetAddress() const { return location_.address(); }

const std::vector<debug::RegisterValue>* FrameImpl::GetRegisterCategorySync(
    RegisterCategory category) const {
  FX_DCHECK(category <= RegisterCategory::kLast);

  size_t category_index = static_cast<size_t>(category);
  FX_DCHECK(category_index < static_cast<size_t>(RegisterCategory::kLast));

  if (registers_[category_index])
    return &*registers_[category_index];
  return nullptr;
}

void FrameImpl::GetRegisterCategoryAsync(
    RegisterCategory category, bool always_request,
    fit::function<void(const Err&, const std::vector<debug::RegisterValue>&)> cb) {
  FX_DCHECK(category < RegisterCategory::kLast && category != RegisterCategory::kNone);

  size_t category_index = static_cast<size_t>(category);
  FX_DCHECK(category_index < static_cast<size_t>(RegisterCategory::kLast));

  if (!always_request && registers_[category_index]) {
    // Registers known already, asynchronously return the result.
    debug::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb), weak_frame = weak_factory_.GetWeakPtr(), category_index]() {
          if (weak_frame)
            cb(Err(), *weak_frame->registers_[category_index]);
          else
            cb(Err("Frame destroyed before registers could be retrieved."), {});
        });
    return;
  }

  // The CPU registers will always refer to the top physical frame so don't fetch them otherwise.
  if (!IsInTopmostPhysicalFrame()) {
    debug::MessageLoop::Current()->PostTask(FROM_HERE, [cb = std::move(cb)]() {
      cb(Err("This type of register is unavailable in non-topmost stack frames."), {});
    });
    return;
  }

  debug_ipc::ReadRegistersRequest request;
  request.id = {.process = thread_->GetProcess()->GetKoid(), .thread = thread_->GetKoid()};
  request.categories.push_back(category);

  session()->remote_api()->ReadRegisters(
      request, [weak_frame = weak_factory_.GetWeakPtr(), category, cb = std::move(cb)](
                   const Err& err, debug_ipc::ReadRegistersReply reply) mutable {
        if (!weak_frame)
          return cb(Err("Frame destroyed before registers could be retrieved."), {});

        weak_frame->registers_[static_cast<size_t>(category)] = reply.registers;
        cb(Err(), std::move(reply.registers));
      });
  return;
}

void FrameImpl::WriteRegister(debug::RegisterID id, std::vector<uint8_t> data,
                              fit::callback<void(const Err&)> cb) {
  const debug::RegisterInfo* info = debug::InfoForRegister(id);
  FX_DCHECK(info);                      // Should always be a valid register.
  FX_DCHECK(info->canonical_id == id);  // Should only write full canonical registers.

  if (!IsInTopmostPhysicalFrame()) {
    debug::MessageLoop::Current()->PostTask(FROM_HERE, [id, cb = std::move(cb)]() mutable {
      cb(Err("Register %s can't be written when the frame is not the topmost.",
             debug::RegisterIDToString(id)));
    });
    return;
  }

  debug_ipc::WriteRegistersRequest request;
  request.id = {.process = thread_->GetProcess()->GetKoid(), .thread = thread_->GetKoid()};
  request.registers.emplace_back(id, data);  // Don't move, used below.

  session()->remote_api()->WriteRegisters(
      request, [weak_frame = weak_factory_.GetWeakPtr(), data, cb = std::move(cb)](
                   const Err& err, debug_ipc::WriteRegistersReply reply) mutable {
        if (err.has_error())
          return cb(err);  // Transport error.

        if (reply.status.has_error()) {
          // Agent error.
          return cb(Err("Error writing register: " + reply.status.message()));
        }

        if (weak_frame)
          weak_frame->SaveRegisterUpdates(std::move(reply.registers));
        cb(Err());
      });
}

std::optional<uint64_t> FrameImpl::GetBasePointer() const {
  // This function is logically const even though EnsureBasePointer does some potentially mutating
  // things underneath (calling callbacks and such).
  if (const_cast<FrameImpl*>(this)->EnsureBasePointer()) {
    FX_DCHECK(computed_base_pointer_);
    return computed_base_pointer_;
  }
  return std::nullopt;
}

void FrameImpl::GetBasePointerAsync(fit::callback<void(uint64_t bp)> cb) {
  if (EnsureBasePointer()) {
    // BP available synchronously but we don't want to reenter the caller.
    FX_DCHECK(computed_base_pointer_);
    debug::MessageLoop::Current()->PostTask(
        FROM_HERE, [bp = *computed_base_pointer_, cb = std::move(cb)]() mutable { cb(bp); });
  } else {
    // Add pending request for when evaluation is complete.
    FX_DCHECK(base_pointer_eval_ && !base_pointer_eval_->is_complete());
    base_pointer_requests_.push_back(std::move(cb));
  }
}

uint64_t FrameImpl::GetStackPointer() const { return sp_; }

uint64_t FrameImpl::GetCanonicalFrameAddress() const { return cfa_; }

bool FrameImpl::IsInTopmostPhysicalFrame() const {
  const Stack& stack = GetThread()->GetStack();
  if (stack.empty())
    return false;

  // Search for the first physical frame, and return true if it or anything above it matches the
  // current frame.
  for (size_t i = 0; i < stack.size(); i++) {
    if (stack[i] == this)
      return true;
    if (!stack[i]->IsInline())
      break;
  }
  return false;
}

fxl::RefPtr<SymbolDataProvider> FrameImpl::GetSymbolDataProvider() const {
  if (!symbol_data_provider_) {
    symbol_data_provider_ =
        fxl::MakeRefCounted<FrameSymbolDataProvider>(const_cast<FrameImpl*>(this)->GetWeakPtr());
  }
  return symbol_data_provider_;
}

fxl::RefPtr<EvalContext> FrameImpl::GetEvalContext() const {
  auto language_setting =
      thread_->session()->system().settings().GetString(ClientSettings::System::kLanguage);

  std::optional<ExprLanguage> language = std::nullopt;
  if (language_setting == ClientSettings::System::kLanguage_Rust) {
    language = ExprLanguage::kRust;
  } else if (language_setting == ClientSettings::System::kLanguage_Cpp) {
    language = ExprLanguage::kC;
  } else {
    FX_DCHECK(language_setting == ClientSettings::System::kLanguage_Auto);
  }

  if (!symbol_eval_context_)
    symbol_eval_context_ = fxl::MakeRefCounted<ClientEvalContextImpl>(this, language);
  return symbol_eval_context_;
}

bool FrameImpl::IsAmbiguousInlineLocation() const {
  // This object always represents physical frames which aren't ambiguous.
  return false;
}

bool FrameImpl::EnsureBasePointer() {
  if (computed_base_pointer_)
    return true;  // Already have it available synchronously.

  if (base_pointer_eval_) {
    // Already happening asynchronously.
    FX_DCHECK(!base_pointer_eval_->is_complete());
    return false;
  }

  const Location& loc = GetLocation();
  if (!loc.symbol()) {
    // Unsymbolized.
    computed_base_pointer_ = 0;
    return true;
  }

  const Function* function = loc.symbol().Get()->As<Function>();
  const DwarfExpr* location_expr = nullptr;
  if (!function ||
      !(location_expr = function->frame_base().ExprForIP(loc.symbol_context(), GetAddress()))) {
    // No frame base declared for this function.
    computed_base_pointer_ = 0;
    return true;
  }

  // Try to evaluate the location.
  base_pointer_eval_ = std::make_unique<DwarfExprEval>(
      UnitSymbolFactory(function), GetSymbolDataProvider(), loc.symbol_context());

  // Callback when the expression is done. Will normally get called reentrantly by
  // DwarfExprEval::Eval().
  //
  // Binding |this| here is OK because the DwarfExprEval is owned by us and won't give callbacks
  // after it's destroyed.
  auto save_result = [this](DwarfExprEval* eval, const Err&) {
    // We don't currently report errors for frame base requests, but instead just fall back on
    // what was computed by the backend.
    computed_base_pointer_ = 0;
    if (eval->is_success() && eval->GetResultType() == DwarfExprEval::ResultType::kValue) {
      DwarfStackEntry result = eval->GetResult();
      if (result.TreatAsUnsigned())
        computed_base_pointer_ = result.unsigned_value();
    }

    // Issue callbacks for everybody waiting. Moving to a local here prevents weirdness if a
    // callback calls back into us, and also clears the vector.
    std::vector<fit::callback<void(uint64_t)>> callbacks = std::move(base_pointer_requests_);
    for (auto& cb : callbacks)
      cb(*computed_base_pointer_);
  };

  auto eval_result = base_pointer_eval_->Eval(*location_expr, std::move(save_result));

  // In the common case this will complete synchronously and the above callback will have put the
  // result into base_pointer_requests_ before this code is executed.
  return eval_result == DwarfExprEval::Completion::kSync;
}

void FrameImpl::SaveRegisterUpdates(std::vector<debug::RegisterValue> regs) {
  std::map<RegisterCategory, std::vector<debug::RegisterValue>> categorized;
  for (auto& reg : regs) {
    RegisterCategory cat = debug::RegisterIDToCategory(reg.id);
    FX_DCHECK(cat != RegisterCategory::kNone);
    categorized[cat].push_back(std::move(reg));
  }

  // This function replaces entire categories so we want to clear old registers as we go.
  for (auto& [cat, update] : categorized)
    registers_[static_cast<size_t>(cat)] = std::move(update);
}

}  // namespace zxdb
