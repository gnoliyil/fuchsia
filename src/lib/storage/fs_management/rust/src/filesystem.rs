// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Contains the asynchronous version of [`Filesystem`][`crate::Filesystem`].

use {
    crate::{
        error::{CommandError, KillError, QueryError, ShutdownError},
        launch_process, ComponentType, FSConfig, Mode,
    },
    anyhow::{anyhow, bail, ensure, Error},
    cstr::cstr,
    fdio::SpawnAction,
    fidl::{
        encoding::Decodable,
        endpoints::{create_endpoints, create_proxy, ClientEnd, ServerEnd},
    },
    fidl_fuchsia_component::{self as fcomponent, RealmMarker},
    fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_fs_startup::{CheckOptions, StartupMarker},
    fidl_fuchsia_fxfs::MountOptions,
    fidl_fuchsia_hardware_block as fhardware_block, fidl_fuchsia_io as fio,
    fuchsia_async::OnSignals,
    fuchsia_component::client::{
        connect_to_named_protocol_at_dir_root, connect_to_protocol,
        connect_to_protocol_at_dir_root, connect_to_protocol_at_path,
        open_childs_exposed_directory,
    },
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon::{
        self as zx, AsHandleRef as _, Channel, HandleBased as _, Process, Signals, Status, Task,
    },
    std::{
        collections::HashMap,
        sync::{
            atomic::{AtomicU64, Ordering},
            Arc,
        },
    },
    tracing::warn,
};

/// Asynchronously manages a block device for filesystem operations.
pub struct Filesystem {
    /// The filesystem struct keeps the FSConfig in a Box<dyn> instead of holding it directly for
    /// code size reasons. Using a type parameter instead would make monomorphized versions of the
    /// Filesystem impl block for each filesystem type, which duplicates several multi-kilobyte
    /// functions (get_component_exposed_dir and serve in particular) that are otherwise quite
    /// generic over config. Clients that want to be generic over filesystem type also pay the
    /// monomorphization cost, with some, like fshost, paying a lot.
    config: Box<dyn FSConfig>,
    block_device: fio::NodeProxy,
    component: Option<Arc<DynamicComponentInstance>>,
}

// Used to disambiguate children in our component collection.
static COLLECTION_COUNTER: AtomicU64 = AtomicU64::new(0);

impl Filesystem {
    pub fn config(&self) -> &dyn FSConfig {
        self.config.as_ref()
    }

