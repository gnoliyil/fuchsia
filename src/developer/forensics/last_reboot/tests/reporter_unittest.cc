// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/last_reboot/reporter.h"

#include <lib/fpromise/result.h>
#include <lib/inspect/cpp/vmo/types.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback/reboot_log/reboot_log.h"
#include "src/developer/forensics/testing/gpretty_printers.h"
#include "src/developer/forensics/testing/stubs/cobalt_logger_factory.h"
#include "src/developer/forensics/testing/stubs/crash_reporter.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/developer/forensics/utils/cobalt/event.h"
#include "src/developer/forensics/utils/cobalt/logger.h"
#include "src/developer/forensics/utils/cobalt/metrics.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/timekeeper/test_clock.h"

namespace forensics {
namespace last_reboot {
namespace {

using testing::IsEmpty;
using testing::UnorderedElementsAreArray;

constexpr char kHasReportedOnPath[] = "/tmp/has_reported_on_reboot_log.txt";
constexpr char kNoGracefulReason[] = "GRACEFUL REBOOT REASON (NONE)";

struct UngracefulRebootTestParam {
  std::string test_name;
  std::string zircon_reboot_log;
  std::string reboot_reason;

  std::string output_crash_signature;
  std::optional<zx::duration> output_uptime;
  cobalt::LastRebootReason output_last_reboot_reason;
};

struct GracefulRebootTestParam {
  std::string test_name;
  std::optional<std::string> graceful_reboot_log;

  cobalt::LastRebootReason output_last_reboot_reason;
};

struct GracefulRebootWithCrashTestParam {
  std::string test_name;
  std::string graceful_reboot_log;
  std::string reboot_reason;

  std::string output_crash_signature;
  zx::duration output_uptime;
  cobalt::LastRebootReason output_last_reboot_reason;
  bool output_is_fatal;
};

template <typename TestParam>
class ReporterTest : public UnitTestFixture, public testing::WithParamInterface<TestParam> {
 public:
  ReporterTest()
      : cobalt_(dispatcher(), services(), &clock_),
        redactor_(new IdentityRedactor(inspect::BoolProperty())) {}

  void TearDown() override { files::DeletePath(kHasReportedOnPath, /*recursive=*/false); }

 protected:
  void SetUpCrashReporterServer(std::unique_ptr<stubs::CrashReporterBase> server) {
    crash_reporter_server_ = std::move(server);
  }

  void SetUpRedactor(std::unique_ptr<RedactorBase> redactor) { redactor_ = std::move(redactor); }

  void WriteZirconRebootLogContents(const std::string& contents) {
    FX_CHECK(tmp_dir_.NewTempFileWithData(contents, &zircon_reboot_log_path_));
  }

  void WriteGracefulRebootLogContents(const std::string& contents) {
    FX_CHECK(tmp_dir_.NewTempFileWithData(contents, &graceful_reboot_log_path_));
  }

  void SetAsFdr() { not_a_fdr_ = false; }

  void ReportOnRebootLog() {
    const auto reboot_log = feedback::RebootLog::ParseRebootLog(
        zircon_reboot_log_path_, graceful_reboot_log_path_, not_a_fdr_);
    ReportOn(reboot_log);
  }

  void ReportOn(const feedback::RebootLog& reboot_log) {
    Reporter reporter(dispatcher(), services(), &cobalt_, redactor_.get(),
                      crash_reporter_server_.get());
    reporter.ReportOn(reboot_log, /*delay=*/zx::sec(0));
    RunLoopUntilIdle();
  }

  std::string zircon_reboot_log_path_;
  std::string graceful_reboot_log_path_;
  std::unique_ptr<stubs::CrashReporterBase> crash_reporter_server_;

 private:
  timekeeper::TestClock clock_;
  cobalt::Logger cobalt_;
  std::unique_ptr<RedactorBase> redactor_;
  files::ScopedTempDir tmp_dir_;
  bool not_a_fdr_{true};
};

using GenericReporterTest = ReporterTest<UngracefulRebootTestParam /*does not matter*/>;

TEST_F(GenericReporterTest, Succeed_WellFormedRebootLog) {
  const zx::duration uptime = zx::msec(74715002);
  const feedback::RebootLog reboot_log(
      feedback::RebootReason::kKernelPanic,
      "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n74715002", uptime, std::nullopt);

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = ToCrashSignature(reboot_log.RebootReason()),
          .reboot_log = reboot_log.RebootLogStr(),
          .uptime = reboot_log.Uptime(),
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kKernelPanic, uptime.to_usecs()),
              }));
  EXPECT_TRUE(files::IsFile(kHasReportedOnPath));
}

