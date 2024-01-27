// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource},
        model::{
            error::{CapabilityProviderError, ModelError},
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
        },
    },
    anyhow::{format_err, Error},
    async_trait::async_trait,
    cm_task_scope::TaskScope,
    cm_util::channel,
    fidl::endpoints::{ProtocolMarker, ServerEnd},
    fidl_fuchsia_io as fio,
    fuchsia_zircon::{self as zx, ResourceInfo},
    routing::capability_source::InternalCapability,
    std::{
        path::PathBuf,
        sync::{Arc, Weak},
    },
    tracing::warn,
};

/// A builtin capability, whether it be a `framework` capability
/// or a capability that originates above the root realm (from component_manager).
///
/// Implementing this trait takes care of certain boilerplate, like registering
/// event hooks and the creation of a CapabilityProvider.
#[async_trait]
pub trait BuiltinCapability {
    /// Name of the capability. Used for hook registration and logging.
    const NAME: &'static str;

    /// Service marker for the capability.
    type Marker: ProtocolMarker;

    /// Serves an instance of the capability given an appropriate RequestStream.
    /// Returns when the channel backing the RequestStream is closed or an
    /// unrecoverable error occurs.
    async fn serve(
        self: Arc<Self>,
        mut stream: <Self::Marker as ProtocolMarker>::RequestStream,
    ) -> Result<(), Error>;

    /// Returns the registration hooks for the capability.
    fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration>
    where
        Self: 'static + Hook + Sized,
    {
        vec![HooksRegistration::new(
            Self::NAME,
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    /// Returns true if the builtin capability matches the requested `capability`
    /// and should be served.
    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool;
}

#[async_trait]
pub trait ResourceCapability {
    /// The kind of resource.
    const KIND: zx::sys::zx_rsrc_kind_t;

    /// Name of the capability. Used for hook registration and logging.
    const NAME: &'static str;

    /// Service marker for the capability.
    type Marker: ProtocolMarker;

    fn get_resource_info(self: &Arc<Self>) -> Result<ResourceInfo, Error>;

    async fn server_loop(
        self: Arc<Self>,
        mut stream: <Self::Marker as ProtocolMarker>::RequestStream,
    ) -> Result<(), Error>;

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool;
}

#[async_trait]
impl<R: ResourceCapability + Send + Sync> BuiltinCapability for R {
    const NAME: &'static str = R::NAME;
    type Marker = R::Marker;

    async fn serve(
        self: Arc<Self>,
        stream: <R::Marker as ProtocolMarker>::RequestStream,
    ) -> Result<(), Error> {
        let resource_info = self.get_resource_info()?;
        if resource_info.kind != R::KIND || resource_info.base != 0 || resource_info.size != 0 {
            return Err(format_err!("{} not available.", R::NAME));
        }
        self.server_loop(stream).await
    }

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool {
        self.matches_routed_capability(capability)
    }
}

#[async_trait]
impl<B: 'static + BuiltinCapability + Send + Sync> Hook for B {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        if let EventPayload::CapabilityRouted {
            source: CapabilitySource::Builtin { capability, .. },
            capability_provider,
        } = &event.payload
        {
            if self.matches_routed_capability(&capability) {
                let mut provider = capability_provider.lock().await;
                *provider =
                    Some(Box::new(BuiltinCapabilityProvider::<B>::new(Arc::downgrade(&self))));
            }
        }
        Ok(())
    }
}

struct BuiltinCapabilityProvider<B: BuiltinCapability> {
    capability: Weak<B>,
}

impl<B: BuiltinCapability> BuiltinCapabilityProvider<B> {
    pub fn new(capability: Weak<B>) -> Self {
        Self { capability }
    }
}

#[async_trait]
impl<B: 'static + BuiltinCapability + Sync + Send> CapabilityProvider
    for BuiltinCapabilityProvider<B>
{
    async fn open(
        self: Box<Self>,
        task_scope: TaskScope,
        _flags: fio::OpenFlags,
        _relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), CapabilityProviderError> {
        let server_end = channel::take_channel(server_end);
        let server_end = ServerEnd::<B::Marker>::new(server_end);
        let stream =
            server_end.into_stream().map_err(|_| CapabilityProviderError::StreamCreationError)?;
        task_scope
            .add_task(async move {
                if let Some(capability) = self.capability.upgrade() {
                    if let Err(error) = capability.serve(stream).await {
                        warn!(protocol=%B::NAME, %error, "open failed");
                    }
                } else {
                    warn!(protocol=%B::NAME, "handle dropped");
                }
            })
            .await;
        Ok(())
    }
}
