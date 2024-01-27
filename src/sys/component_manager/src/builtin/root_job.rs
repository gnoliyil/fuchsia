// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::builtin::capability::BuiltinCapability,
    ::routing::capability_source::InternalCapability, anyhow::Error, async_trait::async_trait,
    cm_rust::CapabilityName, fidl_fuchsia_kernel as fkernel, fuchsia_runtime::job_default,
    fuchsia_zircon as zx, futures::prelude::*, lazy_static::lazy_static, std::sync::Arc,
};

lazy_static! {
    pub static ref ROOT_JOB_CAPABILITY_NAME: CapabilityName = "fuchsia.kernel.RootJob".into();
    pub static ref ROOT_JOB_FOR_INSPECT_CAPABILITY_NAME: CapabilityName =
        "fuchsia.kernel.RootJobForInspect".into();
}

/// An implementation of the `fuchsia.kernel.RootJob` protocol.
pub struct RootJob {
    capability_name: &'static CapabilityName,
    rights: zx::Rights,
}

impl RootJob {
    pub fn new(capability_name: &'static CapabilityName, rights: zx::Rights) -> Arc<Self> {
        Arc::new(Self { capability_name, rights })
    }
}

#[async_trait]
impl BuiltinCapability for RootJob {
    const NAME: &'static str = "RootJob";
    type Marker = fkernel::RootJobMarker;

    async fn serve(
        self: Arc<Self>,
        mut stream: fkernel::RootJobRequestStream,
    ) -> Result<(), Error> {
        let job = job_default();
        while let Some(fkernel::RootJobRequest::Get { responder }) = stream.try_next().await? {
            responder.send(job.duplicate(self.rights)?)?;
        }
        Ok(())
    }

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool {
        capability.matches_protocol(self.capability_name)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            capability::CapabilitySource,
            model::hooks::{Event, EventPayload, Hooks},
        },
        cm_task_scope::TaskScope,
        fidl::endpoints::ClientEnd,
        fidl_fuchsia_io as fio, fuchsia_async as fasync,
        fuchsia_zircon::sys,
        fuchsia_zircon::AsHandleRef,
        futures::lock::Mutex,
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
        std::path::PathBuf,
        std::sync::Weak,
    };

    #[fuchsia::test]
    async fn has_correct_rights() -> Result<(), Error> {
        let root_job = RootJob::new(&ROOT_JOB_CAPABILITY_NAME, zx::Rights::TRANSFER);
        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<fkernel::RootJobMarker>()?;
        fasync::Task::local(
            root_job.serve(stream).unwrap_or_else(|err| panic!("Error serving root job: {}", err)),
        )
        .detach();

        let root_job = proxy.get().await?;
        let info = zx::Handle::from(root_job).basic_info()?;
        assert_eq!(info.rights, zx::Rights::TRANSFER);
        Ok(())
    }

    #[fuchsia::test]
    async fn can_connect() -> Result<(), Error> {
        let root_job = RootJob::new(&ROOT_JOB_CAPABILITY_NAME, zx::Rights::SAME_RIGHTS);
        let hooks = Hooks::new();
        hooks.install(root_job.hooks()).await;

        let provider = Arc::new(Mutex::new(None));
        let source = CapabilitySource::Builtin {
            capability: InternalCapability::Protocol(ROOT_JOB_CAPABILITY_NAME.clone()),
            top_instance: Weak::new(),
        };

        let event = Event::new_for_test(
            AbsoluteMoniker::root(),
            "fuchsia-pkg://root",
            EventPayload::CapabilityRouted { source, capability_provider: provider.clone() },
        );
        hooks.dispatch(&event).await;

        let (client, mut server) = zx::Channel::create();
        let task_scope = TaskScope::new();
        if let Some(provider) = provider.lock().await.take() {
            provider
                .open(task_scope.clone(), fio::OpenFlags::empty(), 0, PathBuf::new(), &mut server)
                .await?;
        };

        let client = ClientEnd::<fkernel::RootJobMarker>::new(client)
            .into_proxy()
            .expect("Failed to create proxy");
        let handle = client.get().await?;
        assert_ne!(handle.raw_handle(), sys::ZX_HANDLE_INVALID);
        Ok(())
    }
}
