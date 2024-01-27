// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::args::AutoTestGeneratorCommand;
use crate::cm_parser::read_cm;
use crate::generate_build::{CppBuildGenerator, RustBuildGenerator};
use crate::generate_cpp_test::{CppTestCode, CppTestCodeGenerator};
use crate::generate_manifest::{CppManifestGenerator, RustManifestGenerator};
use crate::generate_rust_test::{RustTestCode, RustTestCodeGenerator, RustTestGenGenerator};
use crate::test_code::{convert_to_snake, CodeGenerator, TestCodeBuilder};
use ansi_term::Colour::*;
use anyhow::{format_err, Result};
use fidl_fuchsia_component_decl::*;
use regex::Regex;
use std::collections::HashMap;
use std::fs::File;
use std::path::Path;
use std::path::PathBuf;
mod args;
mod cm_parser;
mod generate_build;
mod generate_cpp_test;
mod generate_manifest;
mod generate_rust_test;
mod test_code;

// Protocols provided by component test framework, if the component-under-test 'use' any of
// these protocols, the capability source will be routed from RouteEndpoint::above_root()
const TEST_REALM_CAPABILITIES: &'static [&'static str] = &["fuchsia.logger.LogSink"];

// Capabilities provided by component framework. These capabilities are skipped from generated
// mocked templates.
const COMPONENT_FRAMEWORK_CAPABILITIES_PREFIX_RUST: &'static str = "fidl_fuchsia_component";
const COMPONENT_FRAMEWORK_CAPABILITIES_PREFIX_CPP: &'static str = "fuchsia/component/";

fn main() -> Result<()> {
    let input: AutoTestGeneratorCommand = argh::from_env();

    // component_name is the filename from the full path passed to the --cm-location.
    // ex: for '--cm-location src/diagnostics/log-stats/cml/component/log-stats.cm',
    // component_name will be log-stats
    let component_name: &str = Path::new(&input.cm_location)
        .file_stem()
        .ok_or_else(|| format_err!("{} is missing a file name", &input.cm_location))?
        .to_str()
        .ok_or_else(|| format_err!("{} cannot be converted to utf-8", &input.cm_location))?;

    let cm_decl = read_cm(&input.cm_location)
        .map_err(|e| format_err!("parsing .cm file '{}' errored: {:?}", &input.cm_location, e))?;

    // test_program_name defaults to {component_name}_test, this name will be used to generate
    // source code filename, and build rules.
    // ex: if component_name = echo_server
    //   rust code => echo_server_test.rs
    //   manifest  => meta/echo_server_test.cml
    let test_program_name = &format!("{}_test", &component_name);

    // component_url is either what user specified via --component-url or default to the format
    // 'fuchsia-pkg://fuchsia.com/{test_program_name}#meta/{component_neame}.cm'
    let component_url: &str = &input.component_url.clone().unwrap_or(format!(
        "fuchsia-pkg://fuchsia.com/{}#meta/{}.cm",
        test_program_name, component_name
    ));

    if input.cpp {
        write_cpp(&cm_decl, component_name, component_url, &test_program_name, &input)?;
    } else {
        write_rust(&cm_decl, component_name, component_url, &test_program_name, &input)?;
    }

    Ok(())
}

