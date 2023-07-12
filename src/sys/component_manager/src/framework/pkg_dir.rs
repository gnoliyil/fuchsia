// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module implements the ability to use, offer, or expose the `pkg` directory with a source
//! of `framework`.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource},
        model::{
            component::Package,
            error::{CapabilityProviderError, ModelError, PkgDirError},
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
        },
    },
    ::routing::capability_source::InternalCapability,
    async_trait::async_trait,
    cm_task_scope::TaskScope,
    cm_util::channel,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    futures::lock::Mutex,
    moniker::Moniker,
    std::{
        path::PathBuf,
        sync::{Arc, Weak},
    },
};

struct PkgDirectoryProvider {
    _abs_moniker: Moniker,
    package: Option<Package>,
}

impl PkgDirectoryProvider {
    pub fn new(abs_moniker: Moniker, package: Option<Package>) -> Self {
        PkgDirectoryProvider { _abs_moniker: abs_moniker, package }
    }
}

#[async_trait]
impl CapabilityProvider for PkgDirectoryProvider {
    async fn open(
        self: Box<Self>,
        _task_scope: TaskScope,
        flags: fio::OpenFlags,
        relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), CapabilityProviderError> {
        let relative_path =
            relative_path.to_str().ok_or(CapabilityProviderError::BadPath)?.to_string();
        let server_end = ServerEnd::new(channel::take_channel(server_end));
        if let Some(package) = &self.package {
            if relative_path.is_empty() {
                package
                    .package_dir
                    .clone(flags, server_end)
                    .map_err(|err| PkgDirError::OpenFailed { err })?;
            } else {
                package
                    .package_dir
                    .open(flags, fio::ModeType::empty(), &relative_path, server_end)
                    .map_err(|err| PkgDirError::OpenFailed { err })?;
            }
        } else {
            return Err(CapabilityProviderError::PkgDirError { err: PkgDirError::NoPkgDir });
        }
        Ok(())
    }
}

pub struct PkgDirectory;

impl PkgDirectory {
    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "PkgDirectory",
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    /// Given a `CapabilitySource`, determine if it is a framework-provided
    /// pkg capability. If so, update the given `capability_provider` to
    /// provide a pkg directory.
    async fn on_capability_routed_async(
        self: Arc<Self>,
        source: CapabilitySource,
        capability_provider: Arc<Mutex<Option<Box<dyn CapabilityProvider>>>>,
    ) -> Result<(), ModelError> {
        // If this is a scoped framework directory capability, then check the source path
        if let CapabilitySource::Framework {
            capability: InternalCapability::Directory(source_name),
            component,
        } = source
        {
            if source_name.as_str() != "pkg" {
                return Ok(());
            }

            // Set the capability provider, if not already set.
            let mut capability_provider = capability_provider.lock().await;
            if capability_provider.is_none() {
                let component = component.upgrade()?;
                let resolved_state = component.lock_resolved_state().await?;
                let abs_moniker = component.abs_moniker.clone();
                let package = resolved_state.package().cloned();
                *capability_provider =
                    Some(Box::new(PkgDirectoryProvider::new(abs_moniker, package)))
            }
        }
        Ok(())
    }
}

#[async_trait]
impl Hook for PkgDirectory {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        match &event.payload {
            EventPayload::CapabilityRouted { source, capability_provider } => {
                self.on_capability_routed_async(source.clone(), capability_provider.clone())
                    .await?;
            }
            _ => {}
        };
        Ok(())
    }
}
