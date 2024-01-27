// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod config;
pub mod ota;
pub mod setup;
pub mod storage;

use anyhow::{format_err, Error};
use async_trait::async_trait;
use fidl_fuchsia_component::{BinderMarker, CreateChildArgs, RealmMarker, RealmProxy};
use fidl_fuchsia_component_decl::{Child, ChildRef, CollectionRef, StartupMode};
use fidl_fuchsia_logger as flog;
use fuchsia_component::client;
use fuchsia_syslog_listener::LogProcessor;
use futures::{channel::oneshot, lock::Mutex, Future, FutureExt};
use std::pin::Pin;

const COLLECTION_NAME: &str = "ota";
const CHILD_NAME: &str = "system_recovery_ota";
const OTA_COMPONENT_URL: &str = "#meta/system_recovery_ota.cm";

type ChildLauncherRet = Pin<Box<dyn Future<Output = Result<(), Error>> + Send>>;
pub type ChildLauncherFn = Box<dyn Fn() -> ChildLauncherRet + Send + Sync>;

pub async fn child_launcher() -> Result<(), Error> {
    client::connect_to_childs_protocol::<BinderMarker>(
        String::from(CHILD_NAME),
        Some(String::from(COLLECTION_NAME)),
    )
    .await?;
    Ok(())
}

#[derive(Debug, PartialEq, Clone)]
pub enum OtaStatus {
    Succeeded,
    Failed,
    Cancelled,
}

#[async_trait]
pub trait OtaManager {
    async fn start_and_wait_for_result(&self) -> Result<(), Error>;
    async fn stop(&self) -> Result<(), Error>;
    async fn complete_ota(&self, status: OtaStatus);
}

pub struct OtaComponent {
    realm: RealmProxy,
    completers: Mutex<Vec<oneshot::Sender<OtaStatus>>>,
    child_launcher: ChildLauncherFn,
}
impl OtaComponent {
    pub fn new() -> Result<Self, Error> {
        let realm = client::connect_to_protocol::<RealmMarker>()
            .map_err(|e| format_err!("failed to connect to fuchsia.component.Realm: {:?}", e))?;
        Ok(Self::new_with_realm_and_launcher(realm, Box::new(|| child_launcher().boxed())))
    }

    pub fn new_with_realm_and_launcher(realm: RealmProxy, child_launcher: ChildLauncherFn) -> Self {
        Self { realm, completers: Mutex::new(Vec::new()), child_launcher }
    }
}

#[async_trait]
impl OtaManager for OtaComponent {
    async fn start_and_wait_for_result(&self) -> Result<(), Error> {
        // Store the completer even if launching the child may fail. It will be cleaned up
        // when `complete_ota` is called.
        let (sender, receiver) = oneshot::channel::<OtaStatus>();
        self.completers.lock().await.push(sender);

        let collection_ref = CollectionRef { name: String::from(COLLECTION_NAME) };
        let child_decl = Child {
            name: Some(String::from(CHILD_NAME)),
            url: Some(String::from(OTA_COMPONENT_URL)),
            startup: Some(StartupMode::Lazy),
            ..Default::default()
        };

        self.realm
            .create_child(&collection_ref, &child_decl, CreateChildArgs::default())
            .await
            .expect("create_child failed")
            .map_err(|e| format_err!("failed to start OTA child: {:?}", e))?;

        (self.child_launcher)().await?;

        match receiver.await {
            Ok(status) => match status {
                OtaStatus::Succeeded => Ok(()),
                OtaStatus::Failed => Err(format_err!("OTA failed")),
                OtaStatus::Cancelled => Err(format_err!("OTA cancelled")),
            },
            Err(_) => Err(format_err!("sender dropped")),
        }
    }

    async fn stop(&self) -> Result<(), Error> {
        let child_ref = ChildRef {
            name: String::from(CHILD_NAME),
            collection: Some(String::from(COLLECTION_NAME)),
        };

        self.realm
            .destroy_child(&child_ref)
            .await
            .expect("destroy_child failed")
            .map_err(|e| format_err!("failed to destroy OTA child: {:?}", e))?;

        _ = self.complete_ota(OtaStatus::Cancelled);
        Ok(())
    }

    async fn complete_ota(&self, status: OtaStatus) {
        while let Some(completer) = self.completers.lock().await.pop() {
            // If receiver was dropped, ignore. Status is sent in `start_and_wait_for_result`.
            _ = completer.send(status.clone());
        }
    }
}

fn get_log_level(level: i32) -> String {
    // note levels align with syslog logger.h definitions
    match level {
        l if (l == flog::LogLevelFilter::Trace as i32) => "TRACE".to_string(),
        l if (l == flog::LogLevelFilter::Debug as i32) => "DEBUG".to_string(),
        l if (l < flog::LogLevelFilter::Info as i32 && l > flog::LogLevelFilter::Debug as i32) => {
            format!("VLOG({})", (flog::LogLevelFilter::Info as i32) - l)
        }
        l if (l == flog::LogLevelFilter::Info as i32) => "INFO".to_string(),
        l if (l == flog::LogLevelFilter::Warn as i32) => "WARNING".to_string(),
        l if (l == flog::LogLevelFilter::Error as i32) => "ERROR".to_string(),
        l if (l == flog::LogLevelFilter::Fatal as i32) => "FATAL".to_string(),
        l => format!("INVALID({})", l),
    }
}

