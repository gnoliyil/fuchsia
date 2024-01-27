// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    async_trait::async_trait,
    either::Either,
    fs_management::filesystem::{ServingMultiVolumeFilesystem, ServingSingleVolumeFilesystem},
    storage_stress_test_utils::fvm::FvmInstance,
    stress_test::actor::{Actor, ActorError},
};

/// An actor that kills the fs instance and destroys the ramdisk
pub struct InstanceActor {
    pub instance:
        Option<(FvmInstance, Either<ServingSingleVolumeFilesystem, ServingMultiVolumeFilesystem>)>,
}

impl InstanceActor {
    pub fn new(
        fvm: FvmInstance,
        fs: Either<ServingSingleVolumeFilesystem, ServingMultiVolumeFilesystem>,
    ) -> Self {
        Self { instance: Some((fvm, fs)) }
    }
}

#[async_trait]
impl Actor for InstanceActor {
    async fn perform(&mut self) -> Result<(), ActorError> {
        if let Some((fvm_instance, fs)) = self.instance.take() {
            // We want to kill the ram-disk before the filesystem so that we test the filesystem in
            // a simulated power-fail.
            // Wait for the device to go away.
            fvm_instance.shutdown().await;

            // Ignore errors: we've yanked the underlying device, and filesystems have different
            // ways of handling that, and it doesn't really matter how they handle it i.e. crashing
            // is fine.
            match fs {
                Either::Left(fs) => {
                    let _ = fs.kill().await;
                }
                // TODO(fxbug.dev/105888): Make termination more abrupt.
                Either::Right(fs) => {
                    let _ = fs.shutdown().await;
                }
            };
        } else {
            panic!("Instance was already killed!")
        }
        Err(ActorError::ResetEnvironment)
    }
}
