// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use errors::ffx_bail;
use ffx_core::ffx_plugin;
use ffx_repository_server_stop_args::StopCommand;
use fidl_fuchsia_developer_ffx::RepositoryRegistryProxy;
use fidl_fuchsia_developer_ffx_ext::RepositoryError;
use pkg::config as pkg_config;

#[ffx_plugin(RepositoryRegistryProxy = "daemon::protocol")]
pub async fn stop(_cmd: StopCommand, repos: RepositoryRegistryProxy) -> Result<()> {
    match repos.server_stop().await {
        Ok(Ok(())) => {
            println!("Stopped the repository server");

            Ok(())
        }
        Ok(Err(err)) => {
            let err = RepositoryError::from(err);
            match err {
                RepositoryError::ServerNotRunning => {
                    eprintln!("No repository server is running");

                    Ok(())
                }
                err => {
                    // If we failed to communicate with the daemon, disable the server so it doesn't start
                    // next time the daemon starts.
                    let _ = pkg_config::set_repository_server_enabled(false).await;

                    ffx_bail!("Failed to stop the server: {}", RepositoryError::from(err))
                }
            }
        }
        Err(err) => {
            // If we failed to communicate with the daemon, disable the server so it doesn't start
            // next time the daemon starts.
            let _ = pkg_config::set_repository_server_enabled(false).await;

            ffx_bail!("Failed to communicate with the daemon: {}", err)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_developer_ffx::{RepositoryRegistryMarker, RepositoryRegistryRequest};
    use fuchsia_async;
    use futures::channel::oneshot::channel;
    use std::{
        future::Future,
        sync::{Arc, Mutex},
    };

    lazy_static::lazy_static! {
        static ref TEST_LOCK: Arc<Mutex<()>> = Arc::new(Mutex::new(()));
    }

    fn run_test<F: Future>(fut: F) -> F::Output {
        let _guard = TEST_LOCK.lock().unwrap();

        fuchsia_async::TestExecutor::new().run_singlethreaded(async move {
            let _env = ffx_config::test_init().await.unwrap();
            fut.await
        })
    }

    #[test]
    fn test_stop() {
        run_test(async {
            let (sender, receiver) = channel();
            let mut sender = Some(sender);
            let repos = setup_fake_repos(move |req| match req {
                RepositoryRegistryRequest::ServerStop { responder } => {
                    sender.take().unwrap().send(()).unwrap();
                    responder.send(&mut Ok(())).unwrap()
                }
                other => panic!("Unexpected request: {:?}", other),
            });

            stop(StopCommand {}, repos).await.unwrap();
            assert!(receiver.await.is_ok());
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_stop_disables_server_on_error() {
        let _env = ffx_config::test_init().await.unwrap();
        pkg_config::set_repository_server_enabled(true).await.unwrap();

        let (sender, receiver) = channel();
        let mut sender = Some(sender);
        let repos = setup_fake_repos(move |req| match req {
            RepositoryRegistryRequest::ServerStop { responder } => {
                sender.take().unwrap().send(()).unwrap();
                responder.send(&mut Err(RepositoryError::InternalError.into())).unwrap()
            }
            other => panic!("Unexpected request: {:?}", other),
        });

        assert!(stop(StopCommand {}, repos).await.is_err());
        assert!(receiver.await.is_ok());

        assert!(!pkg_config::get_repository_server_enabled().await.unwrap());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_stop_disables_server_on_communication_error() {
        let _env = ffx_config::test_init().await.unwrap();
        pkg_config::set_repository_server_enabled(true).await.unwrap();

        let (repos, stream) =
            fidl::endpoints::create_proxy_and_stream::<RepositoryRegistryMarker>().unwrap();
        drop(stream);

        assert!(stop(StopCommand {}, repos).await.is_err());
        assert!(!pkg_config::get_repository_server_enabled().await.unwrap());
    }
}
