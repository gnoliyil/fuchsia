// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{
        util::{
            array_bounds, for_banjo_transport, get_base_type_from_alias, get_declarations,
            get_doc_comment, get_in_args_c, get_in_params_c, is_derive_debug, is_namespaced,
            name_buffer, name_size, not_callback, primitive_type_to_c_str, to_c_name,
            type_to_c_str, Decl, ProtocolType,
        },
        Backend,
    },
    anyhow::{anyhow, Context, Error},
    fidl_ir_lib::fidl::*,
    std::io,
    std::iter,
};

pub struct CBackend<'a, W: io::Write> {
    // Note: a mutable reference is used here instead of an owned object in
    // order to facilitate testing.
    w: &'a mut W,
}

impl<'a, W: io::Write> CBackend<'a, W> {
    pub fn new(w: &'a mut W) -> Self {
        CBackend { w }
    }
}

fn integer_type_to_c_str(ty: &IntegerType) -> Result<String, Error> {
    primitive_type_to_c_str(&ty.to_primitive())
}

fn integer_constant_to_c_str(
    ty: &IntegerType,
    constant: &Constant,
    ir: &FidlIr,
) -> Result<String, Error> {
    constant_to_c_str(&ty.to_type(), constant, ir)
}

fn constant_to_c_str(ty: &Type, constant: &Constant, ir: &FidlIr) -> Result<String, Error> {
    let value = match constant {
        Constant::Identifier { value, .. } => value,
        Constant::Literal { expression, .. } => expression,
        Constant::BinaryOperator { value, .. } => value,
    };
    match ty {
        Type::Primitive { subtype } => match subtype {
            PrimitiveSubtype::Bool => Ok(value.clone()),
            PrimitiveSubtype::Int8 => Ok(String::from(format!("INT8_C({})", value))),
            PrimitiveSubtype::Int16 => Ok(String::from(format!("INT16_C({})", value))),
            PrimitiveSubtype::Int32 => Ok(String::from(format!("INT32_C({})", value))),
            PrimitiveSubtype::Int64 => Ok(String::from(format!("INT64_C({})", value))),
            PrimitiveSubtype::Uint8 => Ok(String::from(format!("UINT8_C({})", value))),
            PrimitiveSubtype::Uint16 => Ok(String::from(format!("UINT16_C({})", value))),
            PrimitiveSubtype::Uint32 => Ok(String::from(format!("UINT32_C({})", value))),
            PrimitiveSubtype::Uint64 => Ok(String::from(format!("UINT64_C({})", value))),
            t => Err(anyhow!("Can't handle this primitive type: {:?}", t)),
        },
        Type::Identifier { identifier, .. } => match ir
            .get_declaration(identifier)
            .unwrap_or_else(|e| panic!("Can't identify: {:?}: {}", identifier, e))
        {
            Declaration::Enum => {
                let decl = ir.get_enum(identifier)?;
                return integer_constant_to_c_str(&decl._type, constant, ir);
            }
            Declaration::Bits => {
                let decl = ir.get_bits(identifier)?;
                return constant_to_c_str(&decl._type, constant, ir);
            }
            t => Err(anyhow!("Can't handle this constant identifier: {:?}", t)),
        },
        t => Err(anyhow!("Can't handle this constant type: {:?}", t)),
    }
}

fn struct_attrs_to_c_str(maybe_attributes: &Option<Vec<Attribute>>) -> String {
    if let Some(attributes) = maybe_attributes {
        attributes
            .iter()
            .filter_map(|a| match to_lower_snake_case(a.name.as_ref()).as_str() {
                "packed" => Some("__attribute__ ((packed))"),
                _ => None,
            })
            .collect::<Vec<_>>()
            .join(" ")
    } else {
        String::from("")
    }
}

