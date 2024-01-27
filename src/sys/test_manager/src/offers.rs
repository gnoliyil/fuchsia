// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::constants::{
        ENCLOSING_ENV_REALM_NAME, HERMETIC_RESOLVER_REALM_NAME, TEST_ROOT_COLLECTION,
        WRAPPER_REALM_NAME,
    },
    anyhow::{format_err, Error},
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_component_test as ftest,
    fuchsia_component_test::{
        error::Error as RealmBuilderError, Capability, RealmBuilder, Ref, Route, SubRealmBuilder,
    },
};

pub(crate) fn map_offers(offers: Vec<fdecl::Offer>) -> Result<Vec<ftest::Capability>, Error> {
    let mut capabilities = vec![];
    for offer_decl in offers {
        match offer_decl {
            fdecl::Offer::Protocol(fdecl::OfferProtocol {
                target_name: Some(target_name), ..
            }) if target_name != "fuchsia.logger.LogSink"
                && target_name != "fuchsia.diagnostics.InspectSink" =>
            {
                capabilities.push(
                    Capability::protocol_by_name(target_name).availability_same_as_target().into(),
                );
            }
            fdecl::Offer::Directory(fdecl::OfferDirectory {
                target_name: Some(target_name),
                ..
            }) => {
                capabilities
                    .push(Capability::directory(target_name).availability_same_as_target().into());
            }
            fdecl::Offer::Storage(fdecl::OfferStorage {
                target_name: Some(target_name), ..
            }) => {
                let use_path = format!("/{}", target_name);
                capabilities.push(
                    Capability::storage(target_name)
                        .path(use_path)
                        .availability_same_as_target()
                        .into(),
                );
            }
            fdecl::Offer::EventStream(fdecl::OfferEventStream {
                target_name: Some(target_name),
                ..
            }) => {
                capabilities.push(Capability::event_stream(target_name.clone()).into());
            }
            fdecl::Offer::Service(fdecl::OfferService { .. })
            | fdecl::Offer::Runner(fdecl::OfferRunner { .. })
            | fdecl::Offer::Resolver(fdecl::OfferResolver { .. }) => {
                return Err(format_err!(
                    "Services, runners, and resolvers are not supported by realm builder"
                ));
            }
            _ => {
                // Ignore anything else that is routed to the test collection
            }
        }
    }
    Ok(capabilities)
}

pub(crate) async fn apply_offers(
    builder: &RealmBuilder,
    wrapper_realm: &SubRealmBuilder,
    offers: &Vec<ftest::Capability>,
) -> Result<(), RealmBuilderError> {
    for capability in offers {
        let (capability_for_test_wrapper, capability_for_test_root) =
            if let ftest::Capability::EventStream(event_stream) = &capability {
                // In case of event stream, we route that stream to both the test wrapper and test root,
                // scoping each of them to only those realms respectively. The outcome is that wrapper and
                // root see only their own events.
                let mut test_wrapper_event_stream = event_stream.clone();
                test_wrapper_event_stream.scope = Some(vec![Ref::child(WRAPPER_REALM_NAME).into()]);
                let mut test_root_event_stream = event_stream.clone();
                test_root_event_stream.scope = Some(vec![
                    Ref::collection(TEST_ROOT_COLLECTION).into(),
                    Ref::child(ENCLOSING_ENV_REALM_NAME).into(),
                    Ref::child(HERMETIC_RESOLVER_REALM_NAME).into(),
                ]);
                (
                    ftest::Capability::EventStream(test_wrapper_event_stream),
                    ftest::Capability::EventStream(test_root_event_stream),
                )
            } else {
                // we simply route non event capabilities to both test wrapper and test root.
                (capability.clone(), capability.clone())
            };
        builder
            .add_route(
                Route::new()
                    .capability(capability_for_test_wrapper.clone())
                    .from(Ref::parent())
                    .to(wrapper_realm),
            )
            .await?;
        wrapper_realm
            .add_route(
                Route::new()
                    .capability(capability_for_test_root.clone())
                    .from(Ref::parent())
                    .to(Ref::collection(TEST_ROOT_COLLECTION)),
            )
            .await?;
    }
    Ok(())
}
