// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::match_common::node_to_device_property,
    crate::node_group_manager::NodeGroupManager,
    crate::resolved_driver::{load_driver, DriverPackageType, ResolvedDriver},
    anyhow::{self, Context},
    bind::interpreter::decode_bind_rules::DecodedRules,
    fidl_fuchsia_boot as fboot, fidl_fuchsia_driver_development as fdd,
    fidl_fuchsia_driver_framework as fdf, fidl_fuchsia_driver_index as fdi,
    fidl_fuchsia_driver_index::{DriverIndexRequest, DriverIndexRequestStream},
    fidl_fuchsia_driver_registrar as fdr, fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_component::server::ServiceFs,
    fuchsia_zircon::Status,
    futures::prelude::*,
    serde::Deserialize,
    std::{
        cell::RefCell,
        collections::HashMap,
        collections::HashSet,
        ops::Deref,
        ops::DerefMut,
        rc::Rc,
        sync::{Arc, Mutex},
    },
};

mod match_common;
mod node_group_manager;
mod package_resolver;
mod resolved_driver;

#[derive(Deserialize)]
struct JsonDriver {
    driver_url: String,
}

fn ignore_peer_closed(err: fidl::Error) -> Result<(), fidl::Error> {
    if err.is_closed() {
        Ok(())
    } else {
        Err(err)
    }
}

fn log_error(err: anyhow::Error) -> anyhow::Error {
    log::error!("{:#?}", err);
    err
}

/// Wraps all hosted protocols into a single type that can be matched against
/// and dispatched.
enum IncomingRequest {
    DriverIndexProtocol(DriverIndexRequestStream),
    DriverDevelopmentProtocol(fdd::DriverIndexRequestStream),
    DriverRegistrarProtocol(fdr::DriverRegistrarRequestStream),
}

async fn load_boot_drivers(
    dir: fio::DirectoryProxy,
    eager_drivers: &HashSet<url::Url>,
    disabled_drivers: &HashSet<url::Url>,
) -> Result<Vec<ResolvedDriver>, anyhow::Error> {
    let meta = fuchsia_fs::directory::open_directory_no_describe(
        &dir,
        "meta",
        fio::OpenFlags::RIGHT_READABLE,
    )
    .context("boot: Failed to open meta")?;

    let entries =
        fuchsia_fs::directory::readdir(&meta).await.context("boot: failed to read meta")?;

    let mut drivers = std::vec::Vec::new();
    for entry in entries
        .iter()
        .filter(|entry| entry.kind == fuchsia_fs::directory::DirentKind::File)
        .filter(|entry| entry.name.ends_with(".cm"))
    {
        let url_string = "fuchsia-boot:///#meta/".to_string() + &entry.name;
        let url = url::Url::parse(&url_string)?;
        let driver = load_driver(&dir, url, DriverPackageType::Boot, None).await;
        if let Err(e) = driver {
            log::error!("Failed to load boot driver: {}: {}", url_string, e);
            continue;
        }
        if let Ok(Some(mut driver)) = driver {
            if disabled_drivers.contains(&driver.component_url) {
                log::info!("Skipping boot driver: {}", driver.component_url.to_string());
                continue;
            }
            log::info!("Found boot driver: {}", driver.component_url.to_string());
            if eager_drivers.contains(&driver.component_url) {
                driver.fallback = false;
            }
            drivers.push(driver);
        }
    }
    Ok(drivers)
}

enum BaseRepo {
    // We know that Base won't update so we can store these as resolved.
    Resolved(Vec<ResolvedDriver>),
    // If it's not resolved we store the clients waiting for it.
    NotResolved(Vec<fdi::DriverIndexWaitForBaseDriversResponder>),
}

struct Indexer {
    boot_repo: Vec<ResolvedDriver>,

    // |base_repo| needs to be in a RefCell because it starts out NotResolved,
    // but will eventually resolve when base packages are available.
    base_repo: RefCell<BaseRepo>,

    // Manages the node groups. This is wrapped in a RefCell since the
    // node groups are added after the driver index server has started.
    node_group_manager: RefCell<NodeGroupManager>,

    // Whether /system is required. Used to determine if the indexer should
    // return fallback drivers that match.
    require_system: bool,

    // Contains the ephemeral drivers. This is wrapped in a RefCell since the
    // ephemeral drivers are added after the driver index server has started
    // through the FIDL API, fuchsia.driver.registrar.Register.
    ephemeral_drivers: RefCell<HashMap<fidl_fuchsia_pkg::PackageUrl, ResolvedDriver>>,
}

impl Indexer {
    fn new(boot_repo: Vec<ResolvedDriver>, base_repo: BaseRepo, require_system: bool) -> Indexer {
        Indexer {
            boot_repo,
            base_repo: RefCell::new(base_repo),
            node_group_manager: RefCell::new(NodeGroupManager::new()),
            require_system,
            ephemeral_drivers: RefCell::new(HashMap::new()),
        }
    }

    fn include_fallback_drivers(&self) -> bool {
        !self.require_system
            || match *self.base_repo.borrow() {
                BaseRepo::Resolved(_) => true,
                _ => false,
            }
    }

    fn load_base_repo(&self, base_repo: BaseRepo) {
        if let BaseRepo::NotResolved(waiters) = self.base_repo.borrow_mut().deref_mut() {
            while let Some(waiter) = waiters.pop() {
                match waiter.send().or_else(ignore_peer_closed) {
                    Err(e) => log::error!("Error sending to base_waiter: {:?}", e),
                    Ok(_) => continue,
                }
            }
        }
        self.base_repo.replace(base_repo);
    }

    fn match_driver(&self, args: fdi::MatchDriverArgs) -> fdi::DriverIndexMatchDriverResult {
        if args.properties.is_none() {
            return Err(Status::INVALID_ARGS.into_raw());
        }
        let properties = args.properties.unwrap();
        let properties = node_to_device_property(&properties)?;

        // Prioritize node groups to avoid match conflicts with composite drivers.
        let node_group_match =
            self.node_group_manager.borrow().match_node_representations(&properties);
        if let Some(node_group) = node_group_match {
            return Ok(node_group);
        }

        let base_repo = self.base_repo.borrow();
        let base_repo_iter = match base_repo.deref() {
            BaseRepo::Resolved(drivers) => drivers.iter(),
            BaseRepo::NotResolved(_) => [].iter(),
        };
        let (boot_drivers, base_drivers) = if self.include_fallback_drivers() {
            (self.boot_repo.iter(), base_repo_iter.clone())
        } else {
            ([].iter(), [].iter())
        };
        let fallback_boot_drivers = boot_drivers.filter(|&driver| driver.fallback);
        let fallback_base_drivers = base_drivers.filter(|&driver| driver.fallback);

        let ephemeral = self.ephemeral_drivers.borrow();
        // Iterate over all drivers. Match non-fallback boot drivers, then
        // non-fallback base drivers, then fallback boot drivers, then fallback
        // base drivers.
        let (mut fallback, mut non_fallback): (
            Vec<(bool, fdi::MatchedDriver)>,
            Vec<(bool, fdi::MatchedDriver)>,
        ) = self
            .boot_repo
            .iter()
            .filter(|&driver| !driver.fallback)
            .chain(base_repo_iter.filter(|&driver| !driver.fallback))
            .chain(ephemeral.values())
            .chain(fallback_boot_drivers)
            .chain(fallback_base_drivers)
            .filter_map(|driver| {
                if let Ok(Some(matched)) = driver.matches(&properties) {
                    Some((driver.fallback, matched))
                } else {
                    None
                }
            })
            .partition(|(fallback, _)| *fallback);

        match (non_fallback.len(), fallback.len()) {
            (1, _) => Ok(non_fallback.pop().unwrap().1),
            (0, 1) => Ok(fallback.pop().unwrap().1),
            (0, 0) => Err(Status::NOT_FOUND.into_raw()),
            (0, _) => {
                log::error!("Failed to match driver: Encountered unsupported behavior: Zero non-fallback drivers and more than one fallback drivers were matched");
                Err(Status::NOT_SUPPORTED.into_raw())
            }
            _ => {
                log::error!("Failed to match driver: Encountered unsupported behavior: Multiple non-fallback drivers were matched");
                Err(Status::NOT_SUPPORTED.into_raw())
            }
        }
    }

    fn match_drivers_v1(&self, args: fdi::MatchDriverArgs) -> fdi::DriverIndexMatchDriversV1Result {
        if args.properties.is_none() {
            return Err(Status::INVALID_ARGS.into_raw());
        }
        let properties = args.properties.unwrap();
        let properties = node_to_device_property(&properties)?;

        let base_repo = self.base_repo.borrow();
        let base_repo_iter = match base_repo.deref() {
            BaseRepo::Resolved(drivers) => drivers.iter(),
            BaseRepo::NotResolved(_) => [].iter(),
        };
        let (boot_drivers, base_drivers) = if self.include_fallback_drivers() {
            (self.boot_repo.iter(), base_repo_iter.clone())
        } else {
            ([].iter(), [].iter())
        };
        let fallback_boot_drivers = boot_drivers.filter(|&driver| driver.fallback);
        let fallback_base_drivers = base_drivers.filter(|&driver| driver.fallback);

        let ephemeral = self.ephemeral_drivers.borrow();

        // Iterate over all drivers. Match non-fallback boot drivers, then
        // non-fallback base drivers, then fallback boot drivers, then fallback
        // base drivers.
        let mut matched_drivers: Vec<fdi::MatchedDriver> = self
            .boot_repo
            .iter()
            .filter(|&driver| !driver.fallback)
            .chain(base_repo_iter.filter(|&driver| !driver.fallback))
            .chain(ephemeral.values())
            .chain(fallback_boot_drivers)
            .chain(fallback_base_drivers)
            .filter_map(|driver| driver.matches(&properties).ok())
            .filter_map(|d| d)
            .collect();

        // Prioritize node groups to avoid match conflicts with composite drivers.
        let node_group_match =
            self.node_group_manager.borrow().match_node_representations(&properties);
        if let Some(node_group) = node_group_match {
            matched_drivers.insert(0, node_group);
        }

        Ok(matched_drivers)
    }

