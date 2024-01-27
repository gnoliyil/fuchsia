// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::compiler::{dependency_graph, CompilerError};
use crate::linter;
use crate::make_identifier;
use crate::parser::common::{CompoundIdentifier, Include};
use crate::parser::{self, bind_library};
use std::collections::HashMap;
use std::convert::TryFrom;
use std::fmt;
use std::ops::Deref;

pub type SymbolTable = HashMap<CompoundIdentifier, Symbol>;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Symbol {
    DeprecatedKey(u32),
    Key(String, bind_library::ValueType),
    NumberValue(u64),
    StringValue(String),
    BoolValue(bool),
    EnumValue(String),
}

impl fmt::Display for Symbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Symbol::DeprecatedKey(key) => write!(f, "DeprecatedKey({})", key),
            Symbol::Key(key, _) => write!(f, "Key({})", key),
            Symbol::NumberValue(value) => write!(f, "{}", value),
            Symbol::StringValue(value) => write!(f, "\"{}\"", value),
            Symbol::BoolValue(value) => write!(f, "{}", value),
            Symbol::EnumValue(value) => write!(f, "Enum({})", value),
        }
    }
}

pub fn get_symbol_table_from_libraries<'a>(
    using: &Vec<Include>,
    libraries: &[String],
    lint: bool,
) -> Result<SymbolTable, CompilerError> {
    let library_asts: Vec<bind_library::Ast> = libraries
        .into_iter()
        .map(|lib| {
            let ast = bind_library::Ast::try_from(lib.as_str())
                .map_err(CompilerError::BindParserError)?;
            if lint {
                linter::lint_library(&ast).map_err(CompilerError::LinterError)?;
            }
            Ok(ast)
        })
        .collect::<Result<_, CompilerError>>()?;

    let dependencies = resolve_dependencies(using, library_asts.iter())?;
    let aliases = get_aliases(using);
    construct_symbol_table(dependencies.into_iter(), aliases)
}

fn get_aliases(using: &Vec<Include>) -> HashMap<CompoundIdentifier, String> {
    return using
        .iter()
        .filter_map(|using| match &using.alias {
            Some(alias) => Some((using.name.clone(), alias.clone())),
            None => None,
        })
        .collect::<HashMap<_, _>>();
}

pub fn resolve_dependencies<'a>(
    using: &Vec<Include>,
    libraries: impl Iterator<Item = &'a bind_library::Ast> + Clone,
) -> Result<Vec<&'a bind_library::Ast>, CompilerError> {
    (|| {
        let mut graph = dependency_graph::DependencyGraph::new();

        for library in libraries.clone() {
            graph.insert_node(library.name.clone(), library);
        }

        for Include { name, .. } in using {
            graph.insert_edge_from_root(name)?;
        }

        for from in libraries {
            for to in &from.using {
                graph.insert_edge(&from.name, &to.name)?;
            }
        }

        graph.resolve()
    })()
    .map_err(CompilerError::DependencyError)
}

/// Find the namespace of a qualified identifier from the library's includes. Or, if the identifier
/// is unqualified, return the local qualified identifier.
fn find_qualified_identifier(
    declaration: &bind_library::Declaration,
    using: &Vec<parser::common::Include>,
    local_qualified: &CompoundIdentifier,
) -> Result<CompoundIdentifier, CompilerError> {
    if let Some(namespace) = declaration.identifier.parent() {
        // A declaration of a qualified (i.e. non-local) key must be an extension.
        if !declaration.extends {
            return Err(CompilerError::MissingExtendsKeyword(declaration.identifier.clone()));
        }

        // Special case for deprecated symbols (currently in the fuchsia namespace), return the
        // declaration as-is.
        if namespace == make_identifier!["fuchsia"] {
            return Ok(declaration.identifier.clone());
        }

        // Find the fully qualified name from the included libraries.
        let include = using
            .iter()
            .find(|include| {
                namespace == include.name || Some(namespace.to_string()) == include.alias
            })
            .ok_or(CompilerError::UnresolvedQualification(declaration.identifier.clone()))?;

        return Ok(include.name.nest(declaration.identifier.name.clone()));
    }

    // It is not valid to extend an unqualified (i.e. local) key.
    if declaration.extends {
        return Err(CompilerError::InvalidExtendsKeyword(local_qualified.clone()));
    }

    // An unqualified/local key is scoped to the current library.
    Ok(local_qualified.clone())
}

