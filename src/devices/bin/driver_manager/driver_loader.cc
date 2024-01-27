// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_loader.h"

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <sched.h>
#include <unistd.h>
#include <zircon/threads.h>

#include <thread>
#include <variant>

#include <fbl/unique_fd.h>

#include "src/devices/bin/driver_manager/manifest_parser.h"
#include "src/devices/lib/log/log.h"

namespace {

bool VerifyMatchedCompositeNodeParentInfo(fdi::wire::MatchedCompositeNodeParentInfo info) {
  if (!info.has_specs() || info.specs().empty()) {
    return false;
  }

  for (auto& spec : info.specs()) {
    if (!spec.has_name() || spec.name().empty() || !spec.has_node_index()) {
      return false;
    }
  }

  return true;
}

bool ShouldUseUniversalResolver(fdi::wire::DriverPackageType package_type) {
  return package_type == fdi::wire::DriverPackageType::kUniverse ||
         package_type == fdi::wire::DriverPackageType::kCached;
}

}  // namespace

const Driver* DriverLoader::UrlToDriver(const std::string& url) {
  if (driver_index_drivers_.count(url) == 0) {
    return nullptr;
  }
  return driver_index_drivers_[url].get();
}

void DriverLoader::WaitForBaseDrivers(fit::callback<void()> callback) {
  // TODO(dgilhooley): Change this back to an ERROR once DriverIndex is used in all tests.
  if (!driver_index_.is_valid()) {
    LOGF(INFO, "%s: DriverIndex is not initialized", __func__);
    return;
  }

  driver_index_->WaitForBaseDrivers().Then(
      [this, callback = std::move(callback)](
          fidl::WireUnownedResult<fdi::DriverIndex::WaitForBaseDrivers>& result) mutable {
        if (!result.ok()) {
          // Since IsolatedDevmgr doesn't use the ComponentFramework, DriverIndex can be
          // closed before DriverManager during tests, which would mean we would see
          // a ZX_ERR_PEER_CLOSED.
          if (result.status() == ZX_ERR_PEER_CLOSED) {
            LOGF(WARNING, "Connection to DriverIndex closed during WaitForBaseDrivers.");
          } else {
            LOGF(ERROR, "Failed to connect to DriverIndex: %s",
                 result.error().FormatDescription().c_str());
          }

          return;
        }
        include_fallback_drivers_ = true;
        callback();
      });
}

const Driver* DriverLoader::LoadDriverUrl(const std::string& manifest_url,
                                          bool use_universe_resolver) {
  // Check if we've already loaded this driver. If we have then return it.
  if (const Driver* driver = UrlToDriver(manifest_url); driver != nullptr) {
    return driver;
  }

  // Pick the correct package resolver to use.
  internal::PackageResolverInterface* resolver = base_resolver_;
  if (use_universe_resolver && universe_resolver_) {
    resolver = universe_resolver_;
  }

  // We've never seen the driver before so add it, then return it.
  zx::result fetched_driver = resolver->FetchDriver(manifest_url);
  if (fetched_driver.is_error()) {
    LOGF(ERROR, "Error fetching driver: %s: %d", manifest_url.data(), fetched_driver.error_value());
    return nullptr;
  }
  // It's possible the driver is nullptr if it was disabled.
  if (!fetched_driver.value()) {
    return nullptr;
  }

  // Success. Return driver.
  driver_index_drivers_[manifest_url] = std::move(fetched_driver.value());
  return driver_index_drivers_[manifest_url].get();
}

const Driver* DriverLoader::LoadDriverUrl(fdi::wire::MatchedDriverInfo driver_info) {
  if (!driver_info.has_driver_url()) {
    LOGF(ERROR, "Driver info is missing the driver URL");
    return nullptr;
  }
  if (!driver_info.has_url()) {
    LOGF(ERROR, "Driver info is missing the URL");
    return nullptr;
  }

  std::string url(driver_info.url().get());
  bool use_universe_resolver =
      driver_info.has_package_type() && ShouldUseUniversalResolver(driver_info.package_type());
  return LoadDriverUrl(url, use_universe_resolver);
}

void DriverLoader::AddCompositeNodeSpec(fuchsia_driver_framework::wire::CompositeNodeSpec spec,
                                        AddToIndexCallback callback) {
  auto result = driver_index_.sync()->AddCompositeNodeSpec(spec);
  if (!result.ok()) {
    LOGF(ERROR, "DriverIndex::AddCompositeNodeSpec failed %d", result.status());
    callback(zx::error(result.status()));
    return;
  }
  if (result->is_error()) {
    callback(result->take_error());
    return;
  }

  callback(zx::ok(fidl::ToNatural(*result->value())));
}

const std::vector<MatchedDriver> DriverLoader::MatchDeviceDriverIndex(
    const fbl::RefPtr<Device>& dev, const MatchDeviceConfig& config) {
  return MatchDeviceDriverIndex(dev->name().data(), dev->props(), dev->str_props(),
                                dev->protocol_id(), config);
}

