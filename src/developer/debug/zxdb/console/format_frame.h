// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FRAME_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FRAME_H_

#include "src/developer/debug/zxdb/client/pretty_stack_manager.h"
#include "src/developer/debug/zxdb/console/async_output_buffer.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/format_location.h"
#include "src/developer/debug/zxdb/console/format_node_console.h"

namespace zxdb {

struct ConsoleFormatOptions;
struct FormatLocationOptions;
class Frame;
class OutputBuffer;
class PrettyStackManager;
class Thread;

struct FormatFrameOptions {
  enum Detail {
    kSimple,      // Show only function names and file/line information.
    kParameters,  // Additionally show function parameters.
    kVerbose,     // Additionally show IP/SP/BP.
  };

  Detail detail = kSimple;

  // Formatting for the function/file name.
  FormatLocationOptions loc;

  // Formatting options for function parameters if requested in the Detail.
  ConsoleFormatOptions variable;
};

struct FormatStackOptions {
  FormatFrameOptions frame;

  // If non-null, will be used to shorten the frame list.
  fxl::RefPtr<PrettyStackManager> pretty_stack;
};

// Generates the list of frames from the given Thread to the console. This will complete
// asynchronously. The current frame will automatically be queried and will be indicated.
//
// This will request the full frame list from the agent if it has not been synced locally.
//
// If force_update is set, the full frame list will be re-requested (even if a full stack is already
// available locally).
fxl::RefPtr<AsyncOutputBuffer> FormatStack(Thread* thread, bool force_update,
                                           const FormatStackOptions& opts);

// Formats one frame using the long format. Since the long format includes function parameters which
// are computed asynchronously, this returns an AsyncOutputBuffer.
//
// This does not append a newline at the end of the output.
fxl::RefPtr<AsyncOutputBuffer> FormatFrame(const Frame* frame, const FormatFrameOptions& opts,
                                           int id = -1);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FRAME_H_
