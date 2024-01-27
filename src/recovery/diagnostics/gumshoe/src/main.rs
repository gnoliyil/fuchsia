// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod device_info;
mod handlebars_utils;
mod partition_reader;
mod responder;
mod storage_info;
mod webserver;

use crate::device_info::DeviceInfoImpl;
use crate::handlebars_utils::TemplateEngine;
use crate::partition_reader::PartitionReader;
use crate::responder::ResponderImpl;
use crate::storage_info::StorageInfo;
use crate::webserver::{WebServer, WebServerImpl};
use anyhow::Error;
use fidl_fuchsia_hwinfo::BoardInfo;
use fidl_fuchsia_hwinfo::DeviceInfo;
use fidl_fuchsia_hwinfo::ProductInfo;
use fuchsia_component::client::connect_to_protocol;
use futures::lock::Mutex;
use glob::glob;
use handlebars::Handlebars;
use std::sync::Arc;
use std::vec::Vec;

const LISTENING_PORT: u16 = 8080;
const TEMPLATE_GLOB_PATH: Option<&str> = Some("/pkg/templates/*.hbs.html");

/// Retrieves ProductInfo from HWInfo FIDL.
async fn get_product_info() -> Result<ProductInfo, Error> {
    let product = connect_to_protocol::<fidl_fuchsia_hwinfo::ProductMarker>()?;
    Ok(product.get_info().await?)
}

/// Connects to Partition FIDL at given path.
fn partition_provider(
    path: &str,
) -> Result<fidl_fuchsia_hardware_block_partition::PartitionProxy, Error> {
    fuchsia_component::client::connect_to_protocol_at_path::<
        fidl_fuchsia_hardware_block_partition::PartitionMarker,
    >(path)
}

/// Retrieve BoardInfo from HWInfo FIDL.
async fn get_board_info() -> Result<BoardInfo, Error> {
    let board = connect_to_protocol::<fidl_fuchsia_hwinfo::BoardMarker>()?;
    Ok(board.get_info().await?)
}

/// Retrieve DeviceInfo from HWInfo FIDL.
async fn get_device_info() -> Result<DeviceInfo, Error> {
    let device = connect_to_protocol::<fidl_fuchsia_hwinfo::DeviceMarker>()?;
    Ok(device.get_info().await?)
}

/// Send gumshoe on a stakeout. While on a stakeout, gumshoe responds
/// to HTTP requests on their webserver's listening port. Returns the
/// WebServer's run() result: a setup error (eg, if bind() or template
/// registration fails), or (more likely) run() never returns because
/// the component waits indefinitely on new connections.
async fn stakeout(
    web_server: &dyn WebServer,
    mut template_engine: Box<dyn TemplateEngine>,
    templates_glob_path: Option<&str>,
) -> Result<(), Error> {
    if let Some(templates_glob_path) = templates_glob_path {
        // Source resources by globbing paths into utf8 strings.
        let resources: Vec<String> = glob(templates_glob_path)?
            .filter_map(|glob_result| glob_result.ok())
            .filter_map(|path_buf| path_buf.to_str().map_or(None, |s| Some(s.to_string())))
            .collect();

        // Register sourced resources, exiting if any resource can't be registered.
        handlebars_utils::register_template_resources(
            &mut template_engine,
            resources.iter().map(AsRef::as_ref),
        )?;
    }

    let storage_info = match StorageInfo::new(partition_provider).await {
        Ok(storage_info) => Some(storage_info),
        Err(e) => {
            println!("Error getting storage info: {:?}", e);
            None
        }
    };
    let product_info = match get_product_info().await {
        Ok(product_info) => Some(product_info),
        Err(e) => {
            println!("Error getting product info: {:?}", e);
            None
        }
    };
    let board_info = match get_board_info().await {
        Ok(board_info) => Some(board_info),
        Err(e) => {
            eprintln!("Error getting board info: {:?}", e);
            None
        }
    };
    let device_info = match get_device_info().await {
        Ok(device_info) => Some(device_info),
        Err(e) => {
            eprintln!("Error getting device info: {:?}", e);
            None
        }
    };
    let boxed_device_info =
        Box::new(DeviceInfoImpl::new(board_info, device_info, product_info, storage_info));

    let partition_reader = PartitionReader::new(
        fuchsia_component::client::connect_to_protocol_at_path::<
            fidl_fuchsia_hardware_block::BlockMarker,
        >,
    )
    .await?;
    let partition_reader = Box::new(partition_reader);

    // Construct a responder for generating HTTP responses from HTTP requests.
    let responder_impl = ResponderImpl::new(template_engine, partition_reader, boxed_device_info);
    let responder = Arc::new(Mutex::new(responder_impl));

    // Start handling incoming web requests using the responder.
    web_server.run(LISTENING_PORT, responder).await
}

