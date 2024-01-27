// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Based on https://fuchsia.googlesource.com/fuchsia/+/HEAD/tools/fidl/fidlc/schema.json

use {
    anyhow::{anyhow, Error},
    heck::SnakeCase,
    lazy_static::lazy_static,
    regex::Regex,
    serde::{Deserialize, Deserializer, Serialize},
    std::collections::{BTreeMap, HashSet},
    std::ops::Add,
    std::string::ToString,
};

lazy_static! {
    pub static ref IDENTIFIER_RE: Regex =
        Regex::new(r#"^[A-Za-z]([_A-Za-z0-9]*[A-Za-z0-9])?$"#).unwrap();
    pub static ref COMPOUND_IDENTIFIER_RE: Regex =
        Regex::new(r#"([_A-Za-z][_A-Za-z0-9]*-)*[_A-Za-z][_A-Za-z0-9]*/[_A-Za-z][_A-Za-z0-9]*"#)
            .unwrap();
    pub static ref LIBRARY_IDENTIFIER_RE: Regex =
        Regex::new(r#"^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*$"#).unwrap();
    pub static ref UNFLATTENED_PARAMETER_NAME: Identifier = Identifier { 0: "payload".to_string() };
    pub static ref EMPTY_FIELD_SHAPE: FieldShape =
        FieldShape { offset: Count(0), padding: Count(0) };
}

#[derive(Clone, Debug, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Ordinal(#[serde(deserialize_with = "validate_ordinal")] pub u64);

/// Validates that ordinal is non-zero.
fn validate_ordinal<'de, D>(deserializer: D) -> Result<u64, D::Error>
where
    D: Deserializer<'de>,
{
    let ordinal = u64::deserialize(deserializer)?;
    if ordinal == 0 {
        return Err(serde::de::Error::custom("Ordinal must not be equal to 0"));
    }
    Ok(ordinal)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(transparent)]
pub struct Count(pub u32);

impl Add for Count {
    type Output = Self;

    fn add(self, rhs: Self) -> Self::Output {
        Count(self.0 + rhs.0)
    }
}

impl Add<u32> for Count {
    type Output = Self;

    fn add(self, rhs: u32) -> Self::Output {
        Count(self.0 + rhs)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Identifier(#[serde(deserialize_with = "validate_identifier")] pub String);

fn validate_identifier<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: Deserializer<'de>,
{
    let id = String::deserialize(deserializer)?;
    if !IDENTIFIER_RE.is_match(&id) {
        return Err(serde::de::Error::custom(format!("Invalid identifier: {}", id)));
    }
    Ok(id)
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize, Hash)]
#[serde(transparent)]
pub struct CompoundIdentifier(
    #[serde(deserialize_with = "validate_compound_identifier")] pub String,
);

fn validate_compound_identifier<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: Deserializer<'de>,
{
    let id = String::deserialize(deserializer)?;
    if !COMPOUND_IDENTIFIER_RE.is_match(&id) {
        return Err(serde::de::Error::custom(format!("Invalid compound identifier: {}", id)));
    }
    Ok(id)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct LibraryIdentifier(#[serde(deserialize_with = "validate_library_identifier")] pub String);

fn validate_library_identifier<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: Deserializer<'de>,
{
    let id = String::deserialize(deserializer)?;
    if !LIBRARY_IDENTIFIER_RE.is_match(&id) {
        return Err(serde::de::Error::custom(format!("Invalid library identifier: {}", id)));
    }
    Ok(id)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Declaration {
    Bits,
    Const,
    Enum,
    Protocol,
    Service,
    ExperimentalResource,
    Struct,
    Table,
    Union,
    Alias,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct DeclarationsMap(pub BTreeMap<CompoundIdentifier, Declaration>);

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ExternalDeclaration {
    Bits,
    Const,
    Enum,
    Protocol,
    Service,
    ExperimentalResource,
    Struct { resource: bool },
    Table { resource: bool },
    Union { resource: bool },
    Alias,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ExternalDeclarationsMap(pub BTreeMap<CompoundIdentifier, ExternalDeclaration>);

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Library {
    pub name: LibraryIdentifier,
    pub declarations: ExternalDeclarationsMap,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Location {
    pub filename: String,
    pub line: u32,
    pub column: u32,
    pub length: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum HandleSubtype {
    Handle,
    Bti,
    Channel,
    Clock,
    Debuglog,
    Event,
    EventPair,
    Exception,
    Fifo,
    Guest,
    Interrupt,
    Iommu,
    Job,
    Msi,
    Pager,
    PciDevice,
    Pmt,
    Port,
    Process,
    Profile,
    Resource,
    Socket,
    Stream,
    SuspendToken,
    Thread,
    Timer,
    Vcpu,
    Vmar,
    Vmo,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum IntegerType {
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
}

// TODO(surajmalhotra): Implement conversion between IntegerType and PrimitiveSubtype.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PrimitiveSubtype {
    Bool,
    Float32,
    Float64,
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum InternalSubtype {
    #[serde(rename = "transport_error")]
    TransportErr,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Type {
    Array {
        element_type: Box<Type>,
        element_count: Count,
    },
    Vector {
        element_type: Box<Type>,
        maybe_element_count: Option<Count>,
        nullable: bool,
    },
    #[serde(rename = "string")]
    Str {
        maybe_element_count: Option<Count>,
        nullable: bool,
    },
    Handle {
        subtype: HandleSubtype,
        rights: u32,
        nullable: bool,
    },
    Request {
        subtype: CompoundIdentifier,
        nullable: bool,
    },
    Primitive {
        subtype: PrimitiveSubtype,
    },
    Internal {
        subtype: InternalSubtype,
    },
    Identifier {
        identifier: CompoundIdentifier,
        nullable: bool,
    },
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Literal {
    #[serde(rename = "string")]
    Str {
        value: String,
        expression: String,
    },
    Numeric {
        value: String,
        expression: String,
    },
    Bool {
        value: String,
        expression: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Constant {
    Identifier { identifier: CompoundIdentifier, value: String, expression: String },
    Literal { literal: Literal, value: String, expression: String },
    BinaryOperator { value: String, expression: String },
}

impl Constant {
    pub fn value_string(&self) -> &str {
        match self {
            Constant::Identifier { value, .. } => value,
            Constant::Literal { value, .. } => value,
            Constant::BinaryOperator { value, .. } => value,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Bits {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub naming_context: Vec<String>,
    pub location: Option<Location>,
    #[serde(rename = "type")]
    pub _type: Type,
    pub mask: String,
    pub members: Vec<BitsMember>,
    pub strict: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BitsMember {
    pub name: Identifier,
    pub location: Option<Location>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub value: Constant,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Const {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub location: Option<Location>,
    #[serde(rename = "type")]
    pub _type: Type,
    pub value: Constant,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EnumMember {
    pub name: Identifier,
    pub location: Option<Location>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub value: Constant,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Enum {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub naming_context: Vec<String>,
    pub location: Option<Location>,
    #[serde(rename = "type")]
    pub _type: IntegerType,
    pub members: Vec<EnumMember>,
    pub strict: bool,
    pub maybe_unknown_value: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResourceProperty {
    #[serde(rename = "type")]
    pub _type: Type,
    pub name: Identifier,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Resource {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub location: Option<Location>,
    #[serde(rename = "type")]
    pub _type: Type,
    pub properties: Option<Vec<ResourceProperty>>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct AttributeArg {
    pub name: String,
    pub value: Constant,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Attribute {
    pub name: String,
    pub arguments: Vec<AttributeArg>,
}

impl Attribute {
    /// Count the number of attribute arguments.
    pub fn count(&self) -> usize {
        self.arguments.len()
    }

    /// Check if an argument of a certain name exists on this attribute.
    pub fn has(&self, name: &str) -> bool {
        self.get(name).is_ok()
    }

    /// Get the value of a specific argument on the attribute.
    pub fn get(&self, name: &str) -> Result<&Constant, Error> {
        let lower_name = to_lower_snake_case(name);
        Ok(&self
            .arguments
            .iter()
            .find(|arg| to_lower_snake_case(arg.name.as_str()) == lower_name)
            .ok_or(anyhow!("argument not found"))?
            .value)
    }

    /// For attributes that only have one argument, retrieve the value of that argument without
    /// naming it.
    pub fn get_standalone(&self) -> Result<&Constant, Error> {
        match self.count() {
            0 => Err(anyhow!("attribute {} has no arguments", self.name)),
            1 => Ok(&self.arguments[0].value),
            _ => Err(anyhow!("attribute {} has multiple arguments", self.name)),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FieldShape {
    pub offset: Count,
    pub padding: Count,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TypeShape {
    pub inline_size: Count,
    pub alignment: Count,
    pub depth: Count,
    pub max_handles: Count,
    pub max_out_of_line: Count,
    pub has_padding: bool,
    pub has_flexible_envelope: bool,
}
pub struct MethodParameter<'a> {
    pub field_shape_v1: FieldShape,
    pub maybe_attributes: &'a Option<Vec<Attribute>>,
    pub _type: Type,
    pub name: &'a Identifier,
    pub location: &'a Option<Location>,
    pub experimental_maybe_from_alias: Option<&'a TypeConstructor>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Method {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub ordinal: Ordinal,
    pub name: Identifier,
    pub location: Option<Location>,
    pub has_request: bool,
    pub maybe_request_payload: Option<Type>,
    pub has_response: bool,
    pub maybe_response_payload: Option<Type>,
    pub is_composed: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Protocol {
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub location: Option<Location>,
    pub methods: Vec<Method>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServiceMember {
    #[serde(rename = "type")]
    pub _type: Type,
    pub name: Identifier,
    pub location: Location,
    pub maybe_attributes: Option<Vec<Attribute>>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Service {
    pub name: CompoundIdentifier,
    pub location: Option<Location>,
    pub members: Vec<ServiceMember>,
    pub maybe_attributes: Option<Vec<Attribute>>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct StructMember {
    #[serde(rename = "type")]
    pub _type: Type,
    pub name: Identifier,
    pub location: Option<Location>,
    pub field_shape_v1: FieldShape,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub maybe_default_value: Option<Constant>,
    pub experimental_maybe_from_alias: Option<TypeConstructor>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Struct {
    pub max_handles: Option<Count>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub naming_context: Vec<String>,
    pub location: Option<Location>,
    pub anonymous: Option<bool>,
    pub members: Vec<StructMember>,
    pub resource: bool,
    pub type_shape_v1: TypeShape,
}

impl Struct {
    pub fn is_anonymous(&self) -> bool {
        self.naming_context.len() > 1
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct TableMember {
    pub reserved: bool,
    #[serde(rename = "type")]
    pub _type: Option<Type>,
    pub name: Option<Identifier>,
    pub location: Option<Location>,
    pub ordinal: Ordinal,
    pub size: Option<Count>,
    pub max_out_of_line: Option<Count>,
    pub alignment: Option<Count>,
    pub offset: Option<Count>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub maybe_default_value: Option<Constant>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Table {
    pub max_handles: Option<Count>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub naming_context: Vec<String>,
    pub location: Option<Location>,
    pub members: Vec<TableMember>,
    pub strict: bool,
    pub resource: bool,
    pub type_shape_v1: TypeShape,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct UnionMember {
    pub ordinal: Ordinal,
    pub reserved: bool,
    pub name: Option<Identifier>,
    #[serde(rename = "type")]
    pub _type: Option<Type>,
    pub location: Option<Location>,
    pub max_out_of_line: Option<Count>,
    pub offset: Option<Count>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub experimental_maybe_from_alias: Option<TypeConstructor>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Union {
    pub max_handles: Option<Count>,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub name: CompoundIdentifier,
    pub naming_context: Vec<String>,
    pub location: Option<Location>,
    pub members: Vec<UnionMember>,
    pub strict: bool,
    pub resource: bool,
    pub type_shape_v1: TypeShape,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TypeConstructor {
    pub name: String,
    pub args: Vec<TypeConstructor>,
    pub nullable: bool,
    pub maybe_size: Option<Constant>,
    pub maybe_handle_subtype: Option<HandleSubtype>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Alias {
    pub name: CompoundIdentifier,
    pub location: Location,
    pub partial_type_ctor: TypeConstructor,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FidlIr {
    pub name: LibraryIdentifier,
    pub maybe_attributes: Option<Vec<Attribute>>,
    pub bits_declarations: Vec<Bits>,
    pub const_declarations: Vec<Const>,
    pub enum_declarations: Vec<Enum>,
    pub experimental_resource_declarations: Vec<Resource>,
    pub protocol_declarations: Vec<Protocol>,
    pub service_declarations: Vec<Service>,
    pub struct_declarations: Vec<Struct>,
    pub external_struct_declarations: Vec<Struct>,
    pub table_declarations: Vec<Table>,
    pub union_declarations: Vec<Union>,
    pub alias_declarations: Vec<Alias>,
    pub declaration_order: Vec<CompoundIdentifier>,
    pub declarations: DeclarationsMap,
    pub library_dependencies: Vec<Library>,

    // A set of all FIDL types used as a message payload.  Unlike the public members of this struct,
    // this member is an internal value, generated when the build() method is called. It serves to
    // cache lookups for future use.
    message_body_type_names: Option<HashSet<CompoundIdentifier>>,
}

// Additional methods for IR types.

// Compound identifiers are of the form: my.parent.library/ThisIsMyName
impl CompoundIdentifier {
    pub fn get_name(&self) -> &str {
        self.0.split("/").last().unwrap()
    }

    pub fn is_base_type(&self) -> bool {
        self.0.split("/").next().unwrap() == "zx"
    }
}

macro_rules! fetch_declaration {
    ( $ir: ident, $field: ident, $identifier: ident) => {
        $ir.$field
            .iter()
            .filter(|e| e.name == *$identifier)
            .next()
            .ok_or(anyhow!("Could not find declaration: {:?}", $identifier))
    };
}

impl FidlIr {
    pub fn get_library_name(&self) -> String {
        self.name.0.to_string()
    }

    pub fn get_declaration(&self, identifier: &CompoundIdentifier) -> Result<&Declaration, Error> {
        self.declarations.0.get(identifier).ok_or(anyhow!("~~ error never seen ~~~")).or_else(
            |_| {
                self.library_dependencies
                    .iter()
                    .filter_map(|library| library.declarations.0.get(identifier))
                    .next()
                    .ok_or(anyhow!("Could not find declaration: {:?}", identifier))
                    .map(|decl| match decl {
                        ExternalDeclaration::Bits => &Declaration::Bits,
                        ExternalDeclaration::Const => &Declaration::Const,
                        ExternalDeclaration::Enum => &Declaration::Enum,
                        ExternalDeclaration::Protocol => &Declaration::Protocol,
                        ExternalDeclaration::Service => &Declaration::Service,
                        ExternalDeclaration::ExperimentalResource => {
                            &Declaration::ExperimentalResource
                        }
                        ExternalDeclaration::Struct { .. } => &Declaration::Struct,
                        ExternalDeclaration::Table { .. } => &Declaration::Table,
                        ExternalDeclaration::Union { .. } => &Declaration::Union,
                        ExternalDeclaration::Alias => &Declaration::Alias,
                    })
            },
        )
    }

    pub fn get_enum(&self, identifier: &CompoundIdentifier) -> Result<&Enum, Error> {
        fetch_declaration!(self, enum_declarations, identifier)
    }

    pub fn get_struct(&self, identifier: &CompoundIdentifier) -> Result<&Struct, Error> {
        match fetch_declaration!(self, struct_declarations, identifier) {
            Ok(found_struct) => Ok(found_struct),
            Err(_) => fetch_declaration!(self, external_struct_declarations, identifier),
        }
    }

    pub fn get_table(&self, identifier: &CompoundIdentifier) -> Result<&Table, Error> {
        fetch_declaration!(self, table_declarations, identifier)
    }

    pub fn get_union(&self, identifier: &CompoundIdentifier) -> Result<&Union, Error> {
        fetch_declaration!(self, union_declarations, identifier)
    }

    pub fn get_const(&self, identifier: &CompoundIdentifier) -> Result<&Const, Error> {
        fetch_declaration!(self, const_declarations, identifier)
    }

    pub fn get_alias(&self, identifier: &CompoundIdentifier) -> Result<&Alias, Error> {
        fetch_declaration!(self, alias_declarations, identifier)
    }

    pub fn get_bits(&self, identifier: &CompoundIdentifier) -> Result<&Bits, Error> {
        fetch_declaration!(self, bits_declarations, identifier)
    }

    pub fn is_protocol(&self, identifier: &CompoundIdentifier) -> bool {
        self.get_declaration(identifier).map_or(false, |decl| decl == &Declaration::Protocol)
    }

    pub fn get_protocol_attributes(
        &self,
        identifier: &CompoundIdentifier,
    ) -> Result<&Option<Vec<Attribute>>, Error> {
        if let Some(Declaration::Protocol) = self.declarations.0.get(identifier) {
            return Ok(&self
                .protocol_declarations
                .iter()
                .filter(|e| e.name == *identifier)
                .next()
                .unwrap_or_else(|| panic!("Could not find protocol declaration: {:?}", identifier))
                .maybe_attributes);
        }
        Err(anyhow!("Identifier does not represent a protocol: {:?}", identifier))
    }

    pub fn is_external_decl(&self, identifier: &CompoundIdentifier) -> Result<bool, Error> {
        self.declarations
            .0
            .get(identifier)
            .map(|_| false)
            .ok_or(anyhow!("~~ error never seen ~~~"))
            .or_else(|_| {
                self.library_dependencies
                    .iter()
                    .find(|library| library.declarations.0.get(identifier).is_some())
                    .ok_or(anyhow!("Could not find declaration: {:?}", identifier))
                    .map(|_| true)
            })
    }

    pub fn is_type_used_for_message_body(
        &self,
        identifier: &CompoundIdentifier,
    ) -> Result<bool, Error> {
        match self.message_body_type_names.as_ref() {
            Some(set) => Ok(set.contains(&identifier)),
            None => Err(anyhow!("Must call |build| first")),
        }
    }

    fn build_message_body_type_names(&mut self) -> Result<(), Error> {
        if self.message_body_type_names.is_none() {
            let mut message_body_type_names = HashSet::<CompoundIdentifier>::new();
            self.protocol_declarations.iter().map(|protocol| {
                protocol.methods.iter().map(|method| {
                    if method.maybe_request_payload.is_some() {
                        match method.maybe_request_payload.as_ref().unwrap() {
                            Type::Identifier { identifier, .. } => {
                                message_body_type_names.insert(identifier.clone());
                                Ok(())
                            }
                            _ => Err(anyhow!("The kind of the request payload for method {:?} must be 'identifier'", method.name)),
                        }?;
                    }
                    if method.maybe_response_payload.is_some() {
                        return match method.maybe_response_payload.as_ref().unwrap() {
                            Type::Identifier { identifier, .. } => {
                                message_body_type_names.insert(identifier.clone());
                                Ok(())
                            }
                            _ => Err(anyhow!("The kind of the response payload for method {:?} must be 'identifier'", method.name)),
                        }
                    }
                    Ok(())
                }).collect()
            }).collect::<Result<(), Error>>()?;
            self.message_body_type_names = Some(message_body_type_names);
        }
        Ok(())
    }

    // After the IR has been deserialized from JSON, certain internal properties of the IR, such as
    // cached lookups, should be constructed by calling this method.
    pub fn build(&mut self) -> Result<(), Error> {
        self.build_message_body_type_names()?;
        Ok(())
    }
}

fn get_payload_parameters<'a>(
    has_payload: bool,
    payload: Option<&'a Type>,
    ir: &'a FidlIr,
) -> Result<Option<Vec<MethodParameter<'a>>>, Error> {
    if !has_payload {
        return Ok(None);
    }
    if payload.is_none() {
        return Ok(Some(vec![]));
    }

    let identifier = match payload.unwrap() {
        Type::Identifier { ref identifier, .. } => Ok(identifier),
        _ => Err(anyhow!("payload must be an identifier type: {:?}", payload)),
    }?;
    let decl = ir.get_declaration(identifier)?;

    match decl {
        Declaration::Struct => {
            let mut out = vec![];
            let struct_decl = ir.get_struct(identifier)?;

            // Flatten the struct members into method parameters.
            for member in &struct_decl.members {
                let offset_field_shape = FieldShape {
                    offset: member.field_shape_v1.offset + 16,
                    padding: member.field_shape_v1.padding,
                };
                out.push(MethodParameter {
                    maybe_attributes: &member.maybe_attributes,
                    _type: member._type.clone(),
                    name: &member.name,
                    location: &member.location,
                    field_shape_v1: offset_field_shape,
                    experimental_maybe_from_alias: member.experimental_maybe_from_alias.as_ref(),
                });
            }
            Ok(Some(out))
        }
        Declaration::Table => {
            // TODO(fxbug.dev/82088): Handle tables correctly. This is currently a hack to get
            // parsing to work, as no backends which use this lib actually require support for
            // tables as parameters.
            let mut out = vec![];
            let table_decl = ir.get_table(identifier)?;

            // Flatten the table members into method parameters.
            for member in &table_decl.members {
                let offset_field_shape = FieldShape {
                    offset: member.offset.unwrap_or(Count(0)) + 16,
                    padding: Count(0),
                };
                out.push(MethodParameter {
                    maybe_attributes: &member.maybe_attributes,
                    _type: member._type.clone().unwrap(),
                    name: &member.name.as_ref().unwrap(),
                    location: &member.location,
                    field_shape_v1: offset_field_shape,
                    experimental_maybe_from_alias: None,
                });
            }
            Ok(Some(out))
        }
        Declaration::Union => {
            let mut out = vec![];
            let union_decl = ir.get_union(identifier)?;
            out.push(MethodParameter {
                maybe_attributes: &union_decl.maybe_attributes,
                _type: Type::Identifier { identifier: identifier.clone(), nullable: false },
                name: &UNFLATTENED_PARAMETER_NAME,
                location: &union_decl.location,
                field_shape_v1: EMPTY_FIELD_SHAPE.clone(),
                experimental_maybe_from_alias: None,
            });
            Ok(Some(out))
        }
        _ => Err(anyhow!("payload must point to a struct or union type: {:?}", payload)),
    }
}

impl Method {
    pub fn request_parameters<'a>(
        &'a self,
        ir: &'a FidlIr,
    ) -> Result<Option<Vec<MethodParameter<'a>>>, Error> {
        get_payload_parameters(self.has_request, self.maybe_request_payload.as_ref(), ir)
    }

    pub fn response_parameters<'a>(
        &'a self,
        ir: &'a FidlIr,
    ) -> Result<Option<Vec<MethodParameter<'a>>>, Error> {
        get_payload_parameters(self.has_response, self.maybe_response_payload.as_ref(), ir)
    }
}

impl Type {
    pub fn is_primitive(&self, ir: &FidlIr) -> Result<bool, Error> {
        match self {
            Type::Identifier { ref identifier, .. } => {
                if identifier.is_base_type() {
                    Ok(true)
                } else {
                    match ir.get_declaration(identifier).unwrap() {
                        Declaration::Bits => fetch_declaration!(ir, bits_declarations, identifier)?
                            ._type
                            .is_primitive(ir),
                        Declaration::Const => {
                            fetch_declaration!(ir, const_declarations, identifier)?
                                ._type
                                .is_primitive(ir)
                        }
                        Declaration::Enum => Ok(true),
                        _ => Ok(false),
                    }
                }
            }
            Type::Primitive { .. } => Ok(true),
            _ => Ok(false),
        }
    }
}

impl IntegerType {
    pub fn to_primitive(&self) -> PrimitiveSubtype {
        match self {
            IntegerType::Int8 => PrimitiveSubtype::Int8,
            IntegerType::Int16 => PrimitiveSubtype::Int16,
            IntegerType::Int32 => PrimitiveSubtype::Int32,
            IntegerType::Int64 => PrimitiveSubtype::Int64,
            IntegerType::Uint8 => PrimitiveSubtype::Uint8,
            IntegerType::Uint16 => PrimitiveSubtype::Uint16,
            IntegerType::Uint32 => PrimitiveSubtype::Uint32,
            IntegerType::Uint64 => PrimitiveSubtype::Uint64,
        }
    }

    pub fn to_type(&self) -> Type {
        Type::Primitive { subtype: self.to_primitive() }
    }
}

/// Converts an UpperCamelCased name like "FooBar" into a lower_snake_cased one
/// like "foo_bar."  This is used to normalize attribute names such that names
/// written in either case are synonyms.
pub fn to_lower_snake_case(str: &str) -> String {
    str.to_snake_case().to_lowercase()
}

pub trait AttributeContainer {
    fn has(&self, name: &str) -> bool;
    fn get(&self, name: &str) -> Option<&Attribute>;
}

impl AttributeContainer for Option<Vec<Attribute>> {
    fn has(&self, name: &str) -> bool {
        match self {
            Some(attrs) => {
                attrs.iter().any(|a| to_lower_snake_case(&a.name) == to_lower_snake_case(name))
            }
            None => false,
        }
    }

    fn get(&self, name: &str) -> Option<&Attribute> {
        match self {
            Some(attrs) => attrs
                .iter()
                .filter_map(|a| {
                    if to_lower_snake_case(&a.name) == to_lower_snake_case(name) {
                        Some(a)
                    } else {
                        None
                    }
                })
                .next(),
            None => None,
        }
    }
}
