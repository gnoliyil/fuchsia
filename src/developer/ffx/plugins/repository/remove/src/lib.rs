// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use errors::ffx_bail;
use ffx_core::ffx_plugin;
use ffx_repository_remove_args::RemoveCommand;
use fidl_fuchsia_developer_ffx::RepositoryRegistryProxy;

#[ffx_plugin(RepositoryRegistryProxy = "daemon::protocol")]
pub async fn remove(cmd: RemoveCommand, repos: RepositoryRegistryProxy) -> Result<()> {
    if !repos.remove_repository(&cmd.name).await? {
        ffx_bail!("No repository named \"{}\".", cmd.name);
    }

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_developer_ffx::RepositoryRegistryRequest;
    use fuchsia_async as fasync;
    use futures::channel::oneshot::channel;

    #[fasync::run_singlethreaded(test)]
    async fn test_remove() {
        let (sender, receiver) = channel();
        let mut sender = Some(sender);
        let repos = setup_fake_repos(move |req| match req {
            RepositoryRegistryRequest::RemoveRepository { name, responder } => {
                sender.take().unwrap().send(name).unwrap();
                responder.send(true).unwrap()
            }
            other => panic!("Unexpected request: {:?}", other),
        });
        remove(RemoveCommand { name: "MyRepo".to_owned() }, repos).await.unwrap();
        assert_eq!(receiver.await.unwrap(), "MyRepo".to_owned());
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_remove_fail() {
        let repos = setup_fake_repos(move |req| match req {
            RepositoryRegistryRequest::RemoveRepository { responder, .. } => {
                responder.send(false).unwrap()
            }
            other => panic!("Unexpected request: {:?}", other),
        });
        assert!(remove(RemoveCommand { name: "NotMyRepo".to_owned() }, repos).await.is_err());
    }
}
