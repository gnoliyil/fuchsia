// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use fuchsia_component::server::ServiceFs;
use fuchsia_inspect::*;
use futures::prelude::*;

#[fuchsia::main]
async fn main() -> Result<(), Error> {
    let root = component::inspector().root();
    root.record_int("int", 3);
    root.record_lazy_child("lazy-node", || {
        async move {
            let inspector = Inspector::default();
            inspector.root().record_string("a", "test");
            let child = inspector.root().create_child("child");
            child.record_lazy_values("lazy-values", || {
                async move {
                    let inspector = Inspector::default();
                    inspector.root().record_double("double", 3.25);
                    Ok(inspector)
                }
                .boxed()
            });
            inspector.root().record(child);
            Ok(inspector)
        }
        .boxed()
    });

    let mut fs = ServiceFs::new();
    inspect_runtime::serve(component::inspector(), &mut fs)?;
    fs.take_and_serve_directory_handle()?;

    fs.collect::<()>().await;
    Ok(())
}
