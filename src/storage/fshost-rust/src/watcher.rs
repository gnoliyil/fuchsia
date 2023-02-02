// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::device::{BlockDevice, Device, NandDevice},
    anyhow::{Context as _, Error},
    assert_matches::assert_matches,
    fuchsia_async as fasync,
    futures::{channel::mpsc, lock::Mutex, stream, SinkExt, StreamExt},
    std::sync::Arc,
};

const DEV_CLASS_BLOCK: &'static str = "/dev/class/block";
const DEV_CLASS_NAND: &'static str = "/dev/class/nand";

enum PauseEvent {
    Pause,
    /// This stream is the newly initiated stream of block devices from the directory watcher.
    Resume(stream::BoxStream<'static, Box<dyn Device>>),
}

impl std::fmt::Debug for PauseEvent {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PauseEvent::Pause => std::write!(f, "PauseEvent::Pause"),
            PauseEvent::Resume(_) => std::write!(f, "PauseEvent::Resume"),
        }
    }
}

impl PauseEvent {
    /// Get the stream of block devices generated by the directory watcher out of the event.
    fn stream(self) -> Option<stream::BoxStream<'static, Box<dyn Device>>> {
        match self {
            PauseEvent::Pause => None,
            PauseEvent::Resume(stream) => Some(stream),
        }
    }
}

/// Generates a stream of block devices based off the path events we get from setting up a
/// directory watcher on this path. This will set up a new directory watcher every time stream is
/// called.
#[derive(Clone, Debug)]
struct PathSource {
    path: &'static str,
    is_nand: bool,
}

impl PathSource {
    fn new(path: &'static str, is_nand: bool) -> Self {
        PathSource { path, is_nand }
    }

    /// Sets up a new directory watcher against the configured path and returns the stream of block
    /// devices found at that path. If [`ignore_existing`] is true then we skip
    /// [`PathEvent::Existing`] events, and only add to the stream when new directory entries are
    /// added after this call.
    async fn stream(
        &mut self,
        ignore_existing: bool,
    ) -> Result<stream::BoxStream<'static, Box<dyn Device>>, Error> {
        let path = self.path;
        let is_nand = self.is_nand;
        let dir_proxy =
            fuchsia_fs::directory::open_in_namespace(path, fuchsia_fs::OpenFlags::RIGHT_READABLE)
                .context(format!("Failed to open directory at {path}"))?;
        let watcher = fuchsia_vfs_watcher::Watcher::new(&dir_proxy)
            .await
            .context(format!("Failed to watch {path}"))?;
        Ok(Box::pin(
            watcher
                .filter_map(|result| {
                    futures::future::ready({
                        match result {
                            Ok(message) => Some(message),
                            Err(error) => {
                                tracing::error!(?error, "fshost block watcher stream error");
                                None
                            }
                        }
                    })
                })
                .filter(|message| futures::future::ready(message.filename.as_os_str() != "."))
                .filter_map(move |fuchsia_vfs_watcher::WatchMessage { event, filename }| {
                    futures::future::ready({
                        let file_path = format!("{}/{}", path, filename.to_str().unwrap());
                        match event {
                            fuchsia_vfs_watcher::WatchEvent::ADD_FILE => Some(file_path),
                            fuchsia_vfs_watcher::WatchEvent::EXISTING => {
                                if ignore_existing {
                                    None
                                } else {
                                    Some(file_path)
                                }
                            }
                            _ => None,
                        }
                    })
                })
                .filter_map(move |path| async move {
                    if is_nand {
                        NandDevice::new(path).await.map(|d| Box::new(d) as Box<dyn Device>)
                    } else {
                        BlockDevice::new(path).await.map(|d| Box::new(d) as Box<dyn Device>)
                    }
                    .map_err(|e| {
                        tracing::warn!("Failed to create device (maybe it went away?): {:?}", e);
                        e
                    })
                    .ok()
                }),
        ))
    }
}

/// Watcher generates new [`BlockDevice`]s for fshost to process. It provides pausing and resuming
/// mechanisms, which allow the stream to be temporarily stopped.
#[derive(Clone, Debug)]
pub struct Watcher {
    /// This is a bool in a mutex instead of an AtomicBool because it doubles as a lock for the
    /// pause and resume calls to make sure their event signals get through in the right order.
    paused: Arc<Mutex<bool>>,
    pause_event_tx: mpsc::Sender<PauseEvent>,
    block_source: PathSource,
    nand_source: PathSource,
    _watcher_task: Arc<fasync::Task<()>>,
}