// Assume monotonic time is sufficient for debug logs in recovery.
fn format_time(timestamp: fuchsia_zircon::sys::zx_time_t) -> String {
    format!("{:05}.{:06}", timestamp / 1000000000, (timestamp / 1000) % 1000000)
}

pub type LogHandlerFnPtr = Box<dyn FnMut(String)>;

#[async_trait(?Send)]
pub trait OtaLogListener {
    async fn listen(&self, handler: LogHandlerFnPtr) -> Result<(), Error>;
}

pub struct OtaLogListenerImpl {
    log_proxy: flog::LogProxy,
}

impl OtaLogListenerImpl {
    pub fn new() -> Result<Self, Error> {
        let log_proxy = client::connect_to_protocol::<flog::LogMarker>()
            .map_err(|e| format_err!("failed to connect to fuchsia.logger.Log: {:?}", e))?;
        Ok(Self::new_with_proxy(log_proxy))
    }

    pub fn new_with_proxy(log_proxy: flog::LogProxy) -> Self {
        Self { log_proxy }
    }
}

#[async_trait(?Send)]
impl OtaLogListener for OtaLogListenerImpl {
    async fn listen(&self, handler: LogHandlerFnPtr) -> Result<(), Error> {
        let options = flog::LogFilterOptions {
            filter_by_pid: false,
            pid: 0,
            min_severity: flog::LogLevelFilter::None,
            verbosity: 0,
            filter_by_tid: false,
            tid: 0,
            tags: vec![format!("{}:{}", COLLECTION_NAME, CHILD_NAME)],
        };

        fuchsia_syslog_listener::run_log_listener_with_proxy(
            &self.log_proxy,
            LogProcessorFn(handler),
            Some(&options),
            false,
            None,
        )
        .await
    }
}

// We cannot directly implement LogProcessor for FnMut(String). See rustc error E0210 for more info.
// To work around this, the FnMut(String) must be wrapped in a local type that implements the trait.
struct LogProcessorFn(LogHandlerFnPtr);

impl LogProcessor for LogProcessorFn {
    fn log(&mut self, message: flog::LogMessage) {
        let tags = message.tags.join(", ");

        let line = format!(
            "[{}][{}] {}: {}",
            format_time(message.time),
            tags,
            get_log_level(message.severity),
            message.msg
        );

        (self.0)(line);
    }