    fn add_node_group(&self, group: fdf::NodeGroup) -> fdi::DriverIndexAddNodeGroupResult {
        let base_repo = self.base_repo.borrow();
        let base_repo_iter = match base_repo.deref() {
            BaseRepo::Resolved(drivers) => drivers.iter(),
            BaseRepo::NotResolved(_) => [].iter(),
        };
        let (boot_drivers, base_drivers) = if self.include_fallback_drivers() {
            (self.boot_repo.iter(), base_repo_iter.clone())
        } else {
            ([].iter(), [].iter())
        };
        let fallback_boot_drivers = boot_drivers.filter(|&driver| driver.fallback);
        let fallback_base_drivers = base_drivers.filter(|&driver| driver.fallback);

        let ephemeral = self.ephemeral_drivers.borrow();
        let composite_drivers = self
            .boot_repo
            .iter()
            .filter(|&driver| !driver.fallback)
            .chain(base_repo_iter.filter(|&driver| !driver.fallback))
            .chain(ephemeral.values())
            .chain(fallback_boot_drivers)
            .chain(fallback_base_drivers)
            .filter(|&driver| matches!(driver.bind_rules, DecodedRules::Composite(_)))
            .collect::<Vec<_>>();

        let mut node_group_manager = self.node_group_manager.borrow_mut();
        node_group_manager.add_node_group(group, composite_drivers)
    }

    fn get_driver_info(&self, driver_filter: Vec<String>) -> Vec<fdd::DriverInfo> {
        let mut driver_info = Vec::new();

        for driver in &self.boot_repo {
            if driver_filter.len() == 0 || driver_filter.iter().any(|f| f == &driver.get_libname())
            {
                driver_info.push(driver.create_driver_info());
            }
        }

        let base_repo = self.base_repo.borrow();
        if let BaseRepo::Resolved(drivers) = &base_repo.deref() {
            for driver in drivers {
                if driver_filter.len() == 0
                    || driver_filter.iter().any(|f| f == driver.component_url.as_str())
                {
                    driver_info.push(driver.create_driver_info());
                }
            }
        }

        let ephemeral = self.ephemeral_drivers.borrow();
        for driver in ephemeral.values() {
            if driver_filter.len() == 0
                || driver_filter.iter().any(|f| f == driver.component_url.as_str())
            {
                driver_info.push(driver.create_driver_info());
            }
        }

        driver_info
    }

    async fn register_driver(
        &self,
        pkg_url: fidl_fuchsia_pkg::PackageUrl,
        resolver: &fidl_fuchsia_pkg::PackageResolverProxy,
    ) -> Result<(), i32> {
        for boot_driver in self.boot_repo.iter() {
            if boot_driver.component_url.as_str() == pkg_url.url.as_str() {
                log::warn!("Driver being registered already exists in boot list.");
                return Err(Status::ALREADY_EXISTS.into_raw());
            }
        }

        match self.base_repo.borrow().deref() {
            BaseRepo::Resolved(resolved_base_drivers) => {
                for base_driver in resolved_base_drivers {
                    if base_driver.component_url.as_str() == pkg_url.url.as_str() {
                        log::warn!("Driver being registered already exists in base list.");
                        return Err(Status::ALREADY_EXISTS.into_raw());
                    }
                }
            }
            _ => (),
        };

        let url = match url::Url::parse(&pkg_url.url) {
            Ok(u) => Ok(u),
            Err(e) => {
                log::error!("Couldn't parse driver url: {}: error: {}", &pkg_url.url, e);
                Err(Status::ADDRESS_UNREACHABLE.into_raw())
            }
        }?;

        let resolve = ResolvedDriver::resolve(url, &resolver, DriverPackageType::Universe).await;

        if resolve.is_err() {
            return Err(resolve.err().unwrap().into_raw());
        }

        let resolved_driver = resolve.unwrap();

        let mut node_group_manager = self.node_group_manager.borrow_mut();
        node_group_manager.new_driver_available(resolved_driver.clone());

        let mut ephemeral_drivers = self.ephemeral_drivers.borrow_mut();
        let existing = ephemeral_drivers.insert(pkg_url.clone(), resolved_driver);

        if let Some(existing_driver) = existing {
            log::info!("Updating existing ephemeral driver {}.", existing_driver);
        } else {
            log::info!("Registered driver successfully: {}.", pkg_url.url);
        }

        Ok(())
    }
}

