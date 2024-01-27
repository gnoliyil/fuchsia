// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Errors from `$responder.send` can be safely ignored during regular operation;
// they are handled only by logging to error.
macro_rules! responder_send {
    ($responder:expr, $arg:expr) => {
        $responder.send($arg).unwrap_or_else(|e| ::tracing::error!("Responder send error: {:?}", e))
    };
}
