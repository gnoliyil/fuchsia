// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use ffx_core::ffx_plugin;
use ffx_target_remove_args::RemoveCommand;
use fidl_fuchsia_developer_ffx::TargetCollectionProxy;

#[ffx_plugin(TargetCollectionProxy = "daemon::protocol")]
pub async fn remove(target_collection: TargetCollectionProxy, cmd: RemoveCommand) -> Result<()> {
    remove_impl(target_collection, cmd, &mut std::io::stderr()).await
}

pub async fn remove_impl<W: std::io::Write>(
    target_collection: TargetCollectionProxy,
    cmd: RemoveCommand,
    err_writer: &mut W,
) -> Result<()> {
    let RemoveCommand { mut name_or_addr, .. } = cmd;
    if target_collection.remove_target(&mut name_or_addr).await? {
        writeln!(err_writer, "Removed.")?;
    } else {
        writeln!(err_writer, "No matching target found.")?;
    }
    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_developer_ffx as ffx;

    fn setup_fake_target_collection_proxy<T: 'static + Fn(String) -> bool + Send>(
        test: T,
    ) -> TargetCollectionProxy {
        setup_fake_target_collection(move |req| match req {
            ffx::TargetCollectionRequest::RemoveTarget { target_id, responder } => {
                let result = test(target_id);
                responder.send(result).unwrap();
            }
            _ => assert!(false),
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_remove_existing_target() {
        let server = setup_fake_target_collection_proxy(|id| {
            assert_eq!(id, "correct-horse-battery-staple".to_owned());
            true
        });
        let mut buf = Vec::new();
        remove_impl(
            server,
            RemoveCommand { name_or_addr: "correct-horse-battery-staple".to_owned() },
            &mut buf,
        )
        .await
        .unwrap();

        let output = String::from_utf8(buf).unwrap();
        assert_eq!(output, "Removed.\n");
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_remove_nonexisting_target() {
        let server = setup_fake_target_collection_proxy(|_| false);
        let mut buf = Vec::new();
        remove_impl(
            server,
            RemoveCommand { name_or_addr: "incorrect-donkey-battery-jazz".to_owned() },
            &mut buf,
        )
        .await
        .unwrap();

        let output = String::from_utf8(buf).unwrap();
        assert_eq!(output, "No matching target found.\n");
    }
}
