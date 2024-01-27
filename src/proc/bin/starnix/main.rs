// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]
#![allow(clippy::too_many_arguments)]
// TODO(fxbug.dev/122028): Remove this allow once the lint is fixed.
#![allow(unknown_lints, clippy::extra_unused_type_parameters)]

#[macro_use]
extern crate macro_rules_attribute;

use crate::execution::create_container;
use anyhow::Error;
use fidl::endpoints::ControlHandle;
use fidl_fuchsia_process_lifecycle as flifecycle;
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use fuchsia_runtime as fruntime;
use futures::{StreamExt, TryStreamExt};

#[macro_use]
mod trace;

mod auth;
mod bpf;
mod collections;
mod device;
mod dynamic_thread_pool;
mod execution;
mod fs;
mod loader;
mod lock;
mod logging;
mod mm;
mod mutable_state;
mod selinux;
mod signals;
mod syscalls;
mod task;
mod types;
mod vmex_resource;

#[cfg(test)]
mod testing;

#[fuchsia::main(logging_tags = ["starnix"])]
async fn main() -> Result<(), Error> {
    fuchsia_trace_provider::trace_provider_create_with_fdio();
    fuchsia_trace::instant!(
        trace_category_starnix!(),
        trace_name_start_kernel!(),
        fuchsia_trace::Scope::Thread
    );

    let container = create_container().await?;

    if let Some(lifecycle) =
        fruntime::take_startup_handle(fruntime::HandleInfo::new(fruntime::HandleType::Lifecycle, 0))
    {
        fasync::Task::local(async move {
            if let Ok(mut stream) =
                fidl::endpoints::ServerEnd::<flifecycle::LifecycleMarker>::new(lifecycle.into())
                    .into_stream()
            {
                while let Ok(Some(request)) = stream.try_next().await {
                    match request {
                        flifecycle::LifecycleRequest::Stop { control_handle } => {
                            control_handle.shutdown();
                            std::process::exit(0);
                        }
                    }
                }
            }
        })
        .detach();
    }

    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(|stream| {
        let container = container.clone();
        fasync::Task::local(async move {
            execution::serve_component_runner(stream, container)
                .await
                .expect("failed to start runner.")
        })
        .detach();
    });

    fs.dir("svc").add_fidl_service(|stream| {
        let container = container.clone();
        fasync::Task::local(async move {
            execution::serve_container_controller(stream, container)
                .await
                .expect("failed to start container controller.")
        })
        .detach();
    });

    fs.dir("svc").add_fidl_service(|stream| {
        let container = container.clone();
        fasync::Task::local(async move {
            execution::serve_dev_binder(stream, container).await.expect("failed to start binder.")
        })
        .detach();
    });

    fs.add_remote("linux_root", execution::expose_root(&container)?);

    inspect_runtime::serve(fuchsia_inspect::component::inspector(), &mut fs)?;

    fs.take_and_serve_directory_handle()?;
    fs.collect::<()>().await;

    Ok(())
}
