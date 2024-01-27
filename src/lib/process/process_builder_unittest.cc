// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/process/process_builder.h"

#include <lib/sys/cpp/service_directory.h>

#include <gtest/gtest.h>

namespace process {
namespace {

constexpr char kShell[] = "/pkg/bin/sh";

TEST(ProcessBuilder, Control) {
  ProcessBuilder builder(sys::ServiceDirectory::CreateFromNamespace());
  ASSERT_EQ(ZX_OK, builder.LoadPath(kShell));
  ASSERT_EQ(ZX_OK, builder.AddArgs({kShell}));
  ASSERT_EQ(ZX_OK, builder.CloneAll());
  ASSERT_EQ(ZX_OK, builder.Prepare(nullptr));
  EXPECT_TRUE(builder.data().process.is_valid());
  EXPECT_TRUE(builder.data().root_vmar.is_valid());
  EXPECT_GT(builder.data().stack, 0u);
  EXPECT_GT(builder.data().entry, 0u);
  EXPECT_GT(builder.data().vdso_base, 0u);
  EXPECT_GT(builder.data().base, 0u);

  zx::process process;
  ASSERT_EQ(ZX_OK, builder.Start(&process));
  process.kill();
}

}  // namespace
}  // namespace process
