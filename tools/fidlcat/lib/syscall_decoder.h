// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_H_
#define TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_H_

#include <zircon/types.h>

#include <cstdint>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/client/thread_observer.h"
#include "src/lib/fidl_codec/semantic.h"
#include "src/lib/fidl_codec/wire_object.h"
#include "tools/fidlcat/lib/comparator.h"
#include "tools/fidlcat/lib/decoder.h"
#include "tools/fidlcat/lib/event.h"
#include "tools/fidlcat/lib/syscall_decoder_interface.h"
#include "tools/fidlcat/lib/type_decoder.h"

namespace fidlcat {

class InterceptingThreadObserver;
class Syscall;
class SyscallDecoder;
class SyscallDecoderDispatcher;
class SyscallDisplayDispatcher;

// Handles the decoding of a syscall argument. At the end, it holds the value
// of the argument in |value_| for basic types or in |loaded_values_| for
// buffers and structs.
class SyscallDecoderArgument {
 public:
  explicit SyscallDecoderArgument(uint64_t value) : value_(value) {}

  // For input basic arguments, the value is directly available.
  uint64_t value() const { return value_; }

  // For input struct or buffer arguments or for output arguments (any
  // argument which is Type*), we need to load the data from memory.
  // When we ask for the data, |loading_| is set to true (it's an asynchronous
  // load). When we receive the data, |loading_| stays at true and
  // |loaded_values_| is filled with the data bytes. If the size of
  // |loaded_values_| is less than expected, that means that we had a load
  // error.
  const std::vector<uint8_t>& loaded_values(Stage stage) const {
    return (stage == Stage::kEntry) ? entry_loaded_values_ : exit_loaded_values_;
  }
  std::vector<uint8_t>& loaded_values(Stage stage) {
    return (stage == Stage::kEntry) ? entry_loaded_values_ : exit_loaded_values_;
  }
  bool loading(Stage stage) const {
    return (stage == Stage::kEntry) ? entry_loading_ : exit_loading_;
  }
  void set_loading(Stage stage) {
    if (stage == Stage::kEntry) {
      entry_loading_ = true;
    } else {
      exit_loading_ = true;
    }
  }
  void clear_loading(Stage stage) {
    if (stage == Stage::kEntry) {
      entry_loading_ = false;
    } else {
      exit_loading_ = false;
    }
  }

 private:
  uint64_t value_;
  std::vector<uint8_t> entry_loaded_values_;
  std::vector<uint8_t> exit_loaded_values_;
  bool entry_loading_ = false;
  bool exit_loading_ = false;
};

class SyscallDecoderBuffer {
 public:
  SyscallDecoderBuffer() = default;

  const std::vector<uint8_t>& loaded_values() const { return loaded_values_; }
  std::vector<uint8_t>& loaded_values() { return loaded_values_; }
  bool loading() const { return loading_; }
  void set_loading() { loading_ = true; }
  void clear_loading() { loading_ = false; }

 private:
  std::vector<uint8_t> loaded_values_;
  bool loading_ = false;
};

// Handles the decoding of a syscall.
// The decoding starts when SyscallDecoder::Decode is called. Then all the
// decoding steps are executed one after the other (see the comments for Decode
// and the following methods).
class SyscallDecoder : public SyscallDecoderInterface {
 public:
  SyscallDecoder(SyscallDecoderDispatcher* dispatcher, InterceptingThreadObserver* thread_observer,
                 zxdb::Thread* thread, const Syscall* syscall, int64_t timestamp);

  zxdb::Thread* get_thread() const { return weak_thread_.get(); }
  const Syscall* syscall() const { return syscall_; }
  int64_t timestamp() const { return timestamp_; }
  const std::vector<zxdb::Location>& caller_locations() const { return caller_locations_; }
  uint64_t return_address() const { return return_address_; }
  uint64_t syscall_result_status() const { return syscall_return_event_->return_code(); }
  int pending_request_count() const { return pending_request_count_; }

  // True if the decoder has been aborted. That means that the process for this decoder
  // terminated but we have still some pending requests.
  bool aborted() const { return aborted_; }
  void set_aborted() { aborted_ = true; }

  std::stringstream& Error(DecoderError::Type type) {
    aborted_ = true;
    return error_.Set(type);
  }

  void SyscallDecodingError();

  // Load the value for a buffer or a struct (field or argument).
  void LoadMemory(uint64_t address, size_t size, std::vector<uint8_t>* destination);

  // Loads the value for a buffer, a struct or an output argument.
  void LoadArgument(Stage stage, int argument_index, size_t size) override;