/// Construct a map of every key and value defined by `libraries`. The identifiers in the symbol
/// table will be fully qualified, i.e. they will contain their full namespace. A symbol is
/// namespaced according to the name of the library it is defined in. If a library defines a value
/// by extending a previously defined key, then that value will be namespaced to the current library
/// and not the library of its key.
pub fn construct_symbol_table(
    libraries: impl Iterator<Item = impl Deref<Target = bind_library::Ast>>,
    aliases: HashMap<CompoundIdentifier, String>,
) -> Result<SymbolTable, CompilerError> {
    let mut symbol_table = get_deprecated_symbols();
    for lib in libraries {
        let bind_library::Ast { name, using, declarations } = &*lib;

        let aliased_name = match aliases.get(name) {
            Some(alias) => Some(make_identifier!(alias)),
            None => None,
        };

        for declaration in declarations {
            // Construct a qualified identifier for this key that's namespaced to the current
            // library, discarding any other qualifiers. This identifier is used to scope values
            // defined under this key. We have separate entries for symbol table keys (k) and
            // values (v) because the key might be aliased.
            let local_qualified_v = name.nest(declaration.identifier.name.clone());
            let local_qualified_k = match &aliased_name {
                Some(alias) => alias.nest(declaration.identifier.name.clone()),
                None => local_qualified_v.clone(),
            };

            // Attempt to match the namespace of the key to an include of the current library, or if
            // it is unqualified use the local qualified name. Also do a first pass at checking
            // whether the extend keyword is used correctly. Once again keep separate entries for
            // symbol table keys (k) and values (v) because the key might be aliased.
            let qualified_v = find_qualified_identifier(declaration, using, &local_qualified_v)?;
            let qualified_k = if aliased_name.is_some() {
                find_qualified_identifier(declaration, using, &local_qualified_k)?
            } else {
                qualified_v.clone()
            };

            // Type-check the qualified name against the existing symbols, and check that extended
            // keys are previously defined and that non-extended keys are not.
            match symbol_table.get(&qualified_k) {
                Some(Symbol::Key(_, value_type)) => {
                    if !declaration.extends {
                        return Err(CompilerError::DuplicateIdentifier(qualified_k));
                    }
                    if declaration.value_type != *value_type {
                        return Err(CompilerError::TypeMismatch(qualified_k));
                    }
                }
                Some(Symbol::DeprecatedKey(_)) => (),
                Some(_) => {
                    return Err(CompilerError::TypeMismatch(qualified_k));
                }
                None => {
                    if declaration.extends {
                        return Err(CompilerError::UndeclaredKey(qualified_k));
                    }
                    symbol_table.insert(
                        qualified_k,
                        Symbol::Key(qualified_v.to_string(), declaration.value_type),
                    );
                }
            }

            // Insert each value associated with the declaration into the symbol table, taking care
            // to scope each identifier under the locally qualified identifier of the key. We don't
            // need to type-check values here since the parser has already done that.
            for value in &declaration.values {
                let qualified_value_k = local_qualified_k.nest(value.identifier().to_string());
                let qualified_value_v = local_qualified_v.nest(value.identifier().to_string());
                if symbol_table.contains_key(&qualified_value_k) {
                    return Err(CompilerError::DuplicateIdentifier(qualified_value_k));
                }

                match value {
                    bind_library::Value::Number(_, value) => {
                        symbol_table.insert(qualified_value_k, Symbol::NumberValue(*value));
                    }
                    bind_library::Value::Str(_, value) => {
                        symbol_table.insert(qualified_value_k, Symbol::StringValue(value.clone()));
                    }
                    bind_library::Value::Bool(_, value) => {
                        symbol_table.insert(qualified_value_k, Symbol::BoolValue(*value));
                    }
                    bind_library::Value::Enum(_) => {
                        symbol_table.insert(
                            qualified_value_k,
                            Symbol::EnumValue(qualified_value_v.to_string()),
                        );
                    }
                };
            }
        }
    }

    Ok(symbol_table)
}

