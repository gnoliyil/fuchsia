// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    cm_fidl_validator,
    cm_json::{self, cm, Error},
    fidl_fuchsia_sys2 as fsys,
    serde_json::{Map, Value},
};

/// Converts the contents of a CM file and produces the equivalent FIDL.
/// The mapping between CM-JSON and CM-FIDL is 1-1. The only difference is the language semantics
/// used to express particular data structures.
/// This function also applies cm_fidl_validator to the generated FIDL.
pub fn translate(buffer: &str) -> Result<fsys::ComponentDecl, Error> {
    let document: cm::Document = serde_json::from_str(&buffer)?;
    let decl = document.cm_into()?;
    cm_fidl_validator::validate(&decl).map_err(|e| Error::validate_fidl(e))?;
    Ok(decl)
}

/// Converts a cm object into its corresponding fidl representation.
trait CmInto<T> {
    fn cm_into(self) -> Result<T, Error>;
}

impl<T, U> CmInto<Option<Vec<U>>> for Option<Vec<T>>
where
    T: CmInto<U>,
{
    fn cm_into(self) -> Result<Option<Vec<U>>, Error> {
        self.and_then(|x| if x.is_empty() { None } else { Some(x.cm_into()) }).transpose()
    }
}

impl<T, U> CmInto<Vec<U>> for Vec<T>
where
    T: CmInto<U>,
{
    fn cm_into(self) -> Result<Vec<U>, Error> {
        self.into_iter().map(|x| x.cm_into()).collect()
    }
}

impl CmInto<fsys::ComponentDecl> for cm::Document {
    fn cm_into(self) -> Result<fsys::ComponentDecl, Error> {
        Ok(fsys::ComponentDecl {
            program: self.program.cm_into()?,
            uses: self.uses.cm_into()?,
            exposes: self.exposes.cm_into()?,
            offers: self.offers.cm_into()?,
            children: self.children.cm_into()?,
            collections: self.collections.cm_into()?,
            facets: self.facets.cm_into()?,
            storage: self.storage.cm_into()?,
            runners: self.runners.cm_into()?,
            environments: self.environments.cm_into()?,
            resolvers: None,
        })
    }
}

impl CmInto<fsys::UseDecl> for cm::Use {
    fn cm_into(self) -> Result<fsys::UseDecl, Error> {
        Ok(match self {
            cm::Use::Service(s) => fsys::UseDecl::Service(s.cm_into()?),
            cm::Use::Protocol(s) => fsys::UseDecl::Protocol(s.cm_into()?),
            cm::Use::Directory(d) => fsys::UseDecl::Directory(d.cm_into()?),
            cm::Use::Storage(s) => fsys::UseDecl::Storage(s.cm_into()?),
            cm::Use::Runner(r) => fsys::UseDecl::Runner(r.cm_into()?),
        })
    }
}

impl CmInto<fsys::ExposeDecl> for cm::Expose {
    fn cm_into(self) -> Result<fsys::ExposeDecl, Error> {
        Ok(match self {
            cm::Expose::Service(s) => fsys::ExposeDecl::Service(s.cm_into()?),
            cm::Expose::Protocol(s) => fsys::ExposeDecl::Protocol(s.cm_into()?),
            cm::Expose::Directory(d) => fsys::ExposeDecl::Directory(d.cm_into()?),
            cm::Expose::Runner(r) => fsys::ExposeDecl::Runner(r.cm_into()?),
            cm::Expose::Resolver(_) => return Err(Error::internal("unimplemented")),
        })
    }
}

impl CmInto<fsys::Ref> for cm::ExposeTarget {
    fn cm_into(self) -> Result<fsys::Ref, Error> {
        Ok(match self {
            cm::ExposeTarget::Realm => fsys::Ref::Realm(fsys::RealmRef {}),
            cm::ExposeTarget::Framework => fsys::Ref::Framework(fsys::FrameworkRef {}),
        })
    }
}

impl CmInto<fsys::DependencyType> for cm::DependencyType {
    fn cm_into(self) -> Result<fsys::DependencyType, Error> {
        Ok(match self {
            cm::DependencyType::Strong => fsys::DependencyType::Strong,
            cm::DependencyType::WeakForMigration => fsys::DependencyType::WeakForMigration,
        })
    }
}

impl CmInto<fsys::OfferDecl> for cm::Offer {
    fn cm_into(self) -> Result<fsys::OfferDecl, Error> {
        Ok(match self {
            cm::Offer::Service(s) => fsys::OfferDecl::Service(s.cm_into()?),
            cm::Offer::Protocol(s) => fsys::OfferDecl::Protocol(s.cm_into()?),
            cm::Offer::Directory(d) => fsys::OfferDecl::Directory(d.cm_into()?),
            cm::Offer::Storage(s) => fsys::OfferDecl::Storage(s.cm_into()?),
            cm::Offer::Runner(r) => fsys::OfferDecl::Runner(r.cm_into()?),
            cm::Offer::Resolver(_) => return Err(Error::internal("unimplemented")),
        })
    }
}

impl CmInto<fsys::UseServiceDecl> for cm::UseService {
    fn cm_into(self) -> Result<fsys::UseServiceDecl, Error> {
        Ok(fsys::UseServiceDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
        })
    }
}

