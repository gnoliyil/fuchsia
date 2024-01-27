// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    async_trait::async_trait,
    fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_zircon::Status,
    futures::{SinkExt as _, StreamExt as _},
    once_cell::sync::OnceCell,
    storage_stress_test_utils::{data::FileFactory, io::Directory},
    stress_test::actor::{Actor, ActorError},
    tracing::info,
};

// TODO(fxbug.dev/67497): This actor is very basic. At the moment, this is fine, since this is a
// v0 implementation of minfs stress tests. Eventually, we should write stress tests that exercise
// minfs as a true POSIX filesystem.
pub struct FileActor {
    pub factory: FileFactory,
    pub home_dir: Directory,
    progress_channel_and_task:
        OnceCell<(futures::channel::mpsc::UnboundedSender<()>, fasync::Task<()>)>,
}

impl FileActor {
    pub fn new(factory: FileFactory, home_dir: Directory) -> Self {
        Self { factory, home_dir, progress_channel_and_task: OnceCell::new() }
    }

    /// Arms a timer which will expire after `duration` and fail the test.
    /// Each time a file is successfully created, the timer is reset.  This ensures forward
    /// progress.
    pub async fn set_progress_timer(&self, duration: std::time::Duration) {
        let (tx, mut rx) = futures::channel::mpsc::unbounded();
        let task = fasync::Task::spawn(async move {
            use futures::future::FutureExt;
            loop {
                let duration_clone = duration.clone();
                futures::select! {
                    () = fuchsia_async::Timer::new(duration_clone).fuse() =>
                        {
                            panic!("No progress made after {:?}", duration_clone);
                        }
                    _ = rx.next().fuse() => continue,
                }
            }
        });
        self.progress_channel_and_task.set((tx, task)).expect("Failed to set timer channel");
    }

    async fn create_file(&mut self) -> Result<(), Status> {
        let filename = self.factory.generate_filename();
        let data_bytes = self.factory.generate_bytes();

        // Write the file to disk
        let file = self
            .home_dir
            .open_file(
                &filename,
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::CREATE_IF_ABSENT
                    | fio::OpenFlags::RIGHT_WRITABLE,
            )
            .await?;
        file.write(&data_bytes).await?;
        file.close().await
    }
}

#[async_trait]
impl Actor for FileActor {
    async fn perform(&mut self) -> Result<(), ActorError> {
        match self.create_file().await {
            Ok(()) => {
                self.progress_channel_and_task.get_mut().unwrap().0.send(()).await.unwrap();
                Ok(())
            }
            Err(Status::NO_SPACE) => Ok(()),
            // Any other error is assumed to come from an intentional crash.
            // The environment verifies that an intentional crash occurred
            // and will panic if that is not the case.
            Err(s) => {
                info!("File actor got status: {}", s);
                Err(ActorError::ResetEnvironment)
            }
        }
    }
}