fn field_to_c_str(
    maybe_attributes: &Option<Vec<Attribute>>,
    ty: &Type,
    ident: &Identifier,
    indent: &str,
    preserve_names: bool,
    alias: &Option<TypeConstructor>,
    ir: &FidlIr,
) -> Result<String, Error> {
    let mut accum = String::new();

    accum.push_str(get_doc_comment(maybe_attributes, 1).as_str());
    let c_name = if preserve_names { String::from(&ident.0) } else { to_c_name(&ident.0) };
    if let Some(arg_type) = get_base_type_from_alias(&alias.as_ref().map(|t| &t.name)) {
        accum.push_str(
            format!("{indent}{ty} {c_name};", indent = indent, ty = arg_type, c_name = c_name)
                .as_str(),
        );
        return Ok(accum);
    }
    let prefix = if maybe_attributes.has("Mutable") { "" } else { "const " };
    let ty_name = type_to_c_str(&ty, ir)?;
    match ty {
        Type::Str { maybe_element_count, .. } => {
            if let Some(count) = maybe_element_count {
                accum.push_str(
                    format!(
                        "{indent}char {c_name}[{size}];",
                        indent = indent,
                        c_name = c_name,
                        size = count.0,
                    )
                    .as_str(),
                );
            } else {
                accum.push_str(
                    format!(
                        "{indent}{prefix}{ty} {c_name};",
                        indent = indent,
                        c_name = c_name,
                        prefix = prefix,
                        ty = ty_name
                    )
                    .as_str(),
                );
            }
        }
        Type::Vector { ref element_type, .. } => {
            let ptr = if maybe_attributes.has("OutOfLineContents") { "*" } else { "" };
            let nullable = match **element_type {
                Type::Vector { nullable, .. } if nullable => "*",
                Type::Str { nullable, .. } if nullable => "*",
                Type::Handle { nullable, .. } if nullable => "*",
                Type::Request { nullable, .. } if nullable => "*",
                Type::Identifier { nullable, .. } if nullable => "*",
                _ => "",
            };
            accum.push_str(
                format!(
                    "{indent}{prefix}{ty}{ptr}*{nullable} {c_name}_{buffer};\n\
                     {indent}size_t {c_name}_{size};",
                    indent = indent,
                    nullable = nullable,
                    buffer = name_buffer(&maybe_attributes),
                    size = name_size(&maybe_attributes),
                    c_name = c_name,
                    prefix = prefix,
                    ty = ty_name,
                    ptr = ptr,
                )
                .as_str(),
            );
        }
        Type::Array { .. } => {
            let bounds = array_bounds(&ty).unwrap();
            accum.push_str(
                format!(
                    "{indent}{ty} {c_name}{bounds};",
                    indent = indent,
                    c_name = c_name,
                    bounds = bounds,
                    ty = ty_name,
                )
                .as_str(),
            );
        }
        _ => {
            accum.push_str(
                format!("{indent}{ty} {c_name};", indent = indent, c_name = c_name, ty = ty_name)
                    .as_str(),
            );
        }
    }
    Ok(accum)
}

fn get_first_param(method: &Method, ir: &FidlIr) -> Result<(bool, String), Error> {
    if let Some(response) = &method.response_parameters(ir)? {
        if let Some(param) = response.get(0) {
            if let Some(arg_type) = get_base_type_from_alias(
                &param.experimental_maybe_from_alias.as_ref().map(|t| &t.name),
            ) {
                return Ok((true, arg_type));
            }
            if param._type.is_primitive(ir)? {
                return Ok((true, type_to_c_str(&param._type, ir)?));
            }
        }
    }
    Ok((false, "void".to_string()))
}

