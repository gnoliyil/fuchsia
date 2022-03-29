// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::common_utils::common::macros::{fx_err_and_bail, with_line};
use crate::component::types::{
    ComponentLaunchRequest, ComponentLaunchResponse, ComponentSearchRequest, ComponentSearchResult,
};
use anyhow::Error;
use component_hub::{
    io::Directory,
    list::{Component as ListComponent, ListFilter},
    show::find_components,
};
use fidl_fuchsia_sys::ComponentControllerEvent;
use fuchsia_component::client;
use fuchsia_syslog::macros::fx_log_info;
use fuchsia_syslog::macros::*;
use futures::StreamExt;
use lazy_static::lazy_static;
use serde_json::{from_value, Value};

lazy_static! {
    static ref HUB_DIR: Directory = Directory::from_namespace("/hub-v2").unwrap();
}

/// Perform operations related to Component.
///
/// Note this object is shared among all threads created by server.
///
#[derive(Debug)]
pub struct ComponentFacade {}

impl ComponentFacade {
    pub fn new() -> ComponentFacade {
        ComponentFacade {}
    }

    /// Parse component url and return app created by launch function
    /// # Arguments
    /// * `args`: will be parsed to ComponentLaunchRequest
    /// * `url`: full url of the component
    /// * `arguments`: optional arguments for the component
    async fn create_launch_app(&self, args: Value) -> Result<client::App, Error> {
        let tag = "ComponentFacade::create_launch_app";
        let req: ComponentLaunchRequest = from_value(args)?;
        // check if it's full url
        let component_url = match req.url {
            Some(x) => {
                if !x.starts_with("fuchsia-pkg") {
                    return Err(format_err!("Need full component url to launch"));
                }
                fx_log_info!(
                    "Executing Launch {} in Component Facade with arguments {:?}.",
                    x,
                    req.arguments
                );
                x
            }
            None => return Err(format_err!("Need full component url to launch")),
        };
        let launcher = match client::launcher() {
            Ok(r) => r,
            Err(err) => fx_err_and_bail!(
                &with_line!(tag),
                format_err!("Failed to get launcher service: {}", err)
            ),
        };
        let app = client::launch(&launcher, component_url.to_string(), req.arguments)?;
        Ok(app)
    }

    /// Launch component with url and optional arguments and detach directly
    /// # Arguments
    /// * `args`: will be parsed to ComponentLaunchRequest in create_launch_app
    /// * `url`: url of the component
    /// * `arguments`: optional arguments for the component
    pub async fn launch(&self, args: Value) -> Result<ComponentLaunchResponse, Error> {
        let tag = "ComponentFacade::launch";
        let launch_app = Some(self.create_launch_app(args).await?);
        let app = match launch_app {
            Some(p) => p,
            None => fx_err_and_bail!(&with_line!(tag), "Failed to launch component."),
        };
        let mut code = 0;
        let mut component_stream = app.controller().take_event_stream();
        match component_stream
            .next()
            .await
            .expect("component event stream ended before termination event")?
        {
            // detach if succeeds
            ComponentControllerEvent::OnDirectoryReady {} => {
                app.controller().detach()?;
            }
            // if there's exception (like url package not found, return fail)
            ComponentControllerEvent::OnTerminated { return_code, termination_reason } => {
                code = return_code;
                if return_code != 0 {
                    fx_log_info!(
                        "Component terminated unexpectedly. Code: {}. Reason: {:?}",
                        return_code,
                        termination_reason
                    );
                }
            }
        }
        match code {
            0 => Ok(ComponentLaunchResponse::Success),
            _ => Ok(ComponentLaunchResponse::Fail(code)),
        }
    }

    /// Search component with component's name under appmgr
    /// # Arguments
    /// * `args`: will be parsed to ComponentSearchRequest
    /// * `name`: name of the component (should be like "component.cmx")
    pub async fn search(&self, args: Value) -> Result<ComponentSearchResult, Error> {
        let tag = "ComponentFacade::search";
        let req: ComponentSearchRequest = from_value(args)?;
        let name = match req.name {
            Some(x) => {
                fx_log_info!("Searching Component {} in ComponentSearch Facade", x,);
                x
            }
            None => return Err(format_err!("Need name of the component to search.")),
        };
        let components = match find_components(name.to_string(), HUB_DIR.clone()?).await {
            Ok(p) => p,
            Err(err) => fx_err_and_bail!(
                &with_line!(tag),
                format_err!("Failed to find component: {}, err: {:}", name.to_string(), err)
            ),
        };
        if components.is_empty() {
            return Ok(ComponentSearchResult::NotFound);
        }
        Ok(ComponentSearchResult::Success)
    }

    fn filter(&self, component: &ListComponent, filter: ListFilter) -> Vec<String> {
        fn filter_recursive(
            component: &ListComponent,
            filter: &ListFilter,
            component_urls: &mut Vec<String>,
        ) {
            if component.should_include(&filter) {
                component_urls.extend([component.url.clone()]);
            }

            for child in &component.children {
                filter_recursive(&child, &filter, component_urls);
            }
        }
        let mut component_urls = Vec::<String>::new();
        filter_recursive(&component, &filter, &mut component_urls);
        component_urls
    }

    /// List running components, returns a vector containing component full URL.
    pub async fn list(&self) -> Result<Vec<String>, Error> {
        fx_log_info!("List running Component under appmgr in ComponentSearch Facade",);
        // let hub_path = PathBuf::from("/hub-v2");
        let hub_dir = Directory::from_namespace("/hub-v2").unwrap();
        match ListComponent::parse("/".to_string(), hub_dir).await {
            Ok(component) => Ok(self.filter(&component, ListFilter::Running)),
            Err(e) => Err(e),
        }
    }
}
