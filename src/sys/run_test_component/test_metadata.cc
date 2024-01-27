// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/run_test_component/test_metadata.h"

#include <fuchsia/boot/cpp/fidl.h>
#include <fuchsia/device/cpp/fidl.h>
#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/hardware/pty/cpp/fidl.h>
#include <fuchsia/kernel/cpp/fidl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/net/name/cpp/fidl.h>
#include <fuchsia/posix/socket/cpp/fidl.h>
#include <fuchsia/scheduler/cpp/fidl.h>
#include <fuchsia/sys/internal/cpp/fidl.h>
#include <fuchsia/sys/test/cpp/fidl.h>
#include <fuchsia/sysinfo/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/test/manager/cpp/fidl.h>
#include <fuchsia/time/cpp/fidl.h>
#include <fuchsia/tracing/kernel/cpp/fidl.h>
#include <fuchsia/tracing/provider/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <fuchsia/vulkan/loader/cpp/fidl.h>

#include <filesystem>
#include <unordered_set>

#include "src/lib/cmx/cmx.h"
#include "src/lib/fxl/strings/substitute.h"

namespace run {
namespace {

using fxl::Substitute;

constexpr char kInjectedServices[] = "injected-services";
constexpr char kSystemServices[] = "system-services";

// Services below were reported by their owners to be impractical to fake in a test environment
// because they depend on devices. Appmgr's test support does not offer the ability to fake the
// device namespace.
//
// Component Manager is able to route and fake devices and early boot capabilities.
//
// At this time the body of tests largely depends on appmgr, so we maintain this list as a necessary
// compromise.
//
// Please add items to this list only if you believe that no other pragmatic alternative is
// currently present.
//
// Please document the rationale for each entry added.  See also:
// docs/concepts/testing/v1_test_component.md
// [START allowed_system_services]
const std::unordered_set<std::string> kAllowedSystemServices = {
    fuchsia::boot::FactoryItems::Name_,
    fuchsia::boot::Items::Name_,
    fuchsia::boot::ReadOnlyLog::Name_,
    fuchsia::boot::RootResource::Name_,
    fuchsia::boot::WriteOnlyLog::Name_,
    fuchsia::device::NameProvider::Name_,
    fuchsia::diagnostics::ArchiveAccessor::Name_,
    fuchsia::hardware::pty::Device::Name_,
    fuchsia::kernel::Counter::Name_,
    fuchsia::kernel::CpuResource::Name_,
    fuchsia::kernel::EnergyInfoResource::Name_,
    fuchsia::kernel::DebugResource::Name_,
    fuchsia::kernel::HypervisorResource::Name_,
    fuchsia::kernel::InfoResource::Name_,
    fuchsia::kernel::IoportResource::Name_,
    fuchsia::kernel::IrqResource::Name_,
    fuchsia::kernel::MexecResource::Name_,
    fuchsia::kernel::MmioResource::Name_,
    fuchsia::kernel::PowerResource::Name_,
    fuchsia::kernel::RootJob::Name_,
    fuchsia::kernel::RootJobForInspect::Name_,
    fuchsia::kernel::SmcResource::Name_,
    fuchsia::kernel::Stats::Name_,
    fuchsia::kernel::VmexResource::Name_,
    fuchsia::media::Audio::Name_,
    fuchsia::media::AudioCore::Name_,
    fuchsia::media::AudioDeviceEnumerator::Name_,
    fuchsia::media::ProfileProvider::Name_,
    fuchsia::scheduler::ProfileProvider::Name_,
    fuchsia::sys::internal::CrashIntrospect::Name_,
    fuchsia::sys::test::CacheControl::Name_,
    fuchsia::sysinfo::SysInfo::Name_,
    fuchsia::sysmem::Allocator::Name_,
    fuchsia::tracing::provider::Registry::Name_,
    fuchsia::tracing::kernel::Controller::Name_,
    fuchsia::tracing::kernel::Reader::Name_,
    fuchsia::ui::composition::Allocator::Name_,
    fuchsia::ui::scenic::Scenic::Name_,
    fuchsia::vulkan::loader::Loader::Name_,
    fuchsia::test::manager::Query::Name_,
    fuchsia::test::manager::RunBuilder::Name_,
};
// [END allowed_system_services]

}  // namespace

TestMetadata::TestMetadata() {}
TestMetadata::~TestMetadata() {}

fuchsia::sys::LaunchInfo TestMetadata::GetLaunchInfo(const rapidjson::Document::ValueType& value,
                                                     const std::string& name) {
  fuchsia::sys::LaunchInfo launch_info;
  if (value.IsString()) {
    launch_info.url = value.GetString();
    return launch_info;
  }

  if (value.IsArray()) {
    const auto& array = value.GetArray();
    // If the element is an array, ensure it is non-empty and all values are
    // strings.
    if (!array.Empty() && std::all_of(array.begin(), array.end(),
                                      [](const rapidjson::Value& val) { return val.IsString(); })) {
      launch_info.url = array[0].GetString();
      launch_info.arguments.emplace();
      for (rapidjson::SizeType i = 1; i < array.Size(); ++i) {
        launch_info.arguments->push_back(array[i].GetString());
      }
      return launch_info;
    }
  }

  json_parser_.ReportError(
      Substitute("'$0' must be a string or a non-empty array of strings.", name));
  return launch_info;
}

bool TestMetadata::ParseFromString(const std::string& cmx_data, const std::string& filename) {
  component::CmxMetadata cmx;
  cmx.ParseFromString(cmx_data, filename, &json_parser_);
  if (json_parser_.HasError()) {
    return false;
  }
  auto& fuchsia_test = cmx.GetFacet(kFuchsiaTest);
  if (!fuchsia_test.IsNull()) {
    null_ = false;
    if (!fuchsia_test.IsObject()) {
      json_parser_.ReportError(Substitute("'$0' in 'facets' should be an object.", kFuchsiaTest));
      return false;
    }
    auto system_services = fuchsia_test.FindMember(kSystemServices);
    if (system_services != fuchsia_test.MemberEnd()) {
      if (!system_services->value.IsArray()) {
        json_parser_.ReportError(
            Substitute("'$0' in '$1' should be a string array.", kSystemServices, kFuchsiaTest));
      } else {
        for (const rapidjson::Value& val : system_services->value.GetArray()) {
          if (!val.IsString()) {
            json_parser_.ReportError(Substitute("'$0' in '$1' should be a string array.",
                                                kSystemServices, kFuchsiaTest));
            return false;
          }
          std::string service = val.GetString();
          if (kAllowedSystemServices.count(service) == 0) {
            json_parser_.ReportError(
                fxl::Substitute("'$0' cannot contain '$1'.", kSystemServices, service));
            return false;
          }
          system_services_.push_back(service);
        };
      }
    }
    auto services = fuchsia_test.FindMember(kInjectedServices);
    if (services != fuchsia_test.MemberEnd()) {
      if (!services->value.IsObject()) {
        json_parser_.ReportError(
            Substitute("'$0' in '$1' should be an object.", kInjectedServices, kFuchsiaTest));
        return false;
      }
      for (auto itr = services->value.MemberBegin(); itr != services->value.MemberEnd(); ++itr) {
        if (!itr->name.IsString()) {
          json_parser_.ReportError(Substitute("'$0' in '$1' should define string service names.",
                                              kInjectedServices, kFuchsiaTest));
          return false;
        }
        auto name = itr->name.GetString();
        auto launch_info = GetLaunchInfo(itr->value, name);
        service_url_pair_.push_back(std::make_pair(name, std::move(launch_info)));
      }
    }
  }
  return !HasError();
}

}  // namespace run
