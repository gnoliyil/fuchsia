// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use fdata::{Dictionary, DictionaryEntry, DictionaryValue};
use fidl::encoding::unpersist;
use fidl_fuchsia_component_decl::*;
use fidl_fuchsia_data as fdata;
use fidl_fuchsia_io as fio;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

fn main() {
    // example.cm has already been compiled by cmc as part of the build process
    // See: https://fuchsia.googlesource.com/fuchsia/+/c4b7ddf8128e782f957374c64f57aa2508ac3fe2/build/package.gni#304
    let mut cm_decl = read_cm("/pkg/meta/example.cm").expect("could not read cm file");

    // profile variant injects this protocol.
    if let Some(uses) = &mut cm_decl.uses {
        uses.retain(|u| match u {
            Use::Protocol(decl) => {
                if decl.source_name == Some("fuchsia.debugdata.Publisher".to_owned()) {
                    let protocol = decl.source_name.clone().unwrap();
                    let target_path = format!("/svc/{}", protocol);
                    assert_eq!(
                        decl,
                        &UseProtocol {
                            dependency_type: Some(DependencyType::Strong),
                            source: Some(Ref::Debug(DebugRef {})),
                            source_name: Some(protocol),
                            target_path: Some(target_path),
                            availability: Some(Availability::Required),
                            ..UseProtocol::EMPTY
                        }
                    );
                    return false;
                }
                return true;
            }
            _ => true,
        })
    }

    let expected_decl = {
        let program = Program {
            runner: Some("elf".to_string()),
            info: Some(fdata::Dictionary {
                entries: Some(vec![
                    fdata::DictionaryEntry {
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str(
                            "bin/example".to_string(),
                        ))),
                    },
                    fdata::DictionaryEntry {
                        key: "lifecycle.stop_event".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str("notify".to_string()))),
                    },
                ]),
                ..fdata::Dictionary::EMPTY
            }),
            ..Program::EMPTY
        };
        let uses = vec![
            Use::Service(UseService {
                dependency_type: Some(DependencyType::Strong),
                source: Some(Ref::Parent(ParentRef {})),
                source_name: Some("fuchsia.fonts.Provider".to_string()),
                target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                availability: Some(Availability::Required),
                ..UseService::EMPTY
            }),
            Use::Protocol(UseProtocol {
                dependency_type: Some(DependencyType::Strong),
                source: Some(Ref::Parent(ParentRef {})),
                source_name: Some("fuchsia.fonts.LegacyProvider".to_string()),
                target_path: Some("/svc/fuchsia.fonts.OldProvider".to_string()),
                availability: Some(Availability::Optional),
                ..UseProtocol::EMPTY
            }),
            Use::Protocol(UseProtocol {
                dependency_type: Some(DependencyType::Strong),
                source: Some(Ref::Debug(DebugRef {})),
                source_name: Some("fuchsia.log.LegacyLog".to_string()),
                target_path: Some("/svc/fuchsia.log.LegacyLog".to_string()),
                availability: Some(Availability::Required),
                ..UseProtocol::EMPTY
            }),
            Use::EventStream(UseEventStream {
                source_name: Some("events".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target_path: Some("/testdir/my_stream".to_string()),
                scope: Some(vec![Ref::Child(ChildRef {
                    collection: None,
                    name: "logger".to_string(),
                })]),
                availability: Some(Availability::Required),
                ..UseEventStream::EMPTY
            }),
            Use::EventStream(UseEventStream {
                source_name: Some("other".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target_path: Some("/testdir/my_stream".to_string()),
                scope: Some(vec![Ref::Child(ChildRef {
                    collection: None,
                    name: "logger".to_string(),
                })]),
                availability: Some(Availability::Required),
                ..UseEventStream::EMPTY
            }),
            Use::EventStream(UseEventStream {
                source_name: Some("some".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target_path: Some("/testdir/my_stream".to_string()),
                scope: Some(vec![Ref::Child(ChildRef {
                    collection: None,
                    name: "logger".to_string(),
                })]),
                availability: Some(Availability::Required),
                ..UseEventStream::EMPTY
            }),
            Use::EventStream(UseEventStream {
                source_name: Some("filtered".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target_path: Some("/svc/fuchsia.component.EventStream".to_string()),
                availability: Some(Availability::Required),
                ..UseEventStream::EMPTY
            }),
            Use::Protocol(UseProtocol {
                dependency_type: Some(DependencyType::Strong),
                source: Some(Ref::Parent(ParentRef {})),
                source_name: Some("fuchsia.logger.LogSink".to_string()),
                target_path: Some("/svc/fuchsia.logger.LogSink".to_string()),
                availability: Some(Availability::Required),
                ..UseProtocol::EMPTY
            }),
        ];
        let exposes = vec![
            Expose::Service(ExposeService {
                source: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                source_name: Some("fuchsia.logger.Log".to_string()),
                target_name: Some("fuchsia.logger.Log".to_string()),
                target: Some(Ref::Parent(ParentRef {})),
                ..ExposeService::EMPTY
            }),
            Expose::Protocol(ExposeProtocol {
                source: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                target_name: Some("fuchsia.logger.OldLog".to_string()),
                target: Some(Ref::Parent(ParentRef {})),
                ..ExposeProtocol::EMPTY
            }),
            Expose::Directory(ExposeDirectory {
                source: Some(Ref::Self_(SelfRef {})),
                source_name: Some("blobfs".to_string()),
                target_name: Some("blobfs".to_string()),
                target: Some(Ref::Parent(ParentRef {})),
                rights: None,
                subdir: Some("blob".to_string()),
                ..ExposeDirectory::EMPTY
            }),
            Expose::EventStream(ExposeEventStream {
                source: Some(Ref::Framework(FrameworkRef {})),
                source_name: Some("started".to_string()),
                target: Some(Ref::Parent(ParentRef {})),
                scope: Some(vec![Ref::Child(ChildRef {
                    name: "logger".to_string(),
                    collection: None,
                })]),
                target_name: Some("started".to_string()),
                ..ExposeEventStream::EMPTY
            }),
            Expose::EventStream(ExposeEventStream {
                source: Some(Ref::Framework(FrameworkRef {})),
                source_name: Some("stopped".to_string()),
                target: Some(Ref::Parent(ParentRef {})),
                scope: Some(vec![Ref::Child(ChildRef {
                    name: "logger".to_string(),
                    collection: None,
                })]),
                target_name: Some("stopped".to_string()),
                ..ExposeEventStream::EMPTY
            }),
        ];
        let offers = vec![
            Offer::Service(OfferService {
                source: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                source_name: Some("fuchsia.logger.Log".to_string()),
                target: Some(Ref::Collection(CollectionRef { name: "modular".to_string() })),
                target_name: Some("fuchsia.logger.Log".to_string()),
                availability: Some(Availability::Required),
                ..OfferService::EMPTY
            }),
            Offer::Protocol(OfferProtocol {
                source: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                target: Some(Ref::Collection(CollectionRef { name: "modular".to_string() })),
                target_name: Some("fuchsia.logger.OldLog".to_string()),
                dependency_type: Some(DependencyType::Strong),
                availability: Some(Availability::Required),
                ..OfferProtocol::EMPTY
            }),
            Offer::EventStream(OfferEventStream {
                source_name: Some("directory_ready".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                filter: Some(Dictionary {
                    entries: Some(vec![DictionaryEntry {
                        key: "name".to_string(),
                        value: Some(Box::new(DictionaryValue::Str("diagnostics".to_string()))),
                    }]),
                    ..Dictionary::EMPTY
                }),
                target_name: Some("directory_ready".to_string()),
                availability: Some(Availability::SameAsTarget),
                ..OfferEventStream::EMPTY
            }),
            Offer::EventStream(OfferEventStream {
                source_name: Some("started".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                scope: Some(vec![Ref::Child(ChildRef {
                    name: "logger".to_string(),
                    collection: None,
                })]),
                target_name: Some("started".to_string()),
                availability: Some(Availability::Required),
                ..OfferEventStream::EMPTY
            }),
            Offer::EventStream(OfferEventStream {
                source_name: Some("stopped".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                target: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                scope: Some(vec![Ref::Child(ChildRef {
                    name: "logger".to_string(),
                    collection: None,
                })]),
                target_name: Some("stopped".to_string()),
                availability: Some(Availability::Required),
                ..OfferEventStream::EMPTY
            }),
            Offer::Protocol(OfferProtocol {
                source: Some(Ref::VoidType(VoidRef {})),
                source_name: Some("fuchsia.logger.LegacyLog2".to_string()),
                target: Some(Ref::Collection(CollectionRef { name: "modular".to_string() })),
                target_name: Some("fuchsia.logger.OldLog2".to_string()),
                dependency_type: Some(DependencyType::Strong),
                availability: Some(Availability::Optional),
                ..OfferProtocol::EMPTY
            }),
            Offer::Protocol(OfferProtocol {
                source: Some(Ref::Child(ChildRef { name: "logger".to_string(), collection: None })),
                source_name: Some("fuchsia.logger.LegacyLog3".to_string()),
                target: Some(Ref::Collection(CollectionRef { name: "modular".to_string() })),
                target_name: Some("fuchsia.logger.OldLog3".to_string()),
                dependency_type: Some(DependencyType::Strong),
                availability: Some(Availability::Required),
                ..OfferProtocol::EMPTY
            }),
            Offer::Protocol(OfferProtocol {
                source: Some(Ref::Parent(ParentRef {})),
                source_name: Some("fuchsia.logger.LegacyLog4".to_string()),
                target: Some(Ref::Collection(CollectionRef { name: "modular".to_string() })),
                target_name: Some("fuchsia.logger.OldLog4".to_string()),
                dependency_type: Some(DependencyType::Strong),
                availability: Some(Availability::Optional),
                ..OfferProtocol::EMPTY
            }),
        ];
        let capabilities = vec![
            Capability::Service(Service {
                name: Some("fuchsia.logger.Log".to_string()),
                source_path: Some("/svc/fuchsia.logger.Log".to_string()),
                ..Service::EMPTY
            }),
            Capability::Protocol(Protocol {
                name: Some("fuchsia.logger.Log2".to_string()),
                source_path: Some("/svc/fuchsia.logger.Log2".to_string()),
                ..Protocol::EMPTY
            }),
            Capability::Directory(Directory {
                name: Some("blobfs".to_string()),
                source_path: Some("/volumes/blobfs".to_string()),
                rights: Some(
                    fio::Operations::CONNECT
                        | fio::Operations::READ_BYTES
                        | fio::Operations::WRITE_BYTES
                        | fio::Operations::GET_ATTRIBUTES
                        | fio::Operations::UPDATE_ATTRIBUTES
                        | fio::Operations::ENUMERATE
                        | fio::Operations::TRAVERSE
                        | fio::Operations::MODIFY_DIRECTORY,
                ),
                ..Directory::EMPTY
            }),
            Capability::Storage(Storage {
                name: Some("minfs".to_string()),
                source: Some(Ref::Parent(ParentRef {})),
                backing_dir: Some("data".to_string()),
                subdir: None,
                storage_id: Some(StorageId::StaticInstanceIdOrMoniker),
                ..Storage::EMPTY
            }),
            Capability::Runner(Runner {
                name: Some("dart_runner".to_string()),
                source_path: Some("/svc/fuchsia.sys2.Runner".to_string()),
                ..Runner::EMPTY
            }),
            Capability::Resolver(Resolver {
                name: Some("pkg_resolver".to_string()),
                source_path: Some("/svc/fuchsia.pkg.Resolver".to_string()),
                ..Resolver::EMPTY
            }),
        ];
        let children = vec![Child {
            name: Some("logger".to_string()),
            url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
            startup: Some(StartupMode::Lazy),
            environment: Some("env_one".to_string()),
            ..Child::EMPTY
        }];
        let collections = vec![
            Collection {
                name: Some("modular".to_string()),
                durability: Some(Durability::Transient),
                environment: None,
                allowed_offers: None,
                allow_long_names: None,
                persistent_storage: None,
                ..Collection::EMPTY
            },
            Collection {
                name: Some("explicit_static".to_string()),
                durability: Some(Durability::Transient),
                environment: None,
                allowed_offers: Some(AllowedOffers::StaticOnly),
                allow_long_names: None,
                persistent_storage: None,
                ..Collection::EMPTY
            },
            Collection {
                name: Some("explicit_dynamic".to_string()),
                durability: Some(Durability::Transient),
                environment: None,
                allowed_offers: Some(AllowedOffers::StaticAndDynamic),
                allow_long_names: None,
                persistent_storage: None,
                ..Collection::EMPTY
            },
            Collection {
                name: Some("long_child_names".to_string()),
                durability: Some(Durability::Transient),
                environment: None,
                allowed_offers: None,
                allow_long_names: Some(true),
                persistent_storage: None,
                ..Collection::EMPTY
            },
            Collection {
                name: Some("persistent_storage".to_string()),
                durability: Some(Durability::Transient),
                environment: None,
                allowed_offers: None,
                allow_long_names: None,
                persistent_storage: Some(true),
                ..Collection::EMPTY
            },
        ];
        let facets = fdata::Dictionary {
            entries: Some(vec![
                fdata::DictionaryEntry {
                    key: "author".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::Str("Fuchsia".to_string()))),
                },
                fdata::DictionaryEntry {
                    key: "metadata.publisher".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::Str(
                        "The Books Publisher".to_string(),
                    ))),
                },
                fdata::DictionaryEntry {
                    key: "year".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::Str("2018".to_string()))),
                },
            ]),
            ..fdata::Dictionary::EMPTY
        };
        let envs = vec![
            Environment {
                name: Some("env_one".to_string()),
                extends: Some(EnvironmentExtends::None),
                stop_timeout_ms: Some(1337),
                runners: None,
                resolvers: None,
                debug_capabilities: None,
                ..Environment::EMPTY
            },
            Environment {
                name: Some("env_two".to_string()),
                extends: Some(EnvironmentExtends::Realm),
                stop_timeout_ms: None,
                runners: None,
                resolvers: None,
                debug_capabilities: Some(vec![
                    DebugRegistration::Protocol(DebugProtocolRegistration {
                        source_name: Some("fuchsia.logger.LegacyLog".to_string()),
                        source: Some(Ref::Child(ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        target_name: Some("fuchsia.logger.LegacyLog".to_string()),
                        ..DebugProtocolRegistration::EMPTY
                    }),
                    DebugRegistration::Protocol(DebugProtocolRegistration {
                        source_name: Some("fuchsia.logger.OtherLog".to_string()),
                        source: Some(Ref::Parent(ParentRef {})),
                        target_name: Some("fuchsia.logger.OtherLog".to_string()),
                        ..DebugProtocolRegistration::EMPTY
                    }),
                    DebugRegistration::Protocol(DebugProtocolRegistration {
                        source_name: Some("fuchsia.logger.Log2".to_string()),
                        source: Some(Ref::Self_(SelfRef {})),
                        target_name: Some("fuchsia.logger.Log2".to_string()),
                        ..DebugProtocolRegistration::EMPTY
                    }),
                ]),
                ..Environment::EMPTY
            },
        ];

        let config = ConfigSchema {
            fields: Some(vec![
                ConfigField {
                    key: Some("my_flag".to_string()),
                    type_: Some(ConfigType {
                        layout: ConfigTypeLayout::Bool,
                        parameters: Some(vec![]),
                        constraints: vec![],
                    }),
                    ..ConfigField::EMPTY
                },
                ConfigField {
                    key: Some("my_string".to_string()),
                    type_: Some(ConfigType {
                        layout: ConfigTypeLayout::String,
                        constraints: vec![LayoutConstraint::MaxSize(100)],
                        parameters: Some(vec![]),
                    }),
                    ..ConfigField::EMPTY
                },
                ConfigField {
                    key: Some("my_uint8".to_string()),
                    type_: Some(ConfigType {
                        layout: ConfigTypeLayout::Uint8,
                        parameters: Some(vec![]),
                        constraints: vec![],
                    }),
                    ..ConfigField::EMPTY
                },
                ConfigField {
                    key: Some("my_vector_of_string".to_string()),
                    type_: Some(ConfigType {
                        layout: ConfigTypeLayout::Vector,
                        constraints: vec![LayoutConstraint::MaxSize(100)],
                        parameters: Some(vec![LayoutParameter::NestedType(ConfigType {
                            layout: ConfigTypeLayout::String,
                            constraints: vec![LayoutConstraint::MaxSize(50)],
                            parameters: Some(vec![]),
                        })]),
                    }),
                    ..ConfigField::EMPTY
                },
            ]),
            checksum: Some(ConfigChecksum::Sha256([
                227, 126, 183, 151, 1, 15, 67, 110, 144, 49, 222, 176, 221, 111, 19, 165, 34, 204,
                240, 41, 165, 95, 117, 57, 203, 42, 186, 167, 84, 26, 25, 231,
            ])),
            value_source: Some(ConfigValueSource::PackagePath("meta/example.cvf".to_string())),
            ..ConfigSchema::EMPTY
        };

        Component {
            program: Some(program),
            uses: Some(uses),
            exposes: Some(exposes),
            offers: Some(offers),
            capabilities: Some(capabilities),
            children: Some(children),
            collections: Some(collections),
            facets: Some(facets),
            environments: Some(envs),
            config: Some(config),
            ..Component::EMPTY
        }
    };
    assert_eq!(cm_decl, expected_decl);
}

fn read_cm(file: &str) -> Result<Component, Error> {
    let mut buffer = Vec::new();
    let path = PathBuf::from(file);
    File::open(&path)?.read_to_end(&mut buffer)?;
    Ok(unpersist(&buffer)?)
}
