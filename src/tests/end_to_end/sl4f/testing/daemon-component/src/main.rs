// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{thread, time::Duration};

fn main() {
    // this component run forever.
    loop {
        thread::sleep(Duration::from_secs(10));
    }
}
