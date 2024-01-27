// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::app_set::FuchsiaAppSet;
use fidl_fuchsia_cobalt::{
    SoftwareDistributionInfo, SystemDataUpdaterMarker, SystemDataUpdaterProxy,
};
use fuchsia_component::client::connect_to_protocol;
use futures::lock::Mutex;
use std::rc::Rc;
use tracing::{error, info};

pub async fn notify_cobalt_current_software_distribution(app_set: Rc<Mutex<FuchsiaAppSet>>) {
    info!("Notifying Cobalt about the current channel and app id.");
    let proxy = match connect_to_protocol::<SystemDataUpdaterMarker>() {
        Ok(proxy) => proxy,
        Err(e) => {
            error!("Failed to connect to cobalt: {}", e);
            return;
        }
    };
    notify_cobalt_current_software_distribution_impl(proxy, app_set).await;
}

async fn notify_cobalt_current_software_distribution_impl(
    proxy: SystemDataUpdaterProxy,
    app_set: Rc<Mutex<FuchsiaAppSet>>,
) {
    let distribution_info = {
        let app_set = app_set.lock().await;
        let channel = app_set.get_system_current_channel();

        SoftwareDistributionInfo {
            current_channel: Some(channel.into()),
            ..SoftwareDistributionInfo::EMPTY
        }
    };
    match proxy.set_software_distribution_info(distribution_info).await {
        Ok(fidl_fuchsia_cobalt::Status::Ok) => {}
        error => {
            error!("SystemDataUpdater.SetSoftwareDistributionInfo failed: {:?}", error);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app_set::{AppIdSource, AppMetadata};
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_cobalt::SystemDataUpdaterRequest;
    use fuchsia_async as fasync;
    use futures::prelude::*;
    use omaha_client::{common::App, protocol::Cohort};

    #[fasync::run_singlethreaded(test)]
    async fn test_notify_cobalt() {
        let app_metadata = AppMetadata { appid_source: AppIdSource::VbMetadata };
        let app_set = Rc::new(Mutex::new(FuchsiaAppSet::new(
            App::builder()
                .id("id")
                .version([1, 2])
                .cohort(Cohort { name: Some("current-channel".to_string()), ..Cohort::default() })
                .build(),
            app_metadata,
        )));

        let (proxy, mut stream) = create_proxy_and_stream::<SystemDataUpdaterMarker>().unwrap();
        let stream_fut = async move {
            match stream.next().await {
                Some(Ok(SystemDataUpdaterRequest::SetSoftwareDistributionInfo {
                    info,
                    responder,
                })) => {
                    assert_eq!(info.current_channel.unwrap(), "current-channel");
                    responder.send(fidl_fuchsia_cobalt::Status::Ok).unwrap();
                }
                err => panic!("Err in request handler: {:?}", err),
            }
            assert!(stream.next().await.is_none());
        };
        future::join(notify_cobalt_current_software_distribution_impl(proxy, app_set), stream_fut)
            .await;
    }
}