/// Hard code these symbols during the migration from macros to bind rules. Eventually these
/// will be defined in libraries and the compiler will emit strings for them in the bytecode.
fn deprecated_keys() -> Vec<(String, u32)> {
    let mut keys = Vec::new();

    keys.push(("BIND_PROTOCOL".to_string(), 0x0001));

    keys.push(("BIND_AUTOBIND".to_string(), 0x0002));

    keys.push(("BIND_COMPOSITE".to_string(), 0x0003));

    keys.push(("BIND_FIDL_PROTOCOL".to_string(), 0x0004));

    keys.push(("BIND_PLATFORM_DEV_VID".to_string(), 0x0300));
    keys.push(("BIND_PCI_VID".to_string(), 0x0100));

    keys.push(("BIND_PCI_DID".to_string(), 0x0101));
    keys.push(("BIND_PCI_CLASS".to_string(), 0x0102));
    keys.push(("BIND_PCI_SUBCLASS".to_string(), 0x0103));
    keys.push(("BIND_PCI_INTERFACE".to_string(), 0x0104));
    keys.push(("BIND_PCI_REVISION".to_string(), 0x0105));
    keys.push(("BIND_PCI_TOPO".to_string(), 0x0107));

    // usb binding variables at 0x02XX
    // these are used for both ZX_PROTOCOL_USB_INTERFACE and ZX_PROTOCOL_USB_FUNCTION
    keys.push(("BIND_USB_VID".to_string(), 0x0200));
    keys.push(("BIND_USB_PID".to_string(), 0x0201));
    keys.push(("BIND_USB_CLASS".to_string(), 0x0202));
    keys.push(("BIND_USB_SUBCLASS".to_string(), 0x0203));
    keys.push(("BIND_USB_PROTOCOL".to_string(), 0x0204));
    keys.push(("BIND_USB_INTERFACE_NUMBER".to_string(), 0x0205));

    // Platform bus binding variables at 0x03XX
    keys.push(("BIND_PLATFORM_DEV_VID".to_string(), 0x0300));
    keys.push(("BIND_PLATFORM_DEV_PID".to_string(), 0x0301));
    keys.push(("BIND_PLATFORM_DEV_DID".to_string(), 0x0302));
    keys.push(("BIND_PLATFORM_DEV_INSTANCE_ID".to_string(), 0x0304));
    keys.push(("BIND_PLATFORM_DEV_INTERRUPT_ID".to_string(), 0x0305));

    // ACPI binding variables at 0x04XX
    keys.push(("BIND_ACPI_BUS_TYPE".to_string(), 0x0400));
    keys.push(("BIND_ACPI_ID".to_string(), 0x0401));

    // Intel HDA Codec binding variables at 0x05XX
    keys.push(("BIND_IHDA_CODEC_VID".to_string(), 0x0500));
    keys.push(("BIND_IHDA_CODEC_DID".to_string(), 0x0501));
    keys.push(("BIND_IHDA_CODEC_MAJOR_REV".to_string(), 0x0502));
    keys.push(("BIND_IHDA_CODEC_MINOR_REV".to_string(), 0x0503));
    keys.push(("BIND_IHDA_CODEC_VENDOR_REV".to_string(), 0x0504));
    keys.push(("BIND_IHDA_CODEC_VENDOR_STEP".to_string(), 0x0505));

    // Serial binding variables at 0x06XX
    keys.push(("BIND_SERIAL_CLASS".to_string(), 0x0600));
    keys.push(("BIND_SERIAL_VID".to_string(), 0x0601));
    keys.push(("BIND_SERIAL_PID".to_string(), 0x0602));

    // NAND binding variables at 0x07XX
    keys.push(("BIND_NAND_CLASS".to_string(), 0x0700));

    // SDIO binding variables at 0x09XX
    keys.push(("BIND_SDIO_VID".to_string(), 0x0900));
    keys.push(("BIND_SDIO_PID".to_string(), 0x0901));
    keys.push(("BIND_SDIO_FUNCTION".to_string(), 0x0902));

    // I2C binding variables at 0x0A0X
    keys.push(("BIND_I2C_CLASS".to_string(), 0x0A00));
    keys.push(("BIND_I2C_BUS_ID".to_string(), 0x0A01));
    keys.push(("BIND_I2C_ADDRESS".to_string(), 0x0A02));

    // GPIO binding variables at 0x0A1X
    keys.push(("BIND_GPIO_PIN".to_string(), 0x0A10));

    // POWER binding variables at 0x0A2X
    keys.push(("BIND_POWER_DOMAIN".to_string(), 0x0A20));
    keys.push(("BIND_POWER_DOMAIN_COMPOSITE".to_string(), 0x0A21));

    // POWER binding variables at 0x0A3X
    keys.push(("BIND_CLOCK_ID".to_string(), 0x0A30));

    // SPI binding variables at 0x0A4X
    keys.push(("BIND_SPI_BUS_ID".to_string(), 0x0A41));
    keys.push(("BIND_SPI_CHIP_SELECT".to_string(), 0x0A42));

    // PWM binding variables at 0x0A5X
    keys.push(("BIND_PWM_ID".to_string(), 0x0A50));

    // PWM binding variables at 0x0A6X
    keys.push(("BIND_INIT_STEP".to_string(), 0x0A60));

    // PWM binding variables at 0x0A7X
    keys.push(("BIND_CODEC_INSTANCE".to_string(), 0x0A70));

    // Registers binding variables at 0x0A8X
    keys.push(("BIND_REGISTER_ID".to_string(), 0x0A80));

    // Power sensor binding variables at 0x0A9X
    keys.push(("BIND_POWER_SENSOR_DOMAIN".to_string(), 0x0A90));

    // Mailbox binding variables at 0x0AAX
    keys.push(("BIND_MAILBOX_ID".to_string(), 0x0AA0));

    keys
}