  // True if the argument is loaded correctly.
  bool ArgumentLoaded(Stage stage, int argument_index, size_t size) const override {
    return decoded_arguments_[argument_index].loaded_values(stage).size() == size;
  }

  // Returns the value of an argument for basic types.
  uint64_t ArgumentValue(int argument_index) const override {
    if (static_cast<size_t>(argument_index) >= decoded_arguments_.size()) {
      return 0;
    }
    return decoded_arguments_[argument_index].value();
  }

  // Returns a pointer on the argument content for buffers, structs or
  // output arguments.
  uint8_t* ArgumentContent(Stage stage, int argument_index) override {
    if (static_cast<size_t>(argument_index) >= decoded_arguments_.size()) {
      return nullptr;
    }
    SyscallDecoderArgument& argument = decoded_arguments_[argument_index];
    if (argument.value() == 0) {
      return nullptr;
    }
    return argument.loaded_values(stage).data();
  }

  // Loads a buffer.
  void LoadBuffer(Stage stage, uint64_t address, size_t size) override;

  // True if the buffer is loaded correctly.
  bool BufferLoaded(Stage stage, uint64_t address, size_t size) override {
    return (address == 0) ? true
                          : buffers_[std::make_pair(stage, address)].loaded_values().size() == size;
  }

  // Returns a pointer on the loaded buffer.
  uint8_t* BufferContent(Stage stage, uint64_t address) override {
    return (address == 0) ? nullptr
                          : buffers_[std::make_pair(stage, address)].loaded_values().data();
  }

  // Display the argument.
  void Display(int argument_index, std::string_view name, SyscallType sub_type,
               std::ostream& os) const;

  // Asks for the full statck then calls DoDecode.
  void Decode();

  // Starts the syscall decoding. Gets the values of arguments which stay within
  // a register. Then calls LoadStack.
  void DoDecode();

  // Loads the needed stacks. Then calls LoadInputs.
  void LoadStack();

  // Loads the inputs for buffers and structs. Then call StepToReturnAddress.
  // If several things need to be loaded, they are loaded in parallel.
  void LoadInputs();

  // Puts a breakpoint at the return address (the address just after the call to
  // the syscall) and restarts the stopped thread. When the breakpoint is
  // reached, it calls LoadSyscallReturnValue.
  bool StepToReturnAddress();

  // Decodes the inputs, generates the invoked event if possible and uses the inputs.
  void DecodeInputs();

  // Reads the returned value of the syscall. Then calls LoadOutputs.
  void LoadSyscallReturnValue();

  // Loads the output arguments of the syscall. This is called several time:
  // each time something is loaded, we check if it unlocks a load. This is, for
  // example the case of an output buffer. For an output buffer, we already
  // know the pointer to the buffer (it was in the input arguments) but we need
  // to load the size (because the size is an output argument, we only have a
  // pointer to it). Once we have loaded the size, we are able to load the
  // buffer.
  // When everything is loaded, it calls DecodeAndDisplay.
  // If several independent things need to be loaded, they are loaded in
  // parallel.
  void LoadOutputs();

  // Decodes the outputs, generates the outputevent if possible and uses the outputs.
  void DecodeOutputs();

  // Destroys this object and remove it from the |syscall_decoders_| list in the
  // SyscallDecoderDispatcher. This function is called when the syscall display
  // has been done or if we had an error and no request is pending (|has_error_|
  // is true and |pending_request_count_| is zero).
  void Destroy();

 private:
  InterceptingThreadObserver* const thread_observer_;
  const fxl::WeakPtr<zxdb::Thread> weak_thread_;
  const Syscall* const syscall_;
  const int64_t timestamp_;
  std::vector<zxdb::Location> caller_locations_;
  uint64_t entry_sp_ = 0;
  uint64_t return_address_ = 0;
  std::vector<SyscallDecoderArgument> decoded_arguments_;
  std::map<std::pair<Stage, uint64_t>, SyscallDecoderBuffer> buffers_;

  // The value returned by the function as a fidl_codec::Value
  std::shared_ptr<ReturnEvent> syscall_return_event_;

  int pending_request_count_ = 0;
  bool input_arguments_loaded_ = false;
  bool aborted_ = false;
  DecoderError error_;
  // Keeps a reference on the events.
  std::shared_ptr<InvokedEvent> invoked_event_;
  std::shared_ptr<OutputEvent> output_event_;
};

}  // namespace fidlcat

#endif  // TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_H_