    /// Creates a new `Filesystem` with the block device represented by `node_proxy`.
    pub fn from_node<FSC: FSConfig + 'static>(node_proxy: fio::NodeProxy, config: FSC) -> Self {
        Self { config: Box::new(config), block_device: node_proxy, component: None }
    }

    /// Creates a new `Filesystem` from the block device at the given path.
    pub fn from_path<FSC: FSConfig + 'static>(path: &str, config: FSC) -> Result<Self, Error> {
        let proxy = connect_to_protocol_at_path::<fio::NodeMarker>(path)?;
        Ok(Self::from_node(proxy, config))
    }

    /// Creates a new `Filesystem` with the block device represented by `channel`.
    pub fn from_channel<FSC: FSConfig + 'static>(
        channel: Channel,
        config: FSC,
    ) -> Result<Self, Error> {
        Ok(Self::from_node(ClientEnd::<fio::NodeMarker>::new(channel).into_proxy()?, config))
    }

    // Clone a Channel to the block device.
    fn get_block_handle(
        &self,
    ) -> Result<fidl::endpoints::ClientEnd<fhardware_block::BlockMarker>, fidl::Error> {
        let (client, server) = fidl::endpoints::create_endpoints()?;
        // TODO(https://fxbug.dev/112484): this relies on multiplexing.
        let () = self.block_device.clone(fio::OpenFlags::CLONE_SAME_RIGHTS, server)?;
        let client = client.into_channel();
        Ok(client.into())
    }

    async fn get_component_exposed_dir(&mut self) -> Result<fio::DirectoryProxy, Error> {
        let mode = self.config.mode();
        let component_name = mode
            .component_name()
            .expect("BUG: called get_component_exposed_dir when mode was not component");
        let component_type = mode
            .component_type()
            .expect("BUG: called get_component_exposed_dir when mode was not component");
        let realm_proxy = connect_to_protocol::<RealmMarker>()?;

        match component_type {
            ComponentType::StaticChild => open_childs_exposed_directory(component_name, None).await,
            ComponentType::DynamicChild { collection_name } => {
                if let Some(component) = &self.component {
                    return open_childs_exposed_directory(
                        component.name.clone(),
                        Some(component.collection.clone()),
                    )
                    .await;
                }

                // We need a unique name, so we pull in the process Koid here since it's possible
                // for the same binary in a component to be launched multiple times and we don't
                // want to collide with children created by other processes.
                let name = format!(
                    "{}-{}-{}",
                    component_name,
                    fuchsia_runtime::process_self().get_koid().unwrap().raw_koid(),
                    COLLECTION_COUNTER.fetch_add(1, Ordering::Relaxed)
                );

                let mut collection_ref = fdecl::CollectionRef { name: collection_name };
                let child_decl = fdecl::Child {
                    name: Some(name.clone()),
                    url: Some(format!("#meta/{}.cm", component_name)),
                    startup: Some(fdecl::StartupMode::Lazy),
                    ..fdecl::Child::EMPTY
                };
                // Launch a new component in our collection.
                realm_proxy
                    .create_child(
                        &mut collection_ref,
                        child_decl,
                        fcomponent::CreateChildArgs::EMPTY,
                    )
                    .await?
                    .map_err(|e| anyhow!("create_child failed: {:?}", e))?;

                let component =
                    Arc::new(DynamicComponentInstance { name, collection: collection_ref.name });

                let proxy = open_childs_exposed_directory(
                    component.name.clone(),
                    Some(component.collection.clone()),
                )
                .await?;

                self.component = Some(component);
                Ok(proxy)
            }
        }
    }

    /// Runs `mkfs`, which formats the filesystem onto the block device.
    ///
    /// Which flags are passed to the `mkfs` command are controlled by the config this `Filesystem`
    /// was created with.
    ///
    /// See [`FSConfig`].
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if the filesystem process failed to launch or returned a non-zero exit code.
    pub async fn format(&mut self) -> Result<(), Error> {
        match self.config.mode() {
            Mode::Component { mut format_options, .. } => {
                let exposed_dir = self.get_component_exposed_dir().await?;
                let proxy = connect_to_protocol_at_dir_root::<StartupMarker>(&exposed_dir)?;
                proxy
                    .format(self.get_block_handle()?.into(), &mut format_options)
                    .await?
                    .map_err(Status::from_raw)?;
            }
            Mode::Legacy(mut config) => {
                // SpawnAction is not Send, so make sure it is dropped before any `await`s.
                let process = {
                    let mut args = vec![config.binary_path, cstr!("mkfs")];
                    args.append(&mut config.generic_args);
                    args.append(&mut config.format_args);
                    let actions = vec![
                        // device handle is passed in as a PA_USER0 handle at argument 1
                        SpawnAction::add_handle(
                            HandleInfo::new(HandleType::User0, 1),
                            self.get_block_handle()?.into_handle(),
                        ),
                    ];
                    launch_process(&args, actions)?
                };
                wait_for_successful_exit(process).await?;
            }
        }
        Ok(())
    }

    /// Runs `fsck`, which checks and optionally repairs the filesystem on the block device.
    ///
    /// Which flags are passed to the `fsck` command are controlled by the config this `Filesystem`
    /// was created with.
    ///
    /// See [`FSConfig`].
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if the filesystem process failed to launch or returned a non-zero exit code.
    pub async fn fsck(&mut self) -> Result<(), Error> {
        let handle = self.get_block_handle()?;
        match self.config.mode() {
            Mode::Component { .. } => {
                let exposed_dir = self.get_component_exposed_dir().await?;
                let proxy = connect_to_protocol_at_dir_root::<StartupMarker>(&exposed_dir)?;
                let mut options = CheckOptions::new_empty();
                proxy.check(handle, &mut options).await?.map_err(Status::from_raw)?;
            }
            Mode::Legacy(mut config) => {
                // SpawnAction is not Send, so make sure it is dropped before any `await`s.
                let process = {
                    let mut args = vec![config.binary_path, cstr!("fsck")];
                    args.append(&mut config.generic_args);
                    let actions = vec![
                        // device handle is passed in as a PA_USER0 handle at argument 1
                        SpawnAction::add_handle(
                            HandleInfo::new(HandleType::User0, 1),
                            handle.into(),
                        ),
                    ];
                    launch_process(&args, actions)?
                };
                wait_for_successful_exit(process).await?;
            }
        }
        Ok(())
    }

    /// Serves the filesystem on the block device and returns a [`ServingSingleVolumeFilesystem`]
    /// representing the running filesystem process.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if serving the filesystem failed.
    pub async fn serve(&mut self) -> Result<ServingSingleVolumeFilesystem, Error> {
        if self.config.is_multi_volume() {
            bail!("Can't serve a multivolume filesystem; use serve_multi_volume");
        }
        if let Mode::Component { mut start_options, reuse_component_after_serving, .. } =
            self.config.mode()
        {
            let exposed_dir = self.get_component_exposed_dir().await?;
            let proxy = connect_to_protocol_at_dir_root::<StartupMarker>(&exposed_dir)?;
            proxy
                .start(self.get_block_handle()?.into(), &mut start_options)
                .await?
                .map_err(Status::from_raw)?;

            let (root_dir, server_end) = create_endpoints::<fio::NodeMarker>()?;
            exposed_dir.open(
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::POSIX_EXECUTABLE
                    | fio::OpenFlags::POSIX_WRITABLE,
                0,
                "root",
                server_end,
            )?;
            let component = self.component.clone();
            if !reuse_component_after_serving {
                self.component = None;
            }
            Ok(ServingSingleVolumeFilesystem {
                process: None,
                _component: component,
                exposed_dir,
                root_dir: ClientEnd::<fio::DirectoryMarker>::new(root_dir.into_channel())
                    .into_proxy()?,
                binding: None,
            })
        } else {
            self.serve_legacy().await
        }
    }

    /// Serves the filesystem on the block device and returns a [`ServingMultiVolumeFilesystem`]
    /// representing the running filesystem process.  No volumes are opened; clients have to do that
    /// explicitly.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if serving the filesystem failed.
    pub async fn serve_multi_volume(&mut self) -> Result<ServingMultiVolumeFilesystem, Error> {
        if !self.config.is_multi_volume() {
            bail!("Can't serve_multi_volume a single-volume filesystem; use serve");
        }
        if let Mode::Component { mut start_options, .. } = self.config.mode() {
            let exposed_dir = self.get_component_exposed_dir().await?;
            let proxy = connect_to_protocol_at_dir_root::<StartupMarker>(&exposed_dir)?;
            proxy
                .start(self.get_block_handle()?.into(), &mut start_options)
                .await?
                .map_err(Status::from_raw)?;

            Ok(ServingMultiVolumeFilesystem {
                _component: self.component.clone(),
                exposed_dir: Some(exposed_dir),
                volumes: HashMap::default(),
            })
        } else {
            bail!("Can't serve a multivolume filesystem which isn't componentized")
        }
    }

    // TODO(fxbug.dev/87511): This is temporarily public so that we can migrate an OOT user.
    pub async fn serve_legacy(&self) -> Result<ServingSingleVolumeFilesystem, Error> {
        let (export_root, server_end) = create_proxy::<fio::DirectoryMarker>()?;

        let mode = self.config.mode();
        let mut config = mode.into_legacy_config().unwrap();

        // SpawnAction is not Send, so make sure it is dropped before any `await`s.
        let process = {
            let mut args = vec![config.binary_path, cstr!("mount")];
            args.append(&mut config.generic_args);
            args.append(&mut config.mount_args);
            let actions = vec![
                // export root handle is passed in as a PA_DIRECTORY_REQUEST handle at argument 0
                SpawnAction::add_handle(
                    HandleInfo::new(HandleType::DirectoryRequest, 0),
                    server_end.into_channel().into(),
                ),
                // device handle is passed in as a PA_USER0 handle at argument 1
                SpawnAction::add_handle(
                    HandleInfo::new(HandleType::User0, 1),
                    self.get_block_handle()?.into(),
                ),
            ];

            launch_process(&args, actions)?
        };

        // Wait until the filesystem is ready to take incoming requests.
        let (root_dir, server_end) = create_proxy::<fio::DirectoryMarker>()?;
        export_root.open(
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::POSIX_EXECUTABLE
                | fio::OpenFlags::POSIX_WRITABLE,
            0,
            "root",
            server_end.into_channel().into(),
        )?;
        let _: Vec<_> = root_dir.query().await?;

        Ok(ServingSingleVolumeFilesystem {
            process: Some(process),
            _component: None,
            exposed_dir: export_root,
            root_dir,
            binding: None,
        })
    }
}