TEST_F(GenericReporterTest, Succeed_RootJobTerminationRebootLog) {
  const zx::duration uptime = zx::msec(74715002);
  const feedback::RebootLog reboot_log(
      feedback::RebootReason::kRootJobTermination,
      "ZIRCON REBOOT REASON (USERSPACE ROOT JOB TERMINATION)\n\nUPTIME (ms)\n74715002\n"
      "ROOT JOB TERMINATED BY CRITICAL PROCESS DEATH: foo (1)",
      uptime, "foo");

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature =
              ToCrashSignature(reboot_log.RebootReason(), reboot_log.CriticalProcess()),
          .reboot_log = reboot_log.RebootLogStr(),
          .uptime = reboot_log.Uptime(),
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kRootJobTermination, uptime.to_usecs()),
              }));
  EXPECT_TRUE(files::IsFile(kHasReportedOnPath));
}

TEST_F(GenericReporterTest, Succeed_NoUptime) {
  const feedback::RebootLog reboot_log(feedback::RebootReason::kKernelPanic,
                                       "ZIRCON REBOOT REASON (KERNEL PANIC)\n", std::nullopt,
                                       std::nullopt);

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = ToCrashSignature(reboot_log.RebootReason()),
          .reboot_log = reboot_log.RebootLogStr(),
          .uptime = std::nullopt,
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kKernelPanic, /*duration=*/0u),
              }));
}

class SimpleRedactor : public RedactorBase {
 public:
  SimpleRedactor() : RedactorBase(inspect::BoolProperty()) {}

  std::string& Redact(std::string& text) override {
    text = "<REDACTED>";
    return text;
  }

  std::string& RedactJson(std::string& text) override { return Redact(text); }

  std::string UnredactedCanary() const override { return ""; }
  std::string RedactedCanary() const override { return ""; }
};

TEST_F(GenericReporterTest, Succeed_RedactsData) {
  const feedback::RebootLog reboot_log(feedback::RebootReason::kKernelPanic,
                                       "ZIRCON REBOOT REASON (KERNEL PANIC)\n", std::nullopt,
                                       std::nullopt);

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = ToCrashSignature(reboot_log.RebootReason()),
          .reboot_log = "<REDACTED>",
          .uptime = std::nullopt,
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());
  SetUpRedactor(std::make_unique<SimpleRedactor>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kKernelPanic, /*duration=*/0u),
              }));
}

TEST_F(GenericReporterTest, Succeed_NoCrashReportFiledCleanReboot) {
  const zx::duration uptime = zx::msec(74715002);
  const feedback::RebootLog reboot_log(feedback::RebootReason::kGenericGraceful,
                                       "ZIRCON REBOOT REASON (NO CRASH)\n\nUPTIME (ms)\n74715002",
                                       uptime, std::nullopt);

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = ToCrashSignature(reboot_log.RebootReason()),
          .reboot_log = reboot_log.RebootLogStr(),
          .uptime = uptime,
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kGenericGraceful, uptime.to_usecs()),
              }));
}

TEST_F(GenericReporterTest, Succeed_NoCrashReportFiledColdReboot) {
  const feedback::RebootLog reboot_log(feedback::RebootReason::kCold, "", std::nullopt,
                                       std::nullopt);

  SetUpCrashReporterServer(std::make_unique<stubs::CrashReporterNoFileExpected>());
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kCold, /*duration=*/0u),
              }));
}

TEST_F(GenericReporterTest, Fail_CrashReporterFailsToFile) {
  const zx::duration uptime = zx::msec(74715002);
  const feedback::RebootLog reboot_log(
      feedback::RebootReason::kKernelPanic,
      "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n74715002", uptime, std::nullopt);
  SetUpCrashReporterServer(std::make_unique<stubs::CrashReporterAlwaysReturnsError>());
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::LastRebootReason::kKernelPanic, uptime.to_usecs()),
              }));
}

TEST_F(GenericReporterTest, Succeed_DoesNothingIfAlreadyReportedOn) {
  ASSERT_TRUE(files::WriteFile(kHasReportedOnPath, /*data=*/"", /*size=*/0));

  const feedback::RebootLog reboot_log(
      feedback::RebootReason::kKernelPanic,
      "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n74715002", zx::msec(74715002),
      std::nullopt);

  SetUpCrashReporterServer(std::make_unique<stubs::CrashReporterNoFileExpected>());
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOn(reboot_log);

  EXPECT_THAT(ReceivedCobaltEvents(), IsEmpty());
}

using UngracefulReporterTest = ReporterTest<UngracefulRebootTestParam>;