async fn run_driver_info_iterator_server(
    driver_info: Arc<Mutex<Vec<fdd::DriverInfo>>>,
    stream: fdd::DriverInfoIteratorRequestStream,
) -> Result<(), anyhow::Error> {
    stream
        .map(|result| result.context("failed request"))
        .try_for_each(|request| async {
            let driver_info_clone = driver_info.clone();
            match request {
                fdd::DriverInfoIteratorRequest::GetNext { responder } => {
                    let result = {
                        let mut driver_info = driver_info_clone.lock().unwrap();
                        let len = driver_info.len();
                        driver_info.split_off(len - std::cmp::min(100, len))
                    };

                    responder
                        .send(&mut result.into_iter())
                        .or_else(ignore_peer_closed)
                        .context("error responding to GetDriverInfo")?;
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

async fn run_node_groups_iterator_server(
    node_groups: Arc<Mutex<Vec<fdd::NodeGroupInfo>>>,
    stream: fdd::NodeGroupsIteratorRequestStream,
) -> Result<(), anyhow::Error> {
    stream
        .map(|result| result.context("failed request"))
        .try_for_each(|request| async {
            let node_groups_clone = node_groups.clone();
            match request {
                fdd::NodeGroupsIteratorRequest::GetNext { responder } => {
                    let result = {
                        let mut node_groups = node_groups_clone.lock().unwrap();
                        let len = node_groups.len();
                        node_groups.split_off(len - std::cmp::min(10, len))
                    };

                    responder
                        .send(&mut result.into_iter())
                        .or_else(ignore_peer_closed)
                        .context("error responding to GetNodeGroups")?;
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

async fn run_driver_development_server(
    indexer: Rc<Indexer>,
    stream: fdd::DriverIndexRequestStream,
) -> Result<(), anyhow::Error> {
    stream
        .map(|result| result.context("failed request"))
        .try_for_each(|request| async {
            let indexer = indexer.clone();
            match request {
                fdd::DriverIndexRequest::GetDriverInfo { driver_filter, iterator, .. } => {
                    let driver_info = indexer.get_driver_info(driver_filter);
                    if driver_info.len() == 0 {
                        iterator.close_with_epitaph(Status::NOT_FOUND)?;
                        return Ok(());
                    }
                    let driver_info = Arc::new(Mutex::new(driver_info));
                    let iterator = iterator.into_stream()?;
                    fasync::Task::spawn(async move {
                        run_driver_info_iterator_server(driver_info, iterator)
                            .await
                            .expect("Failed to run driver info iterator");
                    })
                    .detach();
                }
                fdd::DriverIndexRequest::GetNodeGroups { name_filter, iterator, .. } => {
                    let node_group_manager = indexer.node_group_manager.borrow();
                    let node_groups = node_group_manager.get_node_groups(name_filter);
                    if node_groups.is_empty() {
                        iterator.close_with_epitaph(Status::NOT_FOUND)?;
                        return Ok(());
                    }
                    let node_groups = Arc::new(Mutex::new(node_groups));
                    let iterator = iterator.into_stream()?;
                    fasync::Task::spawn(async move {
                        run_node_groups_iterator_server(node_groups, iterator)
                            .await
                            .expect("Failed to run node groups iterator");
                    })
                    .detach();
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

async fn run_driver_registrar_server(
    indexer: Rc<Indexer>,
    stream: fdr::DriverRegistrarRequestStream,
    universe_resolver: &Option<fidl_fuchsia_pkg::PackageResolverProxy>,
) -> Result<(), anyhow::Error> {
    stream
        .map(|result| result.context("failed request"))
        .try_for_each(|request| async {
            let indexer = indexer.clone();
            match request {
                fdr::DriverRegistrarRequest::Register { package_url, responder } => {
                    match universe_resolver {
                        None => {
                            responder
                                .send(&mut Err(Status::PROTOCOL_NOT_SUPPORTED.into_raw()))
                                .or_else(ignore_peer_closed)
                                .context("error responding to Register")?;
                        }
                        Some(resolver) => {
                            let mut register_result =
                                indexer.register_driver(package_url, resolver).await;

                            responder
                                .send(&mut register_result)
                                .or_else(ignore_peer_closed)
                                .context("error responding to Register")?;
                        }
                    }
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

async fn run_index_server(
    indexer: Rc<Indexer>,
    stream: DriverIndexRequestStream,
) -> Result<(), anyhow::Error> {
    stream
        .map(|result| result.context("failed request"))
        .try_for_each(|request| async {
            let indexer = indexer.clone();
            match request {
                DriverIndexRequest::MatchDriver { args, responder } => {
                    responder
                        .send(&mut indexer.match_driver(args))
                        .or_else(ignore_peer_closed)
                        .context("error responding to MatchDriver")?;
                }
                DriverIndexRequest::WaitForBaseDrivers { responder } => {
                    match indexer.base_repo.borrow_mut().deref_mut() {
                        BaseRepo::Resolved(_) => {
                            responder
                                .send()
                                .or_else(ignore_peer_closed)
                                .context("error responding to WaitForBaseDrivers")?;
                        }
                        BaseRepo::NotResolved(waiters) => {
                            waiters.push(responder);
                        }
                    }
                }
                DriverIndexRequest::MatchDriversV1 { args, responder } => {
                    responder
                        .send(&mut indexer.match_drivers_v1(args))
                        .or_else(ignore_peer_closed)
                        .context("error responding to MatchDriversV1")?;
                }
                DriverIndexRequest::AddNodeGroup { payload, responder } => {
                    responder
                        .send(&mut indexer.add_node_group(payload))
                        .or_else(ignore_peer_closed)
                        .context("error responding to AddNodeGroup")?;
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

async fn load_base_drivers(
    indexer: Rc<Indexer>,
    resolver: &fidl_fuchsia_pkg::PackageResolverProxy,
    eager_drivers: &HashSet<url::Url>,
    disabled_drivers: &HashSet<url::Url>,
) -> Result<(), anyhow::Error> {
    let (dir, dir_server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    let res =
        resolver.resolve("fuchsia-pkg://fuchsia.com/driver-manager-base-config", dir_server_end);
    res.await?.map_err(|e| anyhow::anyhow!("Failed to resolve package: {:?}", e))?;
    let data = fuchsia_fs::directory::open_file_no_describe(
        &dir,
        "config/base-driver-manifest.json",
        fio::OpenFlags::RIGHT_READABLE,
    )?;

    let data: String =
        fuchsia_fs::file::read_to_string(&data).await.context("Failed to read base manifest")?;
    let drivers: Vec<JsonDriver> = serde_json::from_str(&data)?;
    let mut resolved_drivers = std::vec::Vec::new();
    for driver in drivers {
        let url = match url::Url::parse(&driver.driver_url) {
            Ok(u) => u,
            Err(e) => {
                log::error!("Found bad base driver url: {}: error: {}", driver.driver_url, e);
                continue;
            }
        };
        let resolve = ResolvedDriver::resolve(url, &resolver, DriverPackageType::Base).await;

        if resolve.is_err() {
            continue;
        }

        let mut resolved_driver = resolve.unwrap();
        if disabled_drivers.contains(&resolved_driver.component_url) {
            log::info!("Skipping base driver: {}", resolved_driver.component_url.to_string());
            continue;
        }
        log::info!("Found base driver: {}", resolved_driver.component_url.to_string());
        if eager_drivers.contains(&resolved_driver.component_url) {
            resolved_driver.fallback = false;
        }

        let mut node_group_manager = indexer.node_group_manager.borrow_mut();
        node_group_manager.new_driver_available(resolved_driver.clone());
        resolved_drivers.push(resolved_driver);
    }
    indexer.load_base_repo(BaseRepo::Resolved(resolved_drivers));
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), anyhow::Error> {
    // This is to make sure driver_index's logs show up in serial.
    fuchsia_syslog::init_with_tags(&["driver"]).unwrap();

    let mut service_fs = ServiceFs::new_local();

    service_fs.dir("svc").add_fidl_service(IncomingRequest::DriverIndexProtocol);
    service_fs.dir("svc").add_fidl_service(IncomingRequest::DriverDevelopmentProtocol);
    service_fs.dir("svc").add_fidl_service(IncomingRequest::DriverRegistrarProtocol);
    service_fs.take_and_serve_directory_handle().context("failed to serve outgoing namespace")?;

    let (pkgfs_resolver, pkgfs_resolver_stream) =
        fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
            .unwrap();

    let boot_args = client::connect_to_protocol::<fboot::ArgumentsMarker>()
        .context("Failed to connect to boot arguments service")?;

    let enable_ephemeral = boot_args
        .get_bool("devmgr.enable-ephemeral", false)
        .await
        .context("Failed to get devmgr.enable-ephemeral from boot arguments")?;

    let universe_resolver = if enable_ephemeral {
        let package_resolver_client =
            client::connect_to_protocol::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .context("Failed to connect to universe package resolver")?;
        Some(package_resolver_client)
    } else {
        None
    };

    let eager_drivers: HashSet<url::Url> = boot_args
        .get_string("devmgr.bind-eager")
        .await
        .context("Failed to get eager drivers from boot arguments")?
        .unwrap_or_default()
        .split(",")
        .filter(|url| !url.is_empty())
        .filter_map(|url| url::Url::parse(url).ok())
        .collect();
    let disabled_drivers: HashSet<url::Url> = boot_args
        .get_string("devmgr.disabled-drivers")
        .await
        .context("Failed to get disabled drivers from boot arguments")?
        .unwrap_or_default()
        .split(",")
        .filter(|url| !url.is_empty())
        .filter_map(|url| url::Url::parse(url).ok())
        .collect();
    let require_system: bool = boot_args
        .get_bool("devmgr.require-system", false)
        .await
        .context("Failed to get value of `devmgr.require-system` from boot arguments")?;
    for driver in disabled_drivers.iter() {
        log::info!("Disabling driver {}", driver);
    }
    for driver in eager_drivers.iter() {
        log::info!("Marking driver {} as eager", driver);
    }

    let boot = fuchsia_fs::directory::open_in_namespace("/boot", fio::OpenFlags::RIGHT_READABLE)
        .context("Failed to open /boot")?;
    let drivers = load_boot_drivers(boot, &eager_drivers, &disabled_drivers)
        .await
        .context("Failed to load boot drivers")
        .map_err(log_error)?;

    let mut should_load_base_drivers = true;

    for argument in std::env::args() {
        if argument == "--no-base-drivers" {
            should_load_base_drivers = false;
            log::info!("Not loading base drivers");
        }
    }

    let index = Rc::new(Indexer::new(drivers, BaseRepo::NotResolved(std::vec![]), require_system));
    let (res1, res2, _) = futures::future::join3(
        async {
            package_resolver::serve(pkgfs_resolver_stream)
                .await
                .context("Error running package resolver")
                .map_err(log_error)
        },
        async {
            if should_load_base_drivers {
                load_base_drivers(index.clone(), &pkgfs_resolver, &eager_drivers, &disabled_drivers)
                    .await
                    .context("Error loading base packages")
                    .map_err(log_error)
            } else {
                Ok(())
            }
        },
        async {
            service_fs
                .for_each_concurrent(None, |request: IncomingRequest| async {
                    // match on `request` and handle each protocol.
                    match request {
                        IncomingRequest::DriverIndexProtocol(stream) => {
                            run_index_server(index.clone(), stream).await
                        }
                        IncomingRequest::DriverDevelopmentProtocol(stream) => {
                            run_driver_development_server(index.clone(), stream).await
                        }
                        IncomingRequest::DriverRegistrarProtocol(stream) => {
                            run_driver_registrar_server(index.clone(), stream, &universe_resolver)
                                .await
                        }
                    }
                    .unwrap_or_else(|e| log::error!("Error running index_server: {:?}", e))
                })
                .await;
        },
    )
    .await;

    res1?;
    res2?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        bind::{
            compiler::{
                CompiledBindRules, CompositeBindRules, CompositeNode, Symbol, SymbolicInstruction,
                SymbolicInstructionInfo,
            },
            interpreter::decode_bind_rules::DecodedRules,
            parser::bind_library::ValueType,
        },
        fidl_fuchsia_data as fdata, fidl_fuchsia_pkg as fpkg,
        std::collections::HashMap,
    };

    fn create_matched_driver_info(
        url: String,
        driver_url: String,
        colocate: bool,
        device_categories: Vec<fdi::DeviceCategory>,
        package_type: DriverPackageType,
        fallback: bool,
    ) -> fdi::MatchedDriverInfo {
        fdi::MatchedDriverInfo {
            url: Some(url),
            driver_url: Some(driver_url),
            colocate: Some(colocate),
            device_categories: Some(device_categories),
            package_type: fdi::DriverPackageType::from_primitive(package_type as u8),
            is_fallback: Some(fallback),
            ..fdi::MatchedDriverInfo::EMPTY
        }
    }

    fn create_default_device_category() -> fdi::DeviceCategory {
        fdi::DeviceCategory {
            category: Some(resolved_driver::DEFAULT_DEVICE_CATEGORY.to_string()),
            subcategory: None,
            ..fdi::DeviceCategory::EMPTY
        }
    }

    async fn get_driver_info_proxy(
        development_proxy: &fdd::DriverIndexProxy,
        driver_filter: &[&str],
    ) -> Vec<fdd::DriverInfo> {
        let (info_iterator, info_iterator_server) =
            fidl::endpoints::create_proxy::<fdd::DriverInfoIteratorMarker>().unwrap();

        development_proxy
            .get_driver_info(&mut driver_filter.iter().map(|i| *i), info_iterator_server)
            .unwrap();

        let mut driver_infos = Vec::new();
        loop {
            let driver_info = info_iterator.get_next().await;
            if driver_info.is_err() {
                break;
            }
            let mut driver_info = driver_info.unwrap();
            if driver_info.len() == 0 {
                break;
            }
            driver_infos.append(&mut driver_info)
        }

        return driver_infos;
    }

    async fn run_resolver_server(
        stream: fidl_fuchsia_pkg::PackageResolverRequestStream,
    ) -> Result<(), anyhow::Error> {
        stream
            .map(|result| result.context("failed request"))
            .try_for_each(|request| async {
                match request {
                    fidl_fuchsia_pkg::PackageResolverRequest::Resolve {
                        package_url: _,
                        dir,
                        responder,
                    } => {
                        let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY;
                        fuchsia_fs::directory::open_channel_in_namespace("/pkg", flags, dir)
                            .unwrap();
                        responder
                            .send(&mut Ok(fidl_fuchsia_pkg::ResolutionContext { bytes: vec![] }))
                            .context("error sending response")?;
                    }
                    fidl_fuchsia_pkg::PackageResolverRequest::ResolveWithContext {
                        package_url: _,
                        context: _,
                        dir: _,
                        responder,
                    } => {
                        log::error!(
                            "ResolveWithContext is not currently implemented in driver-index"
                        );
                        responder
                            .send(&mut Err(fidl_fuchsia_pkg::ResolveError::Internal))
                            .context("error sending response")?;
                    }
                    fidl_fuchsia_pkg::PackageResolverRequest::GetHash {
                        package_url: _,
                        responder,
                    } => {
                        // This package hash is arbitrary and is not tested for
                        // driver index's tests, however, `GetHash` must return
                        // something in order to avoid failing to load
                        // driver packages.
                        responder
                            .send(&mut Ok(fpkg::BlobId {
                                merkle_root: [
                                    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                                    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
                                ],
                            }))
                            .context("error sending response")?;
                    }
                }
                Ok(())
            })
            .await?;
        Ok(())
    }

    async fn execute_driver_index_test(
        index: Indexer,
        stream: fdi::DriverIndexRequestStream,
        test: impl Future<Output = ()>,
    ) {
        let index = Rc::new(index);
        let index_task = run_index_server(index.clone(), stream).fuse();
        let test = test.fuse();

        futures::pin_mut!(index_task, test);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test => {},
        }
    }

    fn create_always_match_bind_rules() -> DecodedRules {
        let bind_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(bind_rules).unwrap(),
        )
        .unwrap()
    }

    // This test depends on '/pkg/config/drivers_for_test.json' existing in the test package.
    // The test reads that json file to determine which bind rules to read and index.
    #[fasync::run_singlethreaded(test)]
    async fn read_from_json() {
        fuchsia_syslog::init().unwrap();
        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(std::vec![]), false));

        let eager_drivers = HashSet::new();
        let disabled_drivers = HashSet::new();

        // Run two tasks: the fake resolver and the task that loads the base drivers.
        let load_base_drivers_task =
            load_base_drivers(index.clone(), &resolver, &eager_drivers, &disabled_drivers).fuse();
        let resolver_task = run_resolver_server(resolver_stream).fuse();
        futures::pin_mut!(load_base_drivers_task, resolver_task);
        futures::select! {
            result = load_base_drivers_task => {
                result.unwrap();
            },
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
        };

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            // Check the value from the 'test-bind' binary. This should match my-driver.cm
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(1),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_url =
                "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind-component.cm"
                    .to_string();
            let expected_driver_url =
                "fuchsia-pkg://fuchsia.com/driver-index-unittests#driver/fake-driver.so"
                    .to_string();
            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                expected_url,
                expected_driver_url,
                true,
                vec![create_default_device_category()],
                DriverPackageType::Base,
                false,
            ));

            assert_eq!(expected_result, result);

            // Check the value from the 'test-bind2' binary. This should match my-driver2.cm
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_url =
                "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind2-component.cm"
                    .to_string();
            let expected_driver_url =
                "fuchsia-pkg://fuchsia.com/driver-index-unittests#driver/fake-driver2.so"
                    .to_string();
            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                expected_url,
                expected_driver_url,
                false,
                vec![create_default_device_category()],
                DriverPackageType::Base,
                false,
            ));
            assert_eq!(expected_result, result);

            // Check an unknown value. This should return the NOT_FOUND error.
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(3),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap();
            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_bind_string() {
        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("my-key".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("test-value".to_string()),
                },
            }],
            symbol_table: HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        // Make our driver.
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("my-key".to_string()),
                value: fdf::NodePropertyValue::StringValue("test-value".to_string()),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_result = vec![fdi::MatchedDriver::Driver(fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                device_categories: Some(vec![]),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                ..fdi::MatchedDriverInfo::EMPTY
            })];

            assert_eq!(expected_result, result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_bind_enum() {
        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("my-key".to_string(), ValueType::Enum),
                    rhs: Symbol::EnumValue("test-value".to_string()),
                },
            }],
            symbol_table: HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        // Make our driver.
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("my-key".to_string()),
                value: fdf::NodePropertyValue::EnumValue("test-value".to_string()),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_result = vec![fdi::MatchedDriver::Driver(fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                device_categories: Some(vec![]),
                ..fdi::MatchedDriverInfo::EMPTY
            })];

            assert_eq!(expected_result, result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_drivers_v1() {
        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        // Make two drivers that will bind to anything.
        let base_repo = BaseRepo::Resolved(std::vec![
            ResolvedDriver {
                component_url: url::Url::parse(
                    "fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm"
                )
                .unwrap(),
                v1_driver_path: Some("meta/my-driver.so".to_string()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Base,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse(
                    "fuchsia-pkg://fuchsia.com/package#driver/my-driver2.cm"
                )
                .unwrap(),
                v1_driver_path: Some("meta/my-driver2.so".to_string()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Base,
                package_hash: None,
            }
        ]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_url_1 =
                "fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string();
            let expected_driver_url_1 =
                "fuchsia-pkg://fuchsia.com/package#meta/my-driver.so".to_string();
            let expected_url_2 =
                "fuchsia-pkg://fuchsia.com/package#driver/my-driver2.cm".to_string();
            let expected_driver_url_2 =
                "fuchsia-pkg://fuchsia.com/package#meta/my-driver2.so".to_string();

            let expected_result = vec![
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    expected_url_1,
                    expected_driver_url_1,
                    false,
                    vec![],
                    DriverPackageType::Base,
                    false,
                )),
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    expected_url_2,
                    expected_driver_url_2,
                    false,
                    vec![],
                    DriverPackageType::Base,
                    false,
                )),
            ];

            assert_eq!(expected_result, result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_driver_multiple_non_fallbacks() {
        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        let boot_repo = vec![
            ResolvedDriver {
                component_url: url::Url::parse("fuchsia-boot:///#meta/driver-1.cm").unwrap(),
                v1_driver_path: Some("fuchsia-boot:///#driver/driver-1.so".to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse("fuchsia-boot:///#meta/driver-2.cm").unwrap(),
                v1_driver_path: Some("fuchsia-boot:///#driver/driver-2.so".to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
        ];

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(boot_repo, BaseRepo::Resolved(std::vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap();

            assert_eq!(result, Err(Status::NOT_SUPPORTED.into_raw()));
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_driver_non_fallback_boot_priority() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";
        const NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/non-fallback-base.cm";
        const NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/non-fallback-base.so";

        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        let boot_repo = vec![
            ResolvedDriver {
                component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: true,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse(NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
        ];

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(boot_repo, BaseRepo::Resolved(std::vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                format!(
                    "fuchsia-pkg://fuchsia.com/package#{}",
                    NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH
                ),
                false,
                vec![],
                DriverPackageType::Boot,
                false,
            ));

            // The non-fallback boot driver should be returned and not the
            // fallback boot driver.
            assert_eq!(result, expected_result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_driver_non_fallback_base_priority() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";
        const NON_FALLBACK_BASE_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/non-fallback-base.cm";
        const NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH: &str = "meta/non-fallback-base.so";
        const FALLBACK_BASE_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/fallback-base.cm";
        const FALLBACK_BASE_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-base.so";

        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
            v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];

        let base_repo = BaseRepo::Resolved(std::vec![
            ResolvedDriver {
                component_url: url::Url::parse(FALLBACK_BASE_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(FALLBACK_BASE_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: true,
                package_type: DriverPackageType::Base,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse(NON_FALLBACK_BASE_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Base,
                package_hash: None,
            },
        ]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(boot_repo, base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                NON_FALLBACK_BASE_DRIVER_COMPONENT_URL.to_owned(),
                format!(
                    "fuchsia-pkg://fuchsia.com/package#{}",
                    NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH
                ),
                false,
                vec![],
                DriverPackageType::Base,
                false,
            ));

            // The non-fallback base driver should be returned and not the
            // fallback boot driver even though boot drivers get priority
            // because non-fallback drivers get even higher priority.
            assert_eq!(result, expected_result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_drivers_v1_non_fallback_boot_priority() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str = "fuchsia-boot:///#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";
        const NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str =
            "fuchsia-boot:///#driver/non-fallback-boot.cm";
        const NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/non-fallback-boot.so";

        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        let boot_repo = vec![
            ResolvedDriver {
                component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: true,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse(NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Boot,
                package_hash: None,
            },
        ];

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(boot_repo, BaseRepo::Resolved(std::vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_result = vec![
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    NON_FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                    format!("fuchsia-boot:///#{}", NON_FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH),
                    false,
                    vec![],
                    DriverPackageType::Boot,
                    false,
                )),
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                    format!("fuchsia-boot:///#{}", FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH),
                    false,
                    vec![],
                    DriverPackageType::Boot,
                    true,
                )),
            ];

            // The non-fallback boot driver should be returned and not the
            // fallback boot driver.
            assert_eq!(result, expected_result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_drivers_v1_non_fallback_base_priority() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str = "fuchsia-boot:///#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";
        const NON_FALLBACK_BASE_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/non-fallback-base.cm";
        const NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH: &str = "meta/non-fallback-base.so";
        const FALLBACK_BASE_DRIVER_COMPONENT_URL: &str =
            "fuchsia-pkg://fuchsia.com/package#driver/fallback-base.cm";
        const FALLBACK_BASE_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-base.so";

        // Make the bind instructions.
        let always_match = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match).unwrap(),
        )
        .unwrap();

        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
            v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];

        let base_repo = BaseRepo::Resolved(std::vec![
            ResolvedDriver {
                component_url: url::Url::parse(FALLBACK_BASE_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(FALLBACK_BASE_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: true,
                package_type: DriverPackageType::Base,
                package_hash: None,
            },
            ResolvedDriver {
                component_url: url::Url::parse(NON_FALLBACK_BASE_DRIVER_COMPONENT_URL).unwrap(),
                v1_driver_path: Some(NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH.to_owned()),
                bind_rules: always_match.clone(),
                bind_bytecode: vec![],
                colocate: false,
                device_categories: vec![],
                fallback: false,
                package_type: DriverPackageType::Base,
                package_hash: None,
            },
        ]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(boot_repo, base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_result = vec![
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    NON_FALLBACK_BASE_DRIVER_COMPONENT_URL.to_owned(),
                    format!(
                        "fuchsia-pkg://fuchsia.com/package#{}",
                        NON_FALLBACK_BASE_DRIVER_V1_DRIVER_PATH
                    ),
                    false,
                    vec![],
                    DriverPackageType::Base,
                    false,
                )),
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                    format!("fuchsia-boot:///#{}", FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH),
                    false,
                    vec![],
                    DriverPackageType::Boot,
                    true,
                )),
                fdi::MatchedDriver::Driver(create_matched_driver_info(
                    FALLBACK_BASE_DRIVER_COMPONENT_URL.to_owned(),
                    format!(
                        "fuchsia-pkg://fuchsia.com/package#{}",
                        FALLBACK_BASE_DRIVER_V1_DRIVER_PATH
                    ),
                    false,
                    vec![],
                    DriverPackageType::Base,
                    true,
                )),
            ];

            // The non-fallback base driver should be returned and not the
            // fallback boot driver even though boot drivers get priority
            // because non-fallback drivers get even higher priority.
            assert_eq!(result, expected_result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_load_fallback_driver() {
        const DRIVER_URL: &str = "fuchsia-boot:///#meta/test-fallback-component.cm";
        let driver_url = url::Url::parse(&DRIVER_URL).unwrap();
        let pkg = fuchsia_fs::directory::open_in_namespace("/pkg", fio::OpenFlags::RIGHT_READABLE)
            .context("Failed to open /pkg")
            .unwrap();
        let fallback_driver = load_driver(&pkg, driver_url, DriverPackageType::Boot, None)
            .await
            .unwrap()
            .expect("Fallback driver was not loaded");
        assert!(fallback_driver.fallback);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_load_eager_fallback_boot_driver() {
        let eager_driver_component_url =
            url::Url::parse("fuchsia-boot:///#meta/test-fallback-component.cm").unwrap();

        let boot = fuchsia_fs::directory::open_in_namespace("/pkg", fio::OpenFlags::RIGHT_READABLE)
            .unwrap();
        let drivers = load_boot_drivers(
            boot,
            &HashSet::from([eager_driver_component_url.clone()]),
            &HashSet::new(),
        )
        .await
        .unwrap();
        assert!(
            !drivers
                .iter()
                .find(|driver| driver.component_url == eager_driver_component_url)
                .expect("Fallback driver did not load")
                .fallback
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_load_eager_fallback_base_driver() {
        let eager_driver_component_url = url::Url::parse(
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-fallback-component.cm",
        )
        .unwrap();

        fuchsia_syslog::init().unwrap();
        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(std::vec![]), false));

        let eager_drivers = HashSet::from([eager_driver_component_url.clone()]);
        let disabled_drivers = HashSet::new();

        let load_base_drivers_task =
            load_base_drivers(Rc::clone(&index), &resolver, &eager_drivers, &disabled_drivers)
                .fuse();
        let resolver_task = run_resolver_server(resolver_stream).fuse();
        futures::pin_mut!(load_base_drivers_task, resolver_task);
        futures::select! {
            result = load_base_drivers_task => {
                result.unwrap();
            },
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
        };

        let base_repo = index.base_repo.borrow();
        match *base_repo {
            BaseRepo::Resolved(ref drivers) => {
                assert!(
                    !drivers
                        .iter()
                        .find(|driver| driver.component_url == eager_driver_component_url)
                        .expect("Fallback driver did not load")
                        .fallback
                );
            }
            _ => {
                panic!("Base repo was not resolved");
            }
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_dont_load_disabled_fallback_boot_driver() {
        let disabled_driver_component_url =
            url::Url::parse("fuchsia-boot:///#meta/test-fallback-component.cm").unwrap();

        let boot = fuchsia_fs::directory::open_in_namespace("/pkg", fio::OpenFlags::RIGHT_READABLE)
            .unwrap();
        let drivers = load_boot_drivers(
            boot,
            &HashSet::new(),
            &HashSet::from([disabled_driver_component_url.clone()]),
        )
        .await
        .unwrap();
        assert!(drivers
            .iter()
            .find(|driver| driver.component_url == disabled_driver_component_url)
            .is_none());
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_dont_load_disabled_fallback_base_driver() {
        let disabled_driver_component_url = url::Url::parse(
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-fallback-component.cm",
        )
        .unwrap();

        fuchsia_syslog::init().unwrap();
        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(std::vec![]), false));

        let eager_drivers = HashSet::new();
        let disabled_drivers = HashSet::from([disabled_driver_component_url.clone()]);

        let load_base_drivers_task =
            load_base_drivers(Rc::clone(&index), &resolver, &eager_drivers, &disabled_drivers)
                .fuse();
        let resolver_task = run_resolver_server(resolver_stream).fuse();
        futures::pin_mut!(load_base_drivers_task, resolver_task);
        futures::select! {
            result = load_base_drivers_task => {
                result.unwrap();
            },
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
        };

        let base_repo = index.base_repo.borrow();
        match *base_repo {
            BaseRepo::Resolved(ref drivers) => {
                assert!(drivers
                    .iter()
                    .find(|driver| driver.component_url == disabled_driver_component_url)
                    .is_none());
            }
            _ => {
                panic!("Base repo was not resolved");
            }
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_driver_when_require_system_true_and_base_repo_not_resolved() {
        let always_match = create_always_match_bind_rules();
        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-boot:///#driver/fallback-boot.cm").unwrap(),
            v1_driver_path: Some("meta/fallback-boot.so".to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];
        let index = Indexer::new(boot_repo, BaseRepo::NotResolved(vec![]), true);
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        execute_driver_index_test(index, stream, async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap();

            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_driver_when_require_system_false_and_base_repo_not_resolved() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str = "fuchsia-boot:///#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";

        let always_match = create_always_match_bind_rules();
        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
            v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];
        let index = Indexer::new(boot_repo, BaseRepo::NotResolved(vec![]), false);
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        execute_driver_index_test(index, stream, async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                format!("fuchsia-boot:///#{}", FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH),
                false,
                vec![],
                DriverPackageType::Boot,
                true,
            ));
            assert_eq!(result, expected_result);
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_drivers_v1_when_require_system_true_and_base_repo_not_resolved() {
        let always_match = create_always_match_bind_rules();
        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-boot:///#driver/fallback-boot.cm").unwrap(),
            v1_driver_path: Some("meta/fallback-boot.so".to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];
        let index = Indexer::new(boot_repo, BaseRepo::NotResolved(vec![]), true);
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        execute_driver_index_test(index, stream, async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_drivers_v1(args).await.unwrap();

            assert_eq!(result, Ok(vec![]));
        })
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_match_drivers_v1_when_require_system_false_and_base_repo_not_resolved() {
        const FALLBACK_BOOT_DRIVER_COMPONENT_URL: &str = "fuchsia-boot:///#driver/fallback-boot.cm";
        const FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH: &str = "meta/fallback-boot.so";

        let always_match = create_always_match_bind_rules();
        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse(FALLBACK_BOOT_DRIVER_COMPONENT_URL).unwrap(),
            v1_driver_path: Some(FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH.to_owned()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: true,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];
        let index = Indexer::new(boot_repo, BaseRepo::NotResolved(vec![]), false);
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        execute_driver_index_test(index, stream, async move {
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_drivers_v1(args).await.unwrap().unwrap();

            let expected_result = vec![fdi::MatchedDriver::Driver(create_matched_driver_info(
                FALLBACK_BOOT_DRIVER_COMPONENT_URL.to_owned(),
                format!("fuchsia-boot:///#{}", FALLBACK_BOOT_DRIVER_V1_DRIVER_PATH),
                false,
                vec![],
                DriverPackageType::Boot,
                true,
            ))];
            assert_eq!(result, expected_result);
        })
        .await;
    }

    // This test relies on two drivers existing in the /pkg/ directory of the
    // test package.
    #[fasync::run_singlethreaded(test)]
    async fn test_boot_drivers() {
        fuchsia_syslog::init().unwrap();
        let boot = fuchsia_fs::directory::open_in_namespace("/pkg", fio::OpenFlags::RIGHT_READABLE)
            .unwrap();
        let drivers = load_boot_drivers(boot, &HashSet::new(), &HashSet::new()).await.unwrap();

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(drivers, BaseRepo::NotResolved(vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            // Check the value from the 'test-bind' binary. This should match my-driver.cm
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(1),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_url = "fuchsia-boot:///#meta/test-bind-component.cm".to_string();
            let expected_driver_url = "fuchsia-boot:///#driver/fake-driver.so".to_string();
            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                expected_url,
                expected_driver_url,
                true,
                vec![create_default_device_category()],
                DriverPackageType::Boot,
                false,
            ));
            assert_eq!(expected_result, result);

            // Check the value from the 'test-bind2' binary. This should match my-driver2.cm
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(2),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_url = "fuchsia-boot:///#meta/test-bind2-component.cm".to_string();
            let expected_driver_url = "fuchsia-boot:///#driver/fake-driver2.so".to_string();
            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                expected_url,
                expected_driver_url,
                false,
                vec![create_default_device_category()],
                DriverPackageType::Boot,
                false,
            ));
            assert_eq!(expected_result, result);

            // Check an unknown value. This should return the NOT_FOUND error.
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(3),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };
            let result = proxy.match_driver(args).await.unwrap();
            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_bind_composite() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: rules,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            // Match primary node.
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_driver_info = fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                device_categories: Some(vec![]),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                ..fdi::MatchedDriverInfo::EMPTY
            };
            let expected_result = fdi::MatchedDriver::CompositeDriver(fdi::MatchedCompositeInfo {
                node_index: Some(0),
                num_nodes: Some(2),
                composite_name: Some("mimid".to_string()),
                node_names: Some(vec!["catbird".to_string(), "mockingbird".to_string()]),
                driver_info: Some(expected_driver_info),
                ..fdi::MatchedCompositeInfo::EMPTY
            });
            assert_eq!(expected_result, result);

            // Match secondary node.
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![
                    fdf::NodeProperty {
                        key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                        value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                    },
                    fdf::NodeProperty {
                        key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                        value: fdf::NodePropertyValue::IntValue(1),
                    },
                ]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_driver_info = fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                device_categories: Some(vec![]),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                ..fdi::MatchedDriverInfo::EMPTY
            };
            let expected_result = fdi::MatchedDriver::CompositeDriver(fdi::MatchedCompositeInfo {
                node_index: Some(1),
                num_nodes: Some(2),
                composite_name: Some("mimid".to_string()),
                node_names: Some(vec!["catbird".to_string(), "mockingbird".to_string()]),
                driver_info: Some(expected_driver_info),
                ..fdi::MatchedCompositeInfo::EMPTY
            });
            assert_eq!(expected_result, result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_bind_composite_optional() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let optional_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("trembler".to_string()),
            },
        }];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![CompositeNode {
                name: "lapwing".to_string(),
                instructions: optional_node_inst,
            }],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: rules,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            // Match primary node.
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_driver_info = fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                device_categories: Some(vec![]),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                ..fdi::MatchedDriverInfo::EMPTY
            };
            let expected_result = fdi::MatchedDriver::CompositeDriver(fdi::MatchedCompositeInfo {
                node_index: Some(0),
                num_nodes: Some(2),
                composite_name: Some("mimid".to_string()),
                node_names: Some(vec!["catbird".to_string(), "mockingbird".to_string()]),
                driver_info: Some(expected_driver_info),
                ..fdi::MatchedCompositeInfo::EMPTY
            });
            assert_eq!(expected_result, result);

            // Match secondary node.
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![
                    fdf::NodeProperty {
                        key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                        value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                    },
                    fdf::NodeProperty {
                        key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                        value: fdf::NodePropertyValue::IntValue(1),
                    },
                ]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap().unwrap();

            let expected_driver_info = fdi::MatchedDriverInfo {
                url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                colocate: Some(false),
                device_categories: Some(vec![]),
                package_type: Some(fdi::DriverPackageType::Base),
                is_fallback: Some(false),
                ..fdi::MatchedDriverInfo::EMPTY
            };
            let expected_result = fdi::MatchedDriver::CompositeDriver(fdi::MatchedCompositeInfo {
                node_index: Some(1),
                num_nodes: Some(2),
                composite_name: Some("mimid".to_string()),
                node_names: Some(vec!["catbird".to_string(), "mockingbird".to_string()]),
                driver_info: Some(expected_driver_info),
                ..fdi::MatchedCompositeInfo::EMPTY
            });
            assert_eq!(expected_result, result);

            // The optional node should not match.
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                value: fdf::NodePropertyValue::StringValue("trembler".to_string()),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(args).await.unwrap();
            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_node_representation_match() {
        let base_repo = BaseRepo::Resolved(std::vec![]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));
        let index_task = run_index_server(index.clone(), stream).fuse();

        let test_task = async move {
            let bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let properties = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![fdf::NodeRepresentation {
                            bind_rules: bind_rules,
                            properties: properties,
                        }]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let device_properties_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("plover".to_string()),
                },
            ];
            let match_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_match),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(match_args).await.unwrap().unwrap();
            assert_eq!(
                fdi::MatchedDriver::NodeRepresentation(fdi::MatchedNodeRepresentationInfo {
                    node_groups: Some(vec![fdi::MatchedNodeGroupInfo {
                        name: Some("test_group".to_string()),
                        node_index: Some(0),
                        num_nodes: Some(1),
                        ..fdi::MatchedNodeGroupInfo::EMPTY
                    }]),
                    ..fdi::MatchedNodeRepresentationInfo::EMPTY
                }),
                result
            );

            let device_properties_mismatch = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("dotterel".to_string()),
                },
            ];
            let mismatch_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_mismatch),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_driver(mismatch_args).await.unwrap();
            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_node_representation_match_v1() {
        let always_match_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match_rules).unwrap(),
        )
        .unwrap();

        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: always_match,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));
        let index_task = run_index_server(index.clone(), stream).fuse();

        let test_task = async move {
            let bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let properties = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![fdf::NodeRepresentation {
                            bind_rules: bind_rules,
                            properties: properties,
                        }]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let device_properties_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("plover".to_string()),
                },
            ];
            let match_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_match),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(match_args).await.unwrap().unwrap();
            assert_eq!(2, result.len());

            assert_eq!(
                vec![
                    fdi::MatchedDriver::NodeRepresentation(fdi::MatchedNodeRepresentationInfo {
                        node_groups: Some(vec![fdi::MatchedNodeGroupInfo {
                            name: Some("test_group".to_string()),
                            node_index: Some(0),
                            num_nodes: Some(1),
                            ..fdi::MatchedNodeGroupInfo::EMPTY
                        }]),
                        ..fdi::MatchedNodeRepresentationInfo::EMPTY
                    }),
                    fdi::MatchedDriver::Driver(fdi::MatchedDriverInfo {
                        url: Some(
                            "fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()
                        ),
                        colocate: Some(false),
                        device_categories: Some(vec![]),
                        package_type: Some(fdi::DriverPackageType::Base),
                        is_fallback: Some(false),
                        ..fdi::MatchedDriverInfo::EMPTY
                    })
                ],
                result
            );

            let device_properties_mismatch = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("dotterel".to_string()),
                },
            ];
            let mismatch_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_mismatch),
                ..fdi::MatchDriverArgs::EMPTY
            };

            let result = proxy.match_drivers_v1(mismatch_args).await.unwrap().unwrap();
            assert_eq!(1, result.len());
            assert_eq!(
                vec![fdi::MatchedDriver::Driver(fdi::MatchedDriverInfo {
                    url: Some("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm".to_string()),
                    colocate: Some(false),
                    device_categories: Some(vec![]),
                    package_type: Some(fdi::DriverPackageType::Base),
                    is_fallback: Some(false),
                    ..fdi::MatchedDriverInfo::EMPTY
                }),],
                result
            );
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_matched_composite() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url.clone(),
            v1_driver_path: None,
            bind_rules: rules,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            let result = proxy
                .add_node_group(fdf::NodeGroup {
                    name: Some("group_match".to_string()),
                    nodes: Some(vec![
                        fdf::NodeRepresentation {
                            bind_rules: node_1_bind_rules.clone(),
                            properties: node_1_props_match.clone(),
                        },
                        fdf::NodeRepresentation {
                            bind_rules: node_2_bind_rules.clone(),
                            properties: node_2_props_match.clone(),
                        },
                    ]),
                    ..fdf::NodeGroup::EMPTY
                })
                .await
                .unwrap()
                .unwrap();
            assert_eq!(url.to_string(), result.0.driver_info.unwrap().url.unwrap());

            let node_1_props_nonmatch = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("group_non_match_1".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_nonmatch,
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_match,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let node_2_props_nonmatch = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(1),
                value: fdf::NodePropertyValue::IntValue(10),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("group_non_match_2".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_match,
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_nonmatch,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_no_optional_matched_composite_with_optional() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let optional_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("trembler".to_string()),
            },
        }];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![CompositeNode {
                name: "lapwing".to_string(),
                instructions: optional_node_inst,
            }],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url.clone(),
            v1_driver_path: None,
            bind_rules: rules,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            let result = proxy
                .add_node_group(fdf::NodeGroup {
                    name: Some("group_match".to_string()),
                    nodes: Some(vec![
                        fdf::NodeRepresentation {
                            bind_rules: node_1_bind_rules.clone(),
                            properties: node_1_props_match.clone(),
                        },
                        fdf::NodeRepresentation {
                            bind_rules: node_2_bind_rules.clone(),
                            properties: node_2_props_match.clone(),
                        },
                    ]),
                    ..fdf::NodeGroup::EMPTY
                })
                .await
                .unwrap()
                .unwrap();
            assert_eq!(url.to_string(), result.0.driver_info.unwrap().url.unwrap());

            let node_1_props_nonmatch = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("group_non_match_1".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_nonmatch,
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_match,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let node_2_props_nonmatch = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(1),
                value: fdf::NodePropertyValue::IntValue(10),
            }];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("group_non_match_2".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_match,
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_nonmatch,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_with_optional_matched_composite_with_optional() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let optional_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("trembler".to_string()),
            },
        }];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![CompositeNode {
                name: "lapwing".to_string(),
                instructions: optional_node_inst,
            }],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();
        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url.clone(),
            v1_driver_path: None,
            bind_rules: rules,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let optional_1_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(2),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let optional_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                value: fdf::NodePropertyValue::StringValue("trembler".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            let result = proxy
                .add_node_group(fdf::NodeGroup {
                    name: Some("group_match".to_string()),
                    nodes: Some(vec![
                        fdf::NodeRepresentation {
                            bind_rules: node_1_bind_rules.clone(),
                            properties: node_1_props_match.clone(),
                        },
                        fdf::NodeRepresentation {
                            bind_rules: optional_1_bind_rules.clone(),
                            properties: optional_1_props_match.clone(),
                        },
                        fdf::NodeRepresentation {
                            bind_rules: node_2_bind_rules.clone(),
                            properties: node_2_props_match.clone(),
                        },
                    ]),
                    ..fdf::NodeGroup::EMPTY
                })
                .await
                .unwrap()
                .unwrap();
            assert_eq!(url.to_string(), result.0.driver_info.unwrap().url.unwrap());
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_then_driver() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        // Start our index out without any drivers.
        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            // When we add the node group it should get not found since there's no drivers.
            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_match.clone(),
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_match,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let device_properties_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("plover".to_string()),
                },
            ];
            let match_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_match),
                ..fdi::MatchDriverArgs::EMPTY
            };

            // We can see the node group comes back without a matched composite.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(None, info.node_groups.unwrap()[0].composite);
            } else {
                assert!(false, "Did not get back a node group.");
            }

            // Notify the node group manager of a new composite driver.
            {
                let mut node_group_manager = index.node_group_manager.borrow_mut();
                node_group_manager.new_driver_available(ResolvedDriver {
                    component_url: url.clone(),
                    v1_driver_path: None,
                    bind_rules: rules,
                    bind_bytecode: vec![],
                    colocate: false,
                    device_categories: vec![],
                    fallback: false,
                    package_type: DriverPackageType::Base,
                    package_hash: None,
                });
            }

            // Now when we get it back, it has the matching composite driver on it.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(
                    &"mimid".to_string(),
                    info.node_groups.unwrap()[0]
                        .composite
                        .as_ref()
                        .unwrap()
                        .composite_name
                        .as_ref()
                        .unwrap()
                );
            } else {
                assert!(false, "Did not get back a node group.");
            }
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_no_optional_then_driver_with_optional() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let optional_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("trembler".to_string()),
            },
        }];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![CompositeNode {
                name: "lapwing".to_string(),
                instructions: optional_node_inst,
            }],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        // Start our index out without any drivers.
        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            // When we add the node group it should get not found since there's no drivers.
            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_match.clone(),
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_match,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let device_properties_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::IntValue(1),
                    value: fdf::NodePropertyValue::IntValue(200),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    value: fdf::NodePropertyValue::StringValue("plover".to_string()),
                },
            ];
            let match_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_match),
                ..fdi::MatchDriverArgs::EMPTY
            };

            // We can see the node group comes back without a matched composite.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(None, info.node_groups.unwrap()[0].composite);
            } else {
                assert!(false, "Did not get back a node group.");
            }

            // Notify the node group manager of a new composite driver.
            {
                let mut node_group_manager = index.node_group_manager.borrow_mut();
                node_group_manager.new_driver_available(ResolvedDriver {
                    component_url: url.clone(),
                    v1_driver_path: None,
                    bind_rules: rules,
                    bind_bytecode: vec![],
                    colocate: false,
                    device_categories: vec![],
                    fallback: false,
                    package_type: DriverPackageType::Base,
                    package_hash: None,
                });
            }

            // Now when we get it back, it has the matching composite driver on it.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(
                    &"mimid".to_string(),
                    info.node_groups.unwrap()[0]
                        .composite
                        .as_ref()
                        .unwrap()
                        .composite_name
                        .as_ref()
                        .unwrap()
                );
            } else {
                assert!(false, "Did not get back a node group.");
            }
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_with_optional_then_driver_with_optional() {
        // Create the Composite Bind rules.
        let primary_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("trembler".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("thrasher".to_string()),
            },
        }];

        let additional_node_inst = vec![
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                    rhs: Symbol::StringValue("catbird".to_string()),
                },
            },
            SymbolicInstructionInfo {
                location: None,
                instruction: SymbolicInstruction::AbortIfNotEqual {
                    lhs: Symbol::Key("catbird".to_string(), ValueType::Number),
                    rhs: Symbol::NumberValue(1),
                },
            },
        ];

        let optional_node_inst = vec![SymbolicInstructionInfo {
            location: None,
            instruction: SymbolicInstruction::AbortIfNotEqual {
                lhs: Symbol::Key("thrasher".to_string(), ValueType::Str),
                rhs: Symbol::StringValue("trembler".to_string()),
            },
        }];

        let bind_rules = CompositeBindRules {
            device_name: "mimid".to_string(),
            symbol_table: HashMap::new(),
            primary_node: CompositeNode {
                name: "catbird".to_string(),
                instructions: primary_node_inst,
            },
            additional_nodes: vec![CompositeNode {
                name: "mockingbird".to_string(),
                instructions: additional_node_inst,
            }],
            optional_nodes: vec![CompositeNode {
                name: "lapwing".to_string(),
                instructions: optional_node_inst,
            }],
            enable_debug: false,
        };

        let bytecode = CompiledBindRules::CompositeBind(bind_rules).encode_to_bytecode().unwrap();
        let rules = DecodedRules::new(bytecode).unwrap();

        // Make the composite driver.
        let url =
            url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/dg_matched_composite.cm")
                .unwrap();

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        // Start our index out without any drivers.
        let index = Rc::new(Indexer::new(std::vec![], BaseRepo::NotResolved(vec![]), false));

        let index_task = run_index_server(index.clone(), stream).fuse();
        let test_task = async move {
            let node_1_bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let node_2_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(1),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let node_2_props_match = vec![
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("catbird".to_string()),
                    value: fdf::NodePropertyValue::IntValue(1),
                },
                fdf::NodeProperty {
                    key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                    value: fdf::NodePropertyValue::StringValue("catbird".to_string()),
                },
            ];

            let optional_1_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(2),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(10)],
            }];

            let optional_1_props_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("thrasher".to_string()),
                value: fdf::NodePropertyValue::StringValue("trembler".to_string()),
            }];

            // When we add the node group it should get not found since there's no drivers.
            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![
                            fdf::NodeRepresentation {
                                bind_rules: node_1_bind_rules.clone(),
                                properties: node_1_props_match.clone(),
                            },
                            fdf::NodeRepresentation {
                                bind_rules: node_2_bind_rules.clone(),
                                properties: node_2_props_match,
                            },
                            fdf::NodeRepresentation {
                                bind_rules: optional_1_bind_rules.clone(),
                                properties: optional_1_props_match,
                            },
                        ]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let device_properties_match = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(2),
                value: fdf::NodePropertyValue::IntValue(10),
            }];
            let match_args = fdi::MatchDriverArgs {
                properties: Some(device_properties_match),
                ..fdi::MatchDriverArgs::EMPTY
            };

            // We can see the node group comes back without a matched composite.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(None, info.node_groups.unwrap()[0].composite);
            } else {
                assert!(false, "Did not get back a node group.");
            }

            // Notify the node group manager of a new composite driver.
            {
                let mut node_group_manager = index.node_group_manager.borrow_mut();
                node_group_manager.new_driver_available(ResolvedDriver {
                    component_url: url.clone(),
                    v1_driver_path: None,
                    bind_rules: rules,
                    bind_bytecode: vec![],
                    colocate: false,
                    device_categories: vec![],
                    fallback: false,
                    package_type: DriverPackageType::Base,
                    package_hash: None,
                });
            }

            // Now when we get it back, it has the matching composite driver on it.
            let match_result = proxy.match_driver(match_args.clone()).await.unwrap().unwrap();
            if let fdi::MatchedDriver::NodeRepresentation(info) = match_result {
                assert_eq!(
                    &"mimid".to_string(),
                    info.node_groups.unwrap()[0]
                        .composite
                        .as_ref()
                        .unwrap()
                        .composite_name
                        .as_ref()
                        .unwrap()
                );
            } else {
                assert!(false, "Did not get back a node group.");
            }
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_duplicate_path() {
        let always_match_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match_rules).unwrap(),
        )
        .unwrap();

        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: always_match,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));
        let index_task = run_index_server(index.clone(), stream).fuse();

        let test_task = async move {
            let bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(1),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::StringValue("lapwing".to_string()),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            assert_eq!(
                Err(Status::NOT_FOUND.into_raw()),
                proxy
                    .add_node_group(fdf::NodeGroup {
                        name: Some("test_group".to_string()),
                        nodes: Some(vec![fdf::NodeRepresentation {
                            bind_rules: bind_rules,
                            properties: vec![fdf::NodeProperty {
                                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
                            }]
                        }]),
                        ..fdf::NodeGroup::EMPTY
                    })
                    .await
                    .unwrap()
            );

            let duplicate_bind_rules = vec![fdf::BindRule {
                key: fdf::NodePropertyKey::IntValue(200),
                condition: fdf::Condition::Accept,
                values: vec![fdf::NodePropertyValue::IntValue(2)],
            }];

            let node_transform = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let result = proxy
                .add_node_group(fdf::NodeGroup {
                    name: Some("test_group".to_string()),
                    nodes: Some(vec![fdf::NodeRepresentation {
                        bind_rules: duplicate_bind_rules,
                        properties: node_transform,
                    }]),
                    ..fdf::NodeGroup::EMPTY
                })
                .await
                .unwrap();
            assert_eq!(Err(Status::ALREADY_EXISTS.into_raw()), result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_add_node_group_duplicate_key() {
        let always_match_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match_rules).unwrap(),
        )
        .unwrap();

        let base_repo = BaseRepo::Resolved(std::vec![ResolvedDriver {
            component_url: url::Url::parse("fuchsia-pkg://fuchsia.com/package#driver/my-driver.cm")
                .unwrap(),
            v1_driver_path: None,
            bind_rules: always_match,
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        },]);

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));
        let index_task = run_index_server(index.clone(), stream).fuse();

        let test_task = async move {
            let bind_rules = vec![
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(20),
                    condition: fdf::Condition::Accept,
                    values: vec![
                        fdf::NodePropertyValue::IntValue(200),
                        fdf::NodePropertyValue::IntValue(150),
                    ],
                },
                fdf::BindRule {
                    key: fdf::NodePropertyKey::IntValue(20),
                    condition: fdf::Condition::Accept,
                    values: vec![fdf::NodePropertyValue::StringValue("plover".to_string())],
                },
            ];

            let node_transform = vec![fdf::NodeProperty {
                key: fdf::NodePropertyKey::StringValue("trembler".to_string()),
                value: fdf::NodePropertyValue::StringValue("thrasher".to_string()),
            }];

            let result = proxy
                .add_node_group(fdf::NodeGroup {
                    name: Some("test_group".to_string()),
                    nodes: Some(vec![fdf::NodeRepresentation {
                        bind_rules: bind_rules,
                        properties: node_transform,
                    }]),
                    ..fdf::NodeGroup::EMPTY
                })
                .await
                .unwrap();
            assert_eq!(Err(Status::INVALID_ARGS.into_raw()), result);
        }
        .fuse();

        futures::pin_mut!(index_task, test_task);
        futures::select! {
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_register_and_match_ephemeral_driver() {
        fuchsia_syslog::init().unwrap();

        let component_manifest_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind-component.cm";
        let driver_library_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#driver/fake-driver.so";

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fdi::DriverIndexMarker>().unwrap();

        let (registrar_proxy, registrar_stream) =
            fidl::endpoints::create_proxy_and_stream::<fdr::DriverRegistrarMarker>().unwrap();

        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let universe_resolver = Some(resolver);

        let base_repo = BaseRepo::Resolved(std::vec![]);
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let resolver_task = run_resolver_server(resolver_stream).fuse();
        let index_task = run_index_server(index.clone(), stream).fuse();
        let registrar_task =
            run_driver_registrar_server(index.clone(), registrar_stream, &universe_resolver).fuse();

        let test_task = async {
            // Short delay since the resolver server is starting up at the same time.
            fasync::Timer::new(std::time::Duration::from_millis(100)).await;

            // These properties match the bind rules for the "test-bind-componenet.cm".
            let property = fdf::NodeProperty {
                key: fdf::NodePropertyKey::IntValue(bind::ddk_bind_constants::BIND_PROTOCOL),
                value: fdf::NodePropertyValue::IntValue(1),
            };
            let args = fdi::MatchDriverArgs {
                properties: Some(vec![property]),
                ..fdi::MatchDriverArgs::EMPTY
            };

            // First attempt should fail since we haven't registered it.
            let result = proxy.match_driver(args.clone()).await.unwrap();
            assert_eq!(result, Err(Status::NOT_FOUND.into_raw()));
            let result = proxy.match_drivers_v1(args.clone()).await.unwrap().unwrap();
            assert_eq!(result.len(), 0);

            // Now register the ephemeral driver.
            let mut pkg_url =
                fidl_fuchsia_pkg::PackageUrl { url: component_manifest_url.to_string() };
            registrar_proxy.register(&mut pkg_url).await.unwrap().unwrap();

            // Match succeeds now.
            let result = proxy.match_driver(args.clone()).await.unwrap().unwrap();
            let result_v1 = proxy.match_drivers_v1(args.clone()).await.unwrap().unwrap();
            let expected_url = component_manifest_url.to_string();
            let expected_driver_url = driver_library_url.to_string();
            let expected_result = fdi::MatchedDriver::Driver(create_matched_driver_info(
                expected_url,
                expected_driver_url,
                true,
                vec![create_default_device_category()],
                DriverPackageType::Universe,
                false,
            ));
            assert_eq!(expected_result, result);
            assert_eq!(result_v1.len(), 1);
            assert_eq!(expected_result, result_v1[0]);
        }
        .fuse();

        futures::pin_mut!(resolver_task, index_task, registrar_task, test_task);
        futures::select! {
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
            result = index_task => {
                panic!("Index task finished: {:?}", result);
            },
            result = registrar_task => {
                panic!("Registrar task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_register_and_get_ephemeral_driver() {
        fuchsia_syslog::init().unwrap();

        let component_manifest_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind-component.cm";
        let driver_library_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#driver/fake-driver.so";

        let (registrar_proxy, registrar_stream) =
            fidl::endpoints::create_proxy_and_stream::<fdr::DriverRegistrarMarker>().unwrap();

        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let (development_proxy, development_stream) =
            fidl::endpoints::create_proxy_and_stream::<fdd::DriverIndexMarker>().unwrap();

        let universe_resolver = Some(resolver);

        let base_repo = BaseRepo::Resolved(std::vec![]);
        let index = Rc::new(Indexer::new(std::vec![], base_repo, false));

        let resolver_task = run_resolver_server(resolver_stream).fuse();
        let development_task =
            run_driver_development_server(index.clone(), development_stream).fuse();
        let registrar_task =
            run_driver_registrar_server(index.clone(), registrar_stream, &universe_resolver).fuse();
        let test_task = async move {
            // We should not find this before registering it.
            let driver_infos =
                get_driver_info_proxy(&development_proxy, &[component_manifest_url]).await;
            assert_eq!(0, driver_infos.len());

            // Short delay since the resolver server starts at the same time.
            fasync::Timer::new(std::time::Duration::from_millis(100)).await;

            // Register the ephemeral driver.
            let mut pkg_url =
                fidl_fuchsia_pkg::PackageUrl { url: component_manifest_url.to_string() };
            registrar_proxy.register(&mut pkg_url).await.unwrap().unwrap();

            // Now that it's registered we should find it.
            let driver_infos =
                get_driver_info_proxy(&development_proxy, &[component_manifest_url]).await;
            assert_eq!(1, driver_infos.len());
            assert_eq!(&driver_library_url.to_string(), driver_infos[0].libname.as_ref().unwrap());
        }
        .fuse();

        futures::pin_mut!(resolver_task, development_task, registrar_task, test_task);
        futures::select! {
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
            result = development_task => {
                panic!("Development task finished: {:?}", result);
            },
            result = registrar_task => {
                panic!("Registrar task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_register_exists_in_base() {
        let always_match_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match_rules).unwrap(),
        )
        .unwrap();

        let component_manifest_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind-component.cm";

        let (registrar_proxy, registrar_stream) =
            fidl::endpoints::create_proxy_and_stream::<fdr::DriverRegistrarMarker>().unwrap();

        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let universe_resolver = Some(resolver);

        let boot_repo = std::vec![];
        let base_repo = BaseRepo::Resolved(vec![ResolvedDriver {
            component_url: url::Url::parse(component_manifest_url).unwrap(),
            v1_driver_path: Some("meta/fake-driver.so".to_string()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Base,
            package_hash: None,
        }]);

        let index = Rc::new(Indexer::new(boot_repo, base_repo, false));

        let resolver_task = run_resolver_server(resolver_stream).fuse();
        let registrar_task =
            run_driver_registrar_server(index.clone(), registrar_stream, &universe_resolver).fuse();
        let test_task = async move {
            // Try to register the driver.
            let mut pkg_url =
                fidl_fuchsia_pkg::PackageUrl { url: component_manifest_url.to_string() };
            let register_result = registrar_proxy.register(&mut pkg_url).await.unwrap();

            // The register should have failed.
            assert_eq!(fuchsia_zircon::sys::ZX_ERR_ALREADY_EXISTS, register_result.err().unwrap());
        }
        .fuse();

        futures::pin_mut!(resolver_task, registrar_task, test_task);
        futures::select! {
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
            result = registrar_task => {
                panic!("Registrar task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_register_exists_in_boot() {
        let always_match_rules = bind::compiler::BindRules {
            instructions: vec![],
            symbol_table: std::collections::HashMap::new(),
            use_new_bytecode: true,
            enable_debug: false,
        };
        let always_match = DecodedRules::new(
            bind::bytecode_encoder::encode_v2::encode_to_bytecode_v2(always_match_rules).unwrap(),
        )
        .unwrap();

        let component_manifest_url =
            "fuchsia-pkg://fuchsia.com/driver-index-unittests#meta/test-bind-component.cm";

        let (registrar_proxy, registrar_stream) =
            fidl::endpoints::create_proxy_and_stream::<fdr::DriverRegistrarMarker>().unwrap();

        let (resolver, resolver_stream) =
            fidl::endpoints::create_proxy_and_stream::<fidl_fuchsia_pkg::PackageResolverMarker>()
                .unwrap();

        let universe_resolver = Some(resolver);

        let boot_repo = vec![ResolvedDriver {
            component_url: url::Url::parse(component_manifest_url).unwrap(),
            v1_driver_path: Some("meta/fake-driver.so".to_string()),
            bind_rules: always_match.clone(),
            bind_bytecode: vec![],
            colocate: false,
            device_categories: vec![],
            fallback: false,
            package_type: DriverPackageType::Boot,
            package_hash: None,
        }];

        let base_repo = BaseRepo::Resolved(std::vec![]);
        let index = Rc::new(Indexer::new(boot_repo, base_repo, false));

        let resolver_task = run_resolver_server(resolver_stream).fuse();
        let registrar_task =
            run_driver_registrar_server(index.clone(), registrar_stream, &universe_resolver).fuse();
        let test_task = async move {
            // Try to register the driver.
            let mut pkg_url =
                fidl_fuchsia_pkg::PackageUrl { url: component_manifest_url.to_string() };
            let register_result = registrar_proxy.register(&mut pkg_url).await.unwrap();

            // The register should have failed.
            assert_eq!(fuchsia_zircon::sys::ZX_ERR_ALREADY_EXISTS, register_result.err().unwrap());
        }
        .fuse();

        futures::pin_mut!(resolver_task, registrar_task, test_task);
        futures::select! {
            result = resolver_task => {
                panic!("Resolver task finished: {:?}", result);
            },
            result = registrar_task => {
                panic!("Registrar task finished: {:?}", result);
            },
            () = test_task => {},
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_device_categories_from_component_data() {
        assert_eq!(
            resolved_driver::get_device_categories_from_component_data(&vec![
                fdata::Dictionary {
                    entries: Some(vec![fdata::DictionaryEntry {
                        key: "category".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str("usb".to_string())))
                    }]),
                    ..fdata::Dictionary::EMPTY
                },
                fdata::Dictionary {
                    entries: Some(vec![
                        fdata::DictionaryEntry {
                            key: "category".to_string(),
                            value: Some(Box::new(fdata::DictionaryValue::Str(
                                "connectivity".to_string()
                            ))),
                        },
                        fdata::DictionaryEntry {
                            key: "subcategory".to_string(),
                            value: Some(Box::new(fdata::DictionaryValue::Str(
                                "ethernet".to_string()
                            ))),
                        }
                    ]),
                    ..fdata::Dictionary::EMPTY
                }
            ]),
            vec![
                fdi::DeviceCategory {
                    category: Some("usb".to_string()),
                    subcategory: None,
                    ..fdi::DeviceCategory::EMPTY
                },
                fdi::DeviceCategory {
                    category: Some("connectivity".to_string()),
                    subcategory: Some("ethernet".to_string()),
                    ..fdi::DeviceCategory::EMPTY
                }
            ]
        );
    }
}