impl CmInto<fsys::UseProtocolDecl> for cm::UseProtocol {
    fn cm_into(self) -> Result<fsys::UseProtocolDecl, Error> {
        Ok(fsys::UseProtocolDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
        })
    }
}

impl CmInto<fsys::UseDirectoryDecl> for cm::UseDirectory {
    fn cm_into(self) -> Result<fsys::UseDirectoryDecl, Error> {
        Ok(fsys::UseDirectoryDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
            rights: self.rights.into(),
            subdir: self.subdir.map(|s| s.into()),
        })
    }
}

impl CmInto<fsys::UseStorageDecl> for cm::UseStorage {
    fn cm_into(self) -> Result<fsys::UseStorageDecl, Error> {
        Ok(fsys::UseStorageDecl {
            type_: Some(self.type_.cm_into()?),
            target_path: self.target_path.map(|path| path.into()),
        })
    }
}

impl CmInto<fsys::UseRunnerDecl> for cm::UseRunner {
    fn cm_into(self) -> Result<fsys::UseRunnerDecl, Error> {
        Ok(fsys::UseRunnerDecl { source_name: Some(self.source_name.into()) })
    }
}

impl CmInto<fsys::ExposeServiceDecl> for cm::ExposeService {
    fn cm_into(self) -> Result<fsys::ExposeServiceDecl, Error> {
        Ok(fsys::ExposeServiceDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
            target: Some(self.target.cm_into()?),
        })
    }
}

impl CmInto<fsys::ExposeProtocolDecl> for cm::ExposeProtocol {
    fn cm_into(self) -> Result<fsys::ExposeProtocolDecl, Error> {
        Ok(fsys::ExposeProtocolDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
            target: Some(self.target.cm_into()?),
        })
    }
}

impl CmInto<fsys::ExposeDirectoryDecl> for cm::ExposeDirectory {
    fn cm_into(self) -> Result<fsys::ExposeDirectoryDecl, Error> {
        Ok(fsys::ExposeDirectoryDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target_path: Some(self.target_path.into()),
            target: Some(self.target.cm_into()?),
            rights: match self.rights {
                Some(rights) => rights.into(),
                None => None,
            },
            subdir: self.subdir.map(|s| s.into()),
        })
    }
}

impl CmInto<fsys::ExposeRunnerDecl> for cm::ExposeRunner {
    fn cm_into(self) -> Result<fsys::ExposeRunnerDecl, Error> {
        Ok(fsys::ExposeRunnerDecl {
            source: Some(self.source.cm_into()?),
            source_name: Some(self.source_name.into()),
            target: Some(self.target.cm_into()?),
            target_name: Some(self.target_name.into()),
        })
    }
}

impl CmInto<fsys::OfferServiceDecl> for cm::OfferService {
    fn cm_into(self) -> Result<fsys::OfferServiceDecl, Error> {
        Ok(fsys::OfferServiceDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target: Some(self.target.cm_into()?),
            target_path: Some(self.target_path.into()),
        })
    }
}

impl CmInto<fsys::OfferProtocolDecl> for cm::OfferProtocol {
    fn cm_into(self) -> Result<fsys::OfferProtocolDecl, Error> {
        Ok(fsys::OfferProtocolDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target: Some(self.target.cm_into()?),
            target_path: Some(self.target_path.into()),
            dependency_type: Some(self.dependency_type.cm_into()?),
        })
    }
}

impl CmInto<fsys::OfferDirectoryDecl> for cm::OfferDirectory {
    fn cm_into(self) -> Result<fsys::OfferDirectoryDecl, Error> {
        Ok(fsys::OfferDirectoryDecl {
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
            target: Some(self.target.cm_into()?),
            target_path: Some(self.target_path.into()),
            rights: match self.rights {
                Some(rights) => rights.into(),
                None => None,
            },
            subdir: self.subdir.map(|s| s.into()),
            dependency_type: Some(self.dependency_type.cm_into()?),
        })
    }
}

impl CmInto<fsys::OfferStorageDecl> for cm::OfferStorage {
    fn cm_into(self) -> Result<fsys::OfferStorageDecl, Error> {
        Ok(fsys::OfferStorageDecl {
            type_: Some(self.type_.cm_into()?),
            source: Some(self.source.cm_into()?),
            target: Some(self.target.cm_into()?),
        })
    }
}

impl CmInto<fsys::OfferRunnerDecl> for cm::OfferRunner {
    fn cm_into(self) -> Result<fsys::OfferRunnerDecl, Error> {
        Ok(fsys::OfferRunnerDecl {
            source: Some(self.source.cm_into()?),
            source_name: Some(self.source_name.into()),
            target: Some(self.target.cm_into()?),
            target_name: Some(self.target_name.into()),
        })
    }
}

impl CmInto<fsys::StorageType> for cm::StorageType {
    fn cm_into(self) -> Result<fsys::StorageType, Error> {
        match self {
            cm::StorageType::Data => Ok(fsys::StorageType::Data),
            cm::StorageType::Cache => Ok(fsys::StorageType::Cache),
            cm::StorageType::Meta => Ok(fsys::StorageType::Meta),
        }
    }
}

impl CmInto<fsys::ChildDecl> for cm::Child {
    fn cm_into(self) -> Result<fsys::ChildDecl, Error> {
        Ok(fsys::ChildDecl {
            name: Some(self.name.into()),
            url: Some(self.url.into()),
            startup: Some(self.startup.cm_into()?),
            environment: self.environment.map(|e| e.into()),
        })
    }
}