const std::vector<MatchedDriver> DriverLoader::MatchDeviceDriverIndex(
    std::string_view name, const fbl::Array<const zx_device_prop_t>& props,
    const fbl::Array<const StrProperty>& str_props, uint32_t protocol_id,
    const MatchDeviceConfig& config) {
  if (!driver_index_.is_valid()) {
    return std::vector<MatchedDriver>();
  }

  bool autobind = config.driver_url_suffix.empty();

  fidl::Arena allocator;
  size_t size = props.size() + str_props.size() + 2;
  if (!autobind) {
    size += 1;
  }
  fidl::VectorView<fdf::wire::NodeProperty> fidl_props(allocator, size);

  size_t index = 0;
  fidl_props[index++] = fdf::MakeProperty(allocator, BIND_PROTOCOL, protocol_id);
  fidl_props[index++] = fdf::MakeProperty(allocator, BIND_AUTOBIND, autobind);
  // If we are looking for a specific driver, we add a property to the device with the
  // name of the driver we are looking for. Drivers can then bind to this.
  if (!autobind) {
    fidl_props[index++] =
        fdf::MakeProperty(allocator, "fuchsia.compat.LIBNAME", config.driver_url_suffix);
  }

  for (size_t i = 0; i < props.size(); i++) {
    fidl_props[index++] = fdf::MakeProperty(allocator, props[i].id, props[i].value);
  }

  for (size_t i = 0; i < str_props.size(); i++) {
    switch (str_props[i].value.index()) {
      case StrPropValueType::Integer: {
        fidl_props[index++] = fdf::MakeProperty(
            allocator, str_props[i].key, std::get<StrPropValueType::Integer>(str_props[i].value));
        break;
      }
      case StrPropValueType::String: {
        fidl_props[index++] = fdf::MakeProperty(
            allocator, str_props[i].key, std::get<StrPropValueType::String>(str_props[i].value));
        break;
      }
      case StrPropValueType::Bool: {
        fidl_props[index++] = fdf::MakeProperty(
            allocator, str_props[i].key, std::get<StrPropValueType::Bool>(str_props[i].value));
        break;
      }
      case StrPropValueType::Enum: {
        fidl_props[index++] = fdf::MakeProperty(
            allocator, str_props[i].key, std::get<StrPropValueType::Enum>(str_props[i].value));
        break;
      }
    }
  }

  return MatchPropertiesDriverIndex(name, fidl_props, config);
}

const std::vector<MatchedDriver> DriverLoader::MatchPropertiesDriverIndex(
    std::string_view name, fidl::VectorView<fdf::wire::NodeProperty> props,
    const MatchDeviceConfig& config) {
  std::vector<MatchedDriver> matched_drivers;
  std::vector<MatchedDriver> matched_fallback_drivers;
  if (!driver_index_.is_valid()) {
    return matched_drivers;
  }

  fidl::Arena allocator;
  auto args = fdi::wire::MatchDriverArgs::Builder(allocator);
  args.properties(std::move(props));

  auto result = driver_index_.sync()->MatchDriversV1(args.Build());
  if (!result.ok()) {
    if (result.status() != ZX_OK) {
      LOGF(ERROR, "DriverIndex::MatchDriversV1 for '%s' failed: %s", std::string(name).c_str(),
           result.status_string());
      return matched_drivers;
    }
  }
  // If there's no driver to match then DriverIndex will return ZX_ERR_NOT_FOUND.
  if (result->is_error()) {
    if (result->error_value() != ZX_ERR_NOT_FOUND) {
      LOGF(ERROR, "DriverIndex: MatchDriversV1 for '%s' returned error: %d",
           std::string(name).c_str(), result->error_value());
    }
    return matched_drivers;
  }

  const auto& drivers = result->value()->drivers;

  for (auto driver : drivers) {
    if (driver.is_parent_spec()) {
      if (!VerifyMatchedCompositeNodeParentInfo(driver.parent_spec())) {
        LOGF(
            ERROR,
            "DriverIndex: MatchDriverV1 response is missing fields in MatchedCompositeNodeSpecInfo");
        continue;
      }

      matched_drivers.push_back(fidl::ToNatural(driver.parent_spec()));
      continue;
    }

    ZX_ASSERT(driver.is_driver());

    auto fidl_driver_info = driver.driver();
    if (!fidl_driver_info.has_is_fallback()) {
      LOGF(ERROR, "DriverIndex: MatchDriversV1 response is missing is_fallback");
      continue;
    }

    MatchedDriverInfo matched_driver_info = {};
    if (fidl_driver_info.has_colocate()) {
      matched_driver_info.colocate = fidl_driver_info.colocate();
    }

    // If we have a driver_url we are a DFv1 driver. Otherwise are are DFv2.
    if (fidl_driver_info.has_driver_url()) {
      auto loaded_driver = LoadDriverUrl(fidl_driver_info);
      if (!loaded_driver) {
        continue;
      }
      matched_driver_info.driver = loaded_driver;
    } else if (fidl_driver_info.has_url()) {
      matched_driver_info.driver = Dfv2Driver{
          .url = std::string(fidl_driver_info.url().get()),
          .package_type = fidl_driver_info.package_type(),
      };
    } else {
      LOGF(ERROR, "DriverIndex: MatchDriversV1 response is missing url");
      continue;
    }

    auto driver_url = std::string(matched_driver_info.name());
    if (!fidl_driver_info.is_fallback() && config.only_return_base_and_fallback_drivers &&
        IsFuchsiaBootScheme(driver_url)) {
      continue;
    }

    if (config.driver_url_suffix.empty() ||
        cpp20::ends_with(std::string_view(driver_url), config.driver_url_suffix)) {
      if (fidl_driver_info.is_fallback()) {
        if (include_fallback_drivers_ || !config.driver_url_suffix.empty()) {
          matched_fallback_drivers.push_back(matched_driver_info);
        }
      } else {
        matched_drivers.push_back(matched_driver_info);
      }
    }
  }

  // Fallback drivers need to be at the end of the matched drivers.
  matched_drivers.insert(matched_drivers.end(), matched_fallback_drivers.begin(),
                         matched_fallback_drivers.end());

  return matched_drivers;
}
