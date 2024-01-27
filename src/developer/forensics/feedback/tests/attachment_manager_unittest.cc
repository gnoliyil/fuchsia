// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/attachments/attachment_manager.h"

#include <lib/async/cpp/executor.h>
#include <lib/async/cpp/task.h>
#include <lib/fpromise/result.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback/attachments/types.h"
#include "src/developer/forensics/testing/gmatchers.h"
#include "src/developer/forensics/testing/gpretty_printers.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace forensics::feedback {
namespace {

using ::testing::Contains;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pair;
using ::testing::UnorderedElementsAreArray;

class SimpleAttachmentProvider : public AttachmentProvider {
 public:
  SimpleAttachmentProvider(async_dispatcher_t* dispatcher, zx::duration delay, std::string data)
      : dispatcher_(dispatcher), delay_(delay), data_(std::move(data)) {}

  ::fpromise::promise<AttachmentValue> Get(const uint64_t ticket) override {
    ::fpromise::bridge<AttachmentValue> bridge;

    completers_[ticket] = std::move(bridge.completer);

    async::PostDelayedTask(
        dispatcher_,
        [this, ticket]() mutable {
          if (completers_[ticket]) {
            completers_[ticket].complete_ok(AttachmentValue(data_));
          }
        },
        delay_);

    return bridge.consumer.promise_or(::fpromise::error());
  }

  void ForceCompletion(const uint64_t ticket, const Error error) override {
    if (completers_[ticket] && completers_.count(ticket) != 0) {
      completers_[ticket].complete_ok(error);
    }
  }

 private:
  async_dispatcher_t* dispatcher_;

  zx::duration delay_;
  std::string data_;
  std::map<uint64_t, ::fpromise::completer<AttachmentValue>> completers_;
};

using AttachmentManagerTest = UnitTestFixture;

TEST_F(AttachmentManagerTest, Dynamic) {
  async::Executor executor(dispatcher());

  SimpleAttachmentProvider provider1(dispatcher(), zx::sec(1), "value1");
  SimpleAttachmentProvider provider2(dispatcher(), zx::sec(3), "value2");

  AttachmentManager manager(dispatcher(), {"dynamic1", "dynamic2"},
                            {
                                {"dynamic1", &provider1},
                                {"dynamic2", &provider2},
                            });

  Attachments attachments;
  executor.schedule_task(
      manager.GetAttachments(zx::sec(1))
          .and_then([&attachments](Attachments& result) { attachments = std::move(result); })
          .or_else([] { FX_LOGS(FATAL) << "Unreachable branch"; }));

  RunLoopFor(zx::sec(1));
  EXPECT_THAT(attachments, ElementsAreArray({
                               Pair("dynamic1", AttachmentValueIs("value1")),
                               Pair("dynamic2", AttachmentValueIs(Error::kTimeout)),
                           }));

  attachments.clear();

  executor.schedule_task(
      manager.GetAttachments(zx::duration::infinite())
          .and_then([&attachments](Attachments& result) { attachments = std::move(result); })
          .or_else([] { FX_LOGS(FATAL) << "Unreachable branch"; }));

  RunLoopFor(zx::sec(3));
  EXPECT_THAT(attachments, ElementsAreArray({
                               Pair("dynamic1", AttachmentValueIs("value1")),
                               Pair("dynamic2", AttachmentValueIs("value2")),
                           }));
}

TEST_F(AttachmentManagerTest, NoProvider) {
  ASSERT_DEATH({ AttachmentManager manager(dispatcher(), {"unknown.attachment"}); },
               HasSubstr("Attachment \"unknown.attachment\" collected by 0 providers"));
}

}  // namespace
}  // namespace forensics::feedback