#[fuchsia::main(threads = 10)]
async fn main() {
    let web_server_impl = WebServerImpl {};
    let template_engine = Box::new(Handlebars::new());

    match stakeout(&web_server_impl, template_engine, TEMPLATE_GLOB_PATH).await {
        Ok(()) => {
            println!("Stakeout completed");
        }
        Err(e) => {
            println!("Stakeout terminated with error: {:?}", e);
        }
    };
}

#[cfg(test)]
mod tests {
    use crate::handlebars_utils::MockTemplateEngine;
    use crate::{stakeout, TEMPLATE_GLOB_PATH};

    use crate::webserver::MockWebServer;
    use anyhow::{anyhow, Error};
    use fuchsia_async as fasync;
    use mockall::predicate::eq;
    use mockall::Sequence;

    const NO_TEMPLATES: Option<&str> = None;
    const WEBSERVER_ERROR: &str = "WebServer Error!";
    const REGISTRATION_ERROR: &str = "Registration Error!";

    /// Verifies stakeout() runs webserver.
    #[fasync::run_singlethreaded(test)]
    async fn stakeout_starts_webserver() -> Result<(), Error> {
        let mut web_server = MockWebServer::new();

        // Expect the webserver to run (and simulate exiting immediately).
        web_server.expect_run().times(1).returning(|_, _| Ok(()));

        stakeout(&web_server, Box::new(MockTemplateEngine::new()), NO_TEMPLATES).await
    }

    /// Verifies WebServer.run() errors are percolated up from stakeout().
    #[fasync::run_singlethreaded(test)]
    async fn stakeout_percolates_webserver_error() {
        let mut web_server = MockWebServer::new();

        // Expect the webserver to run (and simulate exiting immediately with error).
        web_server.expect_run().times(1).returning(|_, _| Err(anyhow!(WEBSERVER_ERROR)));

        let result = stakeout(&web_server, Box::new(MockTemplateEngine::new()), NO_TEMPLATES).await;

        assert!(result.is_err(), "stakeout should return an error");
        assert_eq!(
            result.err().unwrap().to_string(),
            WEBSERVER_ERROR,
            "stakeout should return a WEBSERVER_ERROR"
        );
    }

    /// Verifies stakeout() registers all files from resource("templates").
    #[fasync::run_singlethreaded(test)]
    async fn stakeout_registers_templates() -> Result<(), Error> {
        let mut web_server = MockWebServer::new();
        let mut template_engine = MockTemplateEngine::new();

        // Expect templates to be registered in alphanumeric order.
        let resources = vec![
            ("404", "/pkg/templates/404.hbs.html"),
            ("chrome", "/pkg/templates/chrome.hbs.html"),
            ("index", "/pkg/templates/index.hbs.html"),
            ("info", "/pkg/templates/info.hbs.html"),
        ];
        let mut call_sequence = Sequence::new();
        for (name, path) in &resources {
            template_engine
                .expect_register_resource()
                .with(eq(*name), eq(*path))
                .times(1)
                .returning(|_, _| Ok(()))
                .in_sequence(&mut call_sequence);
        }

        // Expect the webserver to be started (and simulate exiting immediately).
        web_server.expect_run().times(1).returning(|_, _| Ok(()));

        stakeout(&web_server, Box::new(template_engine), TEMPLATE_GLOB_PATH).await
    }

    /// Verifies stakeout percolates template registration errors.
    #[fasync::run_singlethreaded(test)]
    async fn stakeout_percolates_template_registration_error() {
        // Template initialization error prevents Web Server run() call.
        let web_server = MockWebServer::new();

        let mut template_engine = MockTemplateEngine::new();

        // Verify stakeout() fails when first resource fails to register.
        template_engine
            .expect_register_resource()
            .with(eq("404"), eq("/pkg/templates/404.hbs.html"))
            .times(1)
            .returning(|_, _| Err(anyhow!(REGISTRATION_ERROR)));

        let result = stakeout(&web_server, Box::new(template_engine), TEMPLATE_GLOB_PATH).await;

        assert!(result.is_err(), "stakeout should return an error");
        assert_eq!(
            result.err().unwrap().to_string(),
            REGISTRATION_ERROR,
            "stakeout should return a REGISTRATION_ERROR"
        );
    }
}
