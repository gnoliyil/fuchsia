// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The `element_management` library provides utilities for sessions to service
//! incoming [`fidl_fuchsia_element::ManagerRequest`]s.
//!
//! Elements are instantiated as dynamic component instances in a component collection of the
//! calling component.

use {
    crate::annotation::{
        handle_annotation_controller_stream, AnnotationError, AnnotationHolder, WatchResponder,
    },
    crate::element::Element,
    anyhow::{format_err, Error},
    fidl,
    fidl::endpoints::{
        create_request_stream, ClientEnd, ControlHandle, Proxy, RequestStream, ServerEnd,
    },
    fidl::AsHandleRef,
    fidl_connector::Connect,
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_element as felement, fidl_fuchsia_io as fio,
    fidl_fuchsia_sys as fsys, fidl_fuchsia_ui_app as fuiapp,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_component, fuchsia_scenic as scenic, fuchsia_zircon as zx,
    futures::{lock::Mutex, select, FutureExt, StreamExt, TryStreamExt},
    rand::{
        distributions::{Alphanumeric, DistString},
        thread_rng,
    },
    realm_management,
    std::{collections::HashMap, sync::Arc},
    tracing::{debug, error, info},
};

// Timeout duration for a ViewControllerProxy to close, in seconds.
static VIEW_CONTROLLER_DISMISS_TIMEOUT: zx::Duration = zx::Duration::from_seconds(3_i64);

// Annotations in the ElementManager namespace.
static ELEMENT_MANAGER_NS: &'static str = "element_manager";

/// Errors returned by calls to [`ElementManager`].
#[derive(Debug, thiserror::Error, Clone, PartialEq)]
pub enum ElementManagerError {
    /// Returned when the element manager fails to create an element with additional services for
    /// the given ServiceList. This may be because:
    ///   - `host_directory` is not set
    ///   - `provider` is set
    #[error("Element spec for \"{}/{}\" contains an invalid ServiceList", name, collection)]
    InvalidServiceList { name: String, collection: String },

    /// Returned when the element manager fails to create an element that uses a CFv2 component
    /// because the spec provides a ServiceList with additional services.
    ///
    /// `Spec.additional_services` is only supported for CFv1 components.
    #[error(
        "Element spec for \"{}/{}\" provides additional_services for a CFv2 component",
        name,
        collection
    )]
    AdditionalServicesNotSupported { name: String, collection: String },

    /// Returned when the element manager fails to create an element that uses a CFv2 component
    /// because the spec contains arguments.
    ///
    /// `Spec.arguments` is only supported for CFv1 components.
    #[error(
        "Element spec for \"{}/{}\" provides arguments for a CFv2 component",
        name,
        collection
    )]
    ArgumentsNotSupported { name: String, collection: String },

    /// Returned when the element manager fails to created the component instance associated with
    /// a given element.
    #[error("Element {} not created at \"{}/{}\": {:?}", url, collection, name, err)]
    NotCreated { name: String, collection: String, url: String, err: fcomponent::Error },

    /// Returned when the element manager fails to launch a component with the fuchsia.sys
    /// given launcher. This may be due to an issue with the launcher itself (not bound?).
    #[error("Element {} not launched: {:?}", url, err_str)]
    NotLaunched { url: String, err_str: String },

    /// Returned when the element manager fails to open the exposed directory
    /// of the component instance associated with a given element.
    #[error("Element {} not bound at \"{}/{}\": {:?}", url, collection, name, err)]
    ExposedDirNotOpened { name: String, collection: String, url: String, err: fcomponent::Error },

    /// Returned when the element manager fails to bind to the component instance associated with
    /// a given element.
    #[error("Element {} not bound at \"{}/{}\": {:?}", url, collection, name, err_str)]
    NotBound { name: String, collection: String, url: String, err_str: String },
}

impl ElementManagerError {
    pub fn invalid_service_list(
        name: impl Into<String>,
        collection: impl Into<String>,
    ) -> ElementManagerError {
        ElementManagerError::InvalidServiceList { name: name.into(), collection: collection.into() }
    }

    pub fn additional_services_not_supported(
        name: impl Into<String>,
        collection: impl Into<String>,
    ) -> ElementManagerError {
        ElementManagerError::AdditionalServicesNotSupported {
            name: name.into(),
            collection: collection.into(),
        }
    }

    pub fn arguments_not_supported(
        name: impl Into<String>,
        collection: impl Into<String>,
    ) -> ElementManagerError {
        ElementManagerError::ArgumentsNotSupported {
            name: name.into(),
            collection: collection.into(),
        }
    }

    pub fn not_created(
        name: impl Into<String>,
        collection: impl Into<String>,
        url: impl Into<String>,
        err: impl Into<fcomponent::Error>,
    ) -> ElementManagerError {
        ElementManagerError::NotCreated {
            name: name.into(),
            collection: collection.into(),
            url: url.into(),
            err: err.into(),
        }
    }

    pub fn not_launched(url: impl Into<String>, err_str: impl Into<String>) -> ElementManagerError {
        ElementManagerError::NotLaunched { url: url.into(), err_str: err_str.into() }
    }

    pub fn exposed_dir_not_opened(
        name: impl Into<String>,
        collection: impl Into<String>,
        url: impl Into<String>,
        err: impl Into<fcomponent::Error>,
    ) -> ElementManagerError {
        ElementManagerError::ExposedDirNotOpened {
            name: name.into(),
            collection: collection.into(),
            url: url.into(),
            err: err.into(),
        }
    }

    pub fn not_bound(
        name: impl Into<String>,
        collection: impl Into<String>,
        url: impl Into<String>,
        err_str: impl Into<String>,
    ) -> ElementManagerError {
        ElementManagerError::NotBound {
            name: name.into(),
            collection: collection.into(),
            url: url.into(),
            err_str: err_str.into(),
        }
    }
}

