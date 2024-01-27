// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "128"]

mod config;
mod device_server;
mod provider_server;

use {
    crate::config::Config,
    crate::device_server::{serve_application_passthrough, serve_device_info_passthrough},
    anyhow::{format_err, Context as _, Error},
    fidl::endpoints::{DiscoverableProtocolMarker as _, ServerEnd},
    fidl_fuchsia_hardware_tee::{DeviceConnectorMarker, DeviceConnectorProxy},
    fidl_fuchsia_tee::{self as fuchsia_tee, DeviceInfoMarker},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_fs::OpenFlags,
    fuchsia_syslog as syslog,
    fuchsia_syslog::macros::*,
    fuchsia_vfs_watcher as vfs,
    futures::{prelude::*, select, stream::FusedStream},
    std::path::{Path, PathBuf},
    uuid::Uuid,
};

const DEV_TEE_PATH: &str = "/dev/class/tee";

enum IncomingRequest {
    Application(ServerEnd<fuchsia_tee::ApplicationMarker>, fuchsia_tee::Uuid),
    DeviceInfo(ServerEnd<fuchsia_tee::DeviceInfoMarker>),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["tee_manager"])?;

    let device_list = enumerate_tee_devices().await?;

    let path = match device_list.as_slice() {
        [] => return Err(format_err!("No TEE devices found")),
        [path] => path.to_str().unwrap(),
        _device_list => {
            // Cannot handle more than one TEE device
            // If this becomes supported, Manager will need to provide a method for clients to
            // enumerate and select a device to connect to.
            return Err(format_err!(
                "Found more than 1 TEE device - this is currently not supported"
            ));
        }
    };

    let dev_connector_proxy =
        fuchsia_component::client::connect_to_protocol_at_path::<DeviceConnectorMarker>(path)
            .context("Failed to connect to TEE DeviceConnectorProxy")?;

    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_service_at(DeviceInfoMarker::PROTOCOL_NAME, |channel| {
        Some(IncomingRequest::DeviceInfo(ServerEnd::new(channel)))
    });

    match Config::from_file() {
        Ok(config) => {
            for app_uuid in config.application_uuids {
                let service_name = format!("fuchsia.tee.Application.{}", app_uuid.as_hyphenated());
                fx_log_debug!("Serving {}", service_name);
                let fidl_uuid = uuid_to_fuchsia_tee_uuid(&app_uuid);
                fs.dir("svc").add_service_at(service_name, move |channel| {
                    Some(IncomingRequest::Application(ServerEnd::new(channel), fidl_uuid))
                });
            }
        }
        Err(error) => fx_log_warn!("Unable to serve applications: {:?}", error),
    }

    fs.take_and_serve_directory_handle()?;

    serve(dev_connector_proxy, fs.fuse()).await
}

async fn serve(
    dev_connector_proxy: DeviceConnectorProxy,
    service_stream: impl Stream<Item = IncomingRequest> + FusedStream + Unpin,
) -> Result<(), Error> {
    let mut device_fut = dev_connector_proxy.take_event_stream().into_future();
    let mut service_fut =
        service_stream.for_each_concurrent(None, |request: IncomingRequest| async {
            match request {
                IncomingRequest::Application(channel, uuid) => {
                    fx_log_trace!("Connecting application: {:?}", uuid);
                    serve_application_passthrough(uuid, dev_connector_proxy.clone(), channel).await
                }
                IncomingRequest::DeviceInfo(channel) => {
                    serve_device_info_passthrough(dev_connector_proxy.clone(), channel).await
                }
            }
            .unwrap_or_else(|e| fx_log_err!("{:?}", e));
        });

    select! {
        service_result = service_fut => Ok(service_result),
        _ = device_fut => Err(format_err!("TEE DeviceConnector closed unexpectedly")),
    }
}

async fn enumerate_tee_devices() -> Result<Vec<PathBuf>, Error> {
    let mut device_list = Vec::new();

    let mut watcher = create_watcher(&DEV_TEE_PATH).await?;

    while let Some(msg) = watcher.try_next().await? {
        match msg.event {
            vfs::WatchEvent::EXISTING => {
                if msg.filename == Path::new(".") {
                    continue;
                }
                device_list.push(PathBuf::new().join(DEV_TEE_PATH).join(msg.filename));
            }
            vfs::WatchEvent::IDLE => {
                break;
            }
            _ => {
                unreachable!("Non-fio::WatchEvent::EXISTING found before fio::WatchEvent::IDLE");
            }
        }
    }
    Ok(device_list)
}

async fn create_watcher(path: &str) -> Result<vfs::Watcher, Error> {
    let dir = fuchsia_fs::directory::open_in_namespace(path, OpenFlags::RIGHT_READABLE)?;
    let watcher = vfs::Watcher::new(&dir).await?;
    Ok(watcher)
}