impl Watcher {
    /// Create a new Watcher and BlockDevice stream. The watcher will start watching
    /// /dev/class/block immediately, initially populating the stream with any entries which are
    /// already there, then sending new items on the stream as they are added to the directory.
    ///
    /// Watcher provides pause and resume which will stop the watcher from sending new entries on
    /// the stream.
    pub async fn new() -> Result<(Self, impl futures::Stream<Item = Box<dyn Device>>), Error> {
        let block_source = PathSource::new(DEV_CLASS_BLOCK, false);
        let nand_source = PathSource::new(DEV_CLASS_NAND, true);
        Self::new_with_source(block_source, nand_source).await
    }

    async fn new_with_source(
        mut block_source: PathSource,
        mut nand_source: PathSource,
    ) -> Result<(Self, impl futures::Stream<Item = Box<dyn Device>>), Error> {
        // NB. The mpsc channel for the pause event must have a buffer size of 0. Otherwise, `send`
        // on the Sink doesn't wait for the sent event to be processed, and the guarantees about
        // pause and resume not returning until the block watcher is in the right state won't hold.
        let (mut pause_event_tx, pause_event_rx) = mpsc::channel(0);
        let (device_tx, device_rx) = mpsc::unbounded();

        let task = fasync::Task::spawn(Self::watcher_loop(pause_event_rx, device_tx));
        let block_and_nand_device_stream =
            stream::select(block_source.stream(false).await?, nand_source.stream(false).await?);
        pause_event_tx.send(PauseEvent::Resume(Box::pin(block_and_nand_device_stream))).await?;

        Ok((
            Watcher {
                paused: Arc::new(Mutex::new(false)),
                pause_event_tx,
                block_source,
                nand_source,
                _watcher_task: Arc::new(task),
            },
            device_rx,
        ))
    }

    /// The core watcher loop, which gets spawned as a task and provides devices to a device stream
    /// as they appear. The first event on the pause_event_rx channel should be a Resume event with
    /// the initial device stream.
    async fn watcher_loop(
        mut pause_event_rx: mpsc::Receiver<PauseEvent>,
        mut device_tx: mpsc::UnboundedSender<Box<dyn Device>>,
    ) {
        while let Some(event) = pause_event_rx.next().await {
            // The event should be a Resume, which contains the new device stream. This will panic
            // if the event is not Resume.
            let mut device_stream = event.stream().expect("unexpected event").fuse();
            loop {
                futures::select_biased! {
                    // select_biased prefers the first branch if both futures are available. This
                    // isn't load-bearing - the client of pause should be waiting for pause to
                    // return before assuming the watcher is paused, and pause won't return until
                    // this branch is processed.
                    pause_event = pause_event_rx.next() => {
                        assert_matches!(pause_event, Some(PauseEvent::Pause));
                        break;
                    },
                    device = device_stream.next() => {
                        assert!(device.is_some(), "device stream returned none");
                        device_tx.send(device.unwrap()).await.expect("failed to send device");
                    }
                };
            }
        }
    }

    /// Pause the watcher. This function doesn't return until the watcher task is no longer
    /// processing new block devices.
    ///
    /// This returns an error if it's called while the watcher is already paused.
    pub async fn pause(&mut self) -> Result<(), Error> {
        let mut paused = self.paused.lock().await;
        if *paused {
            anyhow::bail!("already paused");
        }
        *paused = true;
        // We return an error if we were already paused, so if we get to this point, we need to let
        // the watcher know to pause. `send` will wait until the event is removed from the channel
        // by the watcher loop, as long as the channel buffer is 0.
        self.pause_event_tx.send(PauseEvent::Pause).await?;
        tracing::info!("block watcher paused");
        Ok(())
    }

    /// Returns a boolean that indicates whether or not the Watcher is paused
    pub async fn is_paused(&self) -> bool {
        *self.paused.lock().await
    }

