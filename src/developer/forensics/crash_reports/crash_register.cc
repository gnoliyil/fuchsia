// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/crash_register.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/forensics/feedback/annotations/constants.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/fostr/fidl/fuchsia/feedback/formatting.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"

namespace forensics {
namespace crash_reports {

CrashRegister::CrashRegister(std::shared_ptr<InfoContext> info_context,
                             std::string register_filepath)
    : info_(std::move(info_context)), register_filepath_(register_filepath) {
  RestoreFromJson();
}

namespace {
Product ToInternalProduct(const fuchsia::feedback::CrashReportingProduct& fidl_product) {
  FX_CHECK(fidl_product.has_name());
  return {.name = fidl_product.name(),
          .version = fidl_product.has_version() ? ErrorOr<std::string>(fidl_product.version())
                                                : ErrorOr<std::string>(Error::kMissingValue),
          .channel = fidl_product.has_channel() ? ErrorOr<std::string>(fidl_product.channel())
                                                : ErrorOr<std::string>(Error::kMissingValue)};
}

}  // namespace

void CrashRegister::Upsert(std::string component_url,
                           fuchsia::feedback::CrashReportingProduct product) {
  if (!product.has_name()) {
    FX_LOGS(WARNING) << "Missing required name in product:" << product;
    return;
  }

  const Product internal_product = ToInternalProduct(product);
  info_.UpsertComponentToProductMapping(component_url, internal_product);
  UpdateJson(component_url, internal_product);
  component_to_products_.insert_or_assign(component_url, std::move(internal_product));
}

void CrashRegister::UpsertWithAck(std::string component_url,
                                  fuchsia::feedback::CrashReportingProduct product,
                                  UpsertWithAckCallback callback) {
  Upsert(std::move(component_url), std::move(product));
  callback();
}

bool CrashRegister::HasProduct(const std::string& program_name) const {
  return component_to_products_.count(program_name) != 0;
}

Product CrashRegister::GetProduct(const std::string& program_name) const {
  FX_CHECK(HasProduct(program_name)) << "No product for " << program_name;
  return component_to_products_.at(program_name);
}

void CrashRegister::AddVersionAndChannel(Product& product, const AnnotationMap& annotations) {
  if (annotations.Contains(feedback::kBuildVersionKey)) {
    product.version = annotations.Get(feedback::kBuildVersionKey);
  }

  if (annotations.Contains(feedback::kSystemUpdateChannelCurrentKey)) {
    product.channel = annotations.Get(feedback::kSystemUpdateChannelCurrentKey);
  }
}

// The content of the component register will be stored as json where each product for a component
// url is comprised of an object made up of string-string pairs for the name, version, and
// channel, with the latter two being optional.
//
// For example, imagine there are 2 component urls, "foo" and "bar". "foo"'s product has the name
// "foo-product", a version of "1", and a channel of "foo-channel" and "bar"'s product only has a
// name, "bar-product", the json will look like:
// {
//     "foo": {
//         "name": "foo-product",
//         "version": "1",
//         "channel": "foo-channel"
//     },
//     "bar": {
//         "name": "bar-product"
//     }
// }
void CrashRegister::UpdateJson(const std::string& component_url, const Product& product) {
  using namespace rapidjson;

  auto& allocator = register_json_.GetAllocator();

  // If there is already a product for |component_url|, delete it.
  if (register_json_.HasMember(component_url)) {
    register_json_.RemoveMember(component_url);
  }

  // Make an empty object for |component_url|.
  register_json_.AddMember(Value(component_url, register_json_.GetAllocator()),
                           Value(rapidjson::kObjectType), allocator);

  const auto& json_product = register_json_[component_url].GetObject();

  json_product.AddMember(Value("name", allocator), Value(product.name, allocator), allocator);

  if (product.version.HasValue()) {
    json_product.AddMember(Value("version", allocator), Value(product.version.Value(), allocator),
                           allocator);
  }

  if (product.channel.HasValue()) {
    json_product.AddMember(Value("channel", allocator), Value(product.channel.Value(), allocator),
                           allocator);
  }

  StringBuffer buffer;
  PrettyWriter<StringBuffer> writer(buffer);

  register_json_.Accept(writer);

  if (!files::WriteFile(register_filepath_, buffer.GetString(), buffer.GetLength())) {
    FX_LOGS(ERROR) << "Failed to write crash register contents to " << register_filepath_;
  }
}

void CrashRegister::RestoreFromJson() {
  using namespace rapidjson;

  register_json_.SetObject();

  // If the file doesn't exit, return.
  if (!files::IsFile(register_filepath_)) {
    return;
  }

  // Check-fail if the file can't be read.
  std::string json;
  FX_CHECK(files::ReadFileToString(register_filepath_, &json));

  ParseResult ok = register_json_.Parse(json);
  if (!ok) {
    FX_LOGS(ERROR) << "Error parsing crash register as JSON at offset " << ok.Offset() << " "
                   << GetParseError_En(ok.Code());
    files::DeletePath(register_filepath_, /*recursive=*/true);
    return;
  }

  // Each product in the register is represented by an object containing string-string pairs
  // that are the product content.
  FX_CHECK(register_json_.IsObject());
  for (const auto& member : register_json_.GetObject()) {
    // Skip any non-object members.
    if (!member.value.IsObject()) {
      continue;
    }

    const std::string component_url = member.name.GetString();

    const auto& json_product = member.value;

    // If the product doesn't have a name, skip it.
    if (!json_product.HasMember("name") || !json_product["name"].IsString()) {
      continue;
    }

    Product internal_product{
        .name = json_product["name"].GetString(),
        .version = Error::kMissingValue,
        .channel = Error::kMissingValue,
    };

    if (json_product.HasMember("version") && json_product["version"].IsString()) {
      internal_product.version = json_product["version"].GetString();
    }

    if (json_product.HasMember("channel") && json_product["channel"].IsString()) {
      internal_product.channel = json_product["channel"].GetString();
    }

    component_to_products_.insert_or_assign(component_url, std::move(internal_product));
  }
}

}  // namespace crash_reports
}  // namespace forensics
