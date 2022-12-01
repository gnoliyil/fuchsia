// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use tracing::{error, info, warn};

/// A suite of helper methods: instead of writing:
/// ```
///   do_something().map_err(|e| {
///     warn!(format!("Something happened: {:?}", e);
///     return ErrorType::Foo;
///   })
/// ```
/// this trait lets us write:
/// ```
///   do_something().log_warn_then("Something happened", ErrorType::Foo);
/// ```
///
/// NB: Once Result::inspect_err leaves nightly
/// (https://doc.rust-lang.org/std/result/enum.Result.html#method.inspect_err)
/// it _might_ then be preferable to write:
///
/// ```
///   do_something()
///     .inspect_err(|e| format!("Something happened: {:?}", e))
///     .map_err(|_| ErrorType::Foo)
/// ```
///
pub trait LogThen<T, F> {
    fn log_info_then(self, msg: &str, err: F) -> Result<T, F>;
    fn log_warn_then(self, msg: &str, err: F) -> Result<T, F>;
    fn log_error_then(self, msg: &str, err: F) -> Result<T, F>;
}

impl<T, E: std::fmt::Debug, F> LogThen<T, F> for Result<T, E> {
    fn log_info_then(self, msg: &str, err: F) -> Result<T, F> {
        self.map_err(|e| {
            info!("{}: {:?}", msg, e);
            err
        })
    }
    fn log_warn_then(self, msg: &str, err: F) -> Result<T, F> {
        self.map_err(|e| {
            warn!("{}: {:?}", msg, e);
            err
        })
    }
    fn log_error_then(self, msg: &str, err: F) -> Result<T, F> {
        self.map_err(|e| {
            error!("{}: {:?}", msg, e);
            err
        })
    }
}