// Destroys the child when dropped.
struct DynamicComponentInstance {
    name: String,
    collection: String,
}

impl Drop for DynamicComponentInstance {
    fn drop(&mut self) {
        if let Ok(realm_proxy) = connect_to_protocol::<RealmMarker>() {
            let _ = realm_proxy.destroy_child(&mut fdecl::ChildRef {
                name: self.name.clone(),
                collection: Some(self.collection.clone()),
            });
        }
    }
}

/// Manages the binding of a `fuchsia_io::DirectoryProxy` into the local namespace.  When the object
/// is dropped, the binding is removed.
#[derive(Default)]
pub struct NamespaceBinding(String);

impl NamespaceBinding {
    pub fn create(root_dir: &fio::DirectoryProxy, path: String) -> Result<NamespaceBinding, Error> {
        let (client_end, server_end) = create_endpoints()?;
        root_dir
            .clone(fio::OpenFlags::CLONE_SAME_RIGHTS, ServerEnd::new(server_end.into_channel()))?;
        let namespace = fdio::Namespace::installed()?;
        namespace.bind(&path, client_end)?;
        Ok(Self(path))
    }
}

impl std::ops::Deref for NamespaceBinding {
    type Target = str;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Drop for NamespaceBinding {
    fn drop(&mut self) {
        if let Ok(namespace) = fdio::Namespace::installed() {
            let _ = namespace.unbind(&self.0);
        }
    }
}

// TODO(fxbug.dev/93066): Soft migration; remove this after completion
pub type ServingFilesystem = ServingSingleVolumeFilesystem;

/// Asynchronously manages a serving filesystem. Created from [`Filesystem::serve()`].
pub struct ServingSingleVolumeFilesystem {
    // If the filesystem is running as a component, there will be no process.
    process: Option<Process>,
    _component: Option<Arc<DynamicComponentInstance>>,
    exposed_dir: fio::DirectoryProxy,
    root_dir: fio::DirectoryProxy,

    // The path in the local namespace that this filesystem is bound to (optional).
    binding: Option<NamespaceBinding>,
}

impl ServingSingleVolumeFilesystem {
    /// Returns a proxy to the root directory of the serving filesystem.
    pub fn root(&self) -> &fio::DirectoryProxy {
        &self.root_dir
    }

    /// Binds the root directory being served by this filesystem to a path in the local namespace.
    /// The path must be absolute, containing no "." nor ".." entries.  The binding will be dropped
    /// when self is dropped.  Only one binding is supported.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if binding failed.
    pub fn bind_to_path(&mut self, path: &str) -> Result<(), Error> {
        ensure!(self.binding.is_none(), "Already bound");
        self.binding = Some(NamespaceBinding::create(&self.root_dir, path.to_string())?);
        Ok(())
    }

    pub fn bound_path(&self) -> Option<&str> {
        self.binding.as_deref()
    }

    /// Returns a [`FilesystemInfo`] object containing information about the serving filesystem.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if querying the filesystem failed.
    pub async fn query(&self) -> Result<Box<fio::FilesystemInfo>, QueryError> {
        let (status, info) = self.root_dir.query_filesystem().await?;
        Status::ok(status).map_err(QueryError::DirectoryQuery)?;
        info.ok_or(QueryError::DirectoryEmptyResult)
    }

