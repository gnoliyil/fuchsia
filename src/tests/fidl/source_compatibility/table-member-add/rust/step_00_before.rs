// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]

use fidl_fidl_test_tablememberadd as fidl_lib;

// [START contents]
fn use_table(profile: &fidl_lib::Profile) {
    if let Some(tz) = &profile.timezone {
        println!("timezone: {:?}", tz);
    }
    if let Some(unit) = &profile.temperature_unit {
        println!("preferred unit: {:?}", unit);
    }
}
// [END contents]

fn main() {}
