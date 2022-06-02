// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {anyhow::Error, std::env};

fn main() -> Result<(), Error> {
    let mut args = env::args_os();
    // Skip binary name.
    args.next();
    odu::run(args)
}