    /// Attempts to shutdown the filesystem using the
    /// [`fidl_fuchsia_fs::AdminProxy::shutdown()`] FIDL method and waiting for the filesystem
    /// process to terminate.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if the shutdown failed or the filesystem process did not terminate.
    pub async fn shutdown(mut self) -> Result<(), ShutdownError> {
        async fn do_shutdown(exposed_dir: &fio::DirectoryProxy) -> Result<(), Error> {
            connect_to_protocol_at_dir_root::<fidl_fuchsia_fs::AdminMarker>(exposed_dir)?
                .shutdown()
                .await?;
            Ok(())
        }

        if let Err(e) = do_shutdown(&self.exposed_dir).await {
            if let Some(process) = self.process.take() {
                if process.kill().is_ok() {
                    let _ = OnSignals::new(&process, Signals::PROCESS_TERMINATED).await;
                }
            }
            return Err(e.into());
        }

        if let Some(process) = self.process.take() {
            let _ = OnSignals::new(&process, Signals::PROCESS_TERMINATED)
                .await
                .map_err(ShutdownError::ProcessTerminatedSignal)?;

            let info = process.info().map_err(ShutdownError::GetProcessReturnCode)?;
            if info.return_code != 0 {
                warn!(
                    code = info.return_code,
                    "process returned non-zero exit code after shutdown"
                );
            }
        }
        Ok(())
    }

    /// Attempts to kill the filesystem process and waits for the process to terminate.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if the filesystem process could not be terminated. There is no way to
    /// recover the [`Filesystem`] from this error.
    pub async fn kill(mut self) -> Result<(), Error> {
        // Prevent the drop impl from killing the process again.
        if let Some(process) = self.process.take() {
            process.kill().map_err(KillError::TaskKill)?;
            let _ = OnSignals::new(&process, Signals::PROCESS_TERMINATED)
                .await
                .map_err(KillError::ProcessTerminatedSignal)?;
        } else {
            // For components, just shut down the filesystem.
            self.shutdown().await?;
        }
        Ok(())
    }
}

impl Drop for ServingSingleVolumeFilesystem {
    fn drop(&mut self) {
        if let Some(process) = self.process.take() {
            let _ = process.kill();
        } else {
            // For components, make a best effort attempt to shut down to the filesystem.
            if let Ok(proxy) =
                connect_to_protocol_at_dir_root::<fidl_fuchsia_fs::AdminMarker>(&self.exposed_dir)
            {
                let _ = proxy.shutdown();
            }
        }
    }
}

/// Asynchronously manages a serving multivolume filesystem. Created from
/// [`Filesystem::serve_multi_volume()`].
pub struct ServingMultiVolumeFilesystem {
    _component: Option<Arc<DynamicComponentInstance>>,
    // exposed_dir will always be Some, except in Self::shutdown.
    exposed_dir: Option<fio::DirectoryProxy>,
    volumes: HashMap<String, ServingVolume>,
}

/// Represents an opened volume in a [`ServingMultiVolumeFilesystem'] instance.
pub struct ServingVolume {
    root_dir: fio::DirectoryProxy,
    binding: Option<NamespaceBinding>,
}

impl ServingVolume {
    /// Returns a proxy to the root directory of the serving volume.
    pub fn root(&self) -> &fio::DirectoryProxy {
        &self.root_dir
    }

    /// Binds the root directory being served by this filesystem to a path in the local namespace.
    /// The path must be absolute, containing no "." nor ".." entries.  The binding will be dropped
    /// when self is dropped.  Only one binding is supported.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if binding failed.
    pub fn bind_to_path(&mut self, path: &str) -> Result<(), Error> {
        ensure!(self.binding.is_none(), "Already bound");
        self.binding = Some(NamespaceBinding::create(&self.root_dir, path.to_string())?);
        Ok(())
    }

    pub fn bound_path(&self) -> Option<&str> {
        self.binding.as_deref()
    }

    /// Returns a [`FilesystemInfo`] object containing information about the serving volume.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if querying the filesystem failed.
    pub async fn query(&self) -> Result<Box<fio::FilesystemInfo>, QueryError> {
        let (status, info) = self.root_dir.query_filesystem().await?;
        Status::ok(status).map_err(QueryError::DirectoryQuery)?;
        info.ok_or(QueryError::DirectoryEmptyResult)
    }
}

impl ServingMultiVolumeFilesystem {
    /// Gets a reference to the given volume, if it's already open.
    pub fn volume(&self, volume: &str) -> Option<&ServingVolume> {
        self.volumes.get(volume)
    }

    /// Gets a mutable reference to the given volume, if it's already open.
    pub fn volume_mut(&mut self, volume: &str) -> Option<&mut ServingVolume> {
        self.volumes.get_mut(volume)
    }

    #[cfg(test)]
    pub fn close_volume(&mut self, volume: &str) {
        self.volumes.remove(volume);
    }

    /// Returns whether the given volume exists.
    pub async fn has_volume(&mut self, volume: &str) -> Result<bool, Error> {
        if self.volumes.contains_key(volume) {
            return Ok(true);
        }
        let path = format!("volumes/{}", volume);
        fuchsia_fs::directory::open_node(
            self.exposed_dir.as_ref().unwrap(),
            &path,
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NODE_REFERENCE,
            0,
        )
        .await
        .map(|_| true)
        .or_else(|e| {
            if let fuchsia_fs::node::OpenError::OpenError(status) = &e {
                if *status == zx::Status::NOT_FOUND {
                    return Ok(false);
                }
            }
            Err(e.into())
        })
    }

    /// Creates the volume.  Fails if the volume already exists.
    /// If `crypt` is set, the volume will be encrypted using the provided Crypt instance.
    pub async fn create_volume(
        &mut self,
        volume: &str,
        crypt: Option<ClientEnd<fidl_fuchsia_fxfs::CryptMarker>>,
    ) -> Result<&mut ServingVolume, Error> {
        ensure!(!self.volumes.contains_key(volume), "Already bound");
        let (exposed_dir, server) = create_proxy::<fio::DirectoryMarker>()?;
        connect_to_protocol_at_dir_root::<fidl_fuchsia_fxfs::VolumesMarker>(
            self.exposed_dir.as_ref().unwrap(),
        )?
        .create(volume, crypt, server)
        .await?
        .map_err(|e| anyhow!(zx::Status::from_raw(e)))?;
        self.insert_volume(volume.to_string(), exposed_dir).await
    }