fn write_cpp(
    cm_decl: &Component,
    component_name: &str,
    component_url: &str,
    output_file_name: &str,
    input: &AutoTestGeneratorCommand,
) -> Result<()> {
    let code = &mut CppTestCode::new(&component_name);

    // This tells RealmBuilder to wire-up the component-under-test.
    code.add_component(component_name, component_url, "COMPONENT_URL", false);

    // Add imports that all tests will need. For importing fidl libraries that we mock,
    // imports will be added when we call 'update_code_for_use_declaration'.
    code.add_import("<gtest/gtest.h>");
    code.add_import("<lib/sys/component/cpp/testing/realm_builder.h>");
    code.add_import("<src/lib/testing/loop_fixture/real_loop_fixture.h>");

    update_code_for_use_declaration(
        &cm_decl.uses.as_ref().unwrap_or(&Vec::new()),
        code,
        input.generate_mocks,
        true, /* cpp */
    )?;
    update_code_for_expose_declaration(
        &cm_decl.exposes.as_ref().unwrap_or(&Vec::new()),
        code,
        true, /* cpp */
    )?;

    let mut stdout = std::io::stdout();

    // Write cpp test code file
    let mut src_code_dest = PathBuf::from(&input.out_dir);
    src_code_dest.push("src");
    std::fs::create_dir_all(&src_code_dest).unwrap();
    src_code_dest.push(&output_file_name);
    src_code_dest.set_extension("cc");
    println!("writing cpp file to {}", src_code_dest.display());
    let cpp_code_generator = CppTestCodeGenerator { code, copyright: !input.nocopyright };
    cpp_code_generator.write_file(&mut File::create(src_code_dest)?)?;
    if input.verbose {
        cpp_code_generator.write_file(&mut stdout)?;
    }

    let mut manifest_dest = PathBuf::from(&input.out_dir);
    manifest_dest.push("meta");
    std::fs::create_dir_all(&manifest_dest).unwrap();
    manifest_dest.push(&output_file_name);
    manifest_dest.set_extension("cml");
    println!("writing manifest file to {}", manifest_dest.display());
    let manifest_generator = CppManifestGenerator {
        test_program_name: output_file_name.to_string(),
        copyright: !input.nocopyright,
    };
    manifest_generator.write_file(&mut File::create(manifest_dest)?)?;
    if input.verbose {
        manifest_generator.write_file(&mut stdout)?;
    }

    // Write build file
    let mut build_dest = PathBuf::from(&input.out_dir);
    build_dest.push("BUILD.gn");
    println!("writing build file to {}", build_dest.display());
    let build_generator = CppBuildGenerator {
        test_program_name: output_file_name.to_string(),
        component_name: component_name.to_string(),
        copyright: !input.nocopyright,
    };
    build_generator.write_file(&mut File::create(build_dest)?)?;
    if input.verbose {
        build_generator.write_file(&mut stdout)?;
    }

    Ok(())
}

