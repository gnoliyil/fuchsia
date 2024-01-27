// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    fdio, fidl,
    fidl_fuchsia_inspect_deprecated::{InspectProxy, MetricValue, PropertyValue},
    fuchsia_async as fasync,
    fuchsia_inspect::reader::{DiagnosticsHierarchy, Property},
    fuchsia_zircon as zx,
};

/// Loads an inspect node hierarchy in the given path.
pub async fn load_hierarchy_from_path(path: &str) -> Result<DiagnosticsHierarchy, Error> {
    let (client, server) = zx::Channel::create();
    fdio::service_connect(path, server)?;
    let inspect_proxy = InspectProxy::new(fasync::Channel::from_channel(client)?);
    let hierarchy = load_hierarchy(inspect_proxy).await?;
    Ok(hierarchy)
}

/// Loads an inspect node hierarchy from the given root inspect node.
pub async fn load_hierarchy(proxy: InspectProxy) -> Result<DiagnosticsHierarchy, Error> {
    let mut pending_nodes = vec![];
    let root = read_node(proxy).await?;
    pending_nodes.push(root);
    while !pending_nodes.is_empty() {
        let mut current_node = pending_nodes.pop().unwrap();
        if current_node.pending_children.is_empty() {
            if pending_nodes.is_empty() {
                return Ok(current_node.hierarchy);
            }
            match pending_nodes.pop() {
                Some(mut parent) => {
                    parent.hierarchy.children.push(current_node.hierarchy);
                    pending_nodes.push(parent);
                }
                None => return Err(format_err!("failed to load hierarchy")),
            }
        } else {
            let next_child = current_node.pending_children.pop().unwrap();
            let (client, server) = zx::Channel::create();
            current_node
                .proxy
                .open_child(&next_child, fidl::endpoints::ServerEnd::new(server))
                .await?;
            let child_proxy = InspectProxy::new(fasync::Channel::from_channel(client)?);
            let child_node = read_node(child_proxy).await?;
            pending_nodes.push(current_node);
            pending_nodes.push(child_node);
        }
    }
    return Err(format_err!("failed to load hierarchy"));
}

struct PartialNodeHierarchy {
    proxy: InspectProxy,
    hierarchy: DiagnosticsHierarchy,
    pending_children: Vec<String>,
}

/// Loads an inspect hierarchy from a deprecated FIDL service.
async fn read_node(proxy: InspectProxy) -> Result<PartialNodeHierarchy, Error> {
    let object = proxy.read_data().await?;
    let mut properties = object
        .properties
        .into_iter()
        .map(|property| match property.value {
            PropertyValue::Str(v) => Property::String(property.key, v),
            PropertyValue::Bytes(v) => Property::Bytes(property.key, v),
        })
        .collect::<Vec<Property>>();

    properties.extend(object.metrics.into_iter().map(|metric| match metric.value {
        MetricValue::IntValue(v) => Property::Int(metric.key, v),
        MetricValue::DoubleValue(v) => Property::Double(metric.key, v),
        MetricValue::UintValue(v) => Property::Uint(metric.key, v),
    }));

    let pending_children = proxy.list_children().await?;
    let hierarchy = DiagnosticsHierarchy::new(&object.name, properties, vec![]);
    Ok(PartialNodeHierarchy { proxy, hierarchy, pending_children })
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_inspect_deprecated::{
            InspectMarker, InspectRequest, InspectRequestStream, Metric, Object, Property,
        },
        fuchsia_inspect::assert_data_tree,
        futures::{TryFutureExt, TryStreamExt},
        lazy_static::lazy_static,
        maplit::{hashmap, hashset},
        std::collections::{HashMap, HashSet},
    };

    #[fuchsia::test]
    async fn test_load_hierarchy() -> Result<(), Error> {
        let (client_proxy, server_stream) =
            fidl::endpoints::create_proxy_and_stream::<InspectMarker>()?;
        spawn_server(server_stream, "root".to_string());
        let hierarchy = load_hierarchy(client_proxy).await?;
        assert_data_tree!(hierarchy, root: {
            double_value: 5.2,
            a: {
                int_value: -3i64,
                uint_value: 2u64,
                c: {}
            },
            b: {
                bytes_value: vec![0x12u8, 0x34, 0x56],
                string_value: "test",
            }
        });
        Ok(())
    }

    lazy_static! {
        static ref OBJECTS: HashMap<String, TestObject> = hashmap! {
            "root".to_string() => TestObject {
                object: Object {
                    name: "root".to_string(),
                    metrics: vec![
                        Metric {
                            key: "double_value".to_string(),
                            value: MetricValue::DoubleValue(5.2),
                        },
                    ],
                    properties: vec![],
                },
                children: hashset!("a".to_string(), "b".to_string()),
            },
            "a".to_string() => TestObject {
                object: Object {
                    name: "a".to_string(),
                    metrics: vec![
                        Metric {
                            key: "int_value".to_string(),
                            value: MetricValue::IntValue(-3),
                        },
                        Metric {
                            key: "uint_value".to_string(),
                            value: MetricValue::UintValue(2),
                        },
                    ],
                    properties: vec![],
                },
                children: hashset!("c".to_string()),
            },
            "b".to_string() => TestObject {
                object: Object {
                    name: "b".to_string(),
                    metrics: vec![],
                    properties: vec![
                        Property{
                            key: "string_value".to_string(),
                            value: PropertyValue::Str("test".to_string()),
                        },
                        Property {
                            key: "bytes_value".to_string(),
                            value: PropertyValue::Bytes(vec![0x12u8, 0x34, 0x56]),
                        },
                    ],
                },
                children: hashset!(),
            },
            "c".to_string() => TestObject {
                object: Object {
                    name: "c".to_string(),
                    metrics: vec![],
                    properties: vec![],
                },
                children: hashset!(),
            }
        };
    }

    struct TestObject {
        object: Object,
        children: HashSet<String>,
    }

    fn spawn_server(mut stream: InspectRequestStream, object_name: String) {
        fasync::Task::spawn(
            async move {
                let object = OBJECTS.get(&object_name).unwrap();
                while let Some(req) = stream.try_next().await? {
                    match req {
                        InspectRequest::ReadData { responder } => {
                            responder.send(&mut object.object.clone())?;
                        }
                        InspectRequest::ListChildren { responder } => {
                            responder.send(&object.children.iter().cloned().collect::<Vec<_>>())?;
                        }
                        InspectRequest::OpenChild { child_name, child_channel, responder } => {
                            let stream = child_channel.into_stream()?;
                            if object.children.contains(&child_name) {
                                spawn_server(stream, child_name);
                                responder.send(true)?;
                            } else {
                                responder.send(false)?;
                            }
                        }
                    }
                }
                Ok(())
            }
            .unwrap_or_else(|e: Error| eprintln!("error running inspect server: {:?}", e)),
        )
        .detach();
    }
}
