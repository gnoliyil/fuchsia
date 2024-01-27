// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        cli::{format::format_create_error, parse_component_url},
        lifecycle::create_instance_in_collection,
    },
    anyhow::{format_err, Result},
    fidl_fuchsia_sys2 as fsys,
    moniker::{
        AbsoluteMoniker, AbsoluteMonikerBase, ChildMonikerBase, RelativeMoniker,
        RelativeMonikerBase,
    },
};

pub async fn create_cmd<W: std::io::Write>(
    url: String,
    moniker: String,
    lifecycle_controller: fsys::LifecycleControllerProxy,
    mut writer: W,
) -> Result<()> {
    let url = parse_component_url(&url)?;

    let moniker = AbsoluteMoniker::parse_str(&moniker)
        .map_err(|e| format_err!("Error: {} is not a valid moniker ({})", moniker, e))?;

    let parent = moniker
        .parent()
        .ok_or(format_err!("Error: {} does not reference a dynamic instance", moniker))?;
    let leaf = moniker
        .leaf()
        .ok_or(format_err!("Error: {} does not reference a dynamic instance", moniker))?;
    let child_name = leaf.name();
    let collection = leaf
        .collection()
        .ok_or(format_err!("Error: {} does not reference a dynamic instance", moniker))?;

    writeln!(writer, "URL: {}", url)?;
    writeln!(writer, "Moniker: {}", moniker)?;
    writeln!(writer, "Creating component instance...")?;

    // Convert the absolute moniker into a relative moniker w.r.t. root.
    // LifecycleController expects relative monikers only.
    let parent_relative = RelativeMoniker::scope_down(&AbsoluteMoniker::root(), &parent).unwrap();

    create_instance_in_collection(
        &lifecycle_controller,
        &parent_relative,
        collection,
        child_name,
        &url,
    )
    .await
    .map_err(|e| format_create_error(&moniker, &parent, collection, e))?;

    writeln!(writer, "Created component instance!")?;
    Ok(())
}

#[cfg(test)]
mod test {
    use {super::*, fidl::endpoints::create_proxy_and_stream, futures::TryStreamExt};

    fn setup_fake_lifecycle_controller(
        expected_moniker: &'static str,
        expected_collection: &'static str,
        expected_name: &'static str,
        expected_url: &'static str,
    ) -> fsys::LifecycleControllerProxy {
        let (lifecycle_controller, mut stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::CreateInstance {
                    parent_moniker,
                    collection,
                    decl,
                    responder,
                    ..
                } => {
                    assert_eq!(expected_moniker, parent_moniker);
                    assert_eq!(expected_collection, collection.name);
                    assert_eq!(expected_name, decl.name.unwrap());
                    assert_eq!(expected_url, decl.url.unwrap());
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }
        })
        .detach();
        lifecycle_controller
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_success() -> Result<()> {
        let mut output = Vec::new();
        let lifecycle_controller = setup_fake_lifecycle_controller(
            "./core",
            "ffx-laboratory",
            "test",
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm",
        );
        let response = create_cmd(
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm".to_string(),
            "/core/ffx-laboratory:test".to_string(),
            lifecycle_controller,
            &mut output,
        )
        .await;
        response.unwrap();
        Ok(())
    }
}
