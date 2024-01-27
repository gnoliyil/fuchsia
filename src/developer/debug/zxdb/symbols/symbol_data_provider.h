// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYMBOL_DATA_PROVIDER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYMBOL_DATA_PROVIDER_H_

#include <lib/fit/function.h>
#include <lib/stdcompat/span.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <vector>

#include "src/developer/debug/shared/arch.h"
#include "src/developer/debug/shared/register_id.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/common/int128_t.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/lib/fxl/memory/ref_counted.h"

namespace zxdb {

class Err;
class SymbolContext;

// This interface is how the debugger backend provides memory and register data to the symbol system
// to evaluate expressions.
//
// By default, this class returns no information. In this form it can be used to evaluate
// expressions in contexts without a running process. To access data, most callers will want to use
// the implementation associated with a frame or a process.
//
// Registers are the most commonly accessed data type and they are often available synchronously. So
// the interface provides a synchronous main register getter function and a fallback asynchronous
// one. They are separated to avoid overhead of closure creation in the synchronous case, and to
// avoid having a callback that's never issued.
//
// This object is reference counted since evaluating a DWARF expression is asynchronous.
class SymbolDataProvider : public fxl::RefCountedThreadSafe<SymbolDataProvider> {
 public:
  using GetMemoryCallback = fit::callback<void(const Err&, std::vector<uint8_t>)>;

  using GetTLSSegmentCallback = fit::callback<void(ErrOr<uint64_t>)>;

  // The Err indicates whether the operation was successful. Common failure cases are the thread is
  // running or this register wasn't saved on the stack frame.
  using GetRegisterCallback = fit::callback<void(const Err&, std::vector<uint8_t>)>;

  // CAllback for multiple register values. The map will contain the requested register values when
  // the error is not set.
  using GetRegistersCallback =
      fit::callback<void(const Err&, std::map<debug::RegisterID, std::vector<uint8_t>>)>;

  using GetFrameBaseCallback = fit::callback<void(const Err&, uint64_t value)>;

  using WriteCallback = fit::callback<void(const Err&)>;

  virtual debug::Arch GetArch();

  // Returns a SymbolDataProvider that will retrieve register values from the entrypoint of the
  // current function.
  //
  // Returns null if there is no entrypoint (like maybe there's no current function) or can't have
  // registers retrieved from it. This data provider is used to evaluate DW_OP_entry_value
  // expressions.
  virtual fxl::RefPtr<SymbolDataProvider> GetEntryDataProvider() const;

  // Request for synchronous register data if possible.
  //
  // If the value is not synchronously known, the return value will be std::nullopt. In this case,
  // GetRegisterAsync() should be called to retrieve the value.
  //
  // The return value can be an empty view if the implementation knows synchronously that we don't
  // know the value. An example is an unsaved register in a non-topmost stack frame.
  //
  // On successful data return, the data is owned by the implementor and should not be saved.
  virtual std::optional<cpp20::span<const uint8_t>> GetRegister(debug::RegisterID id);

  // Request for register data with an asynchronous callback. The callback will be issued when the
  // register data is available.
  virtual void GetRegisterAsync(debug::RegisterID id, GetRegisterCallback callback);

  // A wrapper around GetRegister and GetRegisterAsync that collects all the requested register
  // values. The callback will be issued with all collected values. If all values are known
  // synchronously, the callback will be called reentrantly.
  void GetRegisters(const std::vector<debug::RegisterID>& regs, GetRegistersCallback cb);

  // Writes the given canonical register ID.
  //
  // This must be a canonical register as identified by debug::RegisterInfo::canonical_id, which
  // means that it's a whole hardware register and needs no shifting nor masking.
  virtual void WriteRegister(debug::RegisterID id, std::vector<uint8_t> data, WriteCallback cb);

  // Synchronously returns the frame base pointer if possible. As with GetRegister, if this is not
  // available the implementation should call GetFrameBaseAsync().
  //
  // The frame base is the DW_AT_frame_base for the current function. Often this will be the "base
  // pointer" register in the CPU, but could be other registers, especially if compiled without full
  // stack frames. Getting this value may involve evaluating another DWARF expression which may or
  // may not be asynchronous.
  virtual std::optional<uint64_t> GetFrameBase();

  // Asynchronous version of GetFrameBase.
  virtual void GetFrameBaseAsync(GetFrameBaseCallback callback);

  // Returns the canonical frame address of the current frame. Returns 0 if it is not known. See
  // Frame::GetCanonicalFrameAddress().
  virtual uint64_t GetCanonicalFrameAddress() const;

  // Synchronously returns the debug address for a symbol context if available.
  virtual std::optional<uint64_t> GetDebugAddressForContext(const SymbolContext& context) const;

  // Get the address of the TLS segment for the given context. The TLS segment is where thread-local
  // variables live.
  virtual void GetTLSSegment(const SymbolContext& symbol_context, GetTLSSegmentCallback cb);

  // Request to retrieve a memory block from the debugged process. On success, the implementation
  // will call the callback with the retrieved data pointer.
  //
  // It will read valid memory up to the maximum. It will do short reads if it encounters invalid
  // memory, so the result may be shorter than requested or empty (if the first byte is invalid).
  virtual void GetMemoryAsync(uint64_t address, uint32_t size, GetMemoryCallback callback);

  // Asynchronously writes to the given memory. The callback will be issued when the write is
  // complete.
  virtual void WriteMemory(uint64_t address, std::vector<uint8_t> data, WriteCallback cb);

  // Call function |fn| in the current target. The callback will be issued when the thread
  // controller has determined that the function has returned (before the thread controller is
  // actually finished, see FinishPhysicalFrameThreadController). Which allows the callback to call
  // GetReturnValue to determine the returned type.
  //
  // |fn| must be resolved to a concrete Function object before calling this function, see
  // ResolveFunction.
  virtual void MakeFunctionCall(const Function* fn, fit::callback<void(const Err&)> cb) const;

 protected:
  FRIEND_MAKE_REF_COUNTED(SymbolDataProvider);
  FRIEND_REF_COUNTED_THREAD_SAFE(SymbolDataProvider);

  SymbolDataProvider() = default;
  virtual ~SymbolDataProvider() = default;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYMBOL_DATA_PROVIDER_H_
