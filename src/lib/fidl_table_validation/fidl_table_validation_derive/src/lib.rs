// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "128"]

use heck::CamelCase;
use proc_macro2::{Span, TokenStream};
use quote::quote;
use std::convert::TryFrom;
use syn::spanned::Spanned;
use syn::{punctuated::*, token::*, *};

type Result<T> = std::result::Result<T, Error>;

const INVALID_FIDL_FIELD_ATTRIBUTE_MSG: &str = concat!(
    "fidl_field_type attribute must be required, optional, ",
    "or default = value; alternatively use fidl_field_with_default for non-literal constants"
);

#[proc_macro_derive(
    ValidFidlTable,
    attributes(fidl_table_src, fidl_field_type, fidl_table_validator, fidl_field_with_default)
)]
pub fn validate_fidl_table(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let input: DeriveInput = syn::parse(input).unwrap();
    match input.data {
        Data::Struct(DataStruct { fields: syn::Fields::Named(fields), .. }) => {
            match impl_valid_fidl_table(&input.ident, fields.named, &input.attrs) {
                Ok(v) => v.into(),
                Err(e) => e.to_compile_error().into(),
            }
        }
        _ => Error::new(input.span(), "ValidateFidlTable only supports non-tuple structs!")
            .to_compile_error()
            .into(),
    }
}

fn metas<'a>(attrs: &'a [Attribute]) -> impl Iterator<Item = Meta> + 'a {
    attrs.iter().filter_map(|a| match a.parse_meta() {
        Ok(meta) => Some(meta),
        Err(_) => None,
    })
}

fn list_with_arg(meta: Meta, call: &str) -> Option<NestedMeta> {
    match meta {
        Meta::List(list) => {
            if list.path.is_ident(call) && list.nested.len() == 1 {
                Some(list.nested.into_pairs().next().unwrap().into_value())
            } else {
                None
            }
        }
        _ => None,
    }
}

fn unique_list_with_arg(attrs: &[Attribute], call: &str) -> Result<Option<NestedMeta>> {
    let metas = metas(attrs);
    let mut lists_with_arg: Vec<NestedMeta> =
        metas.filter_map(|meta| list_with_arg(meta, call)).collect();
    if lists_with_arg.len() > 1 {
        return Err(Error::new(
            lists_with_arg[1].span(),
            &format!("The {} attribute should only be declared once.", call),
        ));
    }

    Ok(lists_with_arg.pop())
}

fn fidl_table_path(span: Span, attrs: &[Attribute]) -> Result<Path> {
    match unique_list_with_arg(attrs, "fidl_table_src")? {
        Some(nested) => match nested {
            NestedMeta::Meta(Meta::Path(fidl_table_path)) => Ok(fidl_table_path),
            _ => Err(Error::new(
                span,
                concat!(
                    "The #[fidl_table_src(FidlTableType)] attribute ",
                    "takes only one argument, a type name."
                ),
            )),
        },
        _ => Err(Error::new(
            span,
            concat!(
                "To derive ValidFidlTable, struct needs ",
                "#[fidl_table_src(FidlTableType)] attribute to mark ",
                "source Fidl type."
            ),
        )),
    }
}

fn fidl_table_validator(span: Span, attrs: &[Attribute]) -> Result<Option<Ident>> {
    unique_list_with_arg(attrs, "fidl_table_validator")?
        .map(|nested| match nested {
            NestedMeta::Meta(Meta::Path(fidl_table_validator)) => fidl_table_validator
                .get_ident()
                .cloned()
                .ok_or(Error::new(fidl_table_validator.span(), "Invalid Identifier")),
            _ => Err(Error::new(
                span,
                concat!(
                    "The #[fidl_table(FidlTableType)] attribute takes ",
                    "only one argument, a type name."
                ),
            )),
        })
        .transpose()
}

#[derive(Clone, Debug)]
struct FidlField {
    ident: Ident,
    #[allow(unused)]
    in_vec: bool,
    kind: FidlFieldKind,
}

impl TryFrom<Field> for FidlField {
    type Error = Error;
    fn try_from(src: Field) -> Result<Self> {
        let span = src.span();
        let ident = match src.ident {
            Some(ident) => Ok(ident),
            None => {
                Err(Error::new(span, "ValidFidlTable can only be derived for non-tuple structs."))
            }
        }?;

        let kind = FidlFieldKind::try_from((span, src.attrs.as_slice()))?;

        let in_vec = match src.ty {
            syn::Type::Path(path) => {
                let first_segment = path.path.segments.iter().next();
                let second_segment = match first_segment {
                    Some(PathSegment {
                        arguments:
                            syn::PathArguments::AngleBracketed(AngleBracketedGenericArguments {
                                args,
                                ..
                            }),
                        ..
                    }) => match args.first() {
                        Some(GenericArgument::Type(syn::Type::Path(path))) => {
                            path.path.segments.iter().next()
                        }
                        _ => None,
                    },
                    _ => None,
                };

                let extract_name = |segment: &PathSegment| format!("{}", segment.ident);
                let first_segment = first_segment.map(&extract_name);
                let second_segment = second_segment.map(&extract_name);

                match (kind.clone(), first_segment, second_segment) {
                    (FidlFieldKind::Required, Some(segment), _) if segment == "Vec" => true,
                    (FidlFieldKind::Optional, _, Some(segment)) if segment == "Vec" => true,
                    _ => false,
                }
            }
            _ => false,
        };

        Ok(FidlField { ident, in_vec, kind })
    }
}