/// Checks whether the component is a *.cm or not
///
/// # Parameters
/// - `component_url`: The component url.
fn is_v2_component(component_url: &str) -> bool {
    component_url.ends_with(".cm")
}

/// A list of services passed to an Element created from a CFv1 component.
///
/// This is a subset of fuchsia::sys::ServiceList that only supports a host
/// directory, not a ServiceProvider.
struct AdditionalServices {
    /// List of service names.
    pub names: Vec<String>,

    /// A channel to the directory hosting the services in `names`.
    pub host_directory: fidl::endpoints::ClientEnd<fio::DirectoryMarker>,
}

pub type GraphicalPresenterConnector =
    Box<dyn Connect<Proxy = felement::GraphicalPresenterProxy> + Send + Sync>;

/// A mapping from component URL to the name of the collection in which
/// components with that URL should be run.
#[derive(Debug, Clone)]
pub struct CollectionConfig {
    pub url_to_collection: HashMap<String, String>,
    pub default_collection: String,
}

impl CollectionConfig {
    /// For a given component URL, returns the collection in which to run
    /// components with that URL.
    fn for_url(&self, url: &str) -> &str {
        self.url_to_collection.get(url).unwrap_or(&self.default_collection)
    }
}

/// Manages the elements associated with a session.
pub struct ElementManager {
    /// The realm which this element manager uses to create components.
    realm: fcomponent::RealmProxy,

    /// The presenter that will make launched elements visible to the user.
    ///
    /// This is typically provided by the system shell, or other similar configurable component.
    graphical_presenter_connector: Option<GraphicalPresenterConnector>,

    /// A proxy to the `fuchsia::sys::Launcher` protocol used to create CFv1 components.
    sys_launcher: fsys::LauncherProxy,

    /// Policy that determines the collection in which elements will be launched.
    ///
    /// This is only used for elements that have a CFv2 (*.cm) component URL, and has no meaning
    /// for CFv1 elementes.
    ///
    /// The component that is running the `ElementManager` must have a collection in its CML file
    /// for each collection listed in `collections_config`.
    collection_config: CollectionConfig,

    /// Returns whether the client should use Flatland to interact with Scenic.
    /// TODO(fxbug.dev/64206): Remove after Flatland migration is completed.
    scenic_uses_flatland: bool,
}

impl ElementManager {
    pub fn new(
        realm: fcomponent::RealmProxy,
        graphical_presenter_connector: Option<GraphicalPresenterConnector>,
        sys_launcher: fsys::LauncherProxy,
        collection_config: CollectionConfig,
        scenic_uses_flatland: bool,
    ) -> ElementManager {
        ElementManager {
            realm,
            graphical_presenter_connector,
            sys_launcher,
            collection_config,
            scenic_uses_flatland,
        }
    }

    /// Adds an element to the session.
    ///
    /// This method creates the component instance and binds to it, causing it to start running.
    ///
    /// # Parameters
    /// - `url`: The component URL of the element to add as a child.
    /// - `additional_services`: Additional services to add the new component's namespace under /svc,
    ///                          in addition to those coming from the environment. Only applicable
    ///                          for legacy (v1) components.
    /// - `arguments`: Arguments passed to the component. Only applicable for legacy (v1) components.
    /// - `child_name`: The name of the element, must be unique within a session. The name must be
    ///                 non-empty, of the form [a-z0-9-_.].
    ///
    /// On success, the [`Element`] is returned back to the session.
    ///
    /// # Errors
    /// If the child cannot be created or bound in the current [`fidl_fuchsia_component::Realm`]. In
    /// particular, it is an error to call [`launch_element`] twice with the same `child_name`.
    pub async fn launch_element(
        &self,
        url: String,
        additional_services: Option<fidl_fuchsia_sys::ServiceList>,
        arguments: Option<Vec<String>>,
        child_name: &str,
    ) -> Result<Element, ElementManagerError> {
        let collection = self.collection_config.for_url(&url);

        let additional_services =
            additional_services.map_or(Ok(None), |services| match services {
                fsys::ServiceList {
                    names,
                    host_directory: Some(service_host_directory),
                    provider: None,
                } => Ok(Some(AdditionalServices { names, host_directory: service_host_directory })),
                _ => Err(ElementManagerError::invalid_service_list(child_name, collection)),
            })?;

        let element = if is_v2_component(&url) {
            // `additional_services` is only supported for CFv1 components.
            if additional_services.is_some() {
                return Err(ElementManagerError::additional_services_not_supported(
                    child_name, collection,
                ));
            }

            // `arguments` is only supported for CFv1 components.
            if arguments.is_some() {
                return Err(ElementManagerError::arguments_not_supported(child_name, collection));
            }

            self.launch_v2_element(&child_name, &url).await?
        } else {
            self.launch_v1_element(&url, additional_services, arguments).await?
        };

        Ok(element)
    }

    /// Handles requests to the [`Manager`] protocol.
    ///
    /// # Parameters
    /// `stream`: The stream that receives [`Manager`] requests.
    ///
    /// # Returns
    /// `Ok` if the request stream has been successfully handled once the client closes
    /// its connection. `Error` if a FIDL IO error was encountered.
    pub async fn handle_requests(
        &self,
        mut stream: felement::ManagerRequestStream,
    ) -> Result<(), fidl::Error> {
        while let Some(request) = stream.try_next().await? {
            match request {
                felement::ManagerRequest::ProposeElement { spec, controller, responder } => {
                    let mut result = self.handle_propose_element(spec, controller).await;
                    let _ = responder.send(&mut result);
                }
            }
        }
        Ok(())
    }