fn get_out_params(name: &str, m: &Method, ir: &FidlIr) -> Result<(Vec<String>, String), Error> {
    let protocol_name = to_c_name(name);
    let method_name = to_c_name(&m.name.0);
    if m.maybe_attributes.has("Async") {
        return Ok((
            vec![
                format!(
                    "{protocol_name}_{method_name}_callback callback",
                    protocol_name = protocol_name,
                    method_name = method_name
                ),
                "void* cookie".to_string(),
            ],
            "void".to_string(),
        ));
    }

    let (skip, return_param) = get_first_param(m, ir)?;
    let skip_amt = if skip { 1 } else { 0 };

    Ok((
        m.response_parameters(ir)?.as_ref()
            .map_or(Vec::new(), |response| {
                response.iter().skip(skip_amt)
                .map(|param| {
                    let c_name = to_c_name(&param.name.0);
                    if let Some(arg_type) = get_base_type_from_alias(
                        &param.experimental_maybe_from_alias.as_ref().map(|t| &t.name),
                    ) {
                        return format!("{}* out_{}", arg_type, c_name);
                    }
                    let ty_name = type_to_c_str(&param._type, ir).unwrap();
                    match &param._type {
                        Type::Identifier { nullable, .. } => {
                            let nullable_str = if *nullable { "*" } else { "" };
                            format!("{}{}* out_{}", ty_name, nullable_str, c_name)
                        }
                        Type::Array { .. } => {
                            let bounds = array_bounds(&param._type).unwrap();
                            format!(
                                "{ty} out_{name}{bounds}",
                                bounds = bounds,
                                ty = ty_name,
                                name = c_name
                            )
                        }
                        Type::Vector { .. } => {
                            let buffer_name = name_buffer(&param.maybe_attributes);
                            let size_name = name_size(&param.maybe_attributes);
                            if param.maybe_attributes.has("CalleeAllocated") {
                                format!("{ty}** out_{name}_{buffer}, size_t* {name}_{size}",
                                        buffer = buffer_name,
                                        size = size_name,
                                        ty = ty_name,
                                        name = c_name)
                            } else {
                                format!("{ty}* out_{name}_{buffer}, size_t {name}_{size}, size_t* out_{name}_actual",
                                        buffer = buffer_name,
                                        size = size_name,
                                        ty = ty_name,
                                        name = c_name)
                            }
                        },
                        Type::Str {..} => {
                            format!("{ty} out_{c_name}, size_t {c_name}_capacity",
                                    ty = ty_name, c_name = c_name)
                        }
                        Type::Handle {..} => format!("{}* out_{}", ty_name, c_name),
                        _ => format!("{}* out_{}", ty_name, c_name),
                    }
                }).collect()
            }), return_param))
}

fn get_out_args(m: &Method, ir: &FidlIr) -> Result<(Vec<String>, bool), Error> {
    if m.maybe_attributes.has("Async") {
        return Ok((vec!["callback".to_string(), "cookie".to_string()], false));
    }

    let (skip, _) = get_first_param(m, ir)?;
    let skip_amt = if skip { 1 } else { 0 };
    Ok((
        m.response_parameters(ir)?.as_ref().map_or(Vec::new(), |response| {
            response
                .iter()
                .skip(skip_amt)
                .map(|param| {
                    let c_name = to_c_name(&param.name.0);
                    match &param._type {
                        Type::Vector { .. } => {
                            let buffer_name = name_buffer(&param.maybe_attributes);
                            let size_name = name_size(&param.maybe_attributes);
                            if param.maybe_attributes.has("CalleeAllocated") {
                                format!(
                                    "out_{name}_{buffer}, {name}_{size}",
                                    buffer = buffer_name,
                                    size = size_name,
                                    name = c_name
                                )
                            } else {
                                format!(
                                    "out_{name}_{buffer}, {name}_{size}, out_{name}_actual",
                                    buffer = buffer_name,
                                    size = size_name,
                                    name = c_name
                                )
                            }
                        }
                        Type::Str { .. } => {
                            format!("out_{c_name}, {c_name}_capacity", c_name = c_name)
                        }
                        _ => format!("out_{}", c_name),
                    }
                })
                .collect()
        }),
        skip,
    ))
}