impl FidlField {
    fn camel_case(&self) -> Ident {
        let name = self.ident.to_string().to_camel_case();
        Ident::new(&name, Span::call_site())
    }

    fn generate_try_from(&self, missing_field_error_type: &Ident) -> TokenStream {
        let ident = &self.ident;
        match &self.kind {
            FidlFieldKind::Required => {
                let camel_case = self.camel_case();
                match self.in_vec {
                    true => quote!(
                        #ident: {
                            let src_vec = src.#ident.ok_or(#missing_field_error_type::#camel_case)?;
                            src_vec
                                .into_iter()
                                .map(std::convert::TryFrom::try_from)
                                .map(|r| r.map_err(anyhow::Error::from))
                                .collect::<std::result::Result<_, anyhow::Error>>()?
                        },
                    ),
                    false => quote!(
                        #ident: std::convert::TryFrom::try_from(
                            src.#ident.ok_or(#missing_field_error_type::#camel_case)?
                        ).map_err(anyhow::Error::from)?,
                    ),
                }
            }
            FidlFieldKind::Optional => match self.in_vec {
                true => quote!(
                    #ident: if let Some(src_vec) = src.#ident {
                        Some(
                            src_vec
                                .into_iter()
                                .map(std::convert::TryFrom::try_from)
                                .map(|r| r.map_err(anyhow::Error::from))
                                .collect::<std::result::Result<_, anyhow::Error>>()?
                        )
                    } else {
                        None
                    },
                ),
                false => quote!(
                    #ident: if let Some(field) = src.#ident {
                        Some(
                            std::convert::TryFrom::try_from(
                                field
                            ).map_err(anyhow::Error::from)?
                        )
                    } else {
                        None
                    },
                ),
            },
            FidlFieldKind::Default => quote!(
                #ident: src.#ident.unwrap_or_default(),
            ),
            FidlFieldKind::ExprDefault(default_ident) => quote!(
                #ident: src.#ident.unwrap_or(#default_ident),
            ),
            FidlFieldKind::HasDefault(value) => quote!(
                #ident: src.#ident.unwrap_or(#value),
            ),
        }
    }
}

#[derive(Clone, Debug)]
enum FidlFieldKind {
    Required,
    Optional,
    Default,
    ExprDefault(Ident),
    HasDefault(Lit),
}

impl TryFrom<(Span, &[Attribute])> for FidlFieldKind {
    type Error = Error;
    fn try_from((span, attrs): (Span, &[Attribute])) -> Result<Self> {
        if let Some(kind) = match unique_list_with_arg(attrs, "fidl_field_type")? {
            Some(NestedMeta::Meta(Meta::Path(field_type))) => {
                field_type.get_ident().and_then(|i| match i.to_string().as_str() {
                    "required" => Some(FidlFieldKind::Required),
                    "optional" => Some(FidlFieldKind::Optional),
                    "default" => Some(FidlFieldKind::Default),
                    _ => None,
                })
            }
            Some(NestedMeta::Meta(Meta::NameValue(ref default_value)))
                if default_value.path.is_ident("default") =>
            {
                Some(FidlFieldKind::HasDefault(default_value.lit.clone()))
            }
            _ => None,
        } {
            return Ok(kind);
        }

        let error = Err(Error::new(span, INVALID_FIDL_FIELD_ATTRIBUTE_MSG));
        match unique_list_with_arg(attrs, "fidl_field_with_default")? {
            Some(NestedMeta::Meta(Meta::Path(field_type))) => match field_type.get_ident() {
                Some(ident) => Ok(FidlFieldKind::ExprDefault(ident.clone())),
                _ => error,
            },
            _ => Ok(FidlFieldKind::Required),
        }
    }
}

