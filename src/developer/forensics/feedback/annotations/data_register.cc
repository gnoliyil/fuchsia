// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/annotations/data_register.h"

#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>

#include "src/developer/forensics/feedback/annotations/types.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"

namespace forensics::feedback {
namespace {

const char kDefaultNamespace[] = "misc";

const char kNamespaceSeparator[] = ".";

Annotations Flatten(const std::map<std::string, Annotations>& namespaced_annotations) {
  Annotations flat_annotations;
  for (const auto& [namespace_, annotations] : namespaced_annotations) {
    for (const auto& [k, v] : annotations) {
      flat_annotations.insert({namespace_ + kNamespaceSeparator + k, v});
    }
  }

  return flat_annotations;
}

}  // namespace

DataRegister::DataRegister(size_t max_num_annotations,
                           std::set<std::string> disallowed_annotation_namespaces,
                           std::string register_filepath)
    : max_num_annotations_(max_num_annotations),
      disallowed_annotation_namespaces_(std::move(disallowed_annotation_namespaces)),
      register_filepath_(std::move(register_filepath)) {
  RestoreFromJson();
}

void DataRegister::Upsert(fuchsia::feedback::ComponentData data, UpsertCallback callback) {
  auto execute_callback = ::fit::defer(std::move(callback));

  if (!data.has_annotations()) {
    FX_LOGS(WARNING) << "No non-platform annotations to upsert";
    return;
  }

  if (data.has_namespace() && disallowed_annotation_namespaces_.count(data.namespace_()) != 0) {
    FX_LOGS(WARNING) << fxl::StringPrintf(
        "Ignoring non-platform annotations, %s is a reserved namespace", data.namespace_().c_str());

    // TODO(fxbug.dev/48664): close connection with ZX_ERR_INVALID_ARGS instead.
    return;
  }

  if (!data.has_namespace()) {
    FX_LOGS(WARNING) << "No namespace specified, defaulting to " << kDefaultNamespace;
  }
  const std::string ns = (data.has_namespace()) ? data.namespace_() : kDefaultNamespace;

  Annotations new_annotations = namespaced_annotations_[ns];
  for (const auto& [key, value] : data.annotations()) {
    new_annotations.insert_or_assign(key, value);
  }

  size_t new_size = new_annotations.size();
  for (const auto& [n, annotations] : namespaced_annotations_) {
    if (n != ns) {
      new_size += annotations.size();
    }
  }

  if (new_size > max_num_annotations_) {
    FX_LOGS(WARNING) << fxl::StringPrintf(
        "Ignoring all %lu new non-platform annotations as only %lu non-platform annotations "
        "are allowed",
        new_size, max_num_annotations_);
    is_missing_annotations_ = true;

    // TODO(fxbug.dev/48666): close all connections.
    return;
  }

  namespaced_annotations_[ns] = std::move(new_annotations);

  UpdateJson(ns, namespaced_annotations_[ns]);
}

Annotations DataRegister::Get() { return Flatten(namespaced_annotations_); }

// The content of the data register will be stored as json where each namespace is comprised of an
// object made up of string-string pairs.
//
// For example, if there are 2 namespaces, "foo" and "bar". "foo" has 2 set of annotations,
// {"k1", "v1} and {"k2, "v2"}, and "bar" has 1 annotation, {"k3", "v3"}, the json will look like:
// {
//     "foo": {
//         "k1": "v1",
//         "k2": "v2"
//     },
//     "bar": {
//         "k3": "v3"
//     }
// }
void DataRegister::UpdateJson(const std::string& _namespace, const Annotations& annotations) {
  using namespace rapidjson;

  auto& allocator = register_json_.GetAllocator();

  // If there are already annotations for |_namespace|, delete them.
  if (register_json_.HasMember(_namespace)) {
    register_json_.RemoveMember(_namespace);
  }

  // Make an empty object for |_namespace|.
  register_json_.AddMember(Value(_namespace, register_json_.GetAllocator()),
                           Value(rapidjson::kObjectType), allocator);

  const auto& json_annotations = register_json_[_namespace].GetObject();
  for (const auto& [k, v] : annotations) {
    auto key = Value(k, allocator);
    auto val = Value(v.Value(), allocator);
    json_annotations.AddMember(key, val, allocator);
  }

  StringBuffer buffer;
  PrettyWriter<StringBuffer> writer(buffer);

  register_json_.Accept(writer);

  if (!files::WriteFile(register_filepath_, buffer.GetString(), buffer.GetLength())) {
    FX_LOGS(ERROR) << "Failed to write data register contents to " << register_filepath_;
  }
}

void DataRegister::RestoreFromJson() {
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
    FX_LOGS(ERROR) << "Error parsing data register as JSON at offset " << ok.Offset() << " "
                   << GetParseError_En(ok.Code());
    files::DeletePath(register_filepath_, /*recursive=*/true);
    return;
  }

  // Each namespace in the register is represented by an object containing at string-string pairs
  // that are the annotations.
  FX_CHECK(register_json_.IsObject());
  for (const auto& member : register_json_.GetObject()) {
    // Skip any non-object members.
    if (!member.value.IsObject()) {
      continue;
    }

    const std::string _namespace = member.name.GetString();
    for (const auto& annotation : member.value.GetObject()) {
      // Annotations must be string pairs.
      if (!annotation.value.IsString()) {
        continue;
      }

      const std::string key = annotation.name.GetString();
      const std::string value = annotation.value.GetString();
      namespaced_annotations_[_namespace].emplace(key, value);
    }
  }
}

}  // namespace forensics::feedback
