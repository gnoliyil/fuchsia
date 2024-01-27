// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {fidl_fuchsia_bluetooth as bt, thiserror::Error};

/// Error type that can be constructed from a Bluetooth FIDL Error or from on its own.
#[derive(Debug, Error)]
#[error("{}", message)]
struct Error {
    message: String,
}

impl From<bt::Error> for Error {
    fn from(err: bt::Error) -> Error {
        Error {
            message: match err.description {
                Some(d) => d,
                None => "unknown Bluetooth FIDL error".to_string(),
            },
        }
    }
}

impl From<bt::ErrorCode> for Error {
    fn from(err: bt::ErrorCode) -> Error {
        Error { message: format!("Bluetooth Error Code {:?}", err) }
    }
}

impl From<fidl::Error> for Error {
    fn from(err: fidl::Error) -> Error {
        Error { message: format!("FIDL error: {}", err) }
    }
}
