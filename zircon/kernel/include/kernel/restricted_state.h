// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_RESTRICTED_STATE_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_RESTRICTED_STATE_H_

#include <lib/zx/result.h>
#include <zircon/syscalls-next.h>

#include <arch/regs.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>
#include <ktl/unique_ptr.h>

// Per thread state to support restricted mode.
// Intentionally kept simple to keep the amount of kernel/thread.h dependencies to a minimum.

class VmObjectPaged;
class VmMapping;

// Must provide a definition of struct ArchSavedNormalState
#include <arch/restricted.h>

// Encapsulates a thread's restricted mode state.
//
// Note everything in this class should be accessed only by the thread that it belongs to,
// since there is no internal locking for efficiency reasons.
class RestrictedState {
 public:
  static zx::result<ktl::unique_ptr<RestrictedState>> Create();

  ~RestrictedState();
  DISALLOW_COPY_ASSIGN_AND_MOVE(RestrictedState);

  bool in_restricted() const { return in_restricted_; }
  uintptr_t vector_ptr() const { return vector_ptr_; }
  uintptr_t context() const { return context_; }
  const ArchSavedNormalState& arch_normal_state() const { return arch_; }
  ArchSavedNormalState& arch_normal_state() { return arch_; }
  zx_restricted_state_t* state_ptr() const {
    return reinterpret_cast<zx_restricted_state_t*>(state_mapping_ptr_);
  }

  fbl::RefPtr<VmObjectPaged> vmo() const;

  void set_in_restricted(bool r) { in_restricted_ = r; }
  void set_vector_ptr(uintptr_t v) { vector_ptr_ = v; }
  void set_context(uintptr_t c) { context_ = c; }

  // Each arch must fill out the following routines prefixed with Arch:
  //
  // Prior to entering restricted mode, ask the arch layer to validate the saved register state is
  // valid. Return ZX_OK if valid.
  // Possible invalid states: program counter outside of user space, invalid processor flags, etc.
  static zx_status_t ArchValidateStatePreRestrictedEntry(const zx_restricted_state_t& state);

  // Just prior to entering restricted mode, give the arch layer a chance to save any state it
  // may need for the return trip back to normal mode into the ArchSavedNormalState state argument.
  // For example, the GS/FS base is saved here for x86.
  static void ArchSaveStatePreRestrictedEntry(ArchSavedNormalState& state);

  // Use an architcturally-specific mechanism to directly enter user space in restricted mode.
  // Does not return.
  [[noreturn]] static void ArchEnterRestricted(const zx_restricted_state_t& state);

  // Having just exited from restricted mode via a syscall, save the necessary restricted mode
  // state from a pointer to the syscall state saved by the exception handler.
  static void ArchSaveRestrictedSyscallState(zx_restricted_state_t& state,
                                             const syscall_regs_t& regs);

  // Enter normal mode at the address pointed to by vector_table with arguments code and context
  // in an architecturally specific register in an architecturally specific way.
  [[noreturn]] static void ArchEnterFull(const ArchSavedNormalState& arch_state,
                                         uintptr_t vector_table, uintptr_t context, uint64_t code);

  // Dump the architecturally specific state out of the restricted mode state
  static void ArchDump(const zx_restricted_state_t& state);

 private:
  RestrictedState(fbl::RefPtr<VmObjectPaged> state_vmo, fbl::RefPtr<VmMapping> state_mapping);

  bool in_restricted_ = false;
  uintptr_t vector_ptr_ = 0;
  uintptr_t context_ = 0;

  // Ref pointer to a vmo holding the restricted state and a kernel mapping of the first page.
  const fbl::RefPtr<VmObjectPaged> state_vmo_;
  const fbl::RefPtr<VmMapping> state_mapping_;

  // Cached pointer to the mapping, to avoid needing to deref the mapping on every access.
  void* const state_mapping_ptr_ = nullptr;

  // Arch specific part of the save state
  ArchSavedNormalState arch_;
};

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_RESTRICTED_STATE_H_