fn impl_valid_fidl_table(
    name: &Ident,
    fields: Punctuated<Field, Comma>,
    attrs: &[Attribute],
) -> Result<TokenStream> {
    let fidl_table_path = fidl_table_path(name.span(), attrs)?;
    let fidl_table_type = match fidl_table_path.segments.last() {
        Some(segment) => segment.ident.clone(),
        None => {
            return Err(Error::new(
                name.span(),
                concat!(
                    "The #[fidl_table_src(FidlTableType)] attribute ",
                    "takes only one argument, a type name."
                ),
            ))
        }
    };

    let missing_field_error_type = {
        let mut error_type_name = fidl_table_type.to_string();
        error_type_name.push_str("MissingFieldError");
        Ident::new(&error_type_name, Span::call_site())
    };

    let error_type_name = {
        let mut error_type_name = fidl_table_type.to_string();
        error_type_name.push_str("ValidationError");
        Ident::new(&error_type_name, Span::call_site())
    };

    let custom_validator = fidl_table_validator(name.span(), attrs)?;
    let custom_validator_error = custom_validator.as_ref().map(|validator| {
        quote!(
            /// Custom validator error.
            Logical(<#validator as Validate<#name>>::Error),
        )
    });
    let custom_validator_call =
        custom_validator.as_ref().map(|validator| quote!(#validator::validate(&maybe_valid)?;));
    let custom_validator_error_from_impl = custom_validator.map(|validator| {
        quote!(
            impl From<<#validator as Validate<#name>>::Error> for #error_type_name {
                fn from(src: <#validator as Validate<#name>>::Error) -> Self {
                    #error_type_name::Logical(src)
                }
            }
        )
    });

    let fields: Vec<FidlField> = fields
        .into_pairs()
        .map(Pair::into_value)
        .map(FidlField::try_from)
        .collect::<Result<Vec<FidlField>>>()?;

    let mut field_validations = TokenStream::new();
    field_validations.extend(fields.iter().map(|f| f.generate_try_from(&missing_field_error_type)));

    let mut field_intos = TokenStream::new();
    field_intos.extend(fields.iter().map(|field| {
        let ident = &field.ident;
        match &field.kind {
            FidlFieldKind::Optional => match field.in_vec {
                true => quote!(
                    #ident: if let Some(field) = src.#ident {
                        Some(field.into_iter().map(Into::into).collect())
                    } else {
                        None
                    },
                ),
                false => quote!(
                    #ident: if let Some(field) = src.#ident {
                        Some(field.into())
                    } else {
                        None
                    },
                ),
            },
            _ => match field.in_vec {
                true => quote!(
                    #ident: Some(
                        src.#ident.into_iter().map(Into::into).collect()
                    ),
                ),
                false => quote!(
                    #ident: Some(
                        src.#ident.into()
                    ),
                ),
            },
        }
    }));

    let mut field_errors = TokenStream::new();
    field_errors.extend(
        fields
            .iter()
            .filter(|field| match field.kind {
                FidlFieldKind::Required => true,
                _ => false,
            })
            .map(|field| {
                let doc = format!("`{}` is missing.", field.ident.to_string());
                let camel_case = FidlField::camel_case(field);
                quote!(
                    #[doc = #doc]
                    #camel_case,
                )
            }),
    );

    let missing_error_doc = format!("Missing fields in `{}`.", fidl_table_type);
    let error_doc = format!("Errors validating `{}`.", fidl_table_type);
    Ok(quote!(
        #[doc = #missing_error_doc]
        #[derive(Debug, Clone, Copy, PartialEq)]
        pub enum #missing_field_error_type {
            #field_errors
        }

        #[doc = #error_doc]
        #[derive(Debug)]
        pub enum #error_type_name {
            /// Missing Field.
            MissingField(#missing_field_error_type),
            /// Invalid Field.
            InvalidField(anyhow::Error),
            #custom_validator_error
        }

        impl std::fmt::Display for #error_type_name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "Validation error: {:?}", self)
            }
        }

        impl std::error::Error for #error_type_name {}

        impl From<anyhow::Error> for #error_type_name {
            fn from(src: anyhow::Error) -> Self {
                #error_type_name::InvalidField(src)
            }
        }

        impl From<#missing_field_error_type> for #error_type_name {
            fn from(src: #missing_field_error_type) -> Self {
                #error_type_name::MissingField(src)
            }
        }

        #custom_validator_error_from_impl

        impl std::convert::TryFrom<#fidl_table_path> for #name {
            type Error = #error_type_name;
            fn try_from(src: #fidl_table_path) -> std::result::Result<Self, Self::Error> {
                use ::fidl_table_validation::Validate;
                let maybe_valid = Self {
                    #field_validations
                };
                #custom_validator_call
                Ok(maybe_valid)
            }
        }

        impl std::convert::From<#name> for #fidl_table_path {
            fn from(src: #name) -> #fidl_table_path {
                Self {
                    #field_intos
                    ..#fidl_table_path::EMPTY
                }
            }
        }
    ))
}