impl<'a, W: io::Write> CBackend<'a, W> {
    fn codegen_enum_decl(&self, data: &Enum, ir: &FidlIr) -> Result<String, Error> {
        let name = &data.name.get_name();
        let c_name_lowercase = to_c_name(&name);
        let c_name_uppercase = to_c_name(&name).to_uppercase();

        struct EnumParts {
            v_name: String,
            c_size: String,
        }
        let enum_parts_list = data
            .members
            .iter()
            .map(|v| {
                Ok(EnumParts {
                    v_name: v.name.0.to_uppercase().trim().to_string(),
                    c_size: integer_constant_to_c_str(&data._type, &v.value, ir)?,
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;

        // #define each enum value
        let enum_defines = enum_parts_list
            .iter()
            .map(|enum_parts| {
                Ok(format!(
                    "#define {c_name}_{v_name} {c_size}",
                    c_name = c_name_uppercase,
                    v_name = enum_parts.v_name,
                    c_size = enum_parts.c_size,
                ))
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");

        let declarations = format!(
            "typedef {ty} {c_name}_t;\n{enum_defines}",
            c_name = c_name_lowercase,
            ty = integer_type_to_c_str(&data._type)?,
            enum_defines = enum_defines,
        );

        if !is_derive_debug(&data.maybe_attributes)? {
            return Ok(declarations);
        }

        // Define {c_name}_to_str() helper function to translate enum values
        // into strings.
        let enum_to_str_prologue = vec![
            // #ifndef used since fidlgen_banjo does not strictly generate code
            // once for each declaration.
            format!("#ifndef FUNC_{c_name}_TO_STR_", c_name = c_name_uppercase),
            format!("#define FUNC_{c_name}_TO_STR_", c_name = c_name_uppercase),
            format!(
                "static inline const char* {c_name}_to_str({c_name}_t value) {{",
                c_name = c_name_lowercase
            ),
            format!("  switch (value) {{"),
        ];
        let mut enum_to_str_body = enum_parts_list
            .iter()
            .map(|enum_parts| {
                Ok(format!(
                    "    case {c_name}_{v_name}:\n      return \"{c_name}_{v_name}\";",
                    c_name = c_name_uppercase,
                    v_name = enum_parts.v_name,
                ))
            })
            .collect::<Result<Vec<_>, Error>>()?;
        let mut enum_to_str_epilogue = vec![
            format!("  }}"),
            format!("  return \"UNKNOWN\";"),
            format!("}}"),
            format!("#endif"),
        ];
        let mut enum_to_str = enum_to_str_prologue;
        enum_to_str.append(&mut enum_to_str_body);
        enum_to_str.append(&mut enum_to_str_epilogue);
        let enum_to_str = enum_to_str.join("\n");

        Ok(format!("{}\n{}", declarations, enum_to_str))
    }

    fn codegen_constant_decl(&self, data: &Const, ir: &FidlIr) -> Result<String, Error> {
        let mut accum = String::new();
        accum.push_str(get_doc_comment(&data.maybe_attributes, 0).as_str());

        let name = data.name.get_name().to_string();
        let namespaced = is_namespaced(&data.maybe_attributes)
            .context(format!("Looking for namespaced attribute on constant {}", name))?;
        let name = if namespaced {
            format!("{}_{}", ir.get_library_name().replace(".", "_"), name)
        } else {
            name
        };

        accum.push_str(
            format!(
                "#define {name} {value}",
                name = name,
                value = constant_to_c_str(&data._type, &data.value, ir)?
            )
            .as_str(),
        );
        Ok(accum)
    }

    fn codegen_bits_decl(&self, data: &Bits, ir: &FidlIr) -> Result<String, Error> {
        let name = &data.name.get_name();
        let c_name_lowercase = to_c_name(&name);
        let c_name_uppercase = to_c_name(&name).to_uppercase();

        struct BitsParts {
            v_name: String,
            c_size: String,
        }
        let bits_defines = data
            .members
            .iter()
            .map(|v| {
                Ok(BitsParts {
                    v_name: v.name.0.to_uppercase().trim().to_string(),
                    c_size: constant_to_c_str(&data._type, &v.value, ir)?,
                })
            })
            .map(|bits_parts: Result<_, Error>| {
                let bits_parts = bits_parts?;
                Ok(format!(
                    "#define {c_name}_{v_name} {c_size}",
                    c_name = c_name_uppercase,
                    v_name = bits_parts.v_name,
                    c_size = bits_parts.c_size,
                ))
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");

        let declarations = format!(
            "typedef {ty} {c_name}_t;\n{bits_defines}",
            c_name = c_name_lowercase,
            ty = type_to_c_str(&data._type, ir)?,
            bits_defines = bits_defines,
        );

        Ok(declarations)
    }

    fn codegen_union_decl(&self, data: &Union) -> Result<String, Error> {
        Ok(format!("typedef union {c_name} {c_name}_t;", c_name = to_c_name(&data.name.get_name())))
    }

    fn codegen_union_def(&self, data: &Union, ir: &FidlIr) -> Result<String, Error> {
        let attrs = struct_attrs_to_c_str(&data.maybe_attributes);
        let members = data
            .members
            .iter()
            .filter_map(|f| {
                if let Some(ty) = &f._type {
                    match ty {
                        Type::Vector { .. } => {
                            Some(Err(anyhow!("unsupported for UnionField: {:?}", f)))
                        }
                        _ => Some(field_to_c_str(
                            &f.maybe_attributes,
                            &ty,
                            &f.name.as_ref().unwrap(),
                            "    ",
                            false,
                            &f.experimental_maybe_from_alias,
                            ir,
                        )),
                    }
                } else {
                    None
                }
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");
        let mut accum = String::new();
        accum.push_str(get_doc_comment(&data.maybe_attributes, 0).as_str());
        accum.push_str(
            format!(
                include_str!("templates/c/struct.h"),
                c_name = to_c_name(&data.name.get_name()),
                decl = "union",
                attrs = if attrs.is_empty() { "".to_string() } else { format!(" {}", attrs) },
                members = members
            )
            .as_str(),
        );
        Ok(accum)
    }

    fn codegen_struct_decl(&self, data: &Struct) -> Result<String, Error> {
        Ok(format!(
            "typedef struct {c_name} {c_name}_t;",
            c_name = to_c_name(&data.name.get_name())
        ))
    }

    fn codegen_struct_def(&self, data: &Struct, ir: &FidlIr) -> Result<String, Error> {
        let attrs = struct_attrs_to_c_str(&data.maybe_attributes);
        let preserve_names = data.maybe_attributes.has("PreserveCNames");
        let members = data
            .members
            .iter()
            .map(|f| {
                field_to_c_str(
                    &f.maybe_attributes,
                    &f._type,
                    &f.name,
                    "    ",
                    preserve_names,
                    &f.experimental_maybe_from_alias,
                    ir,
                )
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");
        // We add an extraneous member if there are no members to support empty structs.
        let members = if members.is_empty() { "    uint8_t unused;".to_string() } else { members };
        let mut accum = String::new();
        accum.push_str(get_doc_comment(&data.maybe_attributes, 0).as_str());
        accum.push_str(
            format!(
                include_str!("templates/c/struct.h"),
                c_name = to_c_name(&data.name.get_name()),
                decl = "struct",
                attrs = if attrs.is_empty() { "".to_string() } else { format!(" {}", attrs) },
                members = members
            )
            .as_str(),
        );
        Ok(accum)
    }

    fn codegen_table_decl(&self, data: &Table) -> Result<String, Error> {
        Ok(format!(
            "typedef struct {c_name} {c_name}_t;",
            c_name = to_c_name(&data.name.get_name())
        ))
    }

    fn codegen_table_def(&self, data: &Table, ir: &FidlIr) -> Result<String, Error> {
        let attrs = struct_attrs_to_c_str(&data.maybe_attributes);
        let preserve_names = data.maybe_attributes.has("PreserveCNames");
        let members = data
            .members
            .iter()
            // Ignore reserved fields as they lack types and names.
            .filter(|f| f.reserved == false)
            .map(|f| {
                field_to_c_str(
                    &f.maybe_attributes,
                    &f._type
                        .as_ref()
                        .unwrap_or_else(|| panic!("Missing type on table field {:?}", f)),
                    &f.name
                        .as_ref()
                        .unwrap_or_else(|| panic!("Missing name on table field {:?}", f)),
                    "    ",
                    preserve_names,
                    &None,
                    ir,
                )
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");
        let mut accum = String::new();
        accum.push_str(get_doc_comment(&data.maybe_attributes, 0).as_str());
        accum.push_str(
            format!(
                include_str!("templates/c/struct.h"),
                c_name = to_c_name(&data.name.get_name()),
                decl = "struct",
                attrs = if attrs.is_empty() { "".to_string() } else { format!(" {}", attrs) },
                members = members
            )
            .as_str(),
        );
        Ok(accum)
    }

    fn codegen_protocol_def2(
        &self,
        name: &str,
        methods: &Vec<Method>,
        ir: &FidlIr,
    ) -> Result<String, Error> {
        let fns = methods
            .iter()
            .map(|m| {
                let (out_params, return_param) = get_out_params(name, &m, ir)?;
                let in_params = get_in_params_c(&m, false, ir)?;

                let params = iter::once("void* ctx".to_string())
                    .chain(in_params)
                    .chain(out_params)
                    .collect::<Vec<_>>()
                    .join(", ");
                Ok(format!(
                    "    {return_param} (*{fn_name})({params});",
                    return_param = return_param,
                    params = params,
                    fn_name = to_c_name(&m.name.0)
                ))
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");
        Ok(format!(include_str!("templates/c/protocol_ops.h"), c_name = to_c_name(name), fns = fns))
    }

    fn codegen_protocol_helper(&self, data: &'a Protocol, ir: &FidlIr) -> Result<String, Error> {
        if ProtocolType::from(&data.maybe_attributes) == ProtocolType::Callback
            || !for_banjo_transport(&data.maybe_attributes)
        {
            return Ok("".to_string());
        }
        let name = data.name.get_name();
        data.methods
            .iter()
            .map(|m| {
                let mut accum = String::new();
                accum.push_str(get_doc_comment(&m.maybe_attributes, 0).as_str());

                let (out_params, return_param) = get_out_params(name, &m, ir)?;
                let in_params = get_in_params_c(&m, true, ir)?;

                let first_param = format!("const {}_protocol_t* proto", to_c_name(name));

                let params = iter::once(first_param)
                    .chain(in_params)
                    .chain(out_params)
                    .collect::<Vec<_>>()
                    .join(", ");

                accum.push_str(
                    format!(
                        "static inline {return_param} {protocol_name}_{fn_name}({params}) {{\n",
                        return_param = return_param,
                        params = params,
                        protocol_name = to_c_name(name),
                        fn_name = to_c_name(&m.name.0)
                    )
                    .as_str(),
                );

                let (out_args, skip) = get_out_args(&m, ir)?;
                let in_args = get_in_args_c(&m, ir)?;

                if let Some(request) = &m.request_parameters(ir)? {
                    let proto_args = request
                        .iter()
                        .filter_map(|param| {
                            if let Type::Identifier { ref identifier, .. } = param._type {
                                if not_callback(identifier, ir).ok()? {
                                    return Some((
                                        to_c_name(&param.name.0),
                                        type_to_c_str(&param._type, ir).unwrap(),
                                    ));
                                }
                            }
                            None
                        })
                        .collect::<Vec<_>>();
                    for (name, ty) in proto_args.iter() {
                        accum.push_str(
                            format!(
                                include_str!("templates/c/proto_transform.h"),
                                ty = ty,
                                name = name
                            )
                            .as_str(),
                        );
                    }
                }

                let args = iter::once("proto->ctx".to_string())
                    .chain(in_args)
                    .chain(out_args)
                    .collect::<Vec<_>>()
                    .join(", ");

                let return_statement = if skip { "return " } else { "" };

                accum.push_str(
                    format!(
                        "    {return_statement}proto->ops->{fn_name}({args});\n",
                        return_statement = return_statement,
                        args = args,
                        fn_name = to_c_name(&m.name.0)
                    )
                    .as_str(),
                );
                accum.push_str("}\n");
                Ok(accum)
            })
            .collect::<Result<Vec<_>, Error>>()
            .map(|x| x.join("\n"))
    }

    fn codegen_protocol_def(&self, data: &'a Protocol, ir: &FidlIr) -> Result<String, Error> {
        if !for_banjo_transport(&data.maybe_attributes) {
            return Ok("".to_string());
        }
        let name = data.name.get_name();
        Ok(match ProtocolType::from(&data.maybe_attributes) {
            ProtocolType::Interface | ProtocolType::Protocol => format!(
                include_str!("templates/c/protocol.h"),
                protocol_name = to_c_name(name),
                protocol_def = self.codegen_protocol_def2(name, &data.methods, ir)?,
            ),
            ProtocolType::Callback => {
                let m = data.methods.get(0).ok_or(anyhow!("callback has no methods"))?;
                let (out_params, return_param) = get_out_params(name, &m, ir)?;
                let in_params = get_in_params_c(&m, false, ir)?;

                let params = iter::once("void* ctx".to_string())
                    .chain(in_params)
                    .chain(out_params)
                    .collect::<Vec<_>>()
                    .join(", ");
                let method = format!(
                    "{return_param} (*{fn_name})({params})",
                    return_param = return_param,
                    params = params,
                    fn_name = to_c_name(&m.name.0)
                );
                format!(
                    include_str!("templates/c/callback.h"),
                    callback_name = to_c_name(name),
                    callback = method,
                )
            }
        })
    }

    fn codegen_async_decls(
        &self,
        name: &String,
        data: &Vec<Method>,
        ir: &FidlIr,
    ) -> Result<String, Error> {
        Ok(data
            .iter()
            .filter(|method| method.maybe_attributes.has("Async"))
            .map(|method| {
                let mut temp_method = method.clone();
                temp_method.maybe_request_payload = method.maybe_response_payload.clone();
                temp_method.maybe_response_payload = None;
                let in_params = get_in_params_c(&temp_method, true, ir)?;
                let params = iter::once("void* ctx".to_string())
                    .chain(in_params)
                    .collect::<Vec<_>>()
                    .join(", ");
                Ok(format!(
                    "typedef void (*{protocol_name}_{method_name}_callback)({params});\n",
                    protocol_name = name,
                    method_name = to_c_name(&method.name.0),
                    params = params
                ))
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join(""))
    }

    fn codegen_protocol_decl(&self, data: &'a Protocol, ir: &FidlIr) -> Result<String, Error> {
        if !for_banjo_transport(&data.maybe_attributes) {
            return Ok("".to_string());
        }
        let name = to_c_name(&data.name.get_name());
        Ok(match ProtocolType::from(&data.maybe_attributes) {
            ProtocolType::Interface | ProtocolType::Protocol => format!(
                "{async_decls}typedef struct {c_name}_protocol {c_name}_protocol_t;\n\
                 typedef struct {c_name}_protocol_ops {c_name}_protocol_ops_t;",
                async_decls = self.codegen_async_decls(&name, &data.methods, ir)?,
                c_name = name
            ),
            ProtocolType::Callback => format!("typedef struct {c_name} {c_name}_t;", c_name = name),
        })
    }

    fn codegen_alias_decl(&self, data: &Alias, _ir: &FidlIr) -> Result<String, Error> {
        match data.partial_type_ctor.name.as_str() {
            "array" | "string" | "vector" => Ok("".to_string()),
            _ => Ok(format!(
                "typedef {from}_t {to}_t;",
                to = to_c_name(&data.name.get_name()),
                from =
                    to_c_name(&CompoundIdentifier(data.partial_type_ctor.name.clone()).get_name()),
            )),
        }
    }

    fn codegen_includes(&self, ir: &FidlIr) -> Result<String, Error> {
        Ok(ir
            .library_dependencies
            .iter()
            .map(|l| &l.name.0)
            .filter(|n| *n != "zx")
            .map(|n| n.replace('.', "/") + "/c/banjo")
            .map(|n| format!("#include <{}.h>", n))
            .collect::<Vec<_>>()
            .join("\n"))
    }
}

impl<'a, W: io::Write> Backend<'a, W> for CBackend<'a, W> {
    fn codegen(&mut self, ir: FidlIr) -> Result<(), Error> {
        self.w.write_fmt(format_args!(
            include_str!("templates/c/header.h"),
            includes = self.codegen_includes(&ir)?,
            primary_namespace = ir.name.0
        ))?;

        let decl_order = get_declarations(&ir)?;

        let declarations = decl_order
            .iter()
            .filter_map(|decl| match decl {
                Decl::Const { data } => Some(self.codegen_constant_decl(data, &ir)),
                Decl::Enum { data } => Some(self.codegen_enum_decl(data, &ir)),
                Decl::Bits { data } => Some(self.codegen_bits_decl(data, &ir)),
                Decl::Protocol { data } => Some(self.codegen_protocol_decl(data, &ir)),
                Decl::Struct { data } => Some(self.codegen_struct_decl(data)),
                Decl::Table { data } => Some(self.codegen_table_decl(data)),
                Decl::Alias { data } => Some(self.codegen_alias_decl(data, &ir)),
                Decl::Union { data } => Some(self.codegen_union_decl(data)),
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");

        let definitions = decl_order
            .iter()
            .filter_map(|decl| match decl {
                Decl::Protocol { data } => Some(self.codegen_protocol_def(data, &ir)),
                Decl::Struct { data } => Some(self.codegen_struct_def(data, &ir)),
                Decl::Table { data } => Some(self.codegen_table_def(data, &ir)),
                Decl::Union { data } => Some(self.codegen_union_def(data, &ir)),
                _ => None,
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");

        let helpers = decl_order
            .iter()
            .filter_map(|decl| match decl {
                Decl::Protocol { data } => Some(self.codegen_protocol_helper(data, &ir)),
                _ => None,
            })
            .collect::<Result<Vec<_>, Error>>()?
            .join("\n");

        self.w.write_fmt(format_args!(
            include_str!("templates/c/body.h"),
            declarations = declarations,
            definitions = definitions,
            helpers = helpers,
        ))?;
        Ok(())
    }
}