    /// Mounts an existing volume.  Fails if the volume is already mounted or doesn't exist.
    /// If `crypt` is set, the volume will be decrypted using the provided Crypt instance.
    pub async fn open_volume(
        &mut self,
        volume: &str,
        crypt: Option<ClientEnd<fidl_fuchsia_fxfs::CryptMarker>>,
    ) -> Result<&mut ServingVolume, Error> {
        ensure!(!self.volumes.contains_key(volume), "Already bound");
        let (exposed_dir, server) = create_proxy::<fio::DirectoryMarker>()?;
        let path = format!("volumes/{}", volume);
        connect_to_named_protocol_at_dir_root::<fidl_fuchsia_fxfs::VolumeMarker>(
            self.exposed_dir.as_ref().unwrap(),
            &path,
        )?
        .mount(server, &mut MountOptions { crypt })
        .await?
        .map_err(|e| anyhow!(zx::Status::from_raw(e)))?;

        self.insert_volume(volume.to_string(), exposed_dir).await
    }

    pub async fn check_volume(
        &mut self,
        volume: &str,
        crypt: Option<ClientEnd<fidl_fuchsia_fxfs::CryptMarker>>,
    ) -> Result<(), Error> {
        ensure!(!self.volumes.contains_key(volume), "Already bound");
        let path = format!("volumes/{}", volume);
        connect_to_named_protocol_at_dir_root::<fidl_fuchsia_fxfs::VolumeMarker>(
            self.exposed_dir.as_ref().unwrap(),
            &path,
        )?
        .check(&mut fidl_fuchsia_fxfs::CheckOptions { crypt })
        .await?
        .map_err(|e| anyhow!(zx::Status::from_raw(e)))?;
        Ok(())
    }

    async fn insert_volume(
        &mut self,
        volume: String,
        exposed_dir: fio::DirectoryProxy,
    ) -> Result<&mut ServingVolume, Error> {
        let (root_dir, server_end) = create_endpoints::<fio::NodeMarker>()?;
        exposed_dir.open(
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::POSIX_EXECUTABLE
                | fio::OpenFlags::POSIX_WRITABLE,
            0,
            "root",
            server_end,
        )?;
        Ok(self.volumes.entry(volume).or_insert(ServingVolume {
            root_dir: ClientEnd::<fio::DirectoryMarker>::new(root_dir.into_channel())
                .into_proxy()?,
            binding: None,
        }))
    }

    /// Provides access to the internal |exposed_dir| for use in testing
    /// callsites which need directory access.
    pub fn exposed_dir(&self) -> &fio::DirectoryProxy {
        self.exposed_dir.as_ref().unwrap()
    }

