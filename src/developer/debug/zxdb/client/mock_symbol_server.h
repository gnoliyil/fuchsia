// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_MOCK_SYMBOL_SERVER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_MOCK_SYMBOL_SERVER_H_

#include "src/developer/debug/zxdb/client/symbol_server.h"

namespace zxdb {

// Mocks out the typical curl interfaces to allow tests to intercept and inspect
// the method calls.
class MockSymbolServer : public SymbolServer {
 public:
  MockSymbolServer(Session* session, const std::string& url) : SymbolServer(session, url) {}

  // Finishes constructing the object. This is manual for the mock class so we can get our
  // instrumentation in place before we do the heavier parts of the initialization.
  void InitForTest() { DoInit(); }

  // The big IO methods are proxied to callbacks for the mock so tests can just intercept them.
  //
  // These are fit::function and not fit::callback because they can be called more than once.
  fit::function<void(const std::string&, DebugSymbolFileType, SymbolServer::CheckFetchCallback)>
      on_check_fetch = {};
  fit::function<void(const std::map<std::string, std::string>&, fit::callback<void(const Err&)>)>
      on_do_authenticate = {};

  // Force the symbol server into the ready state.
  void ForceReady() { ChangeState(SymbolServer::State::kReady); }

  // Implementation of Symbol server.
  void CheckFetch(const std::string& build_id, DebugSymbolFileType file_type,
                  SymbolServer::CheckFetchCallback cb) override {
    on_check_fetch(build_id, file_type, std::move(cb));
  }

 private:
  void DoAuthenticate(const std::map<std::string, std::string>& data,
                      fit::callback<void(const Err&)> cb) override {
    on_do_authenticate(data, std::move(cb));
  }
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_MOCK_SYMBOL_SERVER_H_
