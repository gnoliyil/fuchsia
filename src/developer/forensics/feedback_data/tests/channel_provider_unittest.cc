// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/annotations/channel_provider.h"

#include <lib/async/cpp/executor.h>
#include <lib/fpromise/promise.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <memory>
#include <optional>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback_data/annotations/types.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/testing/stubs/channel_control.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/developer/forensics/utils/errors.h"
#include "src/lib/timekeeper/test_clock.h"

namespace forensics {
namespace feedback_data {
namespace {

using testing::IsEmpty;
using testing::Pair;
using testing::UnorderedElementsAreArray;

class ChannelProviderTest : public UnitTestFixture {
 public:
  ChannelProviderTest() : executor_(dispatcher()) {}

 protected:
  void SetUpChannelProviderServer(std::unique_ptr<stubs::ChannelControlBase> server) {
    channel_provider_server_ = std::move(server);
    if (channel_provider_server_) {
      InjectServiceProvider(channel_provider_server_.get());
    }
  }

  Annotations GetChannels(const AnnotationKeys& allowlist = {kAnnotationSystemUpdateChannelTarget},
                          const zx::duration timeout = zx::sec(1)) {
    ChannelProvider provider(dispatcher(), services());
    auto promise = provider.GetAnnotations(timeout, allowlist);

    bool was_called = false;
    Annotations channels;
    executor_.schedule_task(
        std::move(promise).then([&was_called, &channels](::fpromise::result<Annotations>& res) {
          was_called = true;

          if (res.is_error()) {
            return;
          }

          channels = std::move(res).value();
        }));
    RunLoopFor(timeout);

    FX_CHECK(was_called);

    return channels;
  }

  async::Executor executor_;

 private:
  timekeeper::TestClock clock_;
  std::unique_ptr<stubs::ChannelControlBase> channel_provider_server_;
};

TEST_F(ChannelProviderTest, Succeed_TargetChannel) {
  auto channel_provider_server =
      std::make_unique<stubs::ChannelControl>(stubs::ChannelControlBase::Params{
          .current = "current-channel",
          .target = "target-channel",
      });
  SetUpChannelProviderServer(std::move(channel_provider_server));

  const auto result = GetChannels({kAnnotationSystemUpdateChannelTarget});

  EXPECT_THAT(result, UnorderedElementsAreArray({
                          Pair(kAnnotationSystemUpdateChannelTarget, "target-channel"),
                      }));
}

TEST_F(ChannelProviderTest, Succeed_EmptyChannel) {
  SetUpChannelProviderServer(std::make_unique<stubs::ChannelControlReturnsEmptyChannel>());

  const auto result = GetChannels();

  EXPECT_THAT(result, UnorderedElementsAreArray({
                          Pair(kAnnotationSystemUpdateChannelTarget, ""),
                      }));
}

TEST_F(ChannelProviderTest, Succeed_NoRequestedKeysInAllowlist) {
  SetUpChannelProviderServer(std::make_unique<stubs::ChannelControlReturnsEmptyChannel>());

  const auto result = GetChannels({"not-returned-by-channel-provider"});

  EXPECT_THAT(result, IsEmpty());
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderServerNotAvailable) {
  SetUpChannelProviderServer(nullptr);

  const auto result = GetChannels();

  EXPECT_THAT(result, UnorderedElementsAreArray({
                          Pair(kAnnotationSystemUpdateChannelTarget, Error::kConnectionError),
                      }));
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderServerClosesConnection) {
  SetUpChannelProviderServer(std::make_unique<stubs::ChannelControlClosesConnection>());

  const auto result = GetChannels();

  EXPECT_THAT(result, UnorderedElementsAreArray({
                          Pair(kAnnotationSystemUpdateChannelTarget, Error::kConnectionError),
                      }));
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderServerNeverReturns) {
  SetUpChannelProviderServer(std::make_unique<stubs::ChannelControlNeverReturns>());

  const auto result = GetChannels();

  EXPECT_THAT(result, UnorderedElementsAreArray({
                          Pair(kAnnotationSystemUpdateChannelTarget, Error::kTimeout),
                      }));
}

}  // namespace
}  // namespace feedback_data
}  // namespace forensics