fn get_deprecated_symbols() -> SymbolTable {
    let mut symbol_table = HashMap::new();
    for (key, value) in deprecated_keys() {
        symbol_table.insert(make_identifier!("fuchsia", key), Symbol::DeprecatedKey(value));
    }
    symbol_table
}

pub fn get_deprecated_key_identifiers() -> HashMap<u32, String> {
    let mut key_identifiers = HashMap::new();
    for (key, value) in deprecated_keys() {
        key_identifiers.insert(value, make_identifier!("fuchsia", key).to_string());
    }
    key_identifiers
}

pub fn get_deprecated_key_identifier(key: u32) -> Option<String> {
    match key {
        0x0000 => Some("fuchsia.BIND_FLAGS".to_string()),
        0x0001 => Some("fuchsia.BIND_PROTOCOL".to_string()),
        0x0002 => Some("fuchsia.BIND_AUTOBIND".to_string()),
        0x0003 => Some("fuchsia.BIND_COMPOSITE".to_string()),
        0x0004 => Some("fuchsia.BIND_FIDL_PROTOCOL".to_string()),

        // PCI binding variables at 0x01XX.
        0x0100 => Some("fuchsia.BIND_PCI_VID".to_string()),
        0x0101 => Some("fuchsia.BIND_PCI_DID".to_string()),
        0x0102 => Some("fuchsia.BIND_PCI_CLASS".to_string()),
        0x0103 => Some("fuchsia.BIND_PCI_SUBCLASS".to_string()),
        0x0104 => Some("fuchsia.BIND_PCI_INTERFACE".to_string()),
        0x0105 => Some("fuchsia.BIND_PCI_REVISION".to_string()),
        0x0107 => Some("fuchsia.BIND_PCI_TOPO".to_string()),

        // USB binding variables at 0x02XX.
        0x0200 => Some("fuchsia.BIND_USB_VID".to_string()),
        0x0201 => Some("fuchsia.BIND_USB_PID".to_string()),
        0x0202 => Some("fuchsia.BIND_USB_CLASS".to_string()),
        0x0203 => Some("fuchsia.BIND_USB_SUBCLASS".to_string()),
        0x0204 => Some("fuchsia.BIND_USB_PROTOCOL".to_string()),
        0x0205 => Some("fuchsia.BIND_USB_INTERFACE_NUMBER".to_string()),

        // Platform bus binding variables at 0x03XX.
        0x0300 => Some("fuchsia.BIND_PLATFORM_DEV_VID".to_string()),
        0x0301 => Some("fuchsia.BIND_PLATFORM_DEV_PID".to_string()),
        0x0302 => Some("fuchsia.BIND_PLATFORM_DEV_DID".to_string()),
        0x0304 => Some("fuchsia.BIND_PLATFORM_DEV_INSTANCE_ID".to_string()),
        0x0305 => Some("fuchsia.BIND_PLATFORM_DEV_INTERRUPT_ID".to_string()),

        // ACPI binding variables at 0x04XX.
        0x0400 => Some("fuchsia.BIND_ACPI_BUS_TYPE".to_string()),
        0x0401 => Some("fuchsia.BIND_ACPI_ID".to_string()),

        // Intel HDA Codec binding variables at 0x05XX.
        0x0500 => Some("fuchsia.BIND_IHDA_CODEC_VID".to_string()),
        0x0501 => Some("fuchsia.BIND_IHDA_CODEC_DID".to_string()),
        0x0502 => Some("fuchsia.BIND_IHDA_CODEC_MAJOR_REV".to_string()),
        0x0503 => Some("fuchsia.BIND_IHDA_CODEC_MINOR_REV".to_string()),
        0x0504 => Some("fuchsia.BIND_IHDA_CODEC_VENDOR_REV".to_string()),
        0x0505 => Some("fuchsia.BIND_IHDA_CODEC_VENDOR_STEP".to_string()),

        // Serial binding variables at 0x06XX.
        0x0600 => Some("fuchsia.BIND_SERIAL_CLASS".to_string()),
        0x0601 => Some("fuchsia.BIND_SERIAL_VID".to_string()),
        0x0602 => Some("fuchsia.BIND_SERIAL_PID".to_string()),

        // NAND binding variables at 0x07XX.
        0x0700 => Some("fuchsia.BIND_NAND_CLASS".to_string()),

        // SDIO binding variables at 0x09XX.
        0x0900 => Some("fuchsia.BIND_SDIO_VID".to_string()),
        0x0901 => Some("fuchsia.BIND_SDIO_PID".to_string()),
        0x0902 => Some("fuchsia.BIND_SDIO_FUNCTION".to_string()),

        // I2C binding variables at 0x0A0X.
        0x0A00 => Some("fuchsia.BIND_I2C_CLASS".to_string()),
        0x0A01 => Some("fuchsia.BIND_I2C_BUS_ID".to_string()),
        0x0A02 => Some("fuchsia.BIND_I2C_ADDRESS".to_string()),
        0x0A03 => Some("fuchsia.BIND_I2C_VID".to_string()),
        0x0A04 => Some("fuchsia.BIND_I2C_DID".to_string()),

        // GPIO binding variables at 0x0A1X.
        0x0A10 => Some("fuchsia.BIND_GPIO_PIN".to_string()),

        // POWER binding variables at 0x0A2X.
        0x0A20 => Some("fuchsia.BIND_POWER_DOMAIN".to_string()),
        0x0A21 => Some("fuchsia.BIND_POWER_DOMAIN_COMPOSITE".to_string()),

        // POWER (clock) binding variables at 0x0A3X.
        0x0A30 => Some("fuchsia.BIND_CLOCK_ID".to_string()),

        // SPI binding variables at 0x0A4X.
        0x0A41 => Some("fuchsia.BIND_SPI_BUS_ID".to_string()),
        0x0A42 => Some("fuchsia.BIND_SPI_CHIP_SELECT".to_string()),

        // PWM binding variables at 0x0A5X.
        0x0A50 => Some("fuchsia.BIND_PWM_ID".to_string()),

        // Init step binding variables at 0x0A6X.
        0x0A60 => Some("fuchsia.BIND_INIT_STEP".to_string()),

        // Codec binding variables at 0x0A7X.
        0x0A70 => Some("fuchsia.BIND_CODEC_INSTANCE".to_string()),

        // Registers binding variables at 0x0A8X.
        0x0A80 => Some("fuchsia.BIND_REGISTER_ID".to_string()),

        // Power sensor binding variables at 0x0A9X.
        0x0A90 => Some("fuchsia.BIND_POWER_SENSOR_DOMAIN".to_string()),

        // Mailbox binding variables at 0x0AAX.
        0x0AA0 => Some("fuchsia.BIND_MAILBOX_ID".to_string()),

        _ => None,
    }
}