INSTANTIATE_TEST_SUITE_P(WithVariousRebootLogs, UngracefulReporterTest,
                         ::testing::ValuesIn(std::vector<UngracefulRebootTestParam>({
                             {
                                 "KernelPanic",
                                 "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (KERNEL PANIC)",
                                 "fuchsia-kernel-panic",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kKernelPanic,
                             },
                             {
                                 "OOM",
                                 "ZIRCON REBOOT REASON (OOM)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (OOM)",
                                 "fuchsia-oom",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kSystemOutOfMemory,
                             },
                             {
                                 "Spontaneous",
                                 "ZIRCON REBOOT REASON (UNKNOWN)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (SPONTANEOUS)",
                                 "fuchsia-brief-power-loss",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kBriefPowerLoss,
                             },
                             {
                                 "SoftwareWatchdogTimeout",
                                 "ZIRCON REBOOT REASON (SW WATCHDOG)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (SOFTWARE WATCHDOG TIMEOUT)",
                                 "fuchsia-sw-watchdog-timeout",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kSoftwareWatchdogTimeout,
                             },
                             {
                                 "HardwareWatchdogTimeout",
                                 "ZIRCON REBOOT REASON (HW WATCHDOG)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (HARDWARE WATCHDOG TIMEOUT)",
                                 "fuchsia-hw-watchdog-timeout",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kHardwareWatchdogTimeout,
                             },
                             {
                                 "BrownoutPower",
                                 "ZIRCON REBOOT REASON (BROWNOUT)\n\nUPTIME (ms)\n65487494",
                                 "FINAL REBOOT REASON (BROWNOUT)",
                                 "fuchsia-brownout",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kBrownout,
                             },
                             {
                                 "NotParseable",
                                 "NOT PARSEABLE",
                                 "FINAL REBOOT REASON (NOT PARSEABLE)",
                                 "fuchsia-reboot-log-not-parseable",
                                 std::nullopt,
                                 cobalt::LastRebootReason::kUnknown,
                             },
                         })),
                         [](const testing::TestParamInfo<UngracefulRebootTestParam>& info) {
                           return info.param.test_name;
                         });

TEST_P(UngracefulReporterTest, Succeed) {
  const auto param = GetParam();

  WriteZirconRebootLogContents(param.zircon_reboot_log);
  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = param.output_crash_signature,
          .reboot_log = fxl::StringPrintf("%s\n%s\n\n%s", param.zircon_reboot_log.c_str(),
                                          kNoGracefulReason, param.reboot_reason.c_str()),
          .uptime = param.output_uptime,
          .is_fatal = true,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOnRebootLog();

  const zx::duration expected_uptime =
      (param.output_uptime.has_value()) ? param.output_uptime.value() : zx::usec(0);
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(param.output_last_reboot_reason, expected_uptime.to_usecs()),
              }));
}

using GracefulReporterTest = ReporterTest<GracefulRebootTestParam>;

INSTANTIATE_TEST_SUITE_P(WithVariousRebootLogs, GracefulReporterTest,
                         ::testing::ValuesIn(std::vector<GracefulRebootTestParam>({
                             {
                                 "UserRequest",
                                 "USER REQUEST",
                                 cobalt::LastRebootReason::kUserRequest,
                             },
                             {
                                 "SystemUpdate",
                                 "SYSTEM UPDATE",
                                 cobalt::LastRebootReason::kSystemUpdate,
                             },
                             {
                                 "ZbiSwap",
                                 "ZBI SWAP",
                                 cobalt::LastRebootReason::kZbiSwap,
                             },
                         })),
                         [](const testing::TestParamInfo<GracefulRebootTestParam>& info) {
                           return info.param.test_name;
                         });

TEST_P(GracefulReporterTest, Succeed) {
  const auto param = GetParam();

  WriteZirconRebootLogContents("ZIRCON REBOOT REASON (NO CRASH)\n\nUPTIME (ms)\n65487494");
  if (param.graceful_reboot_log.has_value()) {
    WriteGracefulRebootLogContents(param.graceful_reboot_log.value());
  }

  SetUpCrashReporterServer(std::make_unique<stubs::CrashReporterNoFileExpected>());
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOnRebootLog();

  const zx::duration expected_uptime = zx::msec(65487494);
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(param.output_last_reboot_reason, expected_uptime.to_usecs()),
              }));
}