fn write_rust(
    cm_decl: &Component,
    component_name: &str,
    component_url: &str,
    output_file_name: &str,
    input: &AutoTestGeneratorCommand,
) -> Result<()> {
    // variable name used to refer to the component-under-test in the generated rust code.
    let component_var_name = component_name.replace("-", "_");
    let code = &mut RustTestCode::new(&component_var_name);
    code.add_component(&component_var_name, component_url, "COMPONENT_URL", false);

    match input.generate_mocks {
        true => {
            code.add_import(r#"fuchsia_component_test::{
            Capability, ChildOptions, LocalComponentHandles, RealmBuilder, RealmInstance, Ref, Route,
}"#);
            code.add_import("anyhow::Error");
            code.add_import("async_trait::async_trait")
        }
        false => code.add_import(
            r#"fuchsia_component_test::{
            Capability, ChildOptions, RealmBuilder, RealmInstance, Ref, Route,
}"#,
        ),
    };

    code.add_import("anyhow::Error");

    update_code_for_use_declaration(
        &cm_decl.uses.as_ref().unwrap_or(&Vec::new()),
        code,
        input.generate_mocks,
        false, /* cpp */
    )?;
    update_code_for_expose_declaration(
        &cm_decl.exposes.as_ref().unwrap_or(&Vec::new()),
        code,
        false, /* cpp */
    )?;

    let mut stdout = std::io::stdout();

    let mut code_dest = PathBuf::from(&input.out_dir);
    code_dest.push("src");
    std::fs::create_dir_all(&code_dest).unwrap();

    // Write rust test lib file
    let mut lib_code_dest = code_dest.clone();
    lib_code_dest.push("testgen.rs");
    let rust_lib_code_generator = RustTestGenGenerator { code, copyright: !input.nocopyright };
    println!(
        "writing testgen.rs to {}, {}",
        lib_code_dest.display(),
        Blue.paint("this file will be regenerated each time, edit with caution!")
    );
    rust_lib_code_generator.write_file(&mut File::create(lib_code_dest)?)?;
    if input.verbose {
        rust_lib_code_generator.write_file(&mut stdout)?;
    }

    // Write rust test code file
    let mut test_code_dest = code_dest.clone();
    test_code_dest.push(&output_file_name);
    test_code_dest.set_extension("rs");
    if !test_code_dest.exists() {
        println!(
            "writing rust test file to {}, {}",
            test_code_dest.display(),
            Blue.paint("this file will only get generated once.")
        );
        let rust_test_code_generator =
            RustTestCodeGenerator { code, copyright: !input.nocopyright };
        rust_test_code_generator.write_file(&mut File::create(test_code_dest)?)?;
        if input.verbose {
            rust_test_code_generator.write_file(&mut stdout)?;
        }
    }

    // Write manifest cml file
    let mut manifest_dest = PathBuf::from(&input.out_dir);
    manifest_dest.push("meta");
    std::fs::create_dir_all(&manifest_dest).unwrap();
    manifest_dest.push(&output_file_name);
    manifest_dest.set_extension("cml");
    if !manifest_dest.exists() {
        println!(
            "writing manifest file to {}, {}",
            manifest_dest.display(),
            Blue.paint("this file will only get generated once.")
        );
        let manifest_generator = RustManifestGenerator {
            test_program_name: output_file_name.to_string(),
            copyright: !input.nocopyright,
        };
        manifest_generator.write_file(&mut File::create(manifest_dest)?)?;
        if input.verbose {
            manifest_generator.write_file(&mut stdout)?;
        }
    }

    // Write build file
    let mut build_dest = PathBuf::from(&input.out_dir);
    build_dest.push("BUILD.gn");
    if !build_dest.exists() {
        println!(
            "writing build file to {}, {}",
            build_dest.display(),
            Blue.paint("this file will only get generated once.")
        );
        let build_generator = RustBuildGenerator {
            test_program_name: output_file_name.to_string(),
            component_name: component_name.to_string(),
            mock: input.generate_mocks,
            copyright: !input.nocopyright,
        };
        build_generator.write_file(&mut File::create(build_dest)?)?;
        if input.verbose {
            build_generator.write_file(&mut stdout)?;
        }
    }
    Ok(())
}