    /// Attempts to shutdown the filesystem using the [`fidl_fuchsia_fs::AdminProxy::shutdown()`]
    /// FIDL method.
    ///
    /// # Errors
    ///
    /// Returns [`Err`] if the shutdown failed.
    pub async fn shutdown(mut self) -> Result<(), ShutdownError> {
        connect_to_protocol_at_dir_root::<fidl_fuchsia_fs::AdminMarker>(
            // Take exposed_dir so we don't attempt to shut down again in Drop.
            &self.exposed_dir.take().unwrap(),
        )?
        .shutdown()
        .await?;
        Ok(())
    }
}

impl Drop for ServingMultiVolumeFilesystem {
    fn drop(&mut self) {
        if let Some(exposed_dir) = self.exposed_dir.take() {
            // Make a best effort attempt to shut down to the filesystem.
            if let Ok(proxy) =
                connect_to_protocol_at_dir_root::<fidl_fuchsia_fs::AdminMarker>(&exposed_dir)
            {
                let _ = proxy.shutdown();
            }
        }
    }
}

async fn wait_for_successful_exit(process: Process) -> Result<(), CommandError> {
    let _ = OnSignals::new(&process, Signals::PROCESS_TERMINATED)
        .await
        .map_err(CommandError::ProcessTerminatedSignal)?;

    let info = process.info().map_err(CommandError::GetProcessReturnCode)?;
    if info.return_code == 0 {
        Ok(())
    } else {
        Err(CommandError::ProcessNonZeroReturnCode(info.return_code))
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{BlobCompression, BlobEvictionPolicy, Blobfs, F2fs, Factoryfs, Fxfs, Minfs},
        fidl_fuchsia_io as fio, fuchsia_async as fasync,
        ramdevice_client::RamdiskClient,
        remote_block_device::{BlockClient as _, RemoteBlockClient},
        std::{
            io::{Read as _, Write as _},
            time::Duration,
        },
    };

    async fn ramdisk(block_size: u64) -> RamdiskClient {
        RamdiskClient::create(block_size, 1 << 16).await.unwrap()
    }

    fn new_fs<FSC: FSConfig>(ramdisk: &RamdiskClient, config: FSC) -> Filesystem {
        Filesystem::from_channel(ramdisk.open().unwrap().into_channel(), config).unwrap()
    }

    #[fuchsia::test]
    async fn blobfs_custom_config() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let config = Blobfs {
            verbose: true,
            readonly: true,
            write_compression_algorithm: Some(BlobCompression::Uncompressed),
            cache_eviction_policy_override: Some(BlobEvictionPolicy::EvictImmediately),
            ..Default::default()
        };
        let mut blobfs = new_fs(&ramdisk, config);

        blobfs.format().await.expect("failed to format blobfs");
        blobfs.fsck().await.expect("failed to fsck blobfs");
        let _ = blobfs.serve().await.expect("failed to serve blobfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn blobfs_format_fsck_success() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut blobfs = new_fs(&ramdisk, Blobfs::default());

        blobfs.format().await.expect("failed to format blobfs");
        blobfs.fsck().await.expect("failed to fsck blobfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[ignore]
    #[fuchsia::test]
    async fn blobfs_format_fsck_error() {
        const BLOCK_SIZE: usize = 512;

        let ramdisk = ramdisk(BLOCK_SIZE.try_into().expect("overflow")).await;
        let mut blobfs = new_fs(&ramdisk, Blobfs::default());
        let () = blobfs.format().await.expect("failed to format blobfs");

        // force fsck to fail by stomping all over one of blobfs's metadata blocks after formatting
        // TODO(fxbug.dev/35860): corrupt something other than the superblock
        {
            let device_channel = ramdisk.open().expect("failed to get channel to device");
            let device_proxy = device_channel.into_proxy().expect("into proxy");
            let block_client = RemoteBlockClient::new(device_proxy).await.expect("block client");
            let bytes = Box::new([0xff; BLOCK_SIZE]);
            let () = block_client
                .write_at(remote_block_device::BufferSlice::Memory(&(*bytes)[..]), 0)
                .await
                .expect("write to device");
        }

        let _: anyhow::Error =
            blobfs.fsck().await.expect_err("fsck succeeded when it shouldn't have");

        let () = ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn blobfs_format_serve_write_query_restart_read_shutdown() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut blobfs = new_fs(&ramdisk, Blobfs::default());

        blobfs.format().await.expect("failed to format blobfs");

        let serving = blobfs.serve().await.expect("failed to serve blobfs the first time");

        // snapshot of FilesystemInfo
        let fs_info1 =
            serving.query().await.expect("failed to query filesystem info after first serving");

        // pre-generated merkle test fixture data
        let merkle = "be901a14ec42ee0a8ee220eb119294cdd40d26d573139ee3d51e4430e7d08c28";
        let content = String::from("test content").into_bytes();

        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                merkle,
                fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_WRITABLE,
            )
            .await
            .expect("failed to create test file");
            let () = test_file
                .resize(content.len() as u64)
                .await
                .expect("failed to send resize FIDL")
                .map_err(Status::from_raw)
                .expect("failed to resize file");
            let _: u64 = test_file
                .write(&content)
                .await
                .expect("failed to write to test file")
                .map_err(Status::from_raw)
                .expect("write error");
        }

        // check against the snapshot FilesystemInfo
        let fs_info2 = serving.query().await.expect("failed to query filesystem info after write");
        assert_eq!(
            fs_info2.used_bytes - fs_info1.used_bytes,
            fs_info2.block_size as u64 // assuming content < 8K
        );

        serving.shutdown().await.expect("failed to shutdown blobfs the first time");
        let serving = blobfs.serve().await.expect("failed to serve blobfs the second time");
        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                merkle,
                fio::OpenFlags::RIGHT_READABLE,
            )
            .await
            .expect("failed to open test file");
            let read_content =
                fuchsia_fs::file::read(&test_file).await.expect("failed to read from test file");
            assert_eq!(content, read_content);
        }

        // once more check against the snapshot FilesystemInfo
        let fs_info3 = serving.query().await.expect("failed to query filesystem info after read");
        assert_eq!(
            fs_info3.used_bytes - fs_info1.used_bytes,
            fs_info3.block_size as u64 // assuming content < 8K
        );

        serving.shutdown().await.expect("failed to shutdown blobfs the second time");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn blobfs_bind_to_path() {
        let block_size = 512;
        let merkle = "be901a14ec42ee0a8ee220eb119294cdd40d26d573139ee3d51e4430e7d08c28";
        let test_content = b"test content";
        let ramdisk = ramdisk(block_size).await;
        let mut blobfs = new_fs(&ramdisk, Blobfs::default());

        blobfs.format().await.expect("failed to format blobfs");
        let mut serving = blobfs.serve().await.expect("failed to serve blobfs");
        serving.bind_to_path("/test-blobfs-path").expect("bind_to_path failed");
        let test_path = format!("/test-blobfs-path/{}", merkle);

        {
            let mut file = std::fs::File::create(&test_path).expect("failed to create test file");
            file.set_len(test_content.len() as u64).expect("failed to set size");
            file.write_all(test_content).expect("write bytes");
        }

        {
            let mut file = std::fs::File::open(&test_path).expect("failed to open test file");
            let mut buf = Vec::new();
            file.read_to_end(&mut buf).expect("failed to read test file");
            assert_eq!(buf, test_content);
        }

        serving.shutdown().await.expect("failed to shutdown blobfs");

        std::fs::File::open(&test_path).expect_err("test file was not unbound");
    }

    #[fuchsia::test]
    async fn minfs_custom_config() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let config = Minfs {
            verbose: true,
            readonly: true,
            fsck_after_every_transaction: true,
            ..Default::default()
        };
        let mut minfs = new_fs(&ramdisk, config);

