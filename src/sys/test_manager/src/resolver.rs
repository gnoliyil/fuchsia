// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    diagnostics_log as flog,
    fidl::endpoints::ProtocolMarker,
    fidl_fuchsia_component_resolution as fresolution, fidl_fuchsia_logger as flogger,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::LocalComponentHandles,
    fuchsia_url::{ComponentUrl, PackageUrl},
    futures::{StreamExt, TryStreamExt},
    itertools::Itertools,
    std::{collections::HashSet, sync::Arc},
    tracing::warn,
};

type LogSubscriber = dyn tracing::Subscriber + std::marker::Send + std::marker::Sync + 'static;

// The list of non-hermetic packages allowed to resolved by a test.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AllowedPackages {
    // Strict list of allowed packages.
    pkgs: Arc<HashSet<String>>,
}

impl AllowedPackages {
    pub fn zero_allowed_pkgs() -> Self {
        Self { pkgs: HashSet::new().into() }
    }

    pub fn from_iter<I>(iter: I) -> Self
    where
        I: IntoIterator<Item = String>,
    {
        Self { pkgs: Arc::new(HashSet::from_iter(iter)) }
    }
}

async fn validate_hermetic_package(
    component_url_str: &str,
    subscriber: Arc<LogSubscriber>,
    hermetic_test_package_name: &String,
    other_allowed_packages: &AllowedPackages,
) -> Result<(), fresolution::ResolverError> {
    let component_url = ComponentUrl::parse(component_url_str).map_err(|err| {
        warn!("cannot parse {}, {:?}", component_url_str, err);
        fresolution::ResolverError::InvalidArgs
    })?;

    match component_url.package_url() {
        PackageUrl::Absolute(pkg_url) => {
            let package_name = pkg_url.name();
            if hermetic_test_package_name != package_name.as_ref()
                && !other_allowed_packages.pkgs.contains(package_name.as_ref())
            {
                let s = format!("failed to resolve component {}: package {} is not in the test package allowlist: '{}, {}'
                \nSee https://fuchsia.dev/fuchsia-src/development/testing/components/test_runner_framework?hl=en#hermetic-resolver
                for more information.",
                &component_url_str, package_name, hermetic_test_package_name, other_allowed_packages.pkgs.iter().join(", "));
                // log in both test managers log sink and test's log sink so that it is easy to retrieve.
                tracing::subscriber::with_default(subscriber, || {
                    warn!("{}", s);
                });
                warn!("{}", s);
                return Err(fresolution::ResolverError::PackageNotFound);
            }
        }
        PackageUrl::Relative(_url) => {
            // don't do anything as we don't restrict relative urls.
        }
    }
    Ok(())
}

async fn serve_resolver(
    mut stream: fresolution::ResolverRequestStream,
    subscriber: Arc<LogSubscriber>,
    hermetic_test_package_name: Arc<String>,
    other_allowed_packages: AllowedPackages,
    full_resolver: Arc<fresolution::ResolverProxy>,
) {
    while let Some(request) = stream.try_next().await.expect("failed to serve component resolver") {
        match request {
            fresolution::ResolverRequest::Resolve { component_url, responder } => {
                let result = if let Err(err) = validate_hermetic_package(
                    &component_url,
                    subscriber.clone(),
                    &hermetic_test_package_name,
                    &other_allowed_packages,
                )
                .await
                {
                    Err(err)
                } else {
                    let subscriber = subscriber.clone();
                    full_resolver.resolve(&component_url).await.unwrap_or_else(|err| {
                        tracing::subscriber::with_default(subscriber, || {
                            warn!("failed to resolve component {}: {:?}", component_url, err);
                        });
                        Err(fresolution::ResolverError::Internal)
                    })
                };
                if let Err(e) = responder.send(result) {
                    warn!("Failed sending load response for {}: {}", component_url, e);
                }
            }
            fresolution::ResolverRequest::ResolveWithContext {
                component_url,
                context,
                responder,
            } => {
                // We don't need to worry about validating context because it should have
                // been produced by Resolve call above.
                let result = if let Err(err) = validate_hermetic_package(
                    &component_url,
                    subscriber.clone(),
                    &hermetic_test_package_name,
                    &other_allowed_packages,
                )
                .await
                {
                    Err(err)
                } else {
                    let subscriber = subscriber.clone();
                    full_resolver
                        .resolve_with_context(&component_url, &context)
                        .await
                        .unwrap_or_else(|err| {
                            tracing::subscriber::with_default(subscriber, || {
                                warn!(
                                    "failed to resolve component {} with context {:?}: {:?}",
                                    component_url, context, err
                                );
                            });
                            Err(fresolution::ResolverError::Internal)
                        })
                };
                if let Err(e) = responder.send(result) {
                    warn!("Failed sending load response for {}: {}", component_url, e);
                }
            }
        }
    }
}