// Update TestCodeBuilder based on 'use' declarations in the .cm file
fn update_code_for_use_declaration(
    uses: &Vec<Use>,
    code: &mut dyn TestCodeBuilder,
    gen_mocks: bool,
    cpp: bool,
) -> Result<()> {
    let mut dep_protocols = Protocols { protocols: HashMap::new() };

    for i in 0..uses.len() {
        match &uses[i] {
            Use::Protocol(decl) => {
                if let Some(protocol) = &decl.source_name {
                    if TEST_REALM_CAPABILITIES.into_iter().any(|v| v == &protocol) {
                        code.add_protocol(protocol, "root", vec!["self".to_string()]);
                    } else {
                        let fields: Vec<&str> = protocol.split(".").collect();
                        let component_name = convert_to_snake(fields.last().unwrap());
                        // Note: we don't know which component offers this service, we'll use the
                        // generic name "{URL}" indicating that user needs to fill this value
                        // themselves.
                        if cpp {
                            dep_protocols.add_protocol_cpp(&protocol, &component_name)?;
                        } else {
                            dep_protocols.add_protocol_rust(&protocol, &component_name)?;
                        }
                        code.add_component(
                            &component_name,
                            "{URL}",
                            &format!("{}_URL", &component_name.to_ascii_uppercase()),
                            gen_mocks,
                        );
                        // Note: "root" => test framework (i.e "RouteEndpoint::above_root()")
                        // "self" => component-under-test
                        code.add_protocol(
                            protocol,
                            &component_name,
                            vec!["root".to_string(), "self".to_string()],
                        );
                    }
                }
            }
            // Note: example outputs from parsing cm: http://go/paste/5523376119480320?raw
            Use::Directory(decl) => {
                code.add_directory(
                    decl.source_name
                        .as_ref()
                        .ok_or(format_err!("directory name needs to be specified in manifest."))?,
                    decl.target_path
                        .as_ref()
                        .ok_or(format_err!("directory path needs to be specified in manifest."))?,
                    vec!["self".to_string()],
                );
            }
            Use::Storage(decl) => {
                code.add_storage(
                    decl.source_name
                        .as_ref()
                        .ok_or(format_err!("storage name needs to be specified in manifest."))?,
                    decl.target_path
                        .as_ref()
                        .ok_or(format_err!("storage path needs to be specified in manifest."))?,
                    vec!["self".to_string()],
                );
            }
            _ => (),
        }
    }
    if gen_mocks && dep_protocols.protocols.len() > 0 {
        let mut has_mock = false;
        for (component, protocols) in dep_protocols.protocols.iter() {
            for (fidl_lib, markers) in protocols.iter() {
                if cpp && !fidl_lib.starts_with(COMPONENT_FRAMEWORK_CAPABILITIES_PREFIX_CPP) {
                    code.add_import(&format!("<{}>", fidl_lib));
                    // TODO(yuanzhi) What does the mock look like when we have multiple protocols to mock
                    // within the same component?
                    code.add_mock_impl(&component, &markers[0]);
                    has_mock = true;
                }
                // Rust
                if !cpp && !fidl_lib.starts_with(COMPONENT_FRAMEWORK_CAPABILITIES_PREFIX_RUST) {
                    code.add_import(&format!("{}::*", fidl_lib));
                    code.add_mock_impl(&component, &markers[0]);
                    has_mock = true;
                }
            }
        }
        if has_mock {
            if cpp {
                code.add_import("<zircon/status.h>");
            } else {
                code.add_import("fuchsia_component::server::*");
            }
        }
    }
    Ok(())
}

// Update TestCodeBuilder based on 'expose' declarations in the .cm file
fn update_code_for_expose_declaration(
    exposes: &Vec<Expose>,
    code: &mut dyn TestCodeBuilder,
    cpp: bool,
) -> Result<()> {
    let mut protos_to_test = Protocols { protocols: HashMap::new() };

    for i in 0..exposes.len() {
        match &exposes[i] {
            Expose::Protocol(decl) => {
                if let Some(protocol) = &decl.source_name {
                    code.add_protocol(protocol, "self", vec!["root".to_string()]);
                    if cpp {
                        protos_to_test.add_protocol_cpp(&protocol, "self")?;
                    } else {
                        protos_to_test.add_protocol_rust(&protocol, "self")?;
                    }
                }
            }
            _ => (),
        }
    }

    // Generate test case code for each protocol exposed
    let protocols = protos_to_test.protocols.get("self");
    if let Some(protocols) = protocols {
        for (fidl_lib, markers) in protocols.iter() {
            if cpp {
                code.add_import(&format!("<{}>", fidl_lib));
            } else {
                code.add_import(&format!("{}::*", fidl_lib));
            }
            for i in 0..markers.len() {
                code.add_test_case(&markers[i]);
                code.add_fidl_connect(&markers[i]);
            }
        }
    }
    Ok(())
}

// Keeps track of all the protocol exposed or used by the component-under-test.
#[derive(Clone)]
struct Protocols {
    // This is a nested map
    // Outer map is keyed by component names, value is a map of fidl library and corresponding protocols.
    // Inner map is keyed by fidl library named, value is a list of protocols defined in the fidl_library.
    protocols: HashMap<String, HashMap<String, Vec<String>>>,
}