TEST_P(GracefulReporterTest, Succeed_FDR) {
  WriteZirconRebootLogContents("ZIRCON REBOOT REASON (NO CRASH)\n\nUPTIME (ms)\n65487494");
  SetAsFdr();

  SetUpCrashReporterServer(std::make_unique<stubs::CrashReporterNoFileExpected>());
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOnRebootLog();

  const zx::duration expected_uptime = zx::msec(65487494);
  EXPECT_THAT(
      ReceivedCobaltEvents(),
      UnorderedElementsAreArray({
          cobalt::Event(cobalt::LastRebootReason::kFactoryDataReset, expected_uptime.to_usecs()),
      }));
}

using GracefulWithCrashReporterTest = ReporterTest<GracefulRebootWithCrashTestParam>;

INSTANTIATE_TEST_SUITE_P(WithVariousRebootLogs, GracefulWithCrashReporterTest,
                         ::testing::ValuesIn(std::vector<GracefulRebootWithCrashTestParam>({
                             {
                                 "SessionFailure",
                                 "SESSION FAILURE",
                                 "FINAL REBOOT REASON (SESSION FAILURE)",
                                 "fuchsia-session-failure",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kSessionFailure,
                                 false,
                             },
                             {
                                 "OOM",
                                 "OUT OF MEMORY",
                                 "FINAL REBOOT REASON (OOM)",
                                 "fuchsia-oom",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kSystemOutOfMemory,
                                 true,
                             },
                             {
                                 "SysmgrFailure",
                                 "SYSMGR FAILURE",
                                 "FINAL REBOOT REASON (SYSMGR FAILURE)",
                                 "fuchsia-sysmgr-failure",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kSysmgrFailure,
                                 true,
                             },
                             {
                                 "CriticalComponentFailure",
                                 "CRITICAL COMPONENT FAILURE",
                                 "FINAL REBOOT REASON (CRITICAL COMPONENT FAILURE)",
                                 "fuchsia-critical-component-failure",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kCriticalComponentFailure,
                                 true,
                             },
                             {
                                 "RetrySystemUpdate",
                                 "RETRY SYSTEM UPDATE",
                                 "FINAL REBOOT REASON (RETRY SYSTEM UPDATE)",
                                 "fuchsia-retry-system-update",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kRetrySystemUpdate,
                                 true,
                             },
                             {
                                 "HighTemperature",
                                 "HIGH TEMPERATURE",
                                 "FINAL REBOOT REASON (HIGH TEMPERATURE)",
                                 "fuchsia-reboot-high-temperature",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kHighTemperature,
                                 true,
                             },
                             {
                                 "NotSupported",
                                 "NOT SUPPORTED",
                                 "FINAL REBOOT REASON (GENERIC GRACEFUL)",
                                 "fuchsia-undetermined-userspace-reboot",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kGenericGraceful,
                                 true,
                             },
                             {
                                 "NotParseable",
                                 "NOT PARSEABLE",
                                 "FINAL REBOOT REASON (GENERIC GRACEFUL)",
                                 "fuchsia-undetermined-userspace-reboot",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kGenericGraceful,
                                 true,
                             },
                             {
                                 "None",
                                 "NONE",
                                 "FINAL REBOOT REASON (GENERIC GRACEFUL)",
                                 "fuchsia-undetermined-userspace-reboot",
                                 zx::msec(65487494),
                                 cobalt::LastRebootReason::kGenericGraceful,
                                 true,
                             },
                         })),
                         [](const testing::TestParamInfo<GracefulRebootWithCrashTestParam>& info) {
                           return info.param.test_name;
                         });
TEST_P(GracefulWithCrashReporterTest, Succeed) {
  const auto param = GetParam();

  const std::string zircon_reboot_log = fxl::StringPrintf(
      "ZIRCON REBOOT REASON (NO CRASH)\n\nUPTIME (ms)\n%lu", param.output_uptime.to_msecs());
  WriteZirconRebootLogContents(zircon_reboot_log);

  if (param.graceful_reboot_log != "NONE") {
    WriteGracefulRebootLogContents(param.graceful_reboot_log);
  }

  SetUpCrashReporterServer(
      std::make_unique<stubs::CrashReporter>(stubs::CrashReporter::Expectations{
          .crash_signature = param.output_crash_signature,
          .reboot_log =
              fxl::StringPrintf("%s\nGRACEFUL REBOOT REASON (%s)\n\n%s", zircon_reboot_log.c_str(),
                                param.graceful_reboot_log.c_str(), param.reboot_reason.c_str()),
          .uptime = param.output_uptime,
          .is_fatal = param.output_is_fatal,
      }));
  SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());

  ReportOnRebootLog();

  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(param.output_last_reboot_reason, param.output_uptime.to_usecs()),
              }));
}

}  // namespace
}  // namespace last_reboot
}  // namespace forensics
