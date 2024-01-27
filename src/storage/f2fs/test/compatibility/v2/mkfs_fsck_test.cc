// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/test/compatibility/v2/compatibility.h"

namespace f2fs {
namespace {

using MkfsFsckCompatibilityTest = F2fsGuestTest;

TEST_F(MkfsFsckCompatibilityTest, MkfsFsckLinuxToFuchsia) {
  // mkfs on Linux
  GetEnclosedGuest().GetLinuxOperator().Mkfs();

  // fsck on Fuchsia
  GetEnclosedGuest().GetFuchsiaOperator().Fsck();
}

TEST_F(MkfsFsckCompatibilityTest, MkfsFsckFuchsiaToLinux) {
  // mkfs on Fuchsia
  GetEnclosedGuest().GetFuchsiaOperator().Mkfs();

  // fsck on Linux
  GetEnclosedGuest().GetLinuxOperator().Fsck();
}

}  // namespace
}  // namespace f2fs
