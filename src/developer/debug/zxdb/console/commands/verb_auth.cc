// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_auth.h"

#include "src/developer/debug/zxdb/client/symbol_server.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/console/verbs.h"

namespace zxdb {

namespace {

const char kAuthShortHelp[] = "auth: Authenticate with a symbol server. [deprecated]";
const char kAuthHelp[] =
    R"(DEPRECATED auth [credentials]

  OOB auth workflow is deprecated (go/oauth-oob-deprecation). To authenticate, please run the
  following command and restart zxdb

    rm -f ~/.fuchsia/debug/googleapi_auth && gcloud auth application-default login

  For more information, please see fxbug.dev/119250.
)";

void RunVerbAuth(const Command& cmd, fxl::RefPtr<CommandContext> cmd_context) {
  cmd_context->Output(
      "This command and the OOB auth workflow are deprecated (go/oauth-oob-deprecation). "
      "To authenticate, please run the following command and restart zxdb\n\n"
      "  rm -f ~/.fuchsia/debug/googleapi_auth && gcloud auth application-default login\n\n"
      "For more information, please see fxbug.dev/119250.");
}

}  // namespace

VerbRecord GetAuthVerbRecord() {
  return VerbRecord(&RunVerbAuth, {"auth"}, kAuthShortHelp, kAuthHelp, CommandGroup::kSymbol);
}

}  // namespace zxdb
