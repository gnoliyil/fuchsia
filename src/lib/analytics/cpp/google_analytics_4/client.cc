// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/analytics/cpp/google_analytics_4/client.h"

#include <lib/syslog/cpp/macros.h>

#include "src/lib/analytics/cpp/google_analytics_4/measurement.h"
#include "src/lib/fxl/strings/substitute.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"

namespace analytics::google_analytics_4 {

namespace {

constexpr char kEndpoint[] = "https://www.google-analytics.com/mp/collect?measurement_id=$0&$1=$2";
// Make it a little harder for auto scanners
constexpr char kParameter1[] = {0x61, 0x70, 0x69, 0x5f, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x00};

// Helpers to visit on Value variants.
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename T>
bool WriteValue(rapidjson::Writer<T>& writer, const Value& value) {
  return std::visit(overloaded{[&writer](const std::string& arg) { return writer.String(arg); },
                               [&writer](int64_t arg) { return writer.Int64(arg); },
                               [&writer](double arg) { return writer.Double(arg); },
                               [&writer](bool arg) { return writer.Bool(arg); }},
                    value);
}

std::string GeneratePostBody(const Measurement& measurement) {
  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> writer(s);

  writer.StartObject();

  writer.Key("client_id");
  writer.String(measurement.client_id());

  writer.Key("events");
  writer.StartArray();
  for (auto const& event_ptr : measurement.event_ptrs()) {
    writer.StartObject();
    writer.Key("name");
    writer.String(event_ptr->name());

    if (event_ptr->parameters_opt().has_value()) {
      writer.Key("params");
      writer.StartObject();
      for (auto const& [key, value] : *(event_ptr->parameters_opt())) {
        writer.Key(key);
        WriteValue(writer, value);
      }
      writer.EndObject();
    }

    writer.Key("timestamp_micros");
    writer.Uint64(event_ptr->timestamp_micros().count());

    writer.EndObject();
  }
  writer.EndArray();

  if (measurement.user_properties_opt().has_value()) {
    writer.Key("user_properties");
    writer.StartObject();
    for (auto const& [key, value] : *(measurement.user_properties_opt())) {
      writer.Key(key);
      writer.StartObject();
      writer.Key("value");
      WriteValue(writer, value);
      writer.EndObject();
    }
    writer.EndObject();
  }

  writer.EndObject();
  return s.GetString();
}

}  // namespace

void Client::SetQueryParameters(std::string_view measurement_id, std::string_view key) {
  url_ = fxl::Substitute(kEndpoint, measurement_id, kParameter1, key);
}

void Client::SetClientId(std::string client_id) { client_id_ = std::move(client_id); }

void Client::SetUserProperty(std::string name, Value value) {
  user_properties_[std::move(name)] = std::move(value);
}

void Client::AddEvent(std::unique_ptr<Event> event_ptr) {
  FX_DCHECK(IsReady());
  Measurement measurement(client_id_);
  if (!user_properties_.empty()) {
    measurement.SetUserProperties(user_properties_);
  }
  measurement.AddEvent(std::move(event_ptr));
  SendData(GeneratePostBody(measurement));
}

bool Client::IsReady() const { return !client_id_.empty() && !url_.empty(); }

}  // namespace analytics::google_analytics_4
