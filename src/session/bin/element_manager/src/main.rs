// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Component that serves the `fuchsia.element.Manager` protocol.
//!
//! Elements launched through the `Manager` protocol are created in a collection as
//! children in of this component.
//!
//! # Lifecycle
//!
//! If a client closes its connection to `Manager`, any running elements that it
//! has proposed without an associated `fuchsia.element.Controller` will continue to run.

mod annotation;
mod element;
mod element_manager;

use {
    crate::element_manager::{CollectionConfig, ElementManager},
    anyhow::Error,
    element_config::Config,
    fidl_connector::ServiceReconnector,
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_element as felement,
    fidl_fuchsia_ui_scenic::ScenicMarker,
    fuchsia_component::{client::connect_to_protocol, server::ServiceFs},
    futures::StreamExt,
    std::rc::Rc,
    tracing::info,
};

/// This enum allows the session to match on incoming messages.
enum ExposedServices {
    Manager(felement::ManagerRequestStream),
}

/// The maximum number of concurrent requests.
const NUM_CONCURRENT_REQUESTS: usize = 5;

#[fuchsia::main]
async fn main() -> Result<(), Error> {
    info!("Starting.");
    let config = Config::take_from_startup_handle();
    let inspector = fuchsia_inspect::component::inspector();
    inspector.root().record_child("config", |config_node| config.record_inspect(config_node));

    let realm = connect_to_protocol::<fcomponent::RealmMarker>()
        .expect("Failed to connect to Realm service");

    let graphical_presenter =
        Box::new(ServiceReconnector::<felement::GraphicalPresenterMarker>::new());

    // TODO(fxbug.dev/64206): Remove after Flatland migration is completed.
    let scenic_uses_flatland = fuchsia_component::client::connect_to_protocol::<ScenicMarker>()
        .expect("Failed to connect to Scenic.")
        .uses_flatland()
        .await
        .expect("Failed to get flatland info.");

    let collection_config = CollectionConfig {
        url_to_collection: config
            .url_to_collection
            .into_iter()
            .map(|rule: String| -> (String, String) {
                let split: Vec<&str> = rule.split("|").collect();
                assert_eq!(split.len(), 2, "malformed url_to_collection entry: {:?}", rule);
                (split[0].to_owned(), split[1].to_owned())
            })
            .collect(),
        default_collection: config.default_collection.to_string(),
    };

    info!("Element collection policy: #{:?}", collection_config);

    let element_manager = Rc::new(ElementManager::new(
        realm,
        Some(graphical_presenter),
        collection_config,
        scenic_uses_flatland,
    ));

    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(ExposedServices::Manager);

    fs.take_and_serve_directory_handle()?;

    fs.for_each_concurrent(NUM_CONCURRENT_REQUESTS, |service_request: ExposedServices| async {
        let element_manager = element_manager.clone();
        match service_request {
            ExposedServices::Manager(request_stream) => {
                element_manager
                    .handle_requests(request_stream)
                    .await
                    .expect("Failed to handle element manager requests");
            }
        }
    })
    .await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        crate::element_manager::{CollectionConfig, ElementManager},
        fidl::endpoints::{create_proxy_and_stream, spawn_stream_handler},
        fidl::endpoints::{ProtocolMarker, Proxy},
        fidl_connector::Connect,
        fidl_fuchsia_component as fcomponent, fidl_fuchsia_element as felement,
        fidl_fuchsia_io as fio, fuchsia_async as fasync,
        lazy_static::lazy_static,
        session_testing::spawn_directory_server,
        std::collections::HashMap,
        test_util::Counter,
    };

    /// Spawns a local `Manager` server.
    ///
    /// # Parameters
    /// - `element_manager`: The `ElementManager` that Manager uses to launch elements.
    ///
    /// # Returns
    /// A `ManagerProxy` to the spawned server.
    fn spawn_manager_server(element_manager: Box<ElementManager>) -> felement::ManagerProxy {
        let (proxy, stream) = create_proxy_and_stream::<felement::ManagerMarker>()
            .expect("Failed to create Manager proxy and stream");

        fasync::Task::spawn(async move {
            element_manager
                .handle_requests(stream)
                .await
                .expect("Failed to handle element manager requests");
        })
        .detach();

        proxy
    }

    struct MockConnector<T: Proxy> {
        proxy: Option<T>,
    }

    impl<T: Proxy + Clone> MockConnector<T> {
        fn new(proxy: T) -> Self {
            Self { proxy: Some(proxy) }
        }
    }

    impl<T: Proxy + Clone> Connect for MockConnector<T> {
        type Proxy = T;

        fn connect(&self) -> Result<Self::Proxy, anyhow::Error> {
            self.proxy.clone().ok_or(anyhow::anyhow!("no proxy available"))
        }
    }

    /// Tests that ProposeElement launches the element as a child in a realm.
    #[fuchsia::test]
    async fn propose_element_launches_element() {
        lazy_static! {
            static ref CREATE_CHILD_CALL_COUNT: Counter = Counter::new(0);
            static ref BINDER_CONNECTION_COUNT: Counter = Counter::new(0);
        }

        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let collection_config = CollectionConfig {
            url_to_collection: HashMap::new(),
            default_collection: "elements".to_string(),
        };

        let directory_request_handler = move |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path, .. } => {
                if path == fcomponent::BinderMarker::DEBUG_NAME {
                    BINDER_CONNECTION_COUNT.inc();
                }
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl,
                    args: _,
                    responder,
                } => {
                    assert_eq!(decl.url.unwrap(), component_url);
                    CREATE_CHILD_CALL_COUNT.inc();
                    let _ = responder.send(&mut Ok(()));
                }
                fcomponent::RealmRequest::OpenExposedDir { child: _, exposed_dir, responder } => {
                    spawn_directory_server(exposed_dir, directory_request_handler);
                    let _ = responder.send(&mut Ok(()));
                }
                _ => {
                    panic!("Realm handler received unexpected request");
                }
            }
        })
        .unwrap();

        let graphical_presenter =
            spawn_stream_handler(move |graphical_presenter_request| async move {
                match graphical_presenter_request {
                    felement::GraphicalPresenterRequest::PresentView {
                        view_spec: _,
                        annotation_controller: _,
                        view_controller_request: _,
                        responder,
                    } => {
                        let _ = responder.send(&mut Ok(()));
                    }
                }
            })
            .unwrap();
        let graphical_presenter_connector = Box::new(MockConnector::new(graphical_presenter));

        let element_manager = Box::new(ElementManager::new(
            realm,
            Some(graphical_presenter_connector),
            collection_config,
            false,
        ));
        let manager_proxy = spawn_manager_server(element_manager);

        let result = manager_proxy
            .propose_element(
                felement::Spec {
                    component_url: Some(component_url.to_string()),
                    ..felement::Spec::EMPTY
                },
                None,
            )
            .await;
        assert!(result.is_ok());

        assert_eq!(CREATE_CHILD_CALL_COUNT.get(), 1);
        assert_eq!(BINDER_CONNECTION_COUNT.get(), 1);
    }
}