pub fn get_deprecated_key_value(key: &str) -> Option<u32> {
    match key {
        "fuchsia.BIND_FLAGS" => Some(0x0000),
        "fuchsia.BIND_PROTOCOL" => Some(0x0001),
        "fuchsia.BIND_AUTOBIND" => Some(0x0002),
        "fuchsia.BIND_COMPOSITE" => Some(0x0003),
        "fuchsia.BIND_FIDL_PROTOCOL" => Some(0x0004),

        // PCI binding variables at 0x01XX.
        "fuchsia.BIND_PCI_VID" => Some(0x0100),
        "fuchsia.BIND_PCI_DID" => Some(0x0101),
        "fuchsia.BIND_PCI_CLASS" => Some(0x0102),
        "fuchsia.BIND_PCI_SUBCLASS" => Some(0x0103),
        "fuchsia.BIND_PCI_INTERFACE" => Some(0x0104),
        "fuchsia.BIND_PCI_REVISION" => Some(0x0105),
        "fuchsia.BIND_PCI_TOPO" => Some(0x0107),

        // USB binding variables at 0x02XX.
        "fuchsia.BIND_USB_VID" => Some(0x0200),
        "fuchsia.BIND_USB_PID" => Some(0x0201),
        "fuchsia.BIND_USB_CLASS" => Some(0x0202),
        "fuchsia.BIND_USB_SUBCLASS" => Some(0x0203),
        "fuchsia.BIND_USB_PROTOCOL" => Some(0x0204),
        "fuchsia.BIND_USB_INTERFACE_NUMBER" => Some(0x0205),

        // Platform bus binding variables at 0x03XX
        "fuchsia.BIND_PLATFORM_DEV_VID" => Some(0x0300),
        "fuchsia.BIND_PLATFORM_DEV_PID" => Some(0x0301),
        "fuchsia.BIND_PLATFORM_DEV_DID" => Some(0x0302),
        "fuchsia.BIND_PLATFORM_DEV_INSTANCE_ID" => Some(0x0304),
        "fuchsia.BIND_PLATFORM_DEV_INTERRUPT_ID" => Some(0x0305),

        // ACPI binding variables at 0x04XX
        "fuchsia.BIND_ACPI_BUS_TYPE" => Some(0x0400),
        "fuchsia.BIND_ACPI_ID" => Some(0x0401),

        // Intel HDA Codec binding variables at 0x05XX
        "fuchsia.BIND_IHDA_CODEC_VID" => Some(0x0500),
        "fuchsia.BIND_IHDA_CODEC_DID" => Some(0x0501),
        "fuchsia.BIND_IHDA_CODEC_MAJOR_REV" => Some(0x0502),
        "fuchsia.BIND_IHDA_CODEC_MINOR_REV" => Some(0x0503),
        "fuchsia.BIND_IHDA_CODEC_VENDOR_REV" => Some(0x0504),
        "fuchsia.BIND_IHDA_CODEC_VENDOR_STEP" => Some(0x0505),

        // Serial binding variables at 0x06XX
        "fuchsia.BIND_SERIAL_CLASS" => Some(0x0600),
        "fuchsia.BIND_SERIAL_VID" => Some(0x0601),
        "fuchsia.BIND_SERIAL_PID" => Some(0x0602),

        // NAND binding variables at 0x07XX
        "fuchsia.BIND_NAND_CLASS" => Some(0x0700),

        // SDIO binding variables at 0x09XX
        "fuchsia.BIND_SDIO_VID" => Some(0x0900),
        "fuchsia.BIND_SDIO_PID" => Some(0x0901),
        "fuchsia.BIND_SDIO_FUNCTION" => Some(0x0902),

        // I2C binding variables at 0x0A0X
        "fuchsia.BIND_I2C_CLASS" => Some(0x0A00),
        "fuchsia.BIND_I2C_BUS_ID" => Some(0x0A01),
        "fuchsia.BIND_I2C_ADDRESS" => Some(0x0A02),
        "fuchsia.BIND_I2C_VID" => Some(0x0A03),
        "fuchsia.BIND_I2C_DID" => Some(0x0A04),

        // GPIO binding variables at 0x0A1X
        "fuchsia.BIND_GPIO_PIN" => Some(0x0A10),

        // POWER binding variables at 0x0A2X
        "fuchsia.BIND_POWER_DOMAIN" => Some(0x0A20),
        "fuchsia.BIND_POWER_DOMAIN_COMPOSITE" => Some(0x0A21),

        // POWER binding variables at 0x0A3X
        "fuchsia.BIND_CLOCK_ID" => Some(0x0A30),

        // SPI binding variables at 0x0A4X
        "fuchsia.BIND_SPI_BUS_ID" => Some(0x0A41),
        "fuchsia.BIND_SPI_CHIP_SELECT" => Some(0x0A42),

        // PWM binding variables at 0x0A5X
        "fuchsia.BIND_PWM_ID" => Some(0x0A50),

        // Init step binding variables at 0x0A6X.
        "fuchsia.BIND_INIT_STEP" => Some(0x0A60),

        // Codec binding variables at 0x0A7X.
        "fuchsia.BIND_CODEC_INSTANCE" => Some(0x0A70),

        // Registers binding variables at 0x0A8X
        "fuchsia.BIND_REGISTER_ID" => Some(0x0A80),

        // Power sensor binding variables at 0x0A9X
        "fuchsia.BIND_POWER_SENSOR_DOMAIN" => Some(0x0A90),

        // Mailbox binding variables at 0x0AAX
        "fuchsia.BIND_MAILBOX_ID" => Some(0x0AA0),

        _ => None,
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::make_identifier;
    use crate::parser::bind_library;
    use crate::parser::common::Include;

    mod symbol_table {
        use super::*;

        #[test]
        fn simple_key_and_value() {
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("test"),
                using: vec![],
                declarations: vec![bind_library::Declaration {
                    identifier: make_identifier!["symbol"],
                    value_type: bind_library::ValueType::Number,
                    extends: false,
                    values: vec![(bind_library::Value::Number("x".to_string(), 1))],
                }],
            }];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("test", "symbol")),
                Some(&Symbol::Key("test.symbol".to_string(), bind_library::ValueType::Number))
            );
            assert_eq!(
                st.get(&make_identifier!("test", "symbol", "x")),
                Some(&Symbol::NumberValue(1))
            );
        }

        #[test]
        fn all_value_types() {
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("hummingbird"),
                using: vec![],
                declarations: vec![
                    bind_library::Declaration {
                        identifier: make_identifier!["sunbeam"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![(bind_library::Value::Number("shining".to_string(), 1))],
                    },
                    bind_library::Declaration {
                        identifier: make_identifier!["mountaingem"],
                        value_type: bind_library::ValueType::Bool,
                        extends: false,
                        values: vec![
                            (bind_library::Value::Bool("white-bellied".to_string(), false)),
                        ],
                    },
                    bind_library::Declaration {
                        identifier: make_identifier!["brilliant"],
                        value_type: bind_library::ValueType::Enum,
                        extends: false,
                        values: vec![(bind_library::Value::Enum("black-throated".to_string()))],
                    },
                    bind_library::Declaration {
                        identifier: make_identifier!["woodnymph"],
                        value_type: bind_library::ValueType::Str,
                        extends: false,
                        values: vec![
                            (bind_library::Value::Str(
                                "fork-tailed".to_string(),
                                "sabrewing".to_string(),
                            )),
                        ],
                    },
                ],
            }];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("hummingbird", "sunbeam", "shining")),
                Some(&Symbol::NumberValue(1))
            );
            assert_eq!(
                st.get(&make_identifier!("hummingbird", "mountaingem", "white-bellied")),
                Some(&Symbol::BoolValue(false))
            );
            assert_eq!(
                st.get(&make_identifier!("hummingbird", "brilliant", "black-throated")),
                Some(&Symbol::EnumValue("hummingbird.brilliant.black-throated".to_string()))
            );
            assert_eq!(
                st.get(&make_identifier!("hummingbird", "woodnymph", "fork-tailed")),
                Some(&Symbol::StringValue("sabrewing".to_string()))
            );
        }

        #[test]
        fn extension() {
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![(bind_library::Value::Number("x".to_string(), 1))],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![Include { name: make_identifier!("lib_a"), alias: None }],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["lib_a", "symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: true,
                        values: vec![(bind_library::Value::Number("y".to_string(), 2))],
                    }],
                },
            ];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol")),
                Some(&Symbol::Key("lib_a.symbol".to_string(), bind_library::ValueType::Number))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol", "x")),
                Some(&Symbol::NumberValue(1))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_b", "symbol", "y")),
                Some(&Symbol::NumberValue(2))
            );
        }

        #[test]
        fn aliased_extension() {
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![(bind_library::Value::Number("x".to_string(), 1))],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![Include {
                        name: make_identifier!("lib_a"),
                        alias: Some("alias".to_string()),
                    }],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["alias", "symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: true,
                        values: vec![(bind_library::Value::Number("y".to_string(), 2))],
                    }],
                },
            ];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol")),
                Some(&Symbol::Key("lib_a.symbol".to_string(), bind_library::ValueType::Number))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol", "x")),
                Some(&Symbol::NumberValue(1))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_b", "symbol", "y")),
                Some(&Symbol::NumberValue(2))
            );
        }

        #[test]
        fn aliased_extension_with_library_alias() {
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![(bind_library::Value::Number("x".to_string(), 1))],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![Include {
                        name: make_identifier!("lib_a"),
                        alias: Some("alias".to_string()),
                    }],
                    declarations: vec![
                        bind_library::Declaration {
                            identifier: make_identifier!["alias", "symbol"],
                            value_type: bind_library::ValueType::Number,
                            extends: true,
                            values: vec![(bind_library::Value::Number("y".to_string(), 2))],
                        },
                        bind_library::Declaration {
                            identifier: make_identifier!["enum_symbol"],
                            value_type: bind_library::ValueType::Enum,
                            extends: false,
                            values: vec![(bind_library::Value::Enum("the_val".to_string()))],
                        },
                    ],
                },
            ];

            // The symbol table will be constructed with 'lib_b' aliased as 'opaque'.
            let st = construct_symbol_table(
                libraries.iter(),
                HashMap::from([(make_identifier!("lib_b"), "opaque".to_string())]),
            )
            .unwrap();

            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol")),
                Some(&Symbol::Key("lib_a.symbol".to_string(), bind_library::ValueType::Number))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol", "x")),
                Some(&Symbol::NumberValue(1))
            );
            assert_eq!(
                st.get(&make_identifier!("opaque", "symbol", "y")),
                Some(&Symbol::NumberValue(2))
            );
            assert_eq!(st.get(&make_identifier!("lib_b", "symbol", "y")), None);
            assert_eq!(
                st.get(&make_identifier!("opaque", "enum_symbol")),
                Some(&Symbol::Key("lib_b.enum_symbol".to_string(), bind_library::ValueType::Enum))
            );
            assert_eq!(st.get(&make_identifier!("lib_b", "enum_symbol")), None);
            assert_eq!(
                st.get(&make_identifier!("opaque", "enum_symbol", "the_val")),
                Some(&Symbol::EnumValue("lib_b.enum_symbol.the_val".to_string()))
            );
            assert_eq!(st.get(&make_identifier!("lib_b", "enum_symbol", "the_val")), None);
        }

        #[test]
        fn deprecated_key_extension() {
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("lib_a"),
                using: vec![],
                declarations: vec![bind_library::Declaration {
                    identifier: make_identifier!["fuchsia", "BIND_PCI_DID"],
                    value_type: bind_library::ValueType::Number,
                    extends: true,
                    values: vec![(bind_library::Value::Number("x".to_string(), 0x1234))],
                }],
            }];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("lib_a", "BIND_PCI_DID", "x")),
                Some(&Symbol::NumberValue(0x1234))
            );
        }

        #[test]
        fn duplicate_key() {
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("test"),
                using: vec![],
                declarations: vec![
                    bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    },
                    bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    },
                ],
            }];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::DuplicateIdentifier(make_identifier!("test", "symbol")))
            );
        }

        #[test]
        fn duplicate_value() {
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("test"),
                using: vec![],
                declarations: vec![bind_library::Declaration {
                    identifier: make_identifier!["symbol"],
                    value_type: bind_library::ValueType::Number,
                    extends: false,
                    values: vec![
                        bind_library::Value::Number("a".to_string(), 1),
                        bind_library::Value::Number("a".to_string(), 2),
                    ],
                }],
            }];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::DuplicateIdentifier(make_identifier!("test", "symbol", "a")))
            );
        }

        #[test]
        fn keys_are_qualified() {
            // The same symbol declared in two libraries should not collide.
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    }],
                },
            ];

            let st = construct_symbol_table(libraries.iter(), HashMap::new()).unwrap();
            assert_eq!(
                st.get(&make_identifier!("lib_a", "symbol")),
                Some(&Symbol::Key("lib_a.symbol".to_string(), bind_library::ValueType::Number))
            );
            assert_eq!(
                st.get(&make_identifier!("lib_b", "symbol")),
                Some(&Symbol::Key("lib_b.symbol".to_string(), bind_library::ValueType::Number))
            );
        }

        #[test]
        fn missing_extend_keyword() {
            // A library referring to a previously declared symbol must use the "extend" keyword.
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["lib_a", "symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: false,
                        values: vec![],
                    }],
                },
            ];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::MissingExtendsKeyword(make_identifier!("lib_a", "symbol")))
            );
        }

        #[test]
        fn invalid_extend_keyword() {
            // A library cannot declare an unqualified (and therefore locally namespaced) symbol
            // with the "extend" keyword.
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("lib_a"),
                using: vec![],
                declarations: vec![bind_library::Declaration {
                    identifier: make_identifier!["symbol"],
                    value_type: bind_library::ValueType::Number,
                    extends: true,
                    values: vec![],
                }],
            }];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::InvalidExtendsKeyword(make_identifier!("lib_a", "symbol")))
            );
        }

        #[test]
        fn unresolved_qualification() {
            // A library cannot refer to a qualified identifier where the qualifier is not in its
            // list of includes.
            let libraries = vec![bind_library::Ast {
                name: make_identifier!("lib_a"),
                using: vec![],
                declarations: vec![bind_library::Declaration {
                    identifier: make_identifier!["lib_b", "symbol"],
                    value_type: bind_library::ValueType::Number,
                    extends: true,
                    values: vec![],
                }],
            }];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::UnresolvedQualification(make_identifier!("lib_b", "symbol")))
            );
        }

        #[test]
        fn undeclared_key() {
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![Include { name: make_identifier!("lib_a"), alias: None }],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["lib_a", "symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: true,
                        values: vec![],
                    }],
                },
            ];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::UndeclaredKey(make_identifier!("lib_a", "symbol")))
            );
        }

        #[test]
        fn type_mismatch() {
            let libraries = vec![
                bind_library::Ast {
                    name: make_identifier!("lib_a"),
                    using: vec![],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["symbol"],
                        value_type: bind_library::ValueType::Str,
                        extends: false,
                        values: vec![],
                    }],
                },
                bind_library::Ast {
                    name: make_identifier!("lib_b"),
                    using: vec![Include { name: make_identifier!("lib_a"), alias: None }],
                    declarations: vec![bind_library::Declaration {
                        identifier: make_identifier!["lib_a", "symbol"],
                        value_type: bind_library::ValueType::Number,
                        extends: true,
                        values: vec![],
                    }],
                },
            ];

            assert_eq!(
                construct_symbol_table(libraries.iter(), HashMap::new()),
                Err(CompilerError::TypeMismatch(make_identifier!("lib_a", "symbol")))
            );
        }
    }
}
