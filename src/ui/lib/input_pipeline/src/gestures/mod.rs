// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod args;
mod gesture_arena;
mod motion;
mod one_finger_button;
mod primary_tap;
mod scroll;
mod secondary_button;
mod secondary_tap;
mod utils;

#[cfg(test)]
mod tests;

pub use gesture_arena::make_input_handler as make_touchpad_gestures_handler;
