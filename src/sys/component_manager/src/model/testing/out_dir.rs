// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::mocks::HostFn,
    cm_rust::CapabilityPath,
    fidl::endpoints::ServerEnd,
    fidl_fidl_examples_routing_echo::{EchoRequest, EchoRequestStream},
    fidl_fuchsia_io as fio,
    futures::TryStreamExt,
    std::{collections::HashMap, convert::TryFrom, sync::Arc},
    vfs::{
        self, directory::entry::DirectoryEntry, directory::immutable::simple as pfs,
        execution_scope::ExecutionScope, file::vmo::read_only_const, remote::remote_dir,
        service::host, tree_builder::TreeBuilder,
    },
};

/// Used to construct and then host an outgoing directory.
#[derive(Clone)]
pub struct OutDir {
    paths: HashMap<CapabilityPath, Arc<dyn DirectoryEntry>>,
}

impl OutDir {
    pub fn new() -> OutDir {
        OutDir { paths: HashMap::new() }
    }

    /// Add a `DirectoryEntry` served at the given path.
    pub fn add_entry(&mut self, path: CapabilityPath, entry: Arc<dyn DirectoryEntry>) {
        self.paths.insert(path, entry);
    }

    /// Adds a file providing the echo service at the given path.
    pub fn add_echo_service(&mut self, path: CapabilityPath) {
        self.add_entry(path, host(Self::echo_server_fn));
    }

    /// Adds a static file at the given path.
    pub fn add_static_file(&mut self, path: CapabilityPath, contents: &str) {
        self.add_entry(path, read_only_const(contents.as_bytes()));
    }

    /// Adds the given directory proxy at location "/data".
    pub fn add_directory_proxy(&mut self, test_dir_proxy: &fio::DirectoryProxy) {
        self.add_entry(
            CapabilityPath::try_from("/data").unwrap(),
            remote_dir(
                fuchsia_fs::directory::clone_no_describe(&test_dir_proxy, None)
                    .expect("could not clone directory"),
            ),
        );
    }

    /// Build the output directory.
    fn build_out_dir(&self) -> Result<Arc<pfs::Simple>, anyhow::Error> {
        let mut tree = TreeBuilder::empty_dir();
        // Add any external files.
        for (path, entry) in self.paths.iter() {
            let path = path.split();
            let path = path.iter().map(|x| x as &str).collect::<Vec<_>>();
            tree.add_entry(&path, entry.clone())?;
        }

        Ok(tree.build())
    }

    /// Returns a function that will host this outgoing directory on the given ServerEnd.
    pub fn host_fn(&self) -> HostFn {
        // Build the output directory.
        let dir = self.build_out_dir().expect("could not build out directory");

        // Construct a function. Each time it is invoked, we connect a new Zircon channel
        // `server_end` to the directory.
        Box::new(move |server_end: ServerEnd<fio::DirectoryMarker>| {
            dir.clone().open(
                ExecutionScope::new(),
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
                    | fio::OpenFlags::DIRECTORY,
                vfs::path::Path::dot(),
                ServerEnd::new(server_end.into_channel()),
            );
        })
    }

    /// Hosts a new service on `server_end` that implements `fidl.examples.routing.echo.Echo`.
    async fn echo_server_fn(mut stream: EchoRequestStream) {
        while let Some(EchoRequest::EchoString { value, responder }) =
            stream.try_next().await.unwrap()
        {
            responder.send(value.as_ref().map(|s| &**s)).unwrap();
        }
    }
}
