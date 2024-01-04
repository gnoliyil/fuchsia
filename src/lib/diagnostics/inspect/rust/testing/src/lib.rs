// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{deprecated_fidl_server::*, table::*},
    anyhow::{format_err, Error},
    fuchsia_component::server::ServiceFs,
    fuchsia_inspect::{self as inspect, ArithmeticArrayProperty, ArrayProperty},
    futures::{FutureExt, StreamExt},
    std::ops::AddAssign,
    structopt::StructOpt,
};

mod deprecated_fidl_server;
mod table;

struct PopulateParams<T> {
    floor: T,
    step: T,
    count: usize,
}

fn populated<H: inspect::HistogramProperty>(histogram: H, params: PopulateParams<H::Type>) -> H
where
    H::Type: AddAssign + Copy,
{
    let mut value = params.floor;
    for _ in 0..params.count {
        histogram.insert(value);
        value += params.step;
    }
    histogram
}

#[derive(Debug, StructOpt, Default)]
#[structopt(
    name = "example",
    about = "Example component to showcase Inspect API objects, including an NxM nested table"
)]
pub struct Options {
    #[structopt(long)]
    pub rows: usize,

    #[structopt(long)]
    pub columns: usize,

    /// If set, publish a top-level number called "extra_number".
    #[structopt(long = "extra-number")]
    pub extra_number: Option<i64>,
}

pub async fn emit_example_inspect_data(opts: Options) -> Result<(), Error> {
    let inspector = inspect::Inspector::default();
    let root = inspector.root();
    assert!(inspector.is_valid());

    reset_unique_names();
    let table_node_name = unique_name("table");
    let example_table =
        Table::new(opts.rows, opts.columns, &table_node_name, root.create_child(&table_node_name));

    let int_array = root.create_int_array(unique_name("array"), 3);
    int_array.set(0, 1);
    int_array.add(1, 10);
    int_array.subtract(2, 3);

    let uint_array = root.create_uint_array(unique_name("array"), 3);
    uint_array.set(0, 1u64);
    uint_array.add(1, 10);
    uint_array.set(2, 3u64);
    uint_array.subtract(2, 1);

    let double_array = root.create_double_array(unique_name("array"), 3);
    double_array.set(0, 0.25);
    double_array.add(1, 1.25);
    double_array.subtract(2, 0.75);

    let _int_linear_hist = populated(
        root.create_int_linear_histogram(
            unique_name("histogram"),
            inspect::LinearHistogramParams { floor: -10, step_size: 5, buckets: 3 },
        ),
        PopulateParams { floor: -20, step: 1, count: 40 },
    );
    let _uint_linear_hist = populated(
        root.create_uint_linear_histogram(
            unique_name("histogram"),
            inspect::LinearHistogramParams { floor: 5, step_size: 5, buckets: 3 },
        ),
        PopulateParams { floor: 0, step: 1, count: 40 },
    );
    let _double_linear_hist = populated(
        root.create_double_linear_histogram(
            unique_name("histogram"),
            inspect::LinearHistogramParams { floor: 0.0, step_size: 0.5, buckets: 3 },
        ),
        PopulateParams { floor: -1.0, step: 0.1, count: 40 },
    );

    let _int_exp_hist = populated(
        root.create_int_exponential_histogram(
            unique_name("histogram"),
            inspect::ExponentialHistogramParams {
                floor: -10,
                initial_step: 5,
                step_multiplier: 2,
                buckets: 3,
            },
        ),
        PopulateParams { floor: -20, step: 1, count: 40 },
    );
    let _uint_exp_hist = populated(
        root.create_uint_exponential_histogram(
            unique_name("histogram"),
            inspect::ExponentialHistogramParams {
                floor: 0,
                initial_step: 1,
                step_multiplier: 2,
                buckets: 3,
            },
        ),
        PopulateParams { floor: 0, step: 1, count: 40 },
    );
    let _double_exp_hist = populated(
        root.create_double_exponential_histogram(
            unique_name("histogram"),
            inspect::ExponentialHistogramParams {
                floor: 0.0,
                initial_step: 1.25,
                step_multiplier: 3.0,
                buckets: 5,
            },
        ),
        PopulateParams { floor: -1.0, step: 0.1, count: 40 },
    );

    root.record_lazy_child("lazy-node", || {
        async move {
            let inspector = inspect::Inspector::default();
            inspector.root().record_uint("uint", 3);
            Ok(inspector)
        }
        .boxed()
    });
    root.record_lazy_values("lazy-values", || {
        async move {
            let inspector = inspect::Inspector::default();
            inspector.root().record_double("lazy-double", 3.25);
            Ok(inspector)
        }
        .boxed()
    });

    if let Some(extra_number) = opts.extra_number {
        root.record_int("extra_number", extra_number);
    }

    let mut fs = ServiceFs::new();

    // NOTE: this FIDL service is deprecated and the following *should not* be done.
    // Rust doesn't have a way of writing to the deprecated FIDL service, therefore
    // we read what we wrote to the VMO and provide it through the service for testing
    // purposes.
    fs.dir("diagnostics").add_fidl_service(move |stream| {
        spawn_inspect_server(stream, example_table.get_node_object());
    });

    let inspector_clone = inspector.clone();
    fs.dir("diagnostics").add_fidl_service(move |stream| {
        // Purely for test purposes. Internally inspect creates a pseudo dir diagnostics and
        // adds it as remote in ServiceFs. However, if we try to add the VMO file and the other
        // service in the ServiceFs, an exception occurs. This is purely a workaround for
        // ServiceFS and for the test purpose. A regular component wouldn't do this. It would
        // just do `inspect_runtime::serve(inspector, &mut fs);`.
        inspect_runtime::service::spawn_tree_server_with_stream(
            inspector_clone.clone(),
            inspect_runtime::TreeServerSendPreference::default(),
            stream,
        )
        .detach();
    });

    // TODO(https://fxbug.dev/41952): remove when all clients writing VMO files today have been migrated to write
    // to Tree.
    inspector
        .duplicate_vmo()
        .ok_or(format_err!("Failed to duplicate VMO"))
        .and_then(|vmo| {
            fs.dir("diagnostics").add_vmo_file_at("root.inspect", vmo);
            Ok(())
        })
        .unwrap_or_else(|e| {
            eprintln!("Failed to expose vmo. Error: {:?}", e);
        });

    fs.take_and_serve_directory_handle()?;

    Ok(fs.collect().await)
}