    fn done(&mut self) {
        // No need to do anything since we are streaming rather than requesting a one-time dump.
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_component::{Error, RealmMarker, RealmRequest};
    use fuchsia_async as fasync;
    use futures::StreamExt;
    use std::sync::{
        atomic::{AtomicU8, Ordering},
        Arc,
    };

    fn create_child_launcher(call_count: Arc<AtomicU8>) -> ChildLauncherFn {
        Box::new(move || {
            let call_count = call_count.clone();
            async move {
                call_count.fetch_add(1, Ordering::SeqCst);
                Ok(())
            }
            .boxed()
        })
    }

    fn create_failing_child_launcher(call_count: Arc<AtomicU8>) -> ChildLauncherFn {
        Box::new(move || {
            let call_count = call_count.clone();
            async move {
                call_count.fetch_add(1, Ordering::SeqCst);
                Err(format_err!("failed to launch child"))
            }
            .boxed()
        })
    }

    #[fuchsia::test]
    async fn test_complete_ota_sends_no_requests() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));

        let ota_manager = OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        );
        ota_manager.complete_ota(OtaStatus::Succeeded).await;
        ota_manager.complete_ota(OtaStatus::Failed).await;

        // Drop ota_manager so `stream` closes.
        drop(ota_manager);

        // No requests should be sent.
        assert!(stream.next().await.is_none());
        assert_eq!(0, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_start_propagates_success_on_ota_success() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = Arc::new(OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        ));
        let ota_manager2 = ota_manager.clone();

        fasync::Task::local(async move {
            assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::CreateChild {
                collection,
                decl,
                args,
                responder
            } => {
                assert_eq!(COLLECTION_NAME.to_string(), collection.name);
                assert_eq!(Some(CHILD_NAME.to_string()), decl.name);
                assert_eq!(Some(OTA_COMPONENT_URL.to_string()), decl.url);
                assert_eq!(Some(StartupMode::Lazy), decl.startup);
                assert_eq!(CreateChildArgs::default(), args);
                responder.send(Ok(())).unwrap();
            });

            ota_manager2.complete_ota(OtaStatus::Succeeded).await;
        })
        .detach();

        ota_manager.start_and_wait_for_result().await.unwrap();
        assert_eq!(1, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_start_propagates_error_on_ota_failure() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = Arc::new(OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        ));
        let ota_manager2 = ota_manager.clone();

        fasync::Task::local(async move {
            assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::CreateChild {
                responder, ..
            } => {
                responder.send(Ok(())).unwrap();
            });

            ota_manager2.complete_ota(OtaStatus::Failed).await;
        })
        .detach();

        ota_manager.start_and_wait_for_result().await.unwrap_err();
        assert_eq!(1, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_start_propagates_error_on_launch_child_failure() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = Arc::new(OtaComponent::new_with_realm_and_launcher(
            client,
            create_failing_child_launcher(launch_count.clone()),
        ));
        let ota_manager2 = ota_manager.clone();

        fasync::Task::local(async move {
            assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::CreateChild {
                responder, ..
            } => {
                responder.send(Ok(())).unwrap();
            });

            // complete_ota should have no effect on error state.
            ota_manager2.complete_ota(OtaStatus::Succeeded).await;
        })
        .detach();

        ota_manager.start_and_wait_for_result().await.unwrap_err();
        assert_eq!(1, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_stop_proxies_to_realm_returns_ok() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        );

        fasync::Task::local(async move {
            assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::DestroyChild {
                child,
                responder
            } => {
                assert_eq!(CHILD_NAME.to_string(), child.name);
                assert_eq!(Some(COLLECTION_NAME.to_string()), child.collection);
                responder.send(Ok(())).unwrap();
            });
        })
        .detach();

        ota_manager.stop().await.unwrap();
        assert_eq!(0, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_stop_proxies_to_realm_returns_err() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        );

        fasync::Task::local(async move {
            assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::DestroyChild {
                child,
                responder
            } => {
                assert_eq!(CHILD_NAME.to_string(), child.name);
                assert_eq!(Some(COLLECTION_NAME.to_string()), child.collection);
                responder.send(Err(Error::Internal)).unwrap();
            });
        })
        .detach();

        ota_manager.stop().await.unwrap_err();
        assert_eq!(0, launch_count.load(Ordering::Relaxed));
    }

    #[fuchsia::test]
    async fn test_stop_unblocks_start_with_err() {
        let (client, mut stream) = create_proxy_and_stream::<RealmMarker>().unwrap();
        let launch_count = Arc::new(AtomicU8::new(0));
        let ota_manager = Arc::new(OtaComponent::new_with_realm_and_launcher(
            client,
            create_child_launcher(launch_count.clone()),
        ));
        let ota_manager2 = ota_manager.clone();

        fasync::Task::local(async move {
            ota_manager.start_and_wait_for_result().await.unwrap_err();
            assert_eq!(1, launch_count.load(Ordering::Relaxed));
        })
        .detach();

        assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::CreateChild {
            collection,
            decl,
            args,
            responder
        } => {
            assert_eq!(COLLECTION_NAME.to_string(), collection.name);
            assert_eq!(Some(CHILD_NAME.to_string()), decl.name);
            assert_eq!(Some(OTA_COMPONENT_URL.to_string()), decl.url);
            assert_eq!(Some(StartupMode::Lazy), decl.startup);
            assert_eq!(CreateChildArgs::default(), args);
            responder.send(Ok(())).unwrap();
        });

        fasync::Task::local(async move {
            ota_manager2.stop().await.unwrap_err();
        })
        .detach();

        assert_matches!(stream.next().await.unwrap().unwrap(), RealmRequest::DestroyChild {
            child,
            responder
        } => {
            assert_eq!(CHILD_NAME.to_string(), child.name);
            assert_eq!(Some(COLLECTION_NAME.to_string()), child.collection);
            responder.send(Ok(())).unwrap();
        });
    }

    #[fuchsia::test]
    async fn test_log_listener_listens() -> Result<(), Error> {
        let (log_proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<flog::LogMarker>().unwrap();
        let listener = OtaLogListenerImpl::new_with_proxy(log_proxy);
        let lines = Arc::new(Mutex::new(Vec::new()));
        let lines2 = lines.clone();
        let expected_msg = "this is a test message".to_string();

        fasync::Task::local(async move {
            let lines = lines2.clone();
            listener
                .listen(Box::new(move |line| {
                    let lines = lines.clone();
                    futures::executor::block_on(async move {
                        lines.lock().await.push(line);
                    });
                }))
                .await
                .unwrap();
        })
        .detach();

        let request = stream.next().await.unwrap().unwrap();
        match request {
            flog::LogRequest::ListenSafe { log_listener, options, .. } => {
                let tag = format!("{}:{}", COLLECTION_NAME, CHILD_NAME);
                assert_eq!(tag, options.unwrap().tags[0]);

                log_listener
                    .into_proxy()
                    .expect("create log_listener proxy")
                    .log(&flog::LogMessage {
                        pid: 0,
                        tid: 0,
                        time: 0,
                        severity: 0,
                        dropped_logs: 0,
                        tags: vec![tag],
                        msg: expected_msg.clone(),
                    })
                    .await
                    .unwrap();
            }
            e => panic!("Unexpected request: {:?}", e),
        }

        assert_eq!(1, lines.lock().await.len());
        assert!(lines.lock().await[0].ends_with(&expected_msg));

        Ok(())
    }
}