    /// Resume the watcher. It doesn't return until the watcher task has set up the new directory
    /// watchers and will process new entries again.
    ///
    /// If the watcher hasn't been paused, this function returns an error.
    pub async fn resume(&mut self) -> Result<(), Error> {
        let mut paused = self.paused.lock().await;
        if !*paused {
            anyhow::bail!("not paused");
        }
        *paused = false;
        // We return an error if we weren't paused, so if we get to this point, we need to let the
        // watcher know to resume. `send` will wait until the event is removed from the channel by
        // the watcher loop, as long as the channel buffer is 0.

        let block_and_nand_device_stream = stream::select(
            self.block_source.stream(true).await?,
            self.nand_source.stream(true).await?,
        );
        self.pause_event_tx
            .send(PauseEvent::Resume(Box::pin(block_and_nand_device_stream)))
            .await?;
        tracing::info!("block watcher resumed");
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{PathSource, Watcher},
        fidl_fuchsia_device::{ControllerRequest, ControllerRequestStream},
        fidl_fuchsia_io as fio,
        futures::StreamExt,
        std::sync::Arc,
        vfs::{
            directory::{entry::DirectoryEntry, helper::DirectlyMutable},
            execution_scope::ExecutionScope,
            path::Path,
            service,
        },
    };

    pub fn fshost_controller(path: String) -> Arc<service::Service> {
        service::host(move |mut stream: ControllerRequestStream| {
            let cloned_path = path.clone();
            async move {
                while let Some(request) = stream.next().await {
                    match request {
                        Ok(ControllerRequest::GetTopologicalPath { responder, .. }) => {
                            responder.send(&mut Ok(cloned_path.clone())).unwrap_or_else(|e| {
                                tracing::error!(
                                    "failed to send GetTopologicalPath response. error: {:?}",
                                    e
                                );
                            });
                        }
                        Ok(controller_request) => {
                            panic!("unexpected request: {:?}", controller_request);
                        }
                        Err(error) => {
                            panic!("controller server failed: {}", error);
                        }
                    }
                }
            }
        })
    }

    #[fuchsia::test]
    async fn watcher_populates_device_stream() {
        // Start with a couple of devices
        let block = vfs::mut_pseudo_directory! {
            "000" => fshost_controller("block-000".to_string()),
            "001" => fshost_controller("block-001".to_string()),
        };

        let nand = vfs::mut_pseudo_directory! {
            "000" => fshost_controller("nand-000".to_string()),
            "001" => fshost_controller("nand-001".to_string()),
        };

        let class_block_and_nand = vfs::pseudo_directory! {
            "class" => vfs::pseudo_directory! {
                "block" => block.clone(),
                "nand" => nand.clone(),
            },
        };

        let (client, server) =
            fidl::endpoints::create_endpoints().expect("failed to make channel pair");
        let scope = ExecutionScope::new();
        class_block_and_nand.open(
            scope.clone(),
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY,
            Path::dot(),
            fidl::endpoints::ServerEnd::new(server.into_channel()),
        );

        {
            let ns = fdio::Namespace::installed().expect("failed to get installed namespace");
            ns.bind("/test-dev", client).expect("failed to bind dev in namespace");
        }

        let (mut watcher, mut device_stream) = Watcher::new_with_source(
            PathSource { path: "/test-dev/class/block", is_nand: false },
            PathSource { path: "/test-dev/class/nand", is_nand: true },
        )
        .await
        .expect("failed to make watcher");

        let mut devices =
            std::collections::HashSet::from(["block-000", "block-001", "nand-000", "nand-001"]);

        // There are four devices that were added before we started watching.
        assert!(devices.remove(device_stream.next().await.unwrap().topological_path()));
        assert!(devices.remove(device_stream.next().await.unwrap().topological_path()));
        assert!(devices.remove(device_stream.next().await.unwrap().topological_path()));
        assert!(devices.remove(device_stream.next().await.unwrap().topological_path()));
        assert!(devices.is_empty());

        // Removing an entry for a device already taken off the stream doesn't do anything.
        assert!(block
            .remove_entry("001", false)
            .expect("failed to remove dir entry 001")
            .is_some());

        // Adding an entry generates a new block device.
        block
            .add_entry("002", fshost_controller("block-002".to_string()))
            .expect("failed to add dir entry 002");

        assert_eq!(device_stream.next().await.unwrap().topological_path(), "block-002");

        // Pausing stops events from being generated.
        watcher.pause().await.expect("failed to pause");

        nand.add_entry("002", fshost_controller("nand-002".to_string()))
            .expect("failed to add dir entry 002");

        // When we resume, events start flowing again. We don't see any devices which were added
        // while we were paused (or before).
        watcher.resume().await.expect("failed to resume");

        block
            .add_entry("003", fshost_controller("block-003".to_string()))
            .expect("failed to add dir entry 003");

        assert_eq!(device_stream.next().await.unwrap().topological_path(), "block-003");
    }
}