    /// Launches a CFv1 component as an element.
    ///
    /// `sys_launcher` must not be None.
    ///
    /// # Parameters
    /// - `child_url`: The component url of the child added to the session. This function launches
    ///                components using a fuchsia::sys::Launcher. It supports all CFv1 component
    ///                URL schemes, such as URLs starting with `https`, and Fuchsia component URLs
    ///                ending in `.cmx`. Fuchsia components ending in `.cm` should use
    ///                `launch_child_component()` instead.
    /// - `additional_services`: Additional services to add the new component's namespace under /svc,
    ///                          in addition to those coming from the environment.
    /// - `arguments`: Arguments to pass to the component.
    ///
    /// # Returns
    /// An Element backed by the CFv1 component, or an error if the element could not be launched.
    async fn launch_v1_element(
        &self,
        child_url: &str,
        additional_services: Option<AdditionalServices>,
        arguments: Option<Vec<String>>,
    ) -> Result<Element, ElementManagerError> {
        let mut launch_options = fuchsia_component::client::LaunchOptions::new();
        if let Some(services) = additional_services {
            launch_options.set_additional_services(services.names, services.host_directory);
        }

        let app = fuchsia_component::client::launch_with_options(
            &self.sys_launcher,
            child_url.to_string(),
            arguments,
            launch_options,
        )
        .map_err(|err: Error| ElementManagerError::not_launched(child_url, err.to_string()))?;

        Ok(Element::from_app(app, child_url))
    }

    /// Launches a CFv2 component as an element.
    ///
    /// The component is created as a child in the Element Manager's realm.
    ///
    /// # Parameters
    /// - `child_name`: The name of the element, must be unique within a session. The name must be
    ///                 non-empty, of the form [a-z0-9-_.].
    /// - `child_url`: The component url of the child added to the session.
    ///
    /// # Returns
    /// An Element backed by the CFv2 component.
    async fn launch_v2_element(
        &self,
        child_name: &str,
        child_url: &str,
    ) -> Result<Element, ElementManagerError> {
        let collection = self.collection_config.for_url(child_url);

        info!(
            child_name,
            child_url,
            collection = %collection,
            "launch_v2_element"
        );

        realm_management::create_child_component(&child_name, &child_url, collection, &self.realm)
            .await
            .map_err(|err: fcomponent::Error| {
                ElementManagerError::not_created(child_name, collection, child_url, err)
            })?;

        let exposed_directory = match realm_management::open_child_component_exposed_dir(
            child_name,
            collection,
            &self.realm,
        )
        .await
        {
            Ok(exposed_directory) => exposed_directory,
            Err(err) => {
                return Err(ElementManagerError::exposed_dir_not_opened(
                    child_name,
                    collection.to_owned(),
                    child_url,
                    err,
                ))
            }
        };

        // Connect to fuchsia.component.Binder in order to start the component.
        let _ = fuchsia_component::client::connect_to_protocol_at_dir_root::<
            fcomponent::BinderMarker,
        >(&exposed_directory)
        .map_err(|err| {
            ElementManagerError::not_bound(
                child_name,
                collection.to_owned(),
                child_url,
                err.to_string(),
            )
        })?;

        Ok(Element::from_directory_channel(
            exposed_directory.into_channel().unwrap().into_zx_channel(),
            child_name,
            child_url,
            collection,
        ))
    }

    /// Attempts to connect to the element's view provider and if it does
    /// expose the view provider will tell the proxy to present the view.
    async fn present_view_for_element(
        &self,
        element: &mut Element,
        initial_annotations: Vec<felement::Annotation>,
        annotation_controller: Option<ClientEnd<felement::AnnotationControllerMarker>>,
    ) -> Result<(fuiapp::ViewProviderProxy, felement::ViewControllerProxy), Error> {
        let view_provider = element.connect_to_protocol::<fuiapp::ViewProviderMarker>()?;
        let scenic::ViewRefPair { mut control_ref, mut view_ref } = scenic::ViewRefPair::new()?;
        let view_ref_dup = scenic::duplicate_view_ref(&view_ref)?;
        let mut view_spec = felement::ViewSpec {
            annotations: Some(initial_annotations),
            view_ref: Some(view_ref_dup),
            ..felement::ViewSpec::EMPTY
        };

        if self.scenic_uses_flatland {
            let link_token_pair = scenic::flatland::ViewCreationTokenPair::new()?;

            view_provider.create_view2(fuiapp::CreateView2Args {
                view_creation_token: Some(link_token_pair.view_creation_token),
                ..fuiapp::CreateView2Args::EMPTY
            })?;

            view_spec.viewport_creation_token = Some(link_token_pair.viewport_creation_token);

            // TODO(fxbug.dev/86649): Instead of passing |view_ref_dup| we should let the child send
            // us the one they minted for Flatland.
        } else {
            let token_pair = scenic::ViewTokenPair::new()?;

            // note: this call will never fail since connecting to a service is
            // always successful and create_view doesn't have a return value.
            // If there is no view provider, the view_holder_token will be invalidated
            // and the presenter can choose to close the view controller if it
            // only wants to allow graphical views.
            view_provider.create_view_with_view_ref(
                token_pair.view_token.value,
                &mut control_ref,
                &mut view_ref,
            )?;

            view_spec.view_holder_token = Some(token_pair.view_holder_token);
        }

        let (view_controller_proxy, server_end) =
            fidl::endpoints::create_proxy::<felement::ViewControllerMarker>()?;

        if let Some(graphical_presenter_connector) = &self.graphical_presenter_connector {
            graphical_presenter_connector
                .connect()?
                .present_view(view_spec, annotation_controller, Some(server_end))
                .await?
                .map_err(|err| format_err!("Failed to present element: {:?}", err))?;
        }

        Ok((view_provider, view_controller_proxy))
    }

