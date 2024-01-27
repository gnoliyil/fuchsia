// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use ffx_core::ffx_plugin;
use ffx_hang_args::HangCommand;
use fidl_fuchsia_developer_ffx::TestingProxy;

#[ffx_plugin(TestingProxy = "daemon::protocol")]
pub async fn hang(testing_proxy: TestingProxy, _cmd: HangCommand) -> Result<()> {
    let _ = testing_proxy.hang().await;
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_developer_ffx::TestingRequest;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_hang_with_no_text() {
        static HUNG: AtomicBool = AtomicBool::new(false);
        let proxy = setup_fake_testing_proxy(|req| match req {
            TestingRequest::Hang { .. } => {
                HUNG.store(true, Ordering::SeqCst);
            }
            _ => assert!(false),
        });
        assert!(hang(proxy, HangCommand {}).await.is_ok());
        assert!(HUNG.load(Ordering::SeqCst));
    }
}