/// Converts a `uuid::Uuid` to a `fidl_fuchsia_tee::Uuid`.
fn uuid_to_fuchsia_tee_uuid(uuid: &Uuid) -> fuchsia_tee::Uuid {
    let (time_low, time_mid, time_hi_and_version, clock_seq_and_node) = uuid.as_fields();

    fuchsia_tee::Uuid {
        time_low,
        time_mid,
        time_hi_and_version,
        clock_seq_and_node: *clock_seq_and_node,
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl::{endpoints, Error, HandleBased as _},
        fidl_fuchsia_hardware_tee::DeviceConnectorRequest,
        fidl_fuchsia_io as fio,
        fidl_fuchsia_tee::ApplicationMarker,
        fidl_fuchsia_tee_manager::ProviderProxy,
        fuchsia_zircon_status::Status,
        futures::channel::mpsc,
    };

    fn spawn_device_connector<F>(
        request_handler: impl Fn(DeviceConnectorRequest) -> F + 'static,
    ) -> DeviceConnectorProxy
    where
        F: Future,
    {
        let (proxy, mut stream) = endpoints::create_proxy_and_stream::<DeviceConnectorMarker>()
            .expect("Failed to create DeviceConnector proxy and server.");

        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                request_handler(request).await;
            }
        })
        .detach();

        proxy
    }

    fn get_storage(provider_proxy: &ProviderProxy) -> fio::DirectoryProxy {
        let (client_end, server_end) = endpoints::create_endpoints::<fio::DirectoryMarker>()
            .expect("Failed to create fuchsia::io::Directory endpoints");
        assert!(provider_proxy.request_persistent_storage(server_end).is_ok());
        client_end.into_proxy().expect("Failed to convert ClientEnd to DirectoryProxy")
    }

    fn is_closed_with_status(error: Error, status: Status) -> bool {
        match error {
            Error::ClientChannelClosed { status: s, .. } => s == status,
            _ => false,
        }
    }

    async fn assert_is_valid_storage(storage_proxy: &fio::DirectoryProxy) {
        assert_eq!(storage_proxy.query().await.unwrap(), fio::DIRECTORY_PROTOCOL_NAME.as_bytes());
    }

    #[fasync::run_singlethreaded(test)]
    async fn connect_to_application() {
        let app_uuid = uuid_to_fuchsia_tee_uuid(
            &Uuid::parse_str("8aaaf200-2450-11e4-abe2-0002a5d5c51b").unwrap(),
        );

        let dev_connector = spawn_device_connector(move |request| async move {
            match request {
                DeviceConnectorRequest::ConnectToApplication {
                    application_uuid,
                    service_provider,
                    application_request,
                    control_handle: _,
                } => {
                    assert_eq!(application_uuid, app_uuid);
                    assert!(service_provider.is_some());
                    assert!(!application_request.channel().is_invalid_handle());

                    let provider_proxy = service_provider
                        .unwrap()
                        .into_proxy()
                        .expect("Failed to convert ClientEnd to ProviderProxy");

                    assert_is_valid_storage(&get_storage(&provider_proxy)).await;

                    application_request
                        .close_with_epitaph(Status::OK)
                        .expect("Unable to close tee_request");
                }
                _ => {
                    assert!(false);
                }
            }
        });

        let (mut sender, receiver) = mpsc::channel::<IncomingRequest>(1);

        fasync::Task::local(async move {
            let result = serve(dev_connector, receiver.fuse()).await;
            assert!(result.is_ok(), "{}", result.unwrap_err());
        })
        .detach();

        let (app_client, app_server) = endpoints::create_endpoints::<ApplicationMarker>()
            .expect("Failed to create Device endpoints");

        let app_proxy =
            app_client.into_proxy().expect("Failed to convert ClientEnd to DeviceProxy");
        sender
            .try_send(IncomingRequest::Application(app_server, app_uuid))
            .expect("Unable to send Application Request");

        let (result, _) = app_proxy.take_event_stream().into_future().await;
        assert!(is_closed_with_status(result.unwrap().unwrap_err(), Status::OK));
    }

    #[fasync::run_singlethreaded(test)]
    async fn connect_to_device_info() {
        let dev_connector = spawn_device_connector(|request| async move {
            match request {
                DeviceConnectorRequest::ConnectToDeviceInfo {
                    device_info_request,
                    control_handle: _,
                } => {
                    assert!(!device_info_request.channel().is_invalid_handle());
                    device_info_request
                        .close_with_epitaph(Status::OK)
                        .expect("Unable to close device_info_request");
                }
                _ => {
                    assert!(false);
                }
            }
        });

        let (mut sender, receiver) = mpsc::channel::<IncomingRequest>(1);

        fasync::Task::local(async move {
            let result = serve(dev_connector, receiver.fuse()).await;
            assert!(result.is_ok(), "{}", result.unwrap_err());
        })
        .detach();

        let (device_info_client, device_info_server) =
            endpoints::create_endpoints::<DeviceInfoMarker>()
                .expect("Failed to create DeviceInfo endpoints");

        let device_info_proxy = device_info_client
            .into_proxy()
            .expect("Failed to convert ClientEnd to DeviceInfoProxy");

        sender
            .try_send(IncomingRequest::DeviceInfo(device_info_server))
            .expect("Unable to send DeviceInfo Request");

        let (result, _) = device_info_proxy.take_event_stream().into_future().await;
        assert!(is_closed_with_status(result.unwrap().unwrap_err(), Status::OK));
    }

    #[fasync::run_singlethreaded(test)]
    async fn tee_device_closed() {
        let (dev_connector_proxy, dev_connector_server) =
            fidl::endpoints::create_proxy::<DeviceConnectorMarker>()
                .expect("Unable to create DeviceConnectorProxy");
        let (_sender, receiver) = mpsc::channel::<IncomingRequest>(1);

        dev_connector_server
            .close_with_epitaph(Status::PEER_CLOSED)
            .expect("Could not close DeviceConnector ServerEnd");
        let result = serve(dev_connector_proxy, receiver.fuse()).await;
        assert!(result.is_err());
    }
}