    async fn handle_propose_element(
        &self,
        spec: felement::Spec,
        element_controller: Option<ServerEnd<felement::ControllerMarker>>,
    ) -> Result<(), felement::ProposeElementError> {
        let component_url = spec.component_url.ok_or_else(|| {
            error!("ProposeElement() failed to launch element: spec.component_url is missing");
            felement::ProposeElementError::InvalidArgs
        })?;

        let url_key = felement::AnnotationKey {
            namespace: ELEMENT_MANAGER_NS.to_string(),
            value: "url".to_string(),
        };

        let mut initial_annotations = match spec.annotations {
            Some(annotations) => annotations,
            None => vec![],
        };

        if !initial_annotations.iter().any(|annotation| annotation.key == url_key) {
            initial_annotations.push(felement::Annotation {
                key: url_key,
                value: felement::AnnotationValue::Text(component_url.to_string()),
            });
        }

        // Create AnnotationHolder and populate the initial annotations from the Spec.
        let mut annotation_holder = AnnotationHolder::new();
        annotation_holder.update_annotations(initial_annotations, vec![]).map_err(|err| {
            error!(?err, "ProposeElement() failed to set initial annotations");
            felement::ProposeElementError::InvalidArgs
        })?;

        let mut child_name = Alphanumeric.sample_string(&mut thread_rng(), 16);
        child_name.make_ascii_lowercase();

        let mut element = self
            .launch_element(component_url, spec.additional_services, spec.arguments, &child_name)
            .await
            .map_err(|err| match err {
                ElementManagerError::NotCreated { .. } => felement::ProposeElementError::NotFound,
                err => {
                    error!(?err, "ProposeElement() failed to launch element");
                    felement::ProposeElementError::InvalidArgs
                }
            })?;

        let (annotation_controller_client_end, annotation_controller_stream) =
            create_request_stream::<felement::AnnotationControllerMarker>().unwrap();
        let initial_view_annotations = annotation_holder.get_annotations().unwrap();

        let (view_provider_proxy, view_controller_proxy) = self
            .present_view_for_element(
                &mut element,
                initial_view_annotations,
                Some(annotation_controller_client_end),
            )
            .await
            .map_err(|err| {
                // TODO(fxbug.dev/82894): ProposeElement should propagate GraphicalPresenter errors back to caller
                error!(?err, "ProposeElement() failed to present element");
                felement::ProposeElementError::InvalidArgs
            })?;

        let element_controller_stream = match element_controller {
            Some(controller) => match controller.into_stream() {
                Ok(stream) => Ok(Some(stream)),
                Err(_) => Err(felement::ProposeElementError::InvalidArgs),
            },
            None => Ok(None),
        }?;

        fasync::Task::local(run_element_until_closed(
            element,
            annotation_holder,
            element_controller_stream,
            annotation_controller_stream,
            view_provider_proxy,
            view_controller_proxy,
        ))
        .detach();

        Ok(())
    }
}

/// Runs the Element until it receives a signal to shutdown.
///
/// The Element can receive a signal to shut down from any of the
/// following:
///   - Element. The component represented by the element can close on its own.
///   - ControllerRequestStream. The element controller can signal that the element should close.
///   - ViewControllerProxy. The view controller can signal that the element can close.
///
/// The Element will shutdown when any of these signals are received.
///
/// The Element will also listen for any incoming events from the element controller and
/// forward them to the view controller.
async fn run_element_until_closed(
    element: Element,
    annotation_holder: AnnotationHolder,
    controller_stream: Option<felement::ControllerRequestStream>,
    annotation_controller_stream: felement::AnnotationControllerRequestStream,
    view_provider_proxy: fuiapp::ViewProviderProxy,
    view_controller_proxy: felement::ViewControllerProxy,
) {
    let annotation_holder = Arc::new(Mutex::new(annotation_holder));

    // This task will fall out of scope when the select!() below returns.
    let _annotation_task = fasync::Task::spawn(handle_annotation_controller_stream(
        annotation_holder.clone(),
        annotation_controller_stream,
    ));

    let moniker = format!("{}:{}", element.collection(), element.name());
    select!(
        _ = await_element_close(view_provider_proxy, moniker).fuse() => {
            // signals that a element has died without being told to close.
            // We could tell the view to dismiss here but we need to signal
            // that there was a crash. The current contract is that if the
            // view controller binding closes without a dismiss then the
            // presenter should treat this as a crash and respond accordingly.

            // We want to allow the presenter the ability to dismiss
            // the view so we tell it to dismiss and then wait for
            // the view controller stream to close.
            let _ = view_controller_proxy.dismiss();
            let timeout = fuchsia_async::Timer::new(VIEW_CONTROLLER_DISMISS_TIMEOUT.after_now());
            wait_for_view_controller_close_or_timeout(view_controller_proxy, timeout).await;
        },
        _ = wait_for_view_controller_close(view_controller_proxy.clone()).fuse() =>  {
            // signals that the presenter would like to close the element.
            // We do not need to do anything here but exit which will cause
            // the element to be dropped and will kill the component.
        },
        _ = handle_element_controller_stream(annotation_holder.clone(), controller_stream).fuse() => {
            // the proposer has decided they want to shut down the element.

            // We want to allow the presenter the ability to dismiss
            // the view so we tell it to dismiss and then wait for
            // the view controller stream to close.
            let _ = view_controller_proxy.dismiss();
            let timeout = fuchsia_async::Timer::new(VIEW_CONTROLLER_DISMISS_TIMEOUT.after_now());
            wait_for_view_controller_close_or_timeout(view_controller_proxy, timeout).await;
        },
    );
}

