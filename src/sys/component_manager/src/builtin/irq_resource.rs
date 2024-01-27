// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::builtin::capability::ResourceCapability,
    ::routing::capability_source::InternalCapability,
    anyhow::Error,
    async_trait::async_trait,
    cm_rust::CapabilityName,
    fidl::endpoints::ProtocolMarker,
    fidl_fuchsia_kernel as fkernel,
    fuchsia_zircon::{self as zx, HandleBased, Resource, ResourceInfo},
    futures::prelude::*,
    lazy_static::lazy_static,
    std::sync::Arc,
};

lazy_static! {
    static ref IRQ_RESOURCE_CAPABILITY_NAME: CapabilityName = "fuchsia.kernel.IrqResource".into();
}

/// An implementation of fuchsia.kernel.IrqResource protocol.
pub struct IrqResource {
    resource: Resource,
}

impl IrqResource {
    /// `resource` must be the IRQ resource.
    pub fn new(resource: Resource) -> Arc<Self> {
        Arc::new(Self { resource })
    }
}

#[async_trait]
impl ResourceCapability for IrqResource {
    const KIND: zx::sys::zx_rsrc_kind_t = zx::sys::ZX_RSRC_KIND_IRQ;
    const NAME: &'static str = "IrqResource";
    type Marker = fkernel::IrqResourceMarker;

    fn get_resource_info(self: &Arc<Self>) -> Result<ResourceInfo, Error> {
        Ok(self.resource.info()?)
    }

    async fn server_loop(
        self: Arc<Self>,
        mut stream: <Self::Marker as ProtocolMarker>::RequestStream,
    ) -> Result<(), Error> {
        while let Some(fkernel::IrqResourceRequest::Get { responder }) = stream.try_next().await? {
            responder.send(self.resource.duplicate_handle(zx::Rights::SAME_RIGHTS)?)?;
        }
        Ok(())
    }

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool {
        capability.matches_protocol(&IRQ_RESOURCE_CAPABILITY_NAME)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            builtin::capability::BuiltinCapability,
            capability::CapabilitySource,
            model::hooks::{Event, EventPayload, Hooks},
        },
        cm_task_scope::TaskScope,
        fidl::endpoints::ClientEnd,
        fidl_fuchsia_io as fio, fidl_fuchsia_kernel as fkernel, fuchsia_async as fasync,
        fuchsia_component::client::connect_to_protocol,
        fuchsia_zircon::sys,
        fuchsia_zircon::AsHandleRef,
        futures::lock::Mutex,
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
        std::path::PathBuf,
        std::sync::Weak,
    };

    fn irq_resource_available() -> bool {
        let bin = std::env::args().next();
        match bin.as_ref().map(String::as_ref) {
            Some("/pkg/bin/component_manager_test") => false,
            Some("/pkg/bin/component_manager_boot_env_test") => true,
            _ => panic!("Unexpected test binary name {:?}", bin),
        }
    }

    async fn get_irq_resource() -> Result<Resource, Error> {
        let irq_resource_provider = connect_to_protocol::<fkernel::IrqResourceMarker>()?;
        let irq_resource_handle = irq_resource_provider.get().await?;
        Ok(Resource::from(irq_resource_handle))
    }

    async fn serve_irq_resource() -> Result<fkernel::IrqResourceProxy, Error> {
        let irq_resource = get_irq_resource().await?;

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fkernel::IrqResourceMarker>()?;
        fasync::Task::local(
            IrqResource::new(irq_resource)
                .serve(stream)
                .unwrap_or_else(|e| panic!("Error while serving IRQ resource service: {}", e)),
        )
        .detach();
        Ok(proxy)
    }

    #[fuchsia::test]
    async fn fail_with_no_irq_resource() -> Result<(), Error> {
        if irq_resource_available() {
            return Ok(());
        }
        let (_, stream) = fidl::endpoints::create_proxy_and_stream::<fkernel::IrqResourceMarker>()?;
        assert!(!IrqResource::new(Resource::from(zx::Handle::invalid()))
            .serve(stream)
            .await
            .is_ok());
        Ok(())
    }

    #[fuchsia::test]
    async fn kind_type_is_irq() -> Result<(), Error> {
        if !irq_resource_available() {
            return Ok(());
        }

        let irq_resource_provider = serve_irq_resource().await?;
        let irq_resource: Resource = irq_resource_provider.get().await?;
        let resource_info = irq_resource.info()?;
        assert_eq!(resource_info.kind, zx::sys::ZX_RSRC_KIND_IRQ);
        assert_eq!(resource_info.base, 0);
        assert_eq!(resource_info.size, 0);
        Ok(())
    }

    #[fuchsia::test]
    async fn can_connect_to_irq_service() -> Result<(), Error> {
        if !irq_resource_available() {
            return Ok(());
        }

        let irq_resource = IrqResource::new(get_irq_resource().await?);
        let hooks = Hooks::new();
        hooks.install(irq_resource.hooks()).await;

        let provider = Arc::new(Mutex::new(None));
        let source = CapabilitySource::Builtin {
            capability: InternalCapability::Protocol(IRQ_RESOURCE_CAPABILITY_NAME.clone()),
            top_instance: Weak::new(),
        };

        let event = Event::new_for_test(
            AbsoluteMoniker::root(),
            "fuchsia-pkg://root",
            EventPayload::CapabilityRouted { source, capability_provider: provider.clone() },
        );
        hooks.dispatch(&event).await;

        let (client, mut server) = zx::Channel::create()?;
        let task_scope = TaskScope::new();
        if let Some(provider) = provider.lock().await.take() {
            provider
                .open(task_scope.clone(), fio::OpenFlags::empty(), 0, PathBuf::new(), &mut server)
                .await?;
        };

        let irq_client = ClientEnd::<fkernel::IrqResourceMarker>::new(client)
            .into_proxy()
            .expect("failed to create launcher proxy");
        let irq_resource = irq_client.get().await?;
        assert_ne!(irq_resource.raw_handle(), sys::ZX_HANDLE_INVALID);
        Ok(())
    }
}