impl Protocols {
    pub fn add_protocol_rust<'a>(
        &'a mut self,
        protocol: &str,
        component: &str,
    ) -> Result<(), anyhow::Error> {
        let fields: Vec<&str> = protocol.split(".").collect();
        let mut fidl_lib = "fidl".to_string(); // ex: fidl_fuchsia_metrics
        for i in 0..(fields.len() - 1) {
            fidl_lib.push_str(format!("_{}", fields[i]).as_str());
        }
        let capture =
            Regex::new(r"^(?P<protocol>\w+)").unwrap().captures(fields.last().unwrap()).unwrap();

        let marker = self
            .protocols
            .entry(component.to_string())
            .or_insert_with_key(|_| HashMap::new())
            .entry(fidl_lib)
            .or_insert(Vec::new());
        marker.push(format!("{}", capture.name("protocol").unwrap().as_str()));
        marker.dedup();
        Ok(())
    }

    pub fn add_protocol_cpp<'a>(
        &'a mut self,
        protocol: &str,
        component: &str,
    ) -> Result<(), anyhow::Error> {
        let fields: Vec<&str> = protocol.split(".").collect();
        let mut fidl_lib_path = vec![];
        let mut protocol_type = "".to_string(); // ex: fuchsia::metrics::MetricEventLogger
        for i in 0..(fields.len() - 1) {
            fidl_lib_path.push(fields[i]);
            protocol_type.push_str(format!("{}::", fields[i]).as_str());
        }
        fidl_lib_path.push("cpp");
        fidl_lib_path.push("fidl.h"); // ex: /fuchsia/metrics/cpp/fidl.h

        let capture =
            Regex::new(r"^(?P<protocol>\w+)").unwrap().captures(fields.last().unwrap()).unwrap();

        let marker = self
            .protocols
            .entry(component.to_string())
            .or_insert_with_key(|_| HashMap::new())
            .entry(fidl_lib_path.join("/"))
            .or_insert(Vec::new());
        marker.push(format!("{}{}", protocol_type, capture.name("protocol").unwrap().as_str()));
        marker.dedup();
        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_add_protocol_rust() -> Result<()> {
        let mut p = Protocols { protocols: HashMap::new() };
        p.add_protocol_rust("fuchsia.diagnostics.internal.FooController", "diagnostics")?;
        p.add_protocol_rust("fuchsia.diagnostics.internal.BarController-A", "diagnostics")?;
        p.add_protocol_rust("fuchsia.diagnostics.internal.BarController-B", "diagnostics")?;
        p.add_protocol_rust("fuchsia.metrics.FooController", "cobalt")?;
        p.add_protocol_rust("fuchsia.metrics.BarController", "cobalt")?;
        p.add_protocol_rust("fuchsia.metrics2.BazController", "cobalt")?;

        assert_eq!(p.protocols.len(), 2);
        assert!(p.protocols.contains_key("diagnostics"));
        assert!(p.protocols.contains_key("cobalt"));

        let d = p.protocols.get("diagnostics").unwrap();
        assert_eq!(d.len(), 1);
        assert!(d.contains_key("fidl_fuchsia_diagnostics_internal"));

        let d_markers = d.get("fidl_fuchsia_diagnostics_internal").unwrap();
        assert_eq!(d_markers.len(), 2);
        assert_eq!(d_markers[0], "FooController");
        assert_eq!(d_markers[1], "BarController");

        let c = p.protocols.get("cobalt").unwrap();
        assert_eq!(c.len(), 2);
        assert!(c.contains_key("fidl_fuchsia_metrics"));
        assert!(c.contains_key("fidl_fuchsia_metrics2"));

        let c_markers = c.get("fidl_fuchsia_metrics").unwrap();
        assert_eq!(c_markers.len(), 2);
        assert_eq!(c_markers[0], "FooController");
        assert_eq!(c_markers[1], "BarController");

        Ok(())
    }

    #[test]
    fn test_add_protocol_cpp() -> Result<()> {
        let mut p = Protocols { protocols: HashMap::new() };
        p.add_protocol_cpp("fuchsia.diagnostics.internal.FooController", "diagnostics")?;
        p.add_protocol_cpp("fuchsia.diagnostics.internal.BarController-A", "diagnostics")?;
        p.add_protocol_cpp("fuchsia.diagnostics.internal.BarController-B", "diagnostics")?;
        p.add_protocol_cpp("fuchsia.metrics.FooController", "cobalt")?;
        p.add_protocol_cpp("fuchsia.metrics.BarController", "cobalt")?;
        p.add_protocol_cpp("fuchsia.metrics2.BazController", "cobalt")?;

        assert_eq!(p.protocols.len(), 2);
        assert!(p.protocols.contains_key("diagnostics"));
        assert!(p.protocols.contains_key("cobalt"));

        let d = p.protocols.get("diagnostics").unwrap();
        assert_eq!(d.len(), 1);
        assert!(d.contains_key("fuchsia/diagnostics/internal/cpp/fidl.h"));

        let d_markers = d.get("fuchsia/diagnostics/internal/cpp/fidl.h").unwrap();
        assert_eq!(d_markers.len(), 2);
        assert_eq!(d_markers[0], "fuchsia::diagnostics::internal::FooController");
        assert_eq!(d_markers[1], "fuchsia::diagnostics::internal::BarController");

        let c = p.protocols.get("cobalt").unwrap();
        assert_eq!(c.len(), 2);
        assert!(c.contains_key("fuchsia/metrics/cpp/fidl.h"));
        assert!(c.contains_key("fuchsia/metrics2/cpp/fidl.h"));

        let c_markers = c.get("fuchsia/metrics/cpp/fidl.h").unwrap();
        assert_eq!(c_markers.len(), 2);
        assert_eq!(c_markers[0], "fuchsia::metrics::FooController");
        assert_eq!(c_markers[1], "fuchsia::metrics::BarController");

        Ok(())
    }

    #[test]
    fn test_cpp_update_code_for_use_declaration() -> Result<()> {
        let use_protocol_1 = Use::Protocol(UseProtocol {
            source_name: Some("fuchsia.diagnostics.ArchiveAccessor".to_string()),
            ..Default::default()
        });
        let use_protocol_2 = Use::Protocol(UseProtocol {
            source_name: Some("fuchsia.metrics.MetricEventLoggerFactory".to_string()),
            ..Default::default()
        });
        let use_dir = Use::Directory(UseDirectory {
            source_name: Some("config-data".to_string()),
            target_path: Some("/config/data".to_string()),
            ..Default::default()
        });
        let component_name = "foo_bar";
        let uses = vec![use_protocol_1, use_protocol_2, use_dir];
        let code = &mut CppTestCode::new(&component_name);
        update_code_for_use_declaration(
            &uses, code, true, /* gen_mocks*/
            true, /* cpp */
        )?;
        let create_realm_impl = code.realm_builder_snippets.join("\n");
        let expect_realm_snippets = r#"      .AddLocalChild(
        "archive_accessor",
        &mock_archive_accessor)
      .AddRoute(component_testing::Route {
        .capabilities = {component_testing::Protocol {"fuchsia.diagnostics.ArchiveAccessor"}},
        .source = component_testing::ChildRef{"archive_accessor"},
        .targets = {component_testing::ParentRef(), component_testing::ChildRef{"foo_bar"}, }})
      .AddLocalChild(
        "metric_event_logger_factory",
        &mock_metric_event_logger_factory)
      .AddRoute(component_testing::Route {
        .capabilities = {component_testing::Protocol {"fuchsia.metrics.MetricEventLoggerFactory"}},
        .source = component_testing::ChildRef{"metric_event_logger_factory"},
        .targets = {component_testing::ParentRef(), component_testing::ChildRef{"foo_bar"}, }})
      .AddRoute(component_testing::Route {
        .capabilities = {component_testing::Directory {
          .name = "config-data",
          .path = "/config/data",
          .rights = fuchsia::io::RW_STAR_DIR,}},
        .source = component_testing::ParentRef(),
        .targets = {component_testing::ChildRef{"foo_bar"}, }})"#;
        assert_eq!(create_realm_impl, expect_realm_snippets);

        let mut all_imports = code.imports.clone();
        all_imports.sort();
        let imports = all_imports.join("\n");
        let expect_imports = r#"#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/metrics/cpp/fidl.h>
#include <zircon/status.h>"#;
        assert_eq!(imports, expect_imports);

        Ok(())
    }

    #[test]
    fn test_rust_update_code_for_use_declaration() -> Result<()> {
        let use_protocol_1 = Use::Protocol(UseProtocol {
            source_name: Some("fuchsia.diagnostics.ArchiveAccessor".to_string()),
            ..Default::default()
        });
        let use_protocol_2 = Use::Protocol(UseProtocol {
            source_name: Some("fuchsia.metrics.MetricEventLoggerFactory".to_string()),
            ..Default::default()
        });
        let use_dir = Use::Directory(UseDirectory {
            source_name: Some("config-data".to_string()),
            target_path: Some("/config/data".to_string()),
            ..Default::default()
        });
        let component_name = "foo_bar";
        let uses = vec![use_protocol_1, use_protocol_2, use_dir];
        let code = &mut RustTestCode::new(&component_name);
        update_code_for_use_declaration(
            &uses, code, true,  /* gen_mocks*/
            false, /* cpp */
        )?;
        let create_realm_impl = code.realm_builder_snippets.join("\n");
        let expect_realm_snippets = r#"        let archive_accessor = builder.add_local_child(
            "archive_accessor",
            move |handles: LocalComponentHandles| Box::pin(FooBarTest::archive_accessor_impl(handles)),
            ChildOptions::new()
        )
        .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.diagnostics.ArchiveAccessor"))
                    .from(&archive_accessor)
                    .to(Ref::parent())
                    .to(&foo_bar),
            )
            .await?;
        let metric_event_logger_factory = builder.add_local_child(
            "metric_event_logger_factory",
            move |handles: LocalComponentHandles| Box::pin(FooBarTest::metric_event_logger_factory_impl(handles)),
            ChildOptions::new()
        )
        .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.metrics.MetricEventLoggerFactory"))
                    .from(&metric_event_logger_factory)
                    .to(Ref::parent())
                    .to(&foo_bar),
            )
            .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::directory("config-data").path("/config/data").rights("fio::RW_STAR_DIR"))
                    .from(Ref::parent())
                    .to(&foo_bar),
            )
            .await?;"#;
        assert_eq!(create_realm_impl, expect_realm_snippets);

        let all_imports = code.imports.clone().into_iter().collect::<Vec<_>>();
        let imports = all_imports.join("\n");
        let expect_imports = r#"use fidl_fuchsia_diagnostics::*;
