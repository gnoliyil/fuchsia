// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_sym_stat.h"

#include <gtest/gtest.h>

#include "src/developer/debug/shared/platform_message_loop.h"
#include "src/developer/debug/zxdb/client/mock_remote_api.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/console/console_test.h"
#include "src/developer/debug/zxdb/symbols/loaded_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/mock_source_file_provider.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"

namespace zxdb {

namespace {

class VerbSymStat : public ConsoleTest {};

}  // namespace

TEST_F(VerbSymStat, SymStat) {
  console().ProcessInputLine("attach 1234");

  auto event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  ASSERT_EQ("Attached Process 2 state=Running koid=1234 name=<mock>\n", event.output.AsString());

  auto target = console().context().GetActiveTarget();
  ASSERT_NE(nullptr, target);
  ASSERT_NE(nullptr, target->GetProcess());

  target->GetProcess()->GetSymbols()->InjectModuleForTesting(
      "fakelib", "abc123", std::make_unique<LoadedModuleSymbols>(nullptr, "abc123", 0, 0));

  loop().RunUntilNoTasks();
  console().FlushOutputEvents();

  auto download = session().system().GetDownloadManager()->InjectDownloadForTesting("abc123");
  event = console().GetOutputEvent();
  EXPECT_EQ("Downloading symbols...", event.output.AsString());

  console().ProcessInputLine("sym-stat");

  event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);

  auto text = event.output.AsString();
  EXPECT_NE(text.find("Process 2 symbol status"), std::string::npos);
  EXPECT_NE(text.find("Build ID: abc123 (Downloading...)"), std::string::npos);

  // Releasing the download will cause it to register a failure.
  download = nullptr;

  event = console().GetOutputEvent();
  EXPECT_EQ(
      "Could not load symbols for \"fakelib\" because there was no mapping for build ID \"abc123\".",
      event.output.AsString());
  event = console().GetOutputEvent();
  EXPECT_EQ("Symbol downloading complete. 0 succeeded, 1 failed.", event.output.AsString());
}

}  // namespace zxdb