impl CmInto<fsys::CollectionDecl> for cm::Collection {
    fn cm_into(self) -> Result<fsys::CollectionDecl, Error> {
        Ok(fsys::CollectionDecl {
            name: Some(self.name.into()),
            durability: Some(self.durability.cm_into()?),
        })
    }
}

impl CmInto<fsys::StorageDecl> for cm::Storage {
    fn cm_into(self) -> Result<fsys::StorageDecl, Error> {
        Ok(fsys::StorageDecl {
            name: Some(self.name.into()),
            source_path: Some(self.source_path.into()),
            source: Some(self.source.cm_into()?),
        })
    }
}

impl CmInto<fsys::RunnerDecl> for cm::Runner {
    fn cm_into(self) -> Result<fsys::RunnerDecl, Error> {
        Ok(fsys::RunnerDecl {
            name: Some(self.name.into()),
            source: Some(self.source.cm_into()?),
            source_path: Some(self.source_path.into()),
        })
    }
}

impl CmInto<fsys::EnvironmentDecl> for cm::Environment {
    fn cm_into(self) -> Result<fsys::EnvironmentDecl, Error> {
        Ok(fsys::EnvironmentDecl {
            name: Some(self.name.into()),
            extends: Some(self.extends.cm_into()?),
        })
    }
}

impl CmInto<fsys::EnvironmentExtends> for cm::EnvironmentExtends {
    fn cm_into(self) -> Result<fsys::EnvironmentExtends, Error> {
        Ok(match self {
            cm::EnvironmentExtends::None => fsys::EnvironmentExtends::None,
            cm::EnvironmentExtends::Realm => fsys::EnvironmentExtends::Realm,
        })
    }
}

impl CmInto<fsys::RealmRef> for cm::RealmRef {
    fn cm_into(self) -> Result<fsys::RealmRef, Error> {
        Ok(fsys::RealmRef {})
    }
}

impl CmInto<fsys::SelfRef> for cm::SelfRef {
    fn cm_into(self) -> Result<fsys::SelfRef, Error> {
        Ok(fsys::SelfRef {})
    }
}

impl CmInto<fsys::ChildRef> for cm::ChildRef {
    fn cm_into(self) -> Result<fsys::ChildRef, Error> {
        Ok(fsys::ChildRef { name: self.name.into(), collection: None })
    }
}

impl CmInto<fsys::CollectionRef> for cm::CollectionRef {
    fn cm_into(self) -> Result<fsys::CollectionRef, Error> {
        Ok(fsys::CollectionRef { name: self.name.into() })
    }
}

impl CmInto<fsys::StorageRef> for cm::StorageRef {
    fn cm_into(self) -> Result<fsys::StorageRef, Error> {
        Ok(fsys::StorageRef { name: self.name.into() })
    }
}

impl CmInto<fsys::FrameworkRef> for cm::FrameworkRef {
    fn cm_into(self) -> Result<fsys::FrameworkRef, Error> {
        Ok(fsys::FrameworkRef {})
    }
}

impl CmInto<fsys::Ref> for cm::Ref {
    fn cm_into(self) -> Result<fsys::Ref, Error> {
        Ok(match self {
            cm::Ref::Realm(r) => fsys::Ref::Realm(r.cm_into()?),
            cm::Ref::Self_(s) => fsys::Ref::Self_(s.cm_into()?),
            cm::Ref::Child(c) => fsys::Ref::Child(c.cm_into()?),
            cm::Ref::Collection(c) => fsys::Ref::Collection(c.cm_into()?),
            cm::Ref::Storage(r) => fsys::Ref::Storage(r.cm_into()?),
            cm::Ref::Framework(f) => fsys::Ref::Framework(f.cm_into()?),
        })
    }
}

impl CmInto<fsys::Durability> for cm::Durability {
    fn cm_into(self) -> Result<fsys::Durability, Error> {
        Ok(match self {
            cm::Durability::Persistent => fsys::Durability::Persistent,
            cm::Durability::Transient => fsys::Durability::Transient,
        })
    }
}

impl CmInto<fsys::StartupMode> for cm::StartupMode {
    fn cm_into(self) -> Result<fsys::StartupMode, Error> {
        Ok(match self {
            cm::StartupMode::Lazy => fsys::StartupMode::Lazy,
            cm::StartupMode::Eager => fsys::StartupMode::Eager,
        })
    }
}

impl CmInto<Option<fsys::Object>> for Option<Map<String, Value>> {
    fn cm_into(self) -> Result<Option<fsys::Object>, Error> {
        match self {
            Some(from) => {
                let obj = object_from_map(from)?;
                Ok(Some(obj))
            }
            None => Ok(None),
        }
    }
}

fn object_from_map(in_obj: Map<String, Value>) -> Result<fsys::Object, Error> {
    let mut out = fsys::Object { entries: vec![] };
    for (k, v) in in_obj {
        if let Some(value) = convert_value(v)? {
            out.entries.push(fsys::Entry { key: k, value: Some(value) });
        }
    }
    Ok(out)
}

