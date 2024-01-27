// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.logger/cpp/fidl.h>
#include <lib/driver/testing/cpp/test_environment.h>

namespace fdf_testing {

const char TestEnvironment::kTestEnvironmentThreadSafetyDescription[] =
    "|fdf_testing::TestEnvironment| is thread-unsafe.";

zx::result<> TestEnvironment::Initialize(
    fidl::ServerEnd<fuchsia_io::Directory> incoming_directory_server_end) {
  std::lock_guard guard(checker_);

  zx::result result = incoming_directory_server_.Serve(std::move(incoming_directory_server_end));
  if (result.is_error()) {
    return result.take_error();
  }

  // Forward the LogSink protocol that we have from our own incoming namespace.
  result = incoming_directory_server_.AddUnmanagedProtocol<fuchsia_logger::LogSink>(
      [](fidl::ServerEnd<fuchsia_logger::LogSink> server_end) {
        ZX_ASSERT(component::Connect<fuchsia_logger::LogSink>(std::move(server_end)).is_ok());
      });
  if (result.is_error()) {
    return result.take_error();
  }

  return zx::ok();
}

}  // namespace fdf_testing
