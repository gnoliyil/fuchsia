// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use diagnostics_data::{
    BuilderArgs, Data, InspectHandleName, LogsDataBuilder, LogsField, LogsProperty, Severity,
};
use diagnostics_hierarchy::{DiagnosticsHierarchy, Property};
use fidl_fuchsia_diagnostics::{DataType, Format};
use fuchsia_async as fasync;
use fuchsia_criterion::{
    criterion::{self, Criterion},
    FuchsiaCriterion,
};
use futures::{stream, StreamExt};

use archivist_lib::{
    constants::FORMATTED_CONTENT_CHUNK_SIZE_TARGET,
    formatter::{JsonPacketSerializer, SerializedVmo},
};
use std::time::Duration;

fn bench_serialization(b: &mut criterion::Bencher, n: usize, m: usize, format: Format) {
    let mut children = vec![];
    for i in 0..n {
        let mut properties = vec![];
        for j in 0..m {
            properties.push(Property::Uint(j.to_string(), (5 * i + j) as u64));
        }
        children.push(DiagnosticsHierarchy::new(i.to_string(), properties, vec![]));
    }
    let data = Data::for_inspect(
        "bench", /* moniker */
        Some(DiagnosticsHierarchy::new("root", vec![], children)),
        1, /* timestamp_nanos */
        "fuchsia-pkg://fuchsia.com/testing#meta/bench.cm",
        Some(InspectHandleName::filename("fuchsia.inspect.Tree")),
        vec![],
    );

    b.iter(|| {
        let _ = criterion::black_box(SerializedVmo::serialize(&data, DataType::Inspect, format));
    });
}

fn bench_json_packet_serializer(b: &mut criterion::Bencher, total_logs: u64) {
    let logs = (0u64..total_logs)
        .map(|i| {
            LogsDataBuilder::new(BuilderArgs {
                component_url: Some(format!(
                    "fuchsia-pkg://fuchsia.com/testing#meta/bench-{}.cm",
                    i
                )),
                moniker: format!("moniker-{}", i),
                severity: Severity::Info,
                timestamp_nanos: (i as i64).into(),
            })
            .set_message(format!("Benching #{}", i))
            .set_file(format!("bench-{}.rs", i))
            .set_line(400 + i)
            .set_pid(100 + i)
            .set_tid(2000 + i)
            .set_dropped(2)
            .add_tag("benches")
            .add_key(LogsProperty::Uint(LogsField::Other("id".to_string()), i))
            .build()
        })
        .collect::<Vec<_>>();

    let mut executor = fasync::LocalExecutor::new();
    b.iter(|| {
        let logs_for_fut = logs.clone();
        criterion::black_box(executor.run_singlethreaded(async move {
            let mut stream = JsonPacketSerializer::new_without_stats(
                FORMATTED_CONTENT_CHUNK_SIZE_TARGET,
                stream::iter(logs_for_fut),
            );
            while let Some(res) = stream.next().await {
                let _ = criterion::black_box(res);
            }
        }));
    });
}

fn main() {
    let mut c = FuchsiaCriterion::default();
    let internal_c: &mut Criterion = &mut c;
    *internal_c = std::mem::take(internal_c)
        .warm_up_time(Duration::from_secs(2))
        .measurement_time(Duration::from_secs(2))
        .sample_size(20);

    // The following benchmarks measure the performance of SerializedVmo and JsonPacketSerializer.
    // This is fundamental when serializing responses in the archivist.

    let mut bench = criterion::Benchmark::new("Formatter/JsonPacketSerializer/5", move |b| {
        bench_json_packet_serializer(b, 5);
    });
    bench = bench.with_function("Formatter/JsonPacketSerializer/5k", move |b| {
        bench_json_packet_serializer(b, 5000);
    });
    bench = bench.with_function("Formatter/JsonPacketSerializer/16k", move |b| {
        bench_json_packet_serializer(b, 16000);
    });

    // Benchmark the time needed to serialize a DiagnosticsData into a SerializedVmo with N children
    // and M properties each, for Json and Cbor.
    for (format_name, format) in [("JsonString", Format::Json), ("CborData", Format::Cbor)].iter() {
        for (size_name, n, m) in
            [("5x5", 5, 5), ("10x10", 10, 10), ("100x100", 100, 100), ("1000x500", 1000, 500)]
                .iter()
        {
            let label = format!("Formatter/{}/Fill/{}", format_name, size_name);
            bench = bench.with_function(&label, move |b| {
                bench_serialization(b, *n, *m, *format);
            });
        }
    }

    c.bench("fuchsia.archivist", bench);
}
