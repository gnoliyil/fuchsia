// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {fidl_fuchsia_io as fio, fuchsia_fs::directory::clone_no_describe};

// TODO(https://fxbug.dev/94654): We should probably preserve the original error messages
// instead of dropping them.
pub fn clone_dir(dir: Option<&fio::DirectoryProxy>) -> Option<fio::DirectoryProxy> {
    dir.and_then(|d| clone_no_describe(d, None).ok())
}
