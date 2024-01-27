// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod args;

use {
    crate::common::{
        node_property_key_to_string, node_property_value_to_string, write_node_properties,
    },
    anyhow::{Context, Result},
    args::ListCompositeNodeSpecsCommand,
    fidl_fuchsia_driver_development as fdd, fuchsia_driver_dev,
    std::io::Write,
};

pub async fn list_composite_node_specs(
    cmd: ListCompositeNodeSpecsCommand,
    writer: &mut dyn Write,
    driver_development_proxy: fdd::DriverDevelopmentProxy,
) -> Result<()> {
    let specs = fuchsia_driver_dev::get_composite_node_specs(&driver_development_proxy, cmd.name)
        .await
        .context("Failed to get composite node specs")?;

    if !cmd.verbose {
        for spec in specs {
            let name = spec.name.unwrap_or("N/A".to_string());
            let driver = spec.driver.unwrap_or("None".to_string());
            writeln!(writer, "{:<20}: {}", name, driver)?;
        }
        return Ok(());
    }

    for spec in specs {
        if let Some(name) = spec.name {
            writeln!(writer, "{0: <10}: {1}", "Name", name)?;
        }

        if let Some(driver) = spec.driver {
            writeln!(writer, "{0: <10}: {1}", "Driver", driver)?;
        } else {
            writeln!(writer, "{0: <10}: {1}", "Driver", "None")?;
        }

        if let Some(nodes) = spec.parents {
            writeln!(writer, "{0: <10}: {1}", "Nodes", nodes.len())?;

            for (i, node) in nodes.into_iter().enumerate() {
                let name = match &spec.parent_names {
                    Some(names) => format!("\"{}\"", names.get(i).unwrap()),
                    None => "None".to_string(),
                };

                if &spec.primary_index == &Some(i as u32) {
                    writeln!(writer, "{0: <10}: {1} (Primary)", format!("Node {}", i), name)?;
                } else {
                    writeln!(writer, "{0: <10}: {1}", format!("Node {}", i), name)?;
                }

                let bind_rules_len = node.bind_rules.len();
                writeln!(writer, "  {0} {1}", bind_rules_len, "Bind Rules")?;

                for (j, bind_rule) in node.bind_rules.into_iter().enumerate() {
                    let key = node_property_key_to_string(&bind_rule.key);
                    let values = bind_rule
                        .values
                        .into_iter()
                        .map(|value| node_property_value_to_string(&value))
                        .collect::<Vec<_>>()
                        .join(", ");
                    writeln!(
                        writer,
                        "  [{0:>2}/{1:>2}] : {2:?} {3} {{ {4} }}",
                        j + 1,
                        bind_rules_len,
                        bind_rule.condition,
                        key,
                        values,
                    )?;
                }

                write_node_properties(&node.properties, writer)?;
            }
        }

        writeln!(writer)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        argh::FromArgs,
        fidl::endpoints::ServerEnd,
        fidl_fuchsia_driver_framework as fdf, fuchsia_async as fasync,
        futures::{
            future::{Future, FutureExt},
            stream::StreamExt,
        },
    };

    /// Invokes `list_composite_node_specs` with `cmd` and runs a mock driver development server that
    /// invokes `on_driver_development_request` whenever it receives a request.
    /// The output of `list_composite_node_specs` that is normally written to its `writer` parameter
    /// is returned.
    async fn test_list_composite_node_specs<F, Fut>(
        cmd: ListCompositeNodeSpecsCommand,
        on_driver_development_request: F,
    ) -> Result<String>
    where
        F: Fn(fdd::DriverDevelopmentRequest) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = Result<()>> + Send + Sync,
    {
        let (driver_development_proxy, mut driver_development_requests) =
            fidl::endpoints::create_proxy_and_stream::<fdd::DriverDevelopmentMarker>()
                .context("Failed to create FIDL proxy")?;

        // Run the command and mock driver development server.
        let mut writer = Vec::new();
        let request_handler_task = fasync::Task::spawn(async move {
            while let Some(res) = driver_development_requests.next().await {
                let request = res.context("Failed to get next request")?;
                on_driver_development_request(request).await.context("Failed to handle request")?;
            }
            anyhow::bail!("Driver development request stream unexpectedly closed");
        });
        futures::select! {
            res = request_handler_task.fuse() => {
                res?;
                anyhow::bail!("Request handler task unexpectedly finished");
            }
            res = list_composite_node_specs(cmd, &mut writer, driver_development_proxy).fuse() => res.context("List composite node specs command failed")?,
        }

        String::from_utf8(writer)
            .context("Failed to convert list composite node specs output to a string")
    }

    async fn run_specs_iterator_server(
        mut specs: Vec<fdd::CompositeNodeSpecInfo>,
        iterator: ServerEnd<fdd::CompositeNodeSpecIteratorMarker>,
    ) -> Result<()> {
        let mut iterator =
            iterator.into_stream().context("Failed to convert iterator into a stream")?;
        while let Some(res) = iterator.next().await {
            let request = res.context("Failed to get request")?;
            match request {
                fdd::CompositeNodeSpecIteratorRequest::GetNext { responder } => {
                    responder
                        .send(
                            &mut specs
                                .drain(..)
                                .collect::<Vec<fdd::CompositeNodeSpecInfo>>()
                                .into_iter(),
                        )
                        .context("Failed to send composite node specs to responder")?;
                }
            }
        }
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_verbose() {
        let cmd = ListCompositeNodeSpecsCommand::from_args(
            &["list-composite-node-specs"],
            &["--verbose"],
        )
        .unwrap();

        let output = test_list_composite_node_specs(
            cmd,
            |request: fdd::DriverDevelopmentRequest| async move {
                match request {
                    fdd::DriverDevelopmentRequest::GetCompositeNodeSpecs {
                        name_filter: _,
                        iterator,
                        control_handle: _,
                    } => run_specs_iterator_server(
                        vec![
                            fdd::CompositeNodeSpecInfo {
                                name: Some("test_spec".to_string()),
                                parents: Some(vec![fdf::ParentSpec {
                                    bind_rules: vec![fdf::BindRule {
                                        key: fdf::NodePropertyKey::StringValue(
                                            "rule_key".to_string(),
                                        ),
                                        condition: fdf::Condition::Accept,
                                        values: vec![fdf::NodePropertyValue::StringValue(
                                            "rule_val".to_string(),
                                        )],
                                    }],
                                    properties: vec![fdf::NodeProperty {
                                        key: fdf::NodePropertyKey::StringValue(
                                            "prop_key".to_string(),
                                        ),
                                        value: fdf::NodePropertyValue::StringValue(
                                            "prop_val".to_string(),
                                        ),
                                    }],
                                }]),
                                ..Default::default()
                            },
                            fdd::CompositeNodeSpecInfo {
                                name: Some("test_spec_with_driver".to_string()),
                                driver: Some("driver_url".to_string()),
                                primary_index: Some(1),
                                parent_names: Some(vec![
                                    "name_one".to_string(),
                                    "name_two".to_string(),
                                ]),
                                parents: Some(vec![
                                    fdf::ParentSpec {
                                        bind_rules: vec![fdf::BindRule {
                                            key: fdf::NodePropertyKey::StringValue(
                                                "rule_key".to_string(),
                                            ),
                                            condition: fdf::Condition::Accept,
                                            values: vec![
                                                fdf::NodePropertyValue::StringValue(
                                                    "rule_val".to_string(),
                                                ),
                                                fdf::NodePropertyValue::StringValue(
                                                    "rule_val_2".to_string(),
                                                ),
                                            ],
                                        }],
                                        properties: vec![fdf::NodeProperty {
                                            key: fdf::NodePropertyKey::StringValue(
                                                "prop_key_0".to_string(),
                                            ),
                                            value: fdf::NodePropertyValue::StringValue(
                                                "prop_val_0".to_string(),
                                            ),
                                        }],
                                    },
                                    fdf::ParentSpec {
                                        bind_rules: vec![
                                            fdf::BindRule {
                                                key: fdf::NodePropertyKey::IntValue(0x0001),
                                                condition: fdf::Condition::Accept,
                                                values: vec![
                                                    fdf::NodePropertyValue::IntValue(0x42),
                                                    fdf::NodePropertyValue::IntValue(0x123),
                                                    fdf::NodePropertyValue::IntValue(0x234),
                                                ],
                                            },
                                            fdf::BindRule {
                                                key: fdf::NodePropertyKey::IntValue(0xdeadbeef),
                                                condition: fdf::Condition::Accept,
                                                values: vec![fdf::NodePropertyValue::IntValue(
                                                    0xbeef,
                                                )],
                                            },
                                        ],
                                        properties: vec![
                                            fdf::NodeProperty {
                                                key: fdf::NodePropertyKey::StringValue(
                                                    "prop_key_1".to_string(),
                                                ),
                                                value: fdf::NodePropertyValue::EnumValue(
                                                    "prop_key_1.prop_val".to_string(),
                                                ),
                                            },
                                            fdf::NodeProperty {
                                                key: fdf::NodePropertyKey::StringValue(
                                                    "prop_key_2".to_string(),
                                                ),
                                                value: fdf::NodePropertyValue::IntValue(0x1),
                                            },
                                            fdf::NodeProperty {
                                                key: fdf::NodePropertyKey::StringValue(
                                                    "prop_key_3".to_string(),
                                                ),
                                                value: fdf::NodePropertyValue::BoolValue(true),
                                            },
                                        ],
                                    },
                                ]),
                                ..Default::default()
                            },
                        ],
                        iterator,
                    )
                    .await
                    .context("Failed to run driver info iterator server")?,
                    _ => {}
                }
                Ok(())
            },
        )
        .await
        .unwrap();

        assert_eq!(
            output,
            r#"Name      : test_spec
Driver    : None
Nodes     : 1
Node 0    : None
  1 Bind Rules
  [ 1/ 1] : Accept "rule_key" { "rule_val" }
  1 Properties
  [ 1/ 1] : Key "prop_key"                     Value "prop_val"

Name      : test_spec_with_driver
Driver    : driver_url
Nodes     : 2
Node 0    : "name_one"
  1 Bind Rules
  [ 1/ 1] : Accept "rule_key" { "rule_val", "rule_val_2" }
  1 Properties
  [ 1/ 1] : Key "prop_key_0"                   Value "prop_val_0"
Node 1    : "name_two" (Primary)
  2 Bind Rules
  [ 1/ 2] : Accept fuchsia.BIND_PROTOCOL { 0x000042, 0x000123, 0x000234 }
  [ 2/ 2] : Accept 0xdeadbeef { 0x00beef }
  3 Properties
  [ 1/ 3] : Key "prop_key_1"                   Value Enum(prop_key_1.prop_val)
  [ 2/ 3] : Key "prop_key_2"                   Value 0x000001
  [ 3/ 3] : Key "prop_key_3"                   Value true

"#
        );
    }
}