use fidl_fuchsia_metrics::*;
use fuchsia_component::server::*;"#;
        assert_eq!(imports, expect_imports);

        Ok(())
    }

    #[test]
    fn update_code_with_empty_decl_does_not_fail() {
        struct Case {
            name: &'static str,
            cpp: bool,
            generate_mocks: bool,
        }

        for case in vec![
            Case { name: "cpp w/ mocks", cpp: true, generate_mocks: true },
            Case { name: "cpp w/o mocks", cpp: true, generate_mocks: false },
            Case { name: "rust w/ mocks", cpp: false, generate_mocks: true },
            Case { name: "rust w/o mocks", cpp: false, generate_mocks: false },
        ] {
            let code = &mut CppTestCode::new("test");
            let decl = Component { ..Default::default() };
            update_code_for_use_declaration(
                &decl.uses.as_ref().unwrap_or(&Vec::new()),
                code,
                case.generate_mocks,
                case.cpp,
            )
            .unwrap_or_else(|e| panic!("use {}: {:?}", case.name, e));
            update_code_for_expose_declaration(
                &decl.exposes.as_ref().unwrap_or(&Vec::new()),
                code,
                case.cpp,
            )
            .unwrap_or_else(|e| panic!("expose {}: {:?}", case.name, e));
        }
    }
}
