// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    fidl_fuchsia_factory::{
        AlphaFactoryStoreProviderRequest, AlphaFactoryStoreProviderRequestStream,
        CastCredentialsFactoryStoreProviderRequest,
        CastCredentialsFactoryStoreProviderRequestStream, MiscFactoryStoreProviderRequest,
        MiscFactoryStoreProviderRequestStream, PlayReadyFactoryStoreProviderRequest,
        PlayReadyFactoryStoreProviderRequestStream, WeaveFactoryStoreProviderRequest,
        WeaveFactoryStoreProviderRequestStream, WidevineFactoryStoreProviderRequest,
        WidevineFactoryStoreProviderRequestStream,
    },
    fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self as syslog, macros::*},
    futures::{lock::Mutex, prelude::*},
    serde_json::from_reader,
    std::{collections::HashMap, fs::File, str::FromStr, sync::Arc},
    structopt::StructOpt,
    vfs::{
        directory::entry::DirectoryEntry, execution_scope::ExecutionScope,
        file::vmo::asynchronous::read_only_const, tree_builder::TreeBuilder,
    },
};

type LockedDirectoryProxy = Arc<Mutex<fio::DirectoryProxy>>;

enum IncomingServices {
    AlphaFactoryStoreProvider(AlphaFactoryStoreProviderRequestStream),
    CastCredentialsFactoryStoreProvider(CastCredentialsFactoryStoreProviderRequestStream),
    MiscFactoryStoreProvider(MiscFactoryStoreProviderRequestStream),
    PlayReadyFactoryStoreProvider(PlayReadyFactoryStoreProviderRequestStream),
    WeaveFactoryStoreProvider(WeaveFactoryStoreProviderRequestStream),
    WidevineFactoryStoreProvider(WidevineFactoryStoreProviderRequestStream),
}

fn start_test_dir(config_path: &str) -> Result<fio::DirectoryProxy, Error> {
    let files: HashMap<String, String> = match File::open(&config_path) {
        Ok(file) => from_reader(file)?,
        Err(err) => {
            fx_log_warn!("publishing empty directory for {} due to error: {:?}", &config_path, err);
            HashMap::new()
        }
    };

    fx_log_info!("Files from {}: {:?}", &config_path, files);

    let mut tree = TreeBuilder::empty_dir();

    for (name, contents) in files.into_iter() {
        tree.add_entry(
            &name.split("/").collect::<Vec<&str>>(),
            read_only_const(contents.as_bytes()),
        )
        .unwrap();
    }

    let test_dir = tree.build();

    let (test_dir_proxy, test_dir_service) =
        fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    test_dir.open(
        ExecutionScope::new(),
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY,
        vfs::path::Path::dot(),
        test_dir_service.into_channel().into(),
    );

    Ok(test_dir_proxy)
}

async fn run_server(req: IncomingServices, dir_mtx: LockedDirectoryProxy) -> Result<(), Error> {
    match req {
        IncomingServices::AlphaFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let AlphaFactoryStoreProviderRequest::GetFactoryStore { dir, control_handle: _ } =
                    request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
        IncomingServices::CastCredentialsFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let CastCredentialsFactoryStoreProviderRequest::GetFactoryStore {
                    dir,
                    control_handle: _,
                } = request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
        IncomingServices::MiscFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let MiscFactoryStoreProviderRequest::GetFactoryStore { dir, control_handle: _ } =
                    request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
        IncomingServices::PlayReadyFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let PlayReadyFactoryStoreProviderRequest::GetFactoryStore {
                    dir,
                    control_handle: _,
                } = request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
        IncomingServices::WeaveFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let WeaveFactoryStoreProviderRequest::GetFactoryStore { dir, control_handle: _ } =
                    request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
        IncomingServices::WidevineFactoryStoreProvider(mut stream) => {
            while let Some(request) = stream.try_next().await? {
                let WidevineFactoryStoreProviderRequest::GetFactoryStore { dir, control_handle: _ } =
                    request;
                dir_mtx
                    .lock()
                    .await
                    .clone(fio::OpenFlags::RIGHT_READABLE, dir.into_channel().into())?;
            }
        }
    }
    Ok(())
}

#[derive(Debug, StructOpt)]
enum Provider {
    Alpha,
    Cast,
    Misc,
    Playready,
    Weave,
    Widevine,
}
impl FromStr for Provider {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let formatted_str = s.trim().to_lowercase();
        match formatted_str.as_ref() {
            "alpha" => Ok(Provider::Alpha),
            "cast" => Ok(Provider::Cast),
            "misc" => Ok(Provider::Misc),
            "playready" => Ok(Provider::Playready),
            "weave" => Ok(Provider::Weave),
            "widevine" => Ok(Provider::Widevine),
            _ => Err(format_err!("Could not find '{}' provider", formatted_str)),
        }
    }
}

#[derive(Debug, StructOpt)]
struct Flags {
    /// The factory store provider to fake.
    #[structopt(short, long)]
    provider: Provider,

    /// The path to the config file for the provider.
    #[structopt(short, long)]
    config: String,
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["fake_factory_store_providers"])?;
    let flags = Flags::from_args();
    let dir = Arc::new(Mutex::new(start_test_dir(&flags.config)?));

    let mut fs = ServiceFs::new_local();
    let mut fs_dir = fs.dir("svc");

    match flags.provider {
        Provider::Alpha => fs_dir.add_fidl_service(IncomingServices::AlphaFactoryStoreProvider),
        Provider::Cast => {
            fs_dir.add_fidl_service(IncomingServices::CastCredentialsFactoryStoreProvider)
        }
        Provider::Misc => fs_dir.add_fidl_service(IncomingServices::MiscFactoryStoreProvider),
        Provider::Playready => {
            fs_dir.add_fidl_service(IncomingServices::PlayReadyFactoryStoreProvider)
        }
        Provider::Weave => fs_dir.add_fidl_service(IncomingServices::WeaveFactoryStoreProvider),
        Provider::Widevine => {
            fs_dir.add_fidl_service(IncomingServices::WidevineFactoryStoreProvider)
        }
    };

    fs.take_and_serve_directory_handle()?;
    fs.for_each_concurrent(10, |req| {
        run_server(req, dir.clone()).unwrap_or_else(|e| fx_log_err!("{:?}", e))
    })
    .await;
    Ok(())
}
