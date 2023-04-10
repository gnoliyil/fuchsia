// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    assert_matches::assert_matches,
    component_debug::{
        capability,
        cli::*,
        route::{DeclType, RouteReport},
    },
    fidl_fuchsia_sys2 as fsys,
    fuchsia_component::client::connect_to_protocol,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
};

#[fuchsia::test]
async fn list() {
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();

    let mut instances = list_cmd_serialized(None, realm_query).await.unwrap();

    assert_eq!(instances.len(), 3);

    let instance = instances.remove(0);
    assert_eq!(instance.moniker, AbsoluteMoniker::root());
    assert!(instance.url.ends_with("#meta/test.cm"));
    let resolved = instance.resolved_info.unwrap();
    resolved.execution_info.unwrap();

    let instance = instances.remove(0);
    assert_eq!(instance.moniker, AbsoluteMoniker::parse_str("/echo_server").unwrap());
    assert!(instance.url.ends_with("#meta/echo_server.cm"));

    let instance = instances.remove(0);
    assert_eq!(instance.moniker, AbsoluteMoniker::parse_str("/foo").unwrap());
    assert!(instance.url.ends_with("#meta/foo.cm"));
}

#[fuchsia::test]
async fn show() {
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();

    let mut instances =
        show_cmd_serialized("test.cm".to_string(), realm_query.clone()).await.unwrap();

    assert_eq!(instances.len(), 1);
    let instance = instances.remove(0);

    assert!(instance.url.ends_with("#meta/test.cm"));
    assert!(instance.moniker.is_root());
    let resolved = instance.resolved.unwrap();
    assert!(resolved.resolved_url.ends_with("#meta/test.cm"));

    assert!(resolved.config.is_none());

    // The expected incoming capabilities are:
    // fidl.examples.routing.echo.Echo
    // fuchsia.logger.LogSink
    // fuchsia.sys2.RealmExplorer
    // fuchsia.sys2.RealmQuery
    // fuchsia.sys2.RouteValidator
    // fuchsia.foo.Bar
    assert_eq!(resolved.incoming_capabilities.len(), 6);

    // The expected exposed capabilities are:
    // fuchsia.test.Suite
    // data
    // fuchsia.foo.Bar
    assert_eq!(resolved.exposed_capabilities.len(), 3);

    // This package must have a merkle root.
    assert!(resolved.merkle_root.is_some());

    // We do not verify the contents of the execution, because they are largely dependent on
    // the Rust Test Runner
    resolved.started.unwrap();

    let mut instances = show_cmd_serialized("foo.cm".to_string(), realm_query).await.unwrap();
    assert_eq!(instances.len(), 1);
    let instance = instances.remove(0);
    assert_eq!(instance.moniker, AbsoluteMoniker::parse_str("/foo").unwrap());
    assert!(instance.url.ends_with("#meta/foo.cm"));

    let resolved = instance.resolved.unwrap();
    assert!(resolved.started.is_none());

    // check foo's config
    let mut config = resolved.config.unwrap();
    assert_eq!(config.len(), 2);
    let field1 = config.remove(0);
    let field2 = config.remove(0);
    assert_eq!(field1.key, "my_string");
    assert_eq!(field1.value, "String(\"hello, world!\")");
    assert_eq!(field2.key, "my_uint8");
    assert_eq!(field2.value, "Uint8(255)");
}

#[fuchsia::test]
async fn capability() {
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();

    let mut segments =
        capability::get_all_route_segments("data".to_string(), &realm_query).await.unwrap();

    assert_eq!(segments.len(), 2);

    let segment = segments.remove(0);
    if let capability::RouteSegment::DeclareBy { moniker, .. } = segment {
        assert!(moniker.is_root());
    } else {
        panic!("unexpected segment");
    }

    let segment = segments.remove(0);
    if let capability::RouteSegment::ExposeBy { moniker, .. } = segment {
        assert!(moniker.is_root());
    } else {
        panic!("unexpected segment");
    }
}

#[fuchsia::test]
async fn route() {
    // Exact match, multiple filters
    let route_validator = connect_to_protocol::<fsys::RouteValidatorMarker>().unwrap();
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();
    let mut reports = route_cmd_serialized(
        "/".into(),
        Some("use:fuchsia.foo.Bar,expose:data".into()),
        route_validator,
        realm_query,
    )
    .await
    .unwrap();

    assert_eq!(reports.len(), 2);

    let report = reports.remove(0);
    assert_matches!(
        report,
        RouteReport {
            decl_type: DeclType::Use,
            capability,
            error_summary: Some(_),
            source_moniker: None,
            service_instances: None,
        } if capability == "fuchsia.foo.Bar"
    );

    let report = reports.remove(0);
    assert_matches!(
        report,
        RouteReport {
            decl_type: DeclType::Expose,
            capability,
            error_summary: None,
            source_moniker: Some(m),
            service_instances: None,
        } if capability == "data" && m == "."
    );

    // Fuzzy match
    let route_validator = connect_to_protocol::<fsys::RouteValidatorMarker>().unwrap();
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();
    let mut reports =
        route_cmd_serialized("/".into(), Some("fuchsia.foo".into()), route_validator, realm_query)
            .await
            .unwrap();

    assert_eq!(reports.len(), 2);

    let report = reports.remove(0);
    assert_matches!(
        report,
        RouteReport {
            decl_type: DeclType::Use,
            capability,
            error_summary: Some(_),
            source_moniker: None,
            service_instances: None,
        } if capability == "fuchsia.foo.Bar"
    );

    let report = reports.remove(0);
    assert_matches!(
        report,
        RouteReport {
            decl_type: DeclType::Expose,
            capability,
            error_summary: None,
            source_moniker: Some(m),
            service_instances: None,
        } if capability == "fuchsia.foo.Bar" && m == "."
    );

    // No filter (match all)
    let route_validator = connect_to_protocol::<fsys::RouteValidatorMarker>().unwrap();
    let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();
    let reports =
        route_cmd_serialized("/".into(), None, route_validator, realm_query).await.unwrap();

    // The expected incoming capabilities are:
    // fidl.examples.routing.echo.Echo
    // fuchsia.foo.Bar
    // fuchsia.logger.LogSink
    // fuchsia.sys2.RealmExplorer
    // fuchsia.sys2.RealmQuery
    // fuchsia.sys2.RouteValidator
    //
    // The expected exposed capabilities are:
    // fuchsia.foo.bar
    // fuchsia.test.Suite
    // data
    assert_eq!(reports.len(), 6 + 3);
}