        minfs.format().await.expect("failed to format minfs");
        minfs.fsck().await.expect("failed to fsck minfs");
        let _ = minfs.serve().await.expect("failed to serve minfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn minfs_format_fsck_success() {
        let block_size = 8192;
        let ramdisk = ramdisk(block_size).await;
        let mut minfs = new_fs(&ramdisk, Minfs::default());

        minfs.format().await.expect("failed to format minfs");
        minfs.fsck().await.expect("failed to fsck minfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn minfs_format_fsck_error() {
        const BLOCK_SIZE: usize = 8192;

        let ramdisk = ramdisk(BLOCK_SIZE.try_into().expect("overflow")).await;
        let mut minfs = new_fs(&ramdisk, Minfs::default());

        let () = minfs.format().await.expect("failed to format minfs");

        // force fsck to fail by stomping all over one of minfs's metadata blocks after formatting
        {
            let device_channel = ramdisk.open().expect("failed to get channel to device");
            let device_proxy = device_channel.into_proxy().expect("into proxy");
            let block_client = RemoteBlockClient::new(device_proxy).await.expect("block client");
            let bytes = Box::new([0xff; BLOCK_SIZE]);

            // when minfs isn't on an fvm, the location for its bitmap offset is the 8th block.
            // TODO(fxbug.dev/35861): parse the superblock for this offset and the block size.
            let bitmap_block_offset = 8;
            let bitmap_offset = BLOCK_SIZE * bitmap_block_offset;

            let () = block_client
                .write_at(
                    remote_block_device::BufferSlice::Memory(&(*bytes)[..]),
                    bitmap_offset.try_into().expect("overflow"),
                )
                .await
                .expect("write to device");
        }

        let _: anyhow::Error =
            minfs.fsck().await.expect_err("fsck succeeded when it shouldn't have");

        let () = ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn minfs_format_serve_write_query_restart_read_shutdown() {
        let block_size = 8192;
        let ramdisk = ramdisk(block_size).await;
        let mut minfs = new_fs(&ramdisk, Minfs::default());

        minfs.format().await.expect("failed to format minfs");
        let serving = minfs.serve().await.expect("failed to serve minfs the first time");

        // snapshot of FilesystemInfo
        let fs_info1 =
            serving.query().await.expect("failed to query filesystem info after first serving");

        let filename = "test_file";
        let content = String::from("test content").into_bytes();

        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                filename,
                fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_WRITABLE,
            )
            .await
            .expect("failed to create test file");
            let _: u64 = test_file
                .write(&content)
                .await
                .expect("failed to write to test file")
                .map_err(Status::from_raw)
                .expect("write error");
        }

        // check against the snapshot FilesystemInfo
        let fs_info2 = serving.query().await.expect("failed to query filesystem info after write");
        assert_eq!(
            fs_info2.used_bytes - fs_info1.used_bytes,
            fs_info2.block_size as u64 // assuming content < 8K
        );

        serving.shutdown().await.expect("failed to shutdown minfs the first time");
        let serving = minfs.serve().await.expect("failed to serve minfs the second time");

        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                filename,
                fio::OpenFlags::RIGHT_READABLE,
            )
            .await
            .expect("failed to open test file");
            let read_content =
                fuchsia_fs::file::read(&test_file).await.expect("failed to read from test file");
            assert_eq!(content, read_content);
        }

        // once more check against the snapshot FilesystemInfo
        let fs_info3 = serving.query().await.expect("failed to query filesystem info after read");
        assert_eq!(
            fs_info3.used_bytes - fs_info1.used_bytes,
            fs_info3.block_size as u64 // assuming content < 8K
        );

        let _ = serving.shutdown().await.expect("failed to shutdown minfs the second time");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn minfs_bind_to_path() {
        let block_size = 8192;
        let test_content = b"test content";
        let ramdisk = ramdisk(block_size).await;
        let mut minfs = new_fs(&ramdisk, Minfs::default());

        minfs.format().await.expect("failed to format minfs");
        let mut serving = minfs.serve().await.expect("failed to serve minfs");
        serving.bind_to_path("/test-minfs-path").expect("bind_to_path failed");
        let test_path = "/test-minfs-path/test_file";

        {
            let mut file = std::fs::File::create(test_path).expect("failed to create test file");
            file.write_all(test_content).expect("write bytes");
        }

        {
            let mut file = std::fs::File::open(test_path).expect("failed to open test file");
            let mut buf = Vec::new();
            file.read_to_end(&mut buf).expect("failed to read test file");
            assert_eq!(buf, test_content);
        }

        serving.shutdown().await.expect("failed to shutdown minfs");

        std::fs::File::open(test_path).expect_err("test file was not unbound");
    }

    #[fuchsia::test]
    async fn f2fs_format_fsck_success() {
        let block_size = 4096;
        let ramdisk = ramdisk(block_size).await;
        let mut f2fs = new_fs(&ramdisk, F2fs::default());

        f2fs.format().await.expect("failed to format f2fs");
        f2fs.fsck().await.expect("failed to fsck f2fs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn f2fs_format_serve_write_query_restart_read_shutdown() {
        let block_size = 4096;
        let ramdisk = ramdisk(block_size).await;
        let mut f2fs = new_fs(&ramdisk, F2fs::default());

        f2fs.format().await.expect("failed to format f2fs");
        let serving = f2fs.serve().await.expect("failed to serve f2fs the first time");

        // snapshot of FilesystemInfo
        let fs_info1 =
            serving.query().await.expect("failed to query filesystem info after first serving");

        let filename = "test_file";
        let content = String::from("test content").into_bytes();

        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                filename,
                fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_WRITABLE,
            )
            .await
            .expect("failed to create test file");
            let _: u64 = test_file
                .write(&content)
                .await
                .expect("failed to write to test file")
                .map_err(Status::from_raw)
                .expect("write error");
        }

        // check against the snapshot FilesystemInfo
        let fs_info2 = serving.query().await.expect("failed to query filesystem info after write");
        assert_eq!(
            fs_info2.used_bytes - fs_info1.used_bytes,
            fs_info2.block_size as u64 // assuming content < 4K
        );

        serving.shutdown().await.expect("failed to shutdown f2fs the first time");
        let serving = f2fs.serve().await.expect("failed to serve f2fs the second time");

        {
            let test_file = fuchsia_fs::directory::open_file(
                serving.root(),
                filename,
                fio::OpenFlags::RIGHT_READABLE,
            )
            .await
            .expect("failed to open test file");
            let read_content =
                fuchsia_fs::file::read(&test_file).await.expect("failed to read from test file");
            assert_eq!(content, read_content);
        }

        // once more check against the snapshot FilesystemInfo
        let fs_info3 = serving.query().await.expect("failed to query filesystem info after read");
        assert_eq!(
            fs_info3.used_bytes - fs_info1.used_bytes,
            fs_info3.block_size as u64 // assuming content < 4K
        );

        serving.shutdown().await.expect("failed to shutdown f2fs the second time");
        f2fs.fsck().await.expect("failed to fsck f2fs after shutting down the second time");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn f2fs_bind_to_path() {
        let block_size = 4096;
        let test_content = b"test content";
        let ramdisk = ramdisk(block_size).await;
        let mut f2fs = new_fs(&ramdisk, F2fs::default());

        f2fs.format().await.expect("failed to format f2fs");
        let mut serving = f2fs.serve().await.expect("failed to serve f2fs");
        serving.bind_to_path("/test-f2fs-path").expect("bind_to_path failed");
        let test_path = "/test-f2fs-path/test_file";

        {
            let mut file = std::fs::File::create(test_path).expect("failed to create test file");
            file.write_all(test_content).expect("write bytes");
        }

        {
            let mut file = std::fs::File::open(test_path).expect("failed to open test file");
            let mut buf = Vec::new();
            file.read_to_end(&mut buf).expect("failed to read test file");
            assert_eq!(buf, test_content);
        }

        serving.shutdown().await.expect("failed to shutdown f2fs");

        std::fs::File::open(test_path).expect_err("test file was not unbound");
    }

    #[fuchsia::test]
    async fn factoryfs_custom_config() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let config = Factoryfs { verbose: true };
        let mut factoryfs = new_fs(&ramdisk, config);

        factoryfs.format().await.expect("failed to format factoryfs");
        factoryfs.fsck().await.expect("failed to fsck factoryfs");
        let _ = factoryfs.serve().await.expect("failed to serve factoryfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn factoryfs_format_fsck_success() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut factoryfs = new_fs(&ramdisk, Factoryfs::default());

        factoryfs.format().await.expect("failed to format factoryfs");
        factoryfs.fsck().await.expect("failed to fsck factoryfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn factoryfs_format_serve_shutdown() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut factoryfs = new_fs(&ramdisk, Factoryfs::default());

        factoryfs.format().await.expect("failed to format factoryfs");
        let serving = factoryfs.serve().await.expect("failed to serve factoryfs");
        serving.shutdown().await.expect("failed to shutdown factoryfs");

        ramdisk.destroy().expect("failed to destroy ramdisk");
    }

    #[fuchsia::test]
    async fn factoryfs_bind_to_path() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut factoryfs = new_fs(&ramdisk, Factoryfs::default());

        factoryfs.format().await.expect("failed to format factoryfs");
        {
            let mut serving = factoryfs.serve().await.expect("failed to serve factoryfs");
            serving.bind_to_path("/test-factoryfs-path").expect("bind_to_path failed");

            // factoryfs is read-only, so just check that we can open the root directory.
            {
                let file = std::fs::File::open("/test-factoryfs-path")
                    .expect("failed to open root directory");
                file.metadata().expect("failed to get metadata");
            }
        }

        std::fs::File::open("/test-factoryfs-path").expect_err("factoryfs path is still bound");
    }

    // TODO(fxbug.dev/93066): Re-enable this test; it depends on Fxfs failing repeated calls to
    // Start.
    #[ignore]
    #[fuchsia::test]
    async fn fxfs_shutdown_component_when_dropped() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut fxfs = new_fs(&ramdisk, Fxfs::default());

        fxfs.format().await.expect("failed to format fxfs");
        {
            let _fs = fxfs.serve_multi_volume().await.expect("failed to serve fxfs");

            // Serve should fail for the second time.
            assert!(
                fxfs.serve_multi_volume().await.is_err(),
                "serving succeeded when already mounted"
            );
        }

        // Fxfs should get shut down when dropped, but it's asynchronous, so we need to loop here.
        let mut attempts = 0;
        loop {
            if let Ok(_) = fxfs.serve_multi_volume().await {
                break;
            }
            attempts += 1;
            assert!(attempts < 10);
            fasync::Timer::new(Duration::from_secs(1)).await;
        }
    }

    #[fuchsia::test]
    async fn fxfs_open_volume() {
        let block_size = 512;
        let ramdisk = ramdisk(block_size).await;
        let mut fxfs = new_fs(&ramdisk, Fxfs::default());

        fxfs.format().await.expect("failed to format fxfs");

        let mut fs = fxfs.serve_multi_volume().await.expect("failed to serve fxfs");

        assert_eq!(fs.has_volume("foo").await.expect("has_volume"), false);
        assert!(
            fs.open_volume("foo", None).await.is_err(),
            "Opening nonexistent volume should fail"
        );

        let vol = fs.create_volume("foo", None).await.expect("Create volume failed");
        vol.query().await.expect("Query volume failed");
        fs.close_volume("foo");
        // TODO(fxbug.dev/106555) Closing the volume is not synchronous. Immediately reopening the
        // volume will race with the asynchronous close and sometimes fail because the volume is
        // still mounted.
        // fs.open_volume("foo", None).await.expect("Open volume failed");
        assert_eq!(fs.has_volume("foo").await.expect("has_volume"), true);
    }
}
