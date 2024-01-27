// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl_test_processbuilder::{EnvVar, UtilRequest, UtilRequestStream},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_runtime::{self as fruntime, HandleInfo, HandleType},
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::prelude::*,
    std::env,
    std::fs,
};

async fn run_util_server(mut stream: UtilRequestStream) -> Result<(), Error> {
    // If we've been given a lifecycle channel, figure out its koid
    let lifecycle_koid: u64 =
        match fruntime::take_startup_handle(HandleInfo::new(HandleType::Lifecycle, 0)) {
            Some(handle) => handle
                .as_handle_ref()
                .get_koid()
                .expect("failed to get basic lifecycle handle info")
                .raw_koid(),
            None => zx::sys::ZX_KOID_INVALID,
        };

    while let Some(req) = stream.try_next().await.context("error running echo server")? {
        match req {
            UtilRequest::GetArguments { responder } => {
                let args: Vec<String> = env::args().collect();
                responder.send(&args).context("error sending response")?
            }
            UtilRequest::GetArgumentCount { responder } => {
                responder.send(env::args().len() as u64).context("error sending response")?
            }
            UtilRequest::GetEnvironment { responder } => {
                let mut vars: Vec<EnvVar> =
                    env::vars().map(|v| EnvVar { key: v.0, value: v.1 }).collect();
                responder.send(&mut vars.iter_mut()).context("error sending response")?;
            }
            UtilRequest::GetEnvironmentCount { responder } => {
                responder.send(env::vars().count() as u64).context("error sending response")?;
            }
            UtilRequest::DumpNamespace { responder } => {
                let mut contents = Vec::new();
                let mut a =
                    |entry: &fs::DirEntry| contents.push(format!("{}", entry.path().display()));
                visit(std::path::Path::new("/"), &mut a)?;
                responder.send(&contents.join(", ")).context("error sending response")?;
            }
            UtilRequest::ReadFile { path, responder } => {
                let contents = fs::read_to_string(path)
                    .unwrap_or_else(|e| format!("read_to_string failed: {}", e));
                responder.send(&contents).context("error sending response")?;
            }
            UtilRequest::GetLifecycleKoid { responder } => {
                responder.send(lifecycle_koid as u64)?;
            }
        }
    }
    Ok(())
}

fn visit(dir: &std::path::Path, cb: &mut dyn FnMut(&fs::DirEntry)) -> Result<(), Error> {
    if dir.is_dir() {
        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            cb(&entry);
            if path.is_dir() {
                visit(&path, cb)?;
            }
        }
    }
    Ok(())
}

enum IncomingServices {
    Util(UtilRequestStream),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Note that the util doesn't add services under a subdirectory as would be typical for a
    // component, as that isn't necessary for these tests.
    let mut fs = ServiceFs::new_local();
    fs.add_fidl_service(IncomingServices::Util);

    fs.take_and_serve_directory_handle()?;

    const MAX_CONCURRENT: usize = 10;
    fs.for_each_concurrent(MAX_CONCURRENT, |IncomingServices::Util(stream)| {
        run_util_server(stream).unwrap_or_else(|e| println!("{:?}", e))
    })
    .await;
    Ok(())
}