/// Waits for the element to signal that it closed, via a component stopped
/// event, or (for CFv1 only) by closing its directory channel. (Note that a
/// component process that exits will trigger one of these events.)
async fn await_element_close(view_provider: fuiapp::ViewProviderProxy, moniker: String) {
    let channel = view_provider.into_channel().unwrap_or_else(|e| {
        panic!("could not get ViewProvider channel for moniker: {moniker}: {:?}", e)
    });
    debug!(%moniker, "await_element_close()");
    let _ =
        fasync::OnSignals::new(&channel.as_handle_ref(), zx::Signals::CHANNEL_PEER_CLOSED).await;
    debug!(%moniker, "element closed");
}

/// Waits for this view controller to close.
async fn wait_for_view_controller_close(proxy: felement::ViewControllerProxy) {
    let stream = proxy.take_event_stream();
    let _ = stream.collect::<Vec<_>>().await;
}

/// Waits for this view controller to close.
async fn wait_for_view_controller_close_or_timeout(
    proxy: felement::ViewControllerProxy,
    timeout: fasync::Timer,
) {
    let _ = futures::future::select(timeout, Box::pin(wait_for_view_controller_close(proxy))).await;
}

/// Handles element Controller protocol requests.
///
/// If the `ControllerRequestStream` is None then this future will never resolve.
///
/// # Parameters
/// - `annotation_holder`: The [`AnnotationHolder`] for the controlled element.
/// - `stream`: The stream of [`Controller`] requests.
async fn handle_element_controller_stream(
    annotation_holder: Arc<Mutex<AnnotationHolder>>,
    stream: Option<felement::ControllerRequestStream>,
) {
    // TODO(fxbug.dev/83326): Unify this with handle_annotation_controller_stream(), once FIDL
    // provides a mechanism to do so.
    if let Some(mut stream) = stream {
        let mut watch_subscriber = annotation_holder.lock().await.new_watch_subscriber();
        while let Ok(Some(request)) = stream.try_next().await {
            match request {
                felement::ControllerRequest::UpdateAnnotations {
                    annotations_to_set,
                    annotations_to_delete,
                    responder,
                } => {
                    let result = annotation_holder
                        .lock()
                        .await
                        .update_annotations(annotations_to_set, annotations_to_delete);
                    match result {
                        Ok(()) => responder.send(&mut Ok(())),
                        Err(AnnotationError::Update(e)) => responder.send(&mut Err(e)),
                        Err(_) => unreachable!(),
                    }
                    .ok();
                }
                felement::ControllerRequest::GetAnnotations { responder } => {
                    let result = annotation_holder.lock().await.get_annotations();
                    match result {
                        Ok(annotation_vec) => responder.send(&mut Ok(annotation_vec)),
                        Err(AnnotationError::Get(e)) => responder.send(&mut Err(e)),
                        Err(_) => unreachable!(),
                    }
                    .ok();
                }
                felement::ControllerRequest::WatchAnnotations { responder } => {
                    if let Err(e) = watch_subscriber
                        .watch_annotations(WatchResponder::ElementController(responder))
                    {
                        // There is already a `WatchAnnotations` request pending for the client. Since the responder gets dropped (TODO(fxbug.dev/94602)), the connection will be closed to indicate unexpected client behavior.
                        error!("ControllerRequest error: {}. Dropping connection", e);
                        stream.control_handle().shutdown_with_epitaph(zx::Status::BAD_STATE);
                        return;
                    }
                }
            }
        }
    } else {
        // If the element controller is None then we never exit and rely
        // on the other futures to signal the end of the element.
        futures::future::pending::<()>().await;
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{CollectionConfig, ElementManager, ElementManagerError},
        fidl::{endpoints::spawn_stream_handler, prelude::*},
        fidl_fuchsia_component as fcomponent, fidl_fuchsia_io as fio, fidl_fuchsia_sys as fsys,
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::{channel::mpsc::channel, prelude::*},
        lazy_static::lazy_static,
        maplit::hashmap,
        session_testing::{spawn_directory_server, spawn_noop_directory_server},
        test_util::Counter,
    };

    fn example_collection_config() -> CollectionConfig {
        CollectionConfig {
            url_to_collection: hashmap! {
                "https://special_component.cm".to_string() => "special_collection".to_string(),
            },
            default_collection: "elements".to_string(),
        }
    }

    /// Tests that launching a component with a cmx file successfully returns an Element
    /// with outgoing directory routing appropriate for v1 components.
    #[fuchsia::test]
    async fn launch_v1_element_success() {
        lazy_static! {
            static ref CREATE_COMPONENT_CALL_COUNT: Counter = Counter::new(0);
        }

        const ELEMENT_COUNT: usize = 1;

        let component_url = "test_url.cmx";
        let child_name = "child";

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests as it's only used for v2 components")
        })
        .unwrap();

        let (directory_open_sender, directory_open_receiver) = channel::<String>(1);
        let directory_request_handler = move |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path: capability_path, .. } => {
                let mut result_sender = directory_open_sender.clone();
                fasync::Task::spawn(async move {
                    let _ = result_sender.send(capability_path).await;
                })
                .detach()
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let (create_component_sender, create_component_receiver) = channel::<()>(ELEMENT_COUNT);
        let launcher = spawn_stream_handler(move |launcher_request| {
            let directory_request_handler = directory_request_handler.clone();
            let mut result_sender = create_component_sender.clone();
            async move {
                match launcher_request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info: fsys::LaunchInfo { url, directory_request, .. },
                        ..
                    } => {
                        assert_eq!(url, component_url);
                        spawn_directory_server(
                            directory_request.unwrap(),
                            directory_request_handler,
                        );
                        fasync::Task::spawn(async move {
                            CREATE_COMPONENT_CALL_COUNT.inc();
                            let _ = result_sender.send(()).await;
                        })
                        .detach()
                    }
                }
            }
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);
        let result =
            element_manager.launch_element(component_url.to_string(), None, None, child_name).await;
        let element = result.unwrap();
        assert!(format!("{:?}", element).contains(component_url));

        // Verify that the CreateComponent was actually called.
        create_component_receiver.into_future().await;
        assert_eq!(CREATE_COMPONENT_CALL_COUNT.get(), ELEMENT_COUNT);

        // Now use the element api to open a service in the element's outgoing dir. Verify
        // that the directory channel received the request with the correct path.
        let (_client_channel, server_channel) = zx::Channel::create();
        let _ = element.connect_to_named_protocol_with_channel("myProtocol", server_channel);
        let open_paths = directory_open_receiver.take(1).collect::<Vec<_>>().await;
        // While a standard v1 component publishes its services to "svc/...", LaunchInfo.directory_request,
        // which is mocked in the test setup, points to the "svc" subdirectory in production. This is why
        // the expected path does not contain the "svc/" prefix.
        assert_eq!(vec!["myProtocol"], open_paths);
    }

    /// Tests that launching multiple v1 components (without "*.cm") successfully returns [`Ok`].
    #[fuchsia::test]
    async fn launch_multiple_v1_element_success() {
        lazy_static! {
            static ref CREATE_COMPONENT_CALL_COUNT: Counter = Counter::new(0);
        }

        const ELEMENT_COUNT: usize = 2;

        let (sender, receiver) = channel::<()>(ELEMENT_COUNT);

        let a_component_url = "a_url.cmx";
        let a_child_name = "a_child";

        let b_component_url = "https://google.com";
        let b_child_name = "b_child";

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests as it's only used for v2 components")
        })
        .unwrap();

        let launcher = spawn_stream_handler(move |launcher_request| {
            let mut result_sender = sender.clone();
            async move {
                let _ = &launcher_request;
                match launcher_request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info: fsys::LaunchInfo { .. },
                        ..
                    } => fasync::Task::spawn(async move {
                        CREATE_COMPONENT_CALL_COUNT.inc();
                        let _ = result_sender.send(()).await.expect("Could not create component.");
                    })
                    .detach(),
                }
            }
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);
        assert!(element_manager
            .launch_element(a_component_url.to_string(), None, None, a_child_name)
            .await
            .is_ok());
        assert!(element_manager
            .launch_element(b_component_url.to_string(), None, None, b_child_name)
            .await
            .is_ok());

        // Verify that the CreateComponent was actually called.
        receiver.into_future().await;
        assert_eq!(CREATE_COMPONENT_CALL_COUNT.get(), ELEMENT_COUNT);
    }

    /// Tests that launching an element with a *.cmx URL and `additional_services` ServiceList
    /// passes the services to the component Launcher.
    #[fuchsia::test]
    async fn launch_v1_element_with_additional_services() {
        lazy_static! {
            static ref CREATE_COMPONENT_CALL_COUNT: Counter = Counter::new(0);
        }

        let component_url = "test_url.cmx";
        let child_name = "child";

        const ELEMENT_COUNT: usize = 1;

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests as it's only used for v2 components")
        })
        .unwrap();

        // Spawn a directory server from which the element can connect to services.
        let (directory_open_sender, directory_open_receiver) = channel::<String>(1);
        let directory_request_handler = move |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path: capability_path, .. } => {
                let mut result_sender = directory_open_sender.clone();
                fasync::Task::spawn(async move {
                    let _ = result_sender.send(capability_path).await;
                })
                .detach()
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let (dir_client, dir_server) = fidl::endpoints::create_endpoints().unwrap();
        spawn_directory_server(dir_server, directory_request_handler.clone());

        // The element receives the client side handle to the directory
        // through `additional_services.host_directory`.
        let service_name = "myService";
        let additional_services = fsys::ServiceList {
            names: vec![service_name.to_string()],
            host_directory: Some(dir_client),
            provider: None,
        };

        let (create_component_sender, create_component_receiver) = channel::<()>(ELEMENT_COUNT);
        let launcher = spawn_stream_handler(move |launcher_request| {
            let mut result_sender = create_component_sender.clone();
            async move {
                match launcher_request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info: fsys::LaunchInfo { url, additional_services, .. },
                        ..
                    } => {
                        assert_eq!(url, component_url);

                        assert!(additional_services.is_some());
                        let services = additional_services.unwrap();
                        assert!(services.host_directory.is_some());
                        let host_directory = services.host_directory.unwrap();

                        assert_eq!(vec![service_name.to_string()], services.names);

                        // Connect to the service hosted in `additional_services.host_directory`.
                        let (_client_channel, server_channel) = zx::Channel::create();
                        fdio::service_connect_at(
                            host_directory.channel(),
                            service_name,
                            server_channel,
                        )
                        .expect("could not connect to service");

                        fasync::Task::spawn(async move {
                            CREATE_COMPONENT_CALL_COUNT.inc();
                            let _ = result_sender.send(()).await;
                        })
                        .detach()
                    }
                }
            }
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);
        let result = element_manager
            .launch_element(component_url.to_string(), Some(additional_services), None, child_name)
            .await;
        let element = result.unwrap();
        assert!(format!("{:?}", element).contains(component_url));

        // Verify that the CreateComponent was actually called.
        create_component_receiver.into_future().await;
        assert_eq!(CREATE_COMPONENT_CALL_COUNT.get(), ELEMENT_COUNT);

        // Verify that the element opened a path in `host_directory` that matches the
        // service name as a result of connecting to the service.
        let open_paths = directory_open_receiver.take(1).collect::<Vec<_>>().await;
        assert_eq!(vec![service_name], open_paths);
    }

    /// Tests that launching an element with a *.cmx URL and `arguments` passes the arguments
    /// to the component Launcher.
    #[fuchsia::test]
    async fn launch_v1_element_with_arguments() {
        lazy_static! {
            static ref CREATE_COMPONENT_CALL_COUNT: Counter = Counter::new(0);
            static ref TEST_ARGS: Vec<String> = vec!["hello".to_string(), "world".to_string()];
        }

        let component_url = "test_url.cmx";
        let child_name = "child";

        const ELEMENT_COUNT: usize = 1;

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests as it's only used for v2 components")
        })
        .unwrap();

        let (create_component_sender, create_component_receiver) = channel::<()>(ELEMENT_COUNT);
        let launcher = spawn_stream_handler(move |launcher_request| {
            let mut result_sender = create_component_sender.clone();
            async move {
                match launcher_request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info: fsys::LaunchInfo { url, arguments, .. },
                        ..
                    } => {
                        assert_eq!(url, component_url);

                        assert!(arguments.is_some());
                        assert_eq!(arguments.unwrap(), *TEST_ARGS);

                        fasync::Task::spawn(async move {
                            CREATE_COMPONENT_CALL_COUNT.inc();
                            let _ = result_sender.send(()).await;
                        })
                        .detach()
                    }
                }
            }
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result = element_manager
            .launch_element(component_url.to_string(), None, Some(TEST_ARGS.clone()), child_name)
            .await;
        let element = result.unwrap();
        assert!(format!("{:?}", element).contains(component_url));

        // Verify that the CreateComponent was actually called.
        create_component_receiver.into_future().await;
        assert_eq!(CREATE_COMPONENT_CALL_COUNT.get(), ELEMENT_COUNT);
    }

    /// Tests that launching a *.cm element successfully returns an Element with
    /// outgoing directory routing appropriate for v2 components.
    #[fuchsia::test]
    async fn launch_v2_element_success() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";
        let child_name = "child";

        let (directory_open_sender, directory_open_receiver) = channel::<String>(1);
        let directory_request_handler = move |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path: capability_path, .. } => {
                let mut result_sender = directory_open_sender.clone();
                fasync::Task::spawn(async move {
                    let _ = result_sender.send(capability_path).await;
                })
                .detach()
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let realm = spawn_stream_handler(move |realm_request| {
            let directory_request_handler = directory_request_handler.clone();
            async move {
                match realm_request {
                    fcomponent::RealmRequest::CreateChild {
                        collection,
                        decl,
                        args: _,
                        responder,
                    } => {
                        assert_eq!(decl.url.unwrap(), component_url);
                        assert_eq!(decl.name.unwrap(), child_name);
                        assert_eq!(&collection.name, "elements");

                        let _ = responder.send(&mut Ok(()));
                    }
                    fcomponent::RealmRequest::OpenExposedDir { child, exposed_dir, responder } => {
                        assert_eq!(child.collection, Some("elements".to_string()));
                        spawn_directory_server(exposed_dir, directory_request_handler);
                        let _ = responder.send(&mut Ok(()));
                    }
                    _ => panic!("Realm handler received an unexpected request"),
                }
            }
        })
        .unwrap();

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result =
            element_manager.launch_element(component_url.to_string(), None, None, child_name).await;
        let element = result.unwrap();

        // Now use the element api to open a service in the element's outgoing dir. Verify
        // that the directory channel received the request with the correct path.
        let (_client_channel, server_channel) = zx::Channel::create();
        let _ = element.connect_to_named_protocol_with_channel("myProtocol", server_channel);
        let open_paths = directory_open_receiver.take(2).collect::<Vec<_>>().await;
        assert_eq!(vec![fcomponent::BinderMarker::DEBUG_NAME, "myProtocol"], open_paths);
    }

    #[fuchsia::test]
    async fn launch_v2_element_in_special_collection_success() {
        let component_url = "https://special_component.cm";
        let child_name = "child";

        let (directory_open_sender, directory_open_receiver) = channel::<String>(1);
        let directory_request_handler = move |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path: capability_path, .. } => {
                let mut result_sender = directory_open_sender.clone();
                fasync::Task::spawn(async move {
                    let _ = result_sender.send(capability_path).await;
                })
                .detach()
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let realm = spawn_stream_handler(move |realm_request| {
            let directory_request_handler = directory_request_handler.clone();
            async move {
                match realm_request {
                    fcomponent::RealmRequest::CreateChild {
                        collection,
                        decl,
                        args: _,
                        responder,
                    } => {
                        assert_eq!(decl.url.unwrap(), component_url);
                        assert_eq!(decl.name.unwrap(), child_name);
                        assert_eq!(&collection.name, "special_collection");

                        let _ = responder.send(&mut Ok(()));
                    }
                    fcomponent::RealmRequest::OpenExposedDir { child, exposed_dir, responder } => {
                        assert_eq!(child.collection, Some("special_collection".to_string()));
                        spawn_directory_server(exposed_dir, directory_request_handler);
                        let _ = responder.send(&mut Ok(()));
                    }
                    _ => panic!("Realm handler received an unexpected request"),
                }
            }
        })
        .unwrap();

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result =
            element_manager.launch_element(component_url.to_string(), None, None, child_name).await;
        let element = result.unwrap();

        // Now use the element api to open a service in the element's outgoing dir. Verify
        // that the directory channel received the request with the correct path.
        let (_client_channel, server_channel) = zx::Channel::create();
        let _ = element.connect_to_named_protocol_with_channel("myProtocol", server_channel);
        let open_paths = directory_open_receiver.take(2).collect::<Vec<_>>().await;
        assert_eq!(vec![fcomponent::BinderMarker::DEBUG_NAME, "myProtocol"], open_paths);
    }

    /// Tests that adding a .cm element does not use fuchsia.sys.Launcher.
    #[fuchsia::test]
    async fn launch_element_success_not_use_launcher() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";
        let child_name = "child";

        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild { collection, decl, args: _, responder } => {
                    assert_eq!(decl.url.unwrap(), component_url);
                    assert_eq!(decl.name.unwrap(), child_name);
                    assert_eq!(&collection.name, "elements");

                    let _ = responder.send(&mut Ok(()));
                }
                fcomponent::RealmRequest::OpenExposedDir { child, exposed_dir, responder } => {
                    assert_eq!(child.collection, Some("elements".to_string()));
                    spawn_noop_directory_server(exposed_dir);
                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        assert!(element_manager
            .launch_element(component_url.to_string(), None, None, child_name)
            .await
            .is_ok());
    }

    /// Tests that launching a CFv1 element with a ServiceList that specifies a `provider`
    /// returns `ElementManagerError::InvalidServiceList`.
    #[fuchsia::test]
    async fn launch_v1_element_with_service_list_provider() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cmx";

        let (channel, _) = fidl::Channel::create();
        let provider = fidl::endpoints::ClientEnd::<fsys::ServiceProviderMarker>::new(channel);

        // This ServiceList is invalid because it specifies a `provider` that is not None.
        let additional_services = fsys::ServiceList {
            names: vec!["fuchsia.service.Foo".to_string()],
            host_directory: None,
            provider: Some(provider),
        };

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as the spec is invalid")
        })
        .unwrap();

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests since the child won't be created")
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result = element_manager
            .launch_element(component_url.to_string(), Some(additional_services), None, "")
            .await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::invalid_service_list("", "elements")
        );
    }

    /// Tests that launching a *.cm element with a spec that contains `additional_services`
    /// returns `ProposeElementError::AdditionalServicesNotSupported`
    #[fuchsia::test]
    async fn launch_v2_element_with_additional_services() {
        // This is a CFv2 component URL. `additional_services` is only supported for v1 components.
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let (channel, _) = fidl::endpoints::create_endpoints().unwrap();
        let additional_services = fsys::ServiceList {
            names: vec!["fuchsia.service.Foo".to_string()],
            host_directory: Some(channel),
            provider: None,
        };

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests since the child won't be created")
        })
        .unwrap();
        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result = element_manager
            .launch_element(component_url.to_string(), Some(additional_services), None, "")
            .await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::additional_services_not_supported("", "elements")
        );
    }

    /// Tests that launching a *.cm element with a spec that contains `arguments`
    /// returns `ProposeElementError::ArgumentsNotSupported`
    #[fuchsia::test]
    async fn launch_v2_element_with_arguments() {
        // This is a CFv2 component URL. `arguments` is only supported for v1 components.
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let arguments = vec!["hello".to_string(), "world".to_string()];

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let realm = spawn_stream_handler(move |_realm_request| async move {
            panic!("Realm should not receive any requests since the child won't be created")
        })
        .unwrap();
        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result = element_manager
            .launch_element(component_url.to_string(), None, Some(arguments), "")
            .await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::arguments_not_supported("", "elements")
        );
    }

    /// Tests that launching an element which is not successfully created in the realm returns an
    /// appropriate error.
    #[fuchsia::test]
    async fn launch_element_create_error_internal() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        // The following match errors if it sees a bind request: since the child was not created
        // successfully the bind should not be called.
        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Err(fcomponent::Error::Internal));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();
        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result =
            element_manager.launch_element(component_url.to_string(), None, None, "").await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::not_created(
                "",
                "elements",
                component_url,
                fcomponent::Error::Internal
            )
        );
    }

    /// Tests that adding an element which is not successfully created in the realm returns an
    /// appropriate error.
    #[fuchsia::test]
    async fn launch_element_create_error_no_space() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        // The following match errors if it sees a bind request: since the child was not created
        // successfully the bind should not be called.
        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Err(fcomponent::Error::ResourceUnavailable));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();
        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result =
            element_manager.launch_element(component_url.to_string(), None, None, "").await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::not_created(
                "",
                "elements",
                component_url,
                fcomponent::Error::ResourceUnavailable
            )
        );
    }

    /// Tests that adding an element which can't have its exposed directory opened
    /// returns an appropriate error.
    #[fuchsia::test]
    async fn open_exposed_dir_error() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Ok(()));
                }
                fcomponent::RealmRequest::OpenExposedDir {
                    child: _,
                    exposed_dir: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Err(fcomponent::Error::InstanceCannotResolve));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        let result =
            element_manager.launch_element(component_url.to_string(), None, None, "").await;
        assert!(result.is_err());
        assert_eq!(
            result.err().unwrap(),
            ElementManagerError::exposed_dir_not_opened(
                "",
                "elements",
                component_url,
                fcomponent::Error::InstanceCannotResolve,
            )
        );
    }

    /// Tests that adding an element which is not successfully bound in the realm returns an
    /// appropriate error.
    #[fuchsia::test]
    async fn launch_element_bind_error() {
        let component_url = "fuchsia-pkg://fuchsia.com/simple_element#meta/simple_element.cm";

        let launcher = spawn_stream_handler(move |_launcher_request| async move {
            panic!("Launcher should not receive any requests as it's only used for v1 components")
        })
        .unwrap();

        let realm = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Ok(()));
                }
                fcomponent::RealmRequest::OpenExposedDir { child: _, exposed_dir, responder } => {
                    // Close the incoming channel before responding to avoid race conditions.
                    let () = std::mem::drop(exposed_dir);
                    let () = responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();
        let element_manager =
            ElementManager::new(realm, None, launcher, example_collection_config(), false);

        assert_matches::assert_matches!(
            element_manager.launch_element(component_url.to_string(), None, None, "").await,
            Err(err) => {
                assert_eq!(err, ElementManagerError::not_bound(
                    "",
                    "elements",
                    component_url,
                    "A FIDL client's channel to the service (anonymous) Directory was closed: PEER_CLOSED"
                ));
            }
        );
    }
}