fn convert_value(v: Value) -> Result<Option<Box<fsys::Value>>, Error> {
    Ok(match v {
        Value::Null => None,
        Value::Bool(b) => Some(Box::new(fsys::Value::Bit(b))),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                Some(Box::new(fsys::Value::Inum(i)))
            } else if let Some(f) = n.as_f64() {
                Some(Box::new(fsys::Value::Fnum(f)))
            } else {
                return Err(Error::Parse(format!("Number is out of range: {}", n)));
            }
        }
        Value::String(s) => Some(Box::new(fsys::Value::Str(s.clone()))),
        Value::Array(a) => {
            let mut values = vec![];
            for v in a {
                if let Some(value) = convert_value(v)? {
                    values.push(Some(value));
                }
            }
            let vector = fsys::Vector { values };
            Some(Box::new(fsys::Value::Vec(vector)))
        }
        Value::Object(o) => {
            let obj = object_from_map(o)?;
            Some(Box::new(fsys::Value::Obj(obj)))
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_io2 as fio2;
    use serde_json::json;
    use test_util::assert_matches;

    fn translate_test(input: serde_json::value::Value, expected_output: fsys::ComponentDecl) {
        let component_decl = translate(&format!("{}", input)).expect("translation failed");
        assert_eq!(component_decl, expected_output);
    }

    fn new_component_decl() -> fsys::ComponentDecl {
        fsys::ComponentDecl {
            program: None,
            uses: None,
            exposes: None,
            offers: None,
            facets: None,
            children: None,
            collections: None,
            storage: None,
            runners: None,
            environments: None,
            resolvers: None,
        }
    }

    macro_rules! test_translate {
        (
            $(
                $test_name:ident => {
                    input = $input:expr,
                    output = $output:expr,
                },
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    translate_test($input, $output);
                }
            )+
        }
    }

    #[test]
    fn test_translate_invalid_cm_fails() {
        let input = json!({
            "exposes": [
                {
                }
            ]
        });

        let res = translate(&format!("{}", input));
        assert_matches!(res, Err(Error::Parse(_)));
    }

    test_translate! {
        test_translate_empty => {
            input = json!({}),
            output = new_component_decl(),
        },
        test_translate_program => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                }
            }),
            output = {
                let program = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "binary".to_string(),
                        value: Some(Box::new(fsys::Value::Str("bin/app".to_string()))),
                    }
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_object_primitive => {
            input = json!({
                "program": {
                    "string": "bar",
                    "int": -42,
                    "float": 3.14,
                    "bool": true,
                    "ignore": null
                }
            }),
            output = {
                let program = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "bool".to_string(),
                        value: Some(Box::new(fsys::Value::Bit(true))),
                    },
                    fsys::Entry{
                        key: "float".to_string(),
                        value: Some(Box::new(fsys::Value::Fnum(3.14))),
                    },
                    fsys::Entry{
                        key: "int".to_string(),
                        value: Some(Box::new(fsys::Value::Inum(-42))),
                    },
                    fsys::Entry{
                        key: "string".to_string(),
                        value: Some(Box::new(fsys::Value::Str("bar".to_string()))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_object_nested => {
            input = json!({
                "program": {
                    "obj": {
                        "array": [
                            {
                                "string": "bar"
                            },
                            -42
                        ],
                    },
                    "bool": true
                }
            }),
            output = {
                let obj_inner = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "string".to_string(),
                        value: Some(Box::new(fsys::Value::Str("bar".to_string()))),
                    },
                ]};
                let vector = fsys::Vector{values: vec![
                    Some(Box::new(fsys::Value::Obj(obj_inner))),
                    Some(Box::new(fsys::Value::Inum(-42)))
                ]};
                let obj_outer = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "array".to_string(),
                        value: Some(Box::new(fsys::Value::Vec(vector))),
                    },
                ]};
                let program = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "bool".to_string(),
                        value: Some(Box::new(fsys::Value::Bit(true))),
                    },
                    fsys::Entry{
                        key: "obj".to_string(),
                        value: Some(Box::new(fsys::Value::Obj(obj_outer))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_uses => {
            input = json!({
                "uses": [
                    {
                        "service": {
                            "source": {
                                "realm": {}
                            },
                            "source_path": "/fonts/CoolFonts",
                            "target_path": "/svc/fuchsia.fonts.Provider"
                        }
                    },
                    {
                        "service": {
                            "source": {
                                "framework": {}
                            },
                            "source_path": "/svc/fuchsia.sys2.Realm",
                            "target_path": "/svc/fuchsia.sys2.Realm"
                        }
                    },
                    {
                        "protocol": {
                            "source": {
                                "realm": {}
                            },
                            "source_path": "/fonts/CoolFonts",
                            "target_path": "/svc/fuchsia.fonts.Provider"
                        }
                    },
                    {
                        "protocol": {
                            "source": {
                                "framework": {}
                            },
                            "source_path": "/svc/fuchsia.sys2.Realm",
                            "target_path": "/svc/fuchsia.sys2.Realm"
                        }
                    },
                    {
                        "directory": {
                            "source": {
                                "realm": {}
                            },
                            "source_path": "/data/assets",
                            "target_path": "/data",
                            "rights": ["connect", "write_bytes"]
                        }
                    },
                    {
                        "directory": {
                            "source": {
                                "framework": {}
                            },
                            "source_path": "/pkg",
                            "target_path": "/pkg",
                            "rights": ["connect", "read_bytes"],
                            "subdir": "config/data"
                        }
                    },
                    {
                        "storage": {
                            "type": "cache",
                            "target_path": "/cache"
                        }
                    },
                    {
                        "runner": {
                            "source_name": "elf"
                        }
                    }
                ]
            }),
            output = {
                let uses = vec![
                    fsys::UseDecl::Service(fsys::UseServiceDecl {
                        source: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        source_path: Some("/fonts/CoolFonts".to_string()),
                        target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                    }),
                    fsys::UseDecl::Service(fsys::UseServiceDecl {
                        source: Some(fsys::Ref::Framework(fsys::FrameworkRef {})),
                        source_path: Some("/svc/fuchsia.sys2.Realm".to_string()),
                        target_path: Some("/svc/fuchsia.sys2.Realm".to_string()),
                    }),
                    fsys::UseDecl::Protocol(fsys::UseProtocolDecl {
                        source: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        source_path: Some("/fonts/CoolFonts".to_string()),
                        target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                    }),
                    fsys::UseDecl::Protocol(fsys::UseProtocolDecl {
                        source: Some(fsys::Ref::Framework(fsys::FrameworkRef {})),
                        source_path: Some("/svc/fuchsia.sys2.Realm".to_string()),
                        target_path: Some("/svc/fuchsia.sys2.Realm".to_string()),
                    }),
                    fsys::UseDecl::Directory(fsys::UseDirectoryDecl {
                        source: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        source_path: Some("/data/assets".to_string()),
                        target_path: Some("/data".to_string()),
                        rights: Some(fio2::Operations::Connect | fio2::Operations::WriteBytes),
                        subdir: None,
                    }),
                    fsys::UseDecl::Directory(fsys::UseDirectoryDecl {
                        source: Some(fsys::Ref::Framework(fsys::FrameworkRef {})),
                        source_path: Some("/pkg".to_string()),
                        target_path: Some("/pkg".to_string()),
                        rights: Some(fio2::Operations::Connect | fio2::Operations::ReadBytes),
                        subdir: Some("config/data".to_string()),
                    }),
                    fsys::UseDecl::Storage(fsys::UseStorageDecl {
                        type_: Some(fsys::StorageType::Cache),
                        target_path: Some("/cache".to_string()),
                    }),
                    fsys::UseDecl::Runner(fsys::UseRunnerDecl {
                        source_name: Some("elf".to_string()),
                    }),
                ];
                let mut decl = new_component_decl();
                decl.uses = Some(uses);
                decl
            },
        },
        test_translate_exposes => {
            input = json!({
                "exposes": [
                    {
                        "service": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/loggers/fuchsia.logger.Log1",
                            "target_path": "/svc/fuchsia.logger.Log",
                            "target": "realm"
                        }
                    },
                    {
                        "service": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/loggers/fuchsia.logger.Log2",
                            "target_path": "/svc/fuchsia.logger.Log",
                            "target": "realm"
                        }
                    },
                    {
                        "protocol": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/loggers/fuchsia.logger.LegacyLog",
                            "target_path": "/svc/fuchsia.logger.LegacyLog",
                            "target": "realm"
                        }
                    },
                    {
                        "directory": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/volumes/blobfs",
                            "target_path": "/volumes/blobfs",
                            "target": "framework",
                            "rights": ["connect"]
                        }
                    },
                    {
                        "directory": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/data",
                            "target_path": "/data",
                            "target": "realm",
                            "rights": ["connect"],
                            "subdir": "logs"
                        }
                    },
                    {
                        "runner": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_name": "elf",
                            "target": "realm",
                            "target_name": "elf"
                        }
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    }
                ]
            }),
            output = {
                let exposes = vec![
                    fsys::ExposeDecl::Service(fsys::ExposeServiceDecl {
                        source_path: Some("/loggers/fuchsia.logger.Log1".to_string()),
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                    }),
                    fsys::ExposeDecl::Service(fsys::ExposeServiceDecl {
                        source_path: Some("/loggers/fuchsia.logger.Log2".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                    }),
                    fsys::ExposeDecl::Protocol(fsys::ExposeProtocolDecl {
                        source_path: Some("/loggers/fuchsia.logger.LegacyLog".to_string()),
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        target_path: Some("/svc/fuchsia.logger.LegacyLog".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                    }),
                    fsys::ExposeDecl::Directory(fsys::ExposeDirectoryDecl {
                        source_path: Some("/volumes/blobfs".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                        target_path: Some("/volumes/blobfs".to_string()),
                        target: Some(fsys::Ref::Framework(fsys::FrameworkRef {})),
                        rights: Some(fio2::Operations::Connect),
                        subdir: None,
                    }),
                    fsys::ExposeDecl::Directory(fsys::ExposeDirectoryDecl {
                        source_path: Some("/data".to_string()),
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        target_path: Some("/data".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        rights: Some(fio2::Operations::Connect),
                        subdir: Some("logs".to_string()),
                    }),
                    fsys::ExposeDecl::Runner(fsys::ExposeRunnerDecl {
                        source_name: Some("elf".to_string()),
                        source: Some(fsys::Ref::Child(fsys::ChildRef{
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        target_name: Some("elf".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                    }),
                ];
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                        environment: None,
                    },
                ];
                let mut decl = new_component_decl();
                decl.exposes = Some(exposes);
                decl.children = Some(children);
                decl
            },
        },
        test_translate_offers => {
            input = json!({
                "offers": [
                    {
                        "directory": {
                            "source": {
                                "realm": {}
                            },
                            "source_path": "/data/assets",
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "target_path": "/data/realm_assets",
                            "dependency_type": "strong"
                        },
                    },
                    {
                        "directory": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/data/config",
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            },
                            "target_path": "/data/config",
                            "rights": ["connect"],
                            "subdir": "fonts",
                            "dependency_type": "weak_for_migration"
                        }
                    },
                    {
                        "service": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/svc/fuchsia.netstack.Netstack",
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "target_path": "/svc/fuchsia.netstack.Netstack"
                        }
                    },
                    {
                        "service": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/svc/fuchsia.logger.Log1",
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            },
                            "target_path": "/svc/fuchsia.logger.Log"
                        }
                    },
                    {
                        "service": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/svc/fuchsia.logger.Log2",
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            },
                            "target_path": "/svc/fuchsia.logger.Log"
                        }
                    },
                    {
                        "protocol": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/svc/fuchsia.netstack.LegacyNetstack",
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "target_path": "/svc/fuchsia.netstack.LegacyNetstack",
                            "dependency_type": "strong"
                        }
                    },
                    {
                        "protocol": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/svc/fuchsia.logger.LegacyLog",
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            },
                            "target_path": "/svc/fuchsia.logger.LegacySysLog",
                            "dependency_type": "weak_for_migration"
                        }
                    },
                    {
                        "storage": {
                            "type": "data",
                            "source": {
                                "storage": {
                                    "name": "memfs"
                                }
                            },
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            }
                        }
                    },
                    {
                        "storage": {
                            "type": "data",
                            "source": {
                                "storage": {
                                    "name": "memfs"
                                }
                            },
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            }
                        }
                    },
                    {
                        "storage": {
                            "type": "meta",
                            "source": {
                                "realm": {}
                            },
                            "target": {
                                "collection": {
                                    "name": "modular"
                                }
                            }
                        }
                    },
                    {
                        "storage": {
                            "type": "meta",
                            "source": {
                                "realm": {}
                            },
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            }
                        }
                    },
                    {
                        "runner": {
                            "source": {
                                "self": {}
                            },
                            "source_name": "elf",
                            "target": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "target_name": "elf"
                        }
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                        "startup": "eager"
                    }
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent"
                    }
                ],
                "storage": [
                    {
                        "name": "memfs",
                        "source_path": "/memfs",
                        "source": {
                            "self": {}
                        }
                    }
                ]
            }),
            output = {
                let offers = vec![
                    fsys::OfferDecl::Directory(fsys::OfferDirectoryDecl {
                        source: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        source_path: Some("/data/assets".to_string()),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef {
                               name: "logger".to_string(),
                               collection: None,
                           }
                        )),
                        target_path: Some("/data/realm_assets".to_string()),
                        rights: None,
                        subdir: None,
                        dependency_type: Some(fsys::DependencyType::Strong),
                    }),
                    fsys::OfferDecl::Directory(fsys::OfferDirectoryDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        source_path: Some("/data/config".to_string()),
                        target: Some(fsys::Ref::Collection(
                           fsys::CollectionRef {
                               name: "modular".to_string(),
                           }
                        )),
                        target_path: Some("/data/config".to_string()),
                        rights: Some(fio2::Operations::Connect),
                        subdir: Some("fonts".to_string()),
                        dependency_type: Some(fsys::DependencyType::WeakForMigration),
                    }),
                    fsys::OfferDecl::Service(fsys::OfferServiceDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        source_path: Some("/svc/fuchsia.netstack.Netstack".to_string()),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef {
                               name: "logger".to_string(),
                               collection: None,
                           }
                        )),
                        target_path: Some("/svc/fuchsia.netstack.Netstack".to_string()),
                    }),
                    fsys::OfferDecl::Service(fsys::OfferServiceDecl {
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        source_path: Some("/svc/fuchsia.logger.Log1".to_string()),
                        target: Some(fsys::Ref::Collection(
                           fsys::CollectionRef {
                               name: "modular".to_string(),
                           }
                        )),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                    fsys::OfferDecl::Service(fsys::OfferServiceDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        source_path: Some("/svc/fuchsia.logger.Log2".to_string()),
                        target: Some(fsys::Ref::Collection(
                           fsys::CollectionRef {
                               name: "modular".to_string(),
                           }
                        )),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                    fsys::OfferDecl::Protocol(fsys::OfferProtocolDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        source_path: Some("/svc/fuchsia.netstack.LegacyNetstack".to_string()),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef {
                               name: "logger".to_string(),
                               collection: None,
                           }
                        )),
                        target_path: Some("/svc/fuchsia.netstack.LegacyNetstack".to_string()),
                        dependency_type: Some(fsys::DependencyType::Strong),
                    }),
                    fsys::OfferDecl::Protocol(fsys::OfferProtocolDecl {
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        source_path: Some("/svc/fuchsia.logger.LegacyLog".to_string()),
                        target: Some(fsys::Ref::Collection(
                           fsys::CollectionRef {
                               name: "modular".to_string(),
                           }
                        )),
                        target_path: Some("/svc/fuchsia.logger.LegacySysLog".to_string()),
                        dependency_type: Some(fsys::DependencyType::WeakForMigration),
                    }),
                    fsys::OfferDecl::Storage(fsys::OfferStorageDecl {
                        type_: Some(fsys::StorageType::Data),
                        source: Some(fsys::Ref::Storage(fsys::StorageRef {
                            name: "memfs".to_string(),
                        })),
                        target: Some(fsys::Ref::Collection(
                            fsys::CollectionRef { name: "modular".to_string() }
                        )),
                    }),
                    fsys::OfferDecl::Storage(fsys::OfferStorageDecl {
                        type_: Some(fsys::StorageType::Data),
                        source: Some(fsys::Ref::Storage(fsys::StorageRef {
                            name: "memfs".to_string(),
                        })),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef { name: "logger".to_string(), collection: None }
                        )),
                    }),
                    fsys::OfferDecl::Storage(fsys::OfferStorageDecl {
                        type_: Some(fsys::StorageType::Meta),
                        source: Some(fsys::Ref::Realm(fsys::RealmRef { })),
                        target: Some(fsys::Ref::Collection(
                            fsys::CollectionRef { name: "modular".to_string() }
                        )),
                    }),
                    fsys::OfferDecl::Storage(fsys::OfferStorageDecl {
                        type_: Some(fsys::StorageType::Meta),
                        source: Some(fsys::Ref::Realm(fsys::RealmRef { })),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef { name: "logger".to_string(), collection: None }
                        )),
                    }),
                    fsys::OfferDecl::Runner(fsys::OfferRunnerDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef {})),
                        source_name: Some("elf".to_string()),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef {
                               name: "logger".to_string(),
                               collection: None,
                           }
                        )),
                        target_name: Some("elf".to_string()),
                    }),
                ];
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                        environment: None,
                    },
                    fsys::ChildDecl{
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                        environment: None,
                    },
                ];
                let collections = vec![
                    fsys::CollectionDecl{
                        name: Some("modular".to_string()),
                        durability: Some(fsys::Durability::Persistent),
                    },
                ];
                let storages = vec![
                    fsys::StorageDecl {
                        name: Some("memfs".to_string()),
                        source_path: Some("/memfs".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                    },
                ];
                let mut decl = new_component_decl();
                decl.offers = Some(offers);
                decl.children = Some(children);
                decl.collections = Some(collections);
                decl.storage = Some(storages);
                decl
            },
        },
        test_translate_environments => {
            input = json!({
                "children": [
                    {
                        "name": "echo_server",
                        "url": "fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm",
                        "startup": "lazy",
                        "environment": "test_env",
                    },
                ],
                "environments": [
                    {
                        "name": "test_env",
                        "extends": "none",
                    },
                    {
                        "name": "env",
                        "extends": "realm",
                    }
                ]
            }),
            output = {
                let children = vec![
                    fsys::ChildDecl {
                        name: Some("echo_server".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                        environment: Some("test_env".to_string()),
                    },
                ];
                let environments = vec![
                    fsys::EnvironmentDecl {
                        name: Some("test_env".to_string()),
                        extends: Some(fsys::EnvironmentExtends::None),
                    },
                    fsys::EnvironmentDecl {
                        name: Some("env".to_string()),
                        extends: Some(fsys::EnvironmentExtends::Realm),
                    },
                ];
                let mut decl = new_component_decl();
                decl.children = Some(children);
                decl.environments = Some(environments);
                decl
            },
        },
        test_translate_children => {
            input = json!({
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    },
                    {
                        "name": "echo_server",
                        "url": "fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm",
                        "startup": "eager"
                    }
                ],
            }),
            output = {
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                        environment: None,
                    },
                    fsys::ChildDecl{
                        name: Some("echo_server".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                        environment: None,
                    },
                ];
                let mut decl = new_component_decl();
                decl.children = Some(children);
                decl
            },
        },
        test_translate_collections => {
            input = json!({
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent"
                    },
                    {
                        "name": "tests",
                        "durability": "transient"
                    }
                ]
            }),
            output = {
                let collections = vec![
                    fsys::CollectionDecl{
                        name: Some("modular".to_string()),
                        durability: Some(fsys::Durability::Persistent),
                    },
                    fsys::CollectionDecl{
                        name: Some("tests".to_string()),
                        durability: Some(fsys::Durability::Transient),
                    },
                ];
                let mut decl = new_component_decl();
                decl.collections = Some(collections);
                decl
            },
        },
        test_translate_facets => {
            input = json!({
                "facets": {
                    "authors": [
                        "me",
                        "you"
                    ],
                    "title": "foo",
                    "year": 2018
                }
            }),
            output = {
                let vector = fsys::Vector{values: vec![
                    Some(Box::new(fsys::Value::Str("me".to_string()))),
                    Some(Box::new(fsys::Value::Str("you".to_string()))),
                ]};
                let facets = fsys::Object{entries: vec![
                    fsys::Entry{
                        key: "authors".to_string(),
                        value: Some(Box::new(fsys::Value::Vec(vector))),
                    },
                    fsys::Entry{
                        key: "title".to_string(),
                        value: Some(Box::new(fsys::Value::Str("foo".to_string()))),
                    },
                    fsys::Entry{
                        key: "year".to_string(),
                        value: Some(Box::new(fsys::Value::Inum(2018))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.facets = Some(facets);
                decl
            },
        },
        test_translate_storage => {
            input = json!({
                "storage": [
                    {
                        "name": "memfs",
                        "source": {
                            "self": {}
                        },
                        "source_path": "/memfs"
                    }
                ]
            }),
            output = {
                let storages = vec![
                    fsys::StorageDecl {
                        name: Some("memfs".to_string()),
                        source_path: Some("/memfs".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                    },
                ];
                let mut decl = new_component_decl();
                decl.storage = Some(storages);
                decl

            },
        },
        test_translate_runners => {
            input = json!({
                "runners": [
                    {
                        "name": "elf",
                        "source": {
                            "self": {}
                        },
                        "source_path": "/elf"
                    }
                ]
            }),
            output = {
                let runners = vec![
                    fsys::RunnerDecl {
                        name: Some("elf".to_string()),
                        source_path: Some("/elf".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                    },
                ];
                let mut decl = new_component_decl();
                decl.runners = Some(runners);
                decl
            },
        },
        test_translate_all_sections => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                },
                "uses": [
                    {
                        "service": {
                            "source": {
                                "realm": {}
                            },
                            "source_path": "/fonts/CoolFonts",
                            "target_path": "/svc/fuchsia.fonts.Provider",
                        }
                    }
                ],
                "exposes": [
                    {
                        "directory": {
                            "source": {
                                "self": {}
                            },
                            "source_path": "/volumes/blobfs",
                            "target_path": "/volumes/blobfs",
                            "target": "realm",
                            "rights": ["connect"]
                        }
                    }
                ],
                "offers": [
                    {
                        "service": {
                            "source": {
                                "child": {
                                    "name": "logger"
                                }
                            },
                            "source_path": "/svc/fuchsia.logger.Log",
                            "target": {
                                "child": {
                                    "name": "netstack"
                                }
                            },
                            "target_path": "/svc/fuchsia.logger.Log"
                        }
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                        "startup": "eager"
                    }
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent"
                    }
                ],
                "facets": {
                    "author": "Fuchsia",
                    "year": 2018
                },
                "storage": [
                    {
                        "name": "memfs",
                        "source_path": "/memfs",
                        "source": {
                            "self": {}
                        }
                    }
                ],
                "runners": [
                    {
                        "name": "elf",
                        "source": {
                            "self": {}
                        },
                        "source_path": "/elf"
                    }
                ],
                "environments": [
                    {
                        "name": "test_env",
                        "extends": "realm",
                    }
                ]
            }),
            output = {
                let program = fsys::Object {entries: vec![
                    fsys::Entry {
                        key: "binary".to_string(),
                        value: Some(Box::new(fsys::Value::Str("bin/app".to_string()))),
                    },
                ]};
                let uses = vec![
                    fsys::UseDecl::Service(fsys::UseServiceDecl {
                        source: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        source_path: Some("/fonts/CoolFonts".to_string()),
                        target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                    }),
                ];
                let exposes = vec![
                    fsys::ExposeDecl::Directory(fsys::ExposeDirectoryDecl {
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                        source_path: Some("/volumes/blobfs".to_string()),
                        target_path: Some("/volumes/blobfs".to_string()),
                        target: Some(fsys::Ref::Realm(fsys::RealmRef {})),
                        rights: Some(fio2::Operations::Connect),
                        subdir: None,
                    }),
                ];
                let offers = vec![
                    fsys::OfferDecl::Service(fsys::OfferServiceDecl {
                        source: Some(fsys::Ref::Child(fsys::ChildRef {
                            name: "logger".to_string(),
                            collection: None,
                        })),
                        source_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        target: Some(fsys::Ref::Child(
                           fsys::ChildRef {
                               name: "netstack".to_string(),
                               collection: None,
                           }
                        )),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                ];
                let children = vec![
                    fsys::ChildDecl {
                        name: Some("logger".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                        environment: None,
                    },
                    fsys::ChildDecl {
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                        environment: None,
                    },
                ];
                let collections = vec![
                    fsys::CollectionDecl {
                        name: Some("modular".to_string()),
                        durability: Some(fsys::Durability::Persistent),
                    },
                ];
                let facets = fsys::Object {entries: vec![
                    fsys::Entry {
                        key: "author".to_string(),
                        value: Some(Box::new(fsys::Value::Str("Fuchsia".to_string()))),
                    },
                    fsys::Entry {
                        key: "year".to_string(),
                        value: Some(Box::new(fsys::Value::Inum(2018))),
                    },
                ]};
                let storages = vec![
                    fsys::StorageDecl {
                        name: Some("memfs".to_string()),
                        source_path: Some("/memfs".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                    },
                ];
                let runners = vec![
                    fsys::RunnerDecl {
                        name: Some("elf".to_string()),
                        source: Some(fsys::Ref::Self_(fsys::SelfRef{})),
                        source_path: Some("/elf".to_string()),
                    },
                ];
                let environments = vec![
                    fsys::EnvironmentDecl {
                        name: Some("test_env".to_string()),
                        extends: Some(fsys::EnvironmentExtends::Realm),
                    }
                ];
                fsys::ComponentDecl {
                    program: Some(program),
                    uses: Some(uses),
                    exposes: Some(exposes),
                    offers: Some(offers),
                    children: Some(children),
                    collections: Some(collections),
                    facets: Some(facets),
                    storage: Some(storages),
                    runners: Some(runners),
                    environments: Some(environments),
                    resolvers: None,
                }
            },
        },
    }
}
