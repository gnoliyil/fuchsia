// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Result},
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_sys2 as fsys,
    fuchsia_component::client::connect_to_protocol,
};

pub struct Stressor {
    realm_query: fsys::RealmQueryProxy,
    lifecycle_controller: fsys::LifecycleControllerProxy,
}

impl Stressor {
    pub fn from_namespace() -> Self {
        let realm_query = connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();
        let lifecycle_controller =
            connect_to_protocol::<fsys::LifecycleControllerMarker>().unwrap();
        Self { realm_query, lifecycle_controller }
    }

    pub async fn get_instances_in_realm(&self) -> Vec<String> {
        let iterator = self.realm_query.get_all_instances().await.unwrap().unwrap();
        let iterator = iterator.into_proxy().unwrap();

        let mut instances = vec![];

        loop {
            let mut slice = iterator.next().await.unwrap();
            if slice.is_empty() {
                break;
            }
            instances.append(&mut slice);
        }

        instances.into_iter().map(|i| i.moniker.unwrap()).collect()
    }

    pub async fn create_child(
        &self,
        parent_moniker: &str,
        collection: String,
        child_name: String,
        url: String,
    ) -> Result<()> {
        let mut collection_ref = fdecl::CollectionRef { name: collection.clone() };
        let decl = fdecl::Child {
            name: Some(child_name.clone()),
            url: Some(url),
            startup: Some(fdecl::StartupMode::Lazy),
            ..fdecl::Child::EMPTY
        };
        self.lifecycle_controller
            .create_instance(
                parent_moniker,
                &mut collection_ref,
                decl,
                fcomponent::CreateChildArgs::EMPTY,
            )
            .await
            .unwrap()
            .map_err(|e| {
                format_err!(
                    "Could not create child (parent: {})(child: {}): {:?}",
                    parent_moniker,
                    child_name,
                    e
                )
            })?;

        let child_moniker = format!("{}/{}:{}", parent_moniker, collection, child_name);
        let (_, binder_server) = fidl::endpoints::create_endpoints::<fcomponent::BinderMarker>();
        self.lifecycle_controller
            .start_instance(&child_moniker, binder_server)
            .await
            .unwrap()
            .map_err(|e| format_err!("Could not start {}: {:?}", child_moniker, e))?;

        Ok(())
    }

    pub async fn destroy_child(
        &self,
        parent_moniker: &str,
        collection: String,
        child_name: String,
    ) -> Result<()> {
        let mut child = fdecl::ChildRef { name: child_name.clone(), collection: Some(collection) };
        self.lifecycle_controller
            .destroy_instance(parent_moniker, &mut child)
            .await
            .unwrap()
            .map_err(|e| {
                format_err!(
                    "Could not destroy child (parent: {})(child: {}): {:?}",
                    parent_moniker,
                    child_name,
                    e
                )
            })
    }
}