pub async fn serve_hermetic_resolver(
    handles: LocalComponentHandles,
    hermetic_test_package_name: Arc<String>,
    other_allowed_packages: AllowedPackages,
    full_resolver: Arc<fresolution::ResolverProxy>,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    let mut tasks = vec![];
    let log_proxy = handles
        .connect_to_named_protocol::<flogger::LogSinkMarker>(flogger::LogSinkMarker::DEBUG_NAME)?;
    let tags = ["test_resolver"];
    let log_publisher = match flog::Publisher::new(
        flog::PublisherOptions::default().tags(&tags).use_log_sink(log_proxy),
    ) {
        Ok(publisher) => Arc::new(publisher) as Arc<LogSubscriber>,
        Err(e) => {
            warn!("Error creating log publisher for resolver: {:?}", e);
            Arc::new(tracing::subscriber::NoSubscriber::default()) as Arc<LogSubscriber>
        }
    };

    fs.dir("svc").add_fidl_service(move |stream: fresolution::ResolverRequestStream| {
        let full_resolver = full_resolver.clone();
        let hermetic_test_package_name = hermetic_test_package_name.clone();
        let other_allowed_packages = other_allowed_packages.clone();
        let log_publisher = log_publisher.clone();
        tasks.push(fasync::Task::local(async move {
            serve_resolver(
                stream,
                log_publisher,
                hermetic_test_package_name,
                other_allowed_packages,
                full_resolver,
            )
            .await;
        }));
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::create_proxy_and_stream;
    use maplit::hashset;

    async fn respond_to_resolve_requests(mut stream: fresolution::ResolverRequestStream) {
        while let Some(request) =
            stream.try_next().await.expect("failed to serve component mock resolver")
        {
            match request {
                fresolution::ResolverRequest::Resolve { component_url, responder } => {
                    match component_url.as_str() {
                        "fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm"
                        | "fuchsia-pkg://fuchsia.com/package-three#meta/comp.cm"
                        | "fuchsia-pkg://fuchsia.com/package-four#meta/comp.cm" => {
                            responder.send(Ok(fresolution::Component::default()))
                        }
                        "fuchsia-pkg://fuchsia.com/package-two#meta/comp.cm" => {
                            responder.send(Err(fresolution::ResolverError::ResourceUnavailable))
                        }
                        _ => responder.send(Err(fresolution::ResolverError::Internal)),
                    }
                    .expect("failed sending response");
                }
                fresolution::ResolverRequest::ResolveWithContext {
                    component_url,
                    context: _,
                    responder,
                } => {
                    match component_url.as_str() {
                        "fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm" | "name#resource" => {
                            responder.send(Ok(fresolution::Component::default()))
                        }
                        _ => responder.send(Err(fresolution::ResolverError::PackageNotFound)),
                    }
                    .expect("failed sending response");
                }
            }
        }
    }

    // Run hermetic resolver
    fn run_resolver(
        hermetic_test_package_name: Arc<String>,
        other_allowed_packages: AllowedPackages,
        mock_full_resolver: Arc<fresolution::ResolverProxy>,
    ) -> (fasync::Task<()>, fresolution::ResolverProxy) {
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fresolution::ResolverMarker>().unwrap();
        let subscriber = tracing::subscriber::NoSubscriber::default();
        let task = fasync::Task::local(async move {
            serve_resolver(
                stream,
                Arc::new(subscriber),
                hermetic_test_package_name,
                other_allowed_packages,
                mock_full_resolver,
            )
            .await;
        });
        (task, proxy)
    }

    #[fuchsia::test]
    async fn test_successful_resolve() {
        let pkg_name = "package-one".to_string();

        let (resolver_proxy, resolver_request_stream) =
            create_proxy_and_stream::<fresolution::ResolverMarker>()
                .expect("failed to create mock full resolver proxy");
        let _full_resolver_task = fasync::Task::spawn(async move {
            respond_to_resolve_requests(resolver_request_stream).await;
        });

        let (_task, hermetic_resolver_proxy) = run_resolver(
            pkg_name.into(),
            AllowedPackages::zero_allowed_pkgs(),
            Arc::new(resolver_proxy),
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm")
                .await
                .unwrap(),
            Ok(fresolution::Component::default())
        );
        let mock_context = fresolution::Context { bytes: vec![0] };
        assert_eq!(
            hermetic_resolver_proxy
                .resolve_with_context("name#resource", &mock_context)
                .await
                .unwrap(),
            Ok(fresolution::Component::default())
        );
        assert_eq!(
            hermetic_resolver_proxy
                .resolve_with_context("name#not_found", &mock_context)
                .await
                .unwrap(),
            Err(fresolution::ResolverError::PackageNotFound)
        );
        assert_eq!(
            hermetic_resolver_proxy
                .resolve_with_context(
                    "fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm",
                    &mock_context
                )
                .await
                .unwrap(),
            Ok(fresolution::Component::default())
        );
    }

    #[fuchsia::test]
    async fn drop_connection_on_resolve() {
        let pkg_name = "package-one".to_string();

        let (resolver_proxy, resolver_request_stream) =
            create_proxy_and_stream::<fresolution::ResolverMarker>()
                .expect("failed to create mock full resolver proxy");
        let _full_resolver_task = fasync::Task::spawn(async move {
            respond_to_resolve_requests(resolver_request_stream).await;
        });

        let (_task, hermetic_resolver_proxy) = run_resolver(
            pkg_name.into(),
            AllowedPackages::zero_allowed_pkgs(),
            Arc::new(resolver_proxy),
        );

        let _ =
            hermetic_resolver_proxy.resolve("fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm");
        drop(hermetic_resolver_proxy); // code should not crash
    }

    #[fuchsia::test]
    async fn test_package_not_allowed() {
        let (resolver_proxy, _) = create_proxy_and_stream::<fresolution::ResolverMarker>()
            .expect("failed to create mock full resolver proxy");

        let (_task, hermetic_resolver_proxy) = run_resolver(
            "package-two".to_string().into(),
            AllowedPackages::zero_allowed_pkgs(),
            Arc::new(resolver_proxy),
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm")
                .await
                .unwrap(),
            Err(fresolution::ResolverError::PackageNotFound)
        );
        let mock_context = fresolution::Context { bytes: vec![0] };
        assert_eq!(
            hermetic_resolver_proxy
                .resolve_with_context(
                    "fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm",
                    &mock_context
                )
                .await
                .unwrap(),
            Err(fresolution::ResolverError::PackageNotFound)
        );
    }

    #[fuchsia::test]
    async fn other_packages_allowed() {
        let (resolver_proxy, resolver_request_stream) =
            create_proxy_and_stream::<fresolution::ResolverMarker>()
                .expect("failed to create mock full resolver proxy");

        let list = hashset!("package-three".to_string(), "package-four".to_string());

        let _full_resolver_task = fasync::Task::spawn(async move {
            respond_to_resolve_requests(resolver_request_stream).await;
        });

        let (_task, hermetic_resolver_proxy) = run_resolver(
            "package-two".to_string().into(),
            AllowedPackages::from_iter(list),
            Arc::new(resolver_proxy),
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-one#meta/comp.cm")
                .await
                .unwrap(),
            Err(fresolution::ResolverError::PackageNotFound)
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-three#meta/comp.cm")
                .await
                .unwrap(),
            Ok(fresolution::Component::default())
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-four#meta/comp.cm")
                .await
                .unwrap(),
            Ok(fresolution::Component::default())
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-two#meta/comp.cm")
                .await
                .unwrap(),
            // we return this error from our mock resolver for package-two.
            Err(fresolution::ResolverError::ResourceUnavailable)
        );
    }

    #[fuchsia::test]
    async fn test_failed_resolve() {
        let (resolver_proxy, resolver_request_stream) =
            create_proxy_and_stream::<fresolution::ResolverMarker>()
                .expect("failed to create mock full resolver proxy");
        let _full_resolver_task = fasync::Task::spawn(async move {
            respond_to_resolve_requests(resolver_request_stream).await;
        });

        let pkg_name = "package-two".to_string();
        let (_task, hermetic_resolver_proxy) = run_resolver(
            pkg_name.into(),
            AllowedPackages::zero_allowed_pkgs(),
            Arc::new(resolver_proxy),
        );

        assert_eq!(
            hermetic_resolver_proxy
                .resolve("fuchsia-pkg://fuchsia.com/package-two#meta/comp.cm")
                .await
                .unwrap(),
            Err(fresolution::ResolverError::ResourceUnavailable)
        );
    }

    #[fuchsia::test]
    async fn test_invalid_url() {
        let (resolver_proxy, _) = create_proxy_and_stream::<fresolution::ResolverMarker>()
            .expect("failed to create mock full resolver proxy");

        let pkg_name = "package-two".to_string();
        let (_task, hermetic_resolver_proxy) = run_resolver(
            pkg_name.into(),
            AllowedPackages::zero_allowed_pkgs(),
            Arc::new(resolver_proxy),
        );

        assert_eq!(
            hermetic_resolver_proxy.resolve("invalid_url").await.unwrap(),
            Err(fresolution::ResolverError::InvalidArgs)
        );
    }
}
