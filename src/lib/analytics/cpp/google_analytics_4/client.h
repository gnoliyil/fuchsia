// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_4_CLIENT_H_
#define SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_4_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>

#include "src/lib/analytics/cpp/google_analytics_4/event.h"

namespace analytics::google_analytics_4 {

// This is an abstract class for a GA4 client, where the actual HTTP communications are
// left unimplemented. This is because to provide non-blocking HTTP communications, we have to rely
// on certain async mechanism (such as message loop), which is usually chosen by the embedding app.
// To use this class, the embedding app only needs to implement the SendData() method.
class Client {
 public:
  Client() = default;
  Client(const Client&) = delete;

  virtual ~Client() = default;

  // Set the query parameters needed by the GA4 Measurement Protocol.
  // We do not escape or validate the parameters. The tool analytics implementer is responsible
  // for the correctness of the parameters.
  void SetQueryParameters(std::string_view measurement_id, std::string_view key);

  void SetClientId(std::string client_id);
  // Add user property shared by all metrics.
  void SetUserProperty(std::string name, Value value);

  void AddEvent(std::unique_ptr<Event> event_ptr);

 protected:
  auto& url() { return url_; }

 private:
  bool IsReady() const;
  virtual void SendData(std::string body) = 0;

  std::string client_id_;
  std::string url_;
  // Stores shared parameters
  std::map<std::string, Value> user_properties_;
};

}  // namespace analytics::google_analytics_4

#endif  // SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_4_CLIENT_H_
