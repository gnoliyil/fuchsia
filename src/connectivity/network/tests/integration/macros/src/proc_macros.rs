// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use either::Either;
use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::quote;

/// A specific implementation of a test variant.
#[derive(Clone, Debug)]
struct Implementation {
    type_name: syn::Path,
    suffix: &'static str,
    attrs: Vec<syn::Attribute>,
}

/// A variant tests will be generated for.
struct Variant<'a> {
    trait_bound: syn::Path,
    implementations: &'a [Implementation],
}

/// A specific variation of a test.
#[derive(Default, Debug)]
struct TestVariation {
    /// Params that we use to instantiate the test.
    params: Vec<syn::Path>,
    /// Unrelated bounds that we pass along.
    generics: Vec<syn::TypeParam>,
    /// Suffix of the test name.
    suffix: String,
    /// Extra attributes that get stuck on the test function.
    attributes: Vec<syn::Attribute>,
}

fn str_to_syn_path(path: &str) -> syn::Path {
    let mut segments = syn::punctuated::Punctuated::<_, syn::token::Colon2>::new();
    for seg in path.split("::") {
        segments.push(syn::PathSegment {
            ident: syn::Ident::new(seg, Span::call_site()),
            arguments: syn::PathArguments::None,
        });
    }
    syn::Path { leading_colon: None, segments }
}

fn permutations_over_type_generics<'a>(
    variants: &'a [Variant<'a>],
    type_generics: &'a [&'a syn::TypeParam],
) -> impl Iterator<Item = TestVariation> + 'a {
    if type_generics.is_empty() {
        return Either::Right(std::iter::once(TestVariation::default()));
    }

    let find_implementations = |type_param: &syn::TypeParam| -> Option<&[Implementation]> {
        let trait_bound = type_param.bounds.iter().find_map(|b| match b {
            syn::TypeParamBound::Lifetime(_) => None,
            syn::TypeParamBound::Trait(t) => Some(t),
        })?;
        variants.iter().find_map(|Variant { trait_bound: t, implementations }| {
            (t == &trait_bound.path).then_some(*implementations)
        })
    };

    #[derive(Clone, Debug)]
    enum Piece<'a> {
        PassThroughGeneric(&'a syn::TypeParam),
        Instantiated(&'a Implementation),
    }
    let piece_iterators = type_generics.into_iter().map(|type_param| {
        // If there are multiple implementations, produce an iterator that
        // will yield them all. Otherwise produce an iterator that will
        // yield the generic parameter once.
        match find_implementations(type_param) {
            None => Either::Left(std::iter::once(Piece::PassThroughGeneric(*type_param))),
            Some(implementations) => Either::Right(
                implementations
                    .into_iter()
                    .map(|implementation| Piece::Instantiated(implementation)),
            ),
        }
    });

    Either::Left(itertools::Itertools::multi_cartesian_product(piece_iterators).map(|pieces| {
        let (mut params, mut generics, mut attributes, mut name_pieces) =
            (Vec::new(), Vec::new(), Vec::new(), vec![""]);
        for piece in pieces {
            match piece {
                Piece::Instantiated(Implementation { type_name, suffix, attrs }) => {
                    params.push(type_name.clone());
                    name_pieces.push(*suffix);
                    attributes.extend(attrs.into_iter().cloned());
                }
                Piece::PassThroughGeneric(type_param) => {
                    params.push(type_param.ident.clone().into());
                    generics.push(type_param.clone());
                }
            }
        }
        let suffix = name_pieces.join("_");
        TestVariation { params, generics, suffix, attributes }
    }))
}

fn netstack_test_inner(input: TokenStream, variants: &[Variant<'_>]) -> TokenStream {
    let item = input.clone();
    let mut item = syn::parse_macro_input!(item as syn::ItemFn);
    let impl_attrs = std::mem::replace(&mut item.attrs, Vec::new());
    let syn::ItemFn { attrs: _, vis: _, ref sig, block: _ } = &item;
    let syn::Signature {
        constness: _,
        asyncness: _,
        unsafety: _,
        abi: _,
        fn_token: _,
        ident: name,
        generics: syn::Generics { lt_token: _, params, gt_token: _, where_clause },
        paren_token: _,
        inputs,
        variadic: _,
        output,
    } = sig;

    let arg = if let Some(arg) = inputs.first() {
        arg
    } else {
        return syn::Error::new_spanned(inputs, "test functions must have a name argument")
            .to_compile_error()
            .into();
    };

    let arg_type = match arg {
        syn::FnArg::Typed(syn::PatType { attrs: _, pat: _, colon_token: _, ty }) => ty,
        other => {
            return syn::Error::new_spanned(
                inputs,
                format!(
                    "test function's first argument must be a `&str` for test name; got = {:#?}",
                    other
                ),
            )
            .to_compile_error()
            .into()
        }
    };

    let arg_type = match arg_type.as_ref() {
        syn::Type::Reference(syn::TypeReference {
            and_token: _,
            lifetime: _,
            mutability: _,
            elem,
        }) => elem,
        other => {
            return syn::Error::new_spanned(
                inputs,
                format!(
                    "test function's first argument must be a `&str` for test name; got = {:#?}",
                    other
                ),
            )
            .to_compile_error()
            .into()
        }
    };

    let arg_type = match arg_type.as_ref() {
        syn::Type::Path(syn::TypePath { qself: _, path }) => path,
        other => {
            return syn::Error::new_spanned(
                inputs,
                format!(
                    "test function's first argument must be a `&str` for test name; got = {:#?}",
                    other
                ),
            )
            .to_compile_error()
            .into()
        }
    };

    if !arg_type.is_ident("str") {
        return syn::Error::new_spanned(
            inputs,
            "test function's first argument must be a `&str`  for test name",
        )
        .to_compile_error()
        .into();
    }

    // We only care about generic type parameters and their last trait bound.
    let mut type_generics = Vec::with_capacity(params.len());
    for gen in params.iter() {
        let generic_type = match gen {
            syn::GenericParam::Type(t) => t,
            other => {
                return syn::Error::new_spanned(
                    proc_macro2::TokenStream::from(input),
                    format!("test functions only support generic parameters; got = {:#?}", other),
                )
                .to_compile_error()
                .into()
            }
        };

        type_generics.push(generic_type)
    }

    // Pass the test name as the first argument, and keep other arguments
    // in the generated function which will be passed to the original function.
    let impl_inputs = inputs.iter().skip(1).cloned().collect::<Vec<_>>();

    let mut args = Vec::new();
    for arg in impl_inputs.iter() {
        let arg = match arg {
            syn::FnArg::Typed(syn::PatType { attrs: _, pat, colon_token: _, ty: _ }) => pat,
            other => {
                return syn::Error::new_spanned(
                    proc_macro2::TokenStream::from(input),
                    format!("expected typed fn arg; got = {:#?}", other),
                )
                .to_compile_error()
                .into()
            }
        };

        let arg = match arg.as_ref() {
            syn::Pat::Ident(syn::PatIdent {
                attrs: _,
                by_ref: _,
                mutability: _,
                ident,
                subpat: _,
            }) => ident,
            other => {
                return syn::Error::new_spanned(
                    proc_macro2::TokenStream::from(input),
                    format!("expected ident fn arg; got = {:#?}", other),
                )
                .to_compile_error()
                .into()
            }
        };

        args.push(syn::Expr::Path(syn::ExprPath {
            attrs: Vec::new(),
            qself: None,
            path: arg.clone().into(),
        }));
    }

    let make_args = |name: String| {
        std::iter::once(syn::Expr::Lit(syn::ExprLit {
            attrs: vec![],
            lit: syn::Lit::Str(syn::LitStr::new(&name, Span::call_site())),
        }))
        .chain(args.iter().cloned())
    };

    let mut permutations = permutations_over_type_generics(variants, &type_generics).peekable();
    let first_permutation = permutations.next().expect("at least one permutation");

    // If we're not emitting any variants we're aware of, just re-emit the
    // function with its name passed in as the first argument.
    if permutations.peek().is_none() {
        let TestVariation { params: _, generics, suffix, attributes } = first_permutation;
        // Suffix should be empty for single permutation.
        assert_eq!(suffix, "");
        let args = make_args(name.to_string());
        let result = quote! {
            #(#impl_attrs)*
            #(#attributes)*
            #[fuchsia_async::run_singlethreaded(test)]
            async fn #name < #(#generics),* > ( #(#impl_inputs),* ) #output #where_clause {
                #item
                #name ( #(#args),* ).await
            }
        }
        .into();
        return result;
    }

    // Generate the list of test variations we will generate.
    //
    // Glue the first permutation back on so it gets included in the output.
    let impls = std::iter::once(first_permutation).chain(permutations).map(
        |TestVariation { params, generics, suffix, attributes }| {
            // We don't need to add an "_" between the name and the suffix here as the suffix
            // will start with one.
            let test_name_str = format!("{}{}", name.to_string(), suffix);
            let test_name = syn::Ident::new(&test_name_str, Span::call_site());
            let args = make_args(test_name_str);

            quote! {
                #(#impl_attrs)*
                #(#attributes)*
                #[fuchsia_async::run_singlethreaded(test)]
                async fn #test_name < #(#generics),* > ( #(#impl_inputs),* ) #output #where_clause {
                    #name :: < #(#params),* > ( #(#args),* ).await
                }
            }
        },
    );

    let result = quote! {
        #item
        #(#impls)*
    };

    result.into()
}

/// Runs a test `fn` over different variations of Netstacks, device endpoints
/// and/or network managers based on the test `fn`'s type parameters.
///
/// The test `fn` may only be generic over any combination of `Netstack` and
/// `Manager`. It may only have a single `&str` argument, used to identify the
/// test variation.
///
/// Example:
///
/// ```
/// #[netstack_test]
/// async fn test_foo<N: Netstack>(name: &str) {}
/// ```
///
/// Expands to:
/// ```
/// async fn test_foo<N: Netstack>(name: &str){/*...*/}
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns2() {
///     test_foo::<netstack_testing_common::realms::Netstack2>("test_foo_ns2").await
/// }
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns3() {
///     test_foo::<netstack_testing_common::realms::Netstack3>("test_foo_ns3").await
/// }
/// ```
///
/// Similarly,
/// ```
/// #[netstack_test]
/// async fn test_foo<M: Manager>(name: &str) {/*...*/}
/// ```
///
/// Expands equivalently to the netstack variant.
///
/// This macro also supports expanding with multiple variations, including
/// multiple occurrences of the same trait bound.
/// ```
/// #[netstack_test]
/// async fn test_foo<N1: Netstack, N2: Netstack>(name: &str) {/*...*/}
/// ```
///
/// Expands to:
/// ```
/// async fn test_foo<N1: Netstack, N2: Netstack>(name: &str) {/*...*/}
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns2_ns2() {
///     test_foo::<
///         netstack_testing_common::realms::Netstack2,
///         netstack_testing_common::realms::Netstack2,
///     >("test_foo_ns2_ns2")
///     .await
/// }
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns2_ns3() {
///     test_foo::<
///         netstack_testing_common::realms::Netstack2,
///         netstack_testing_common::realms::Netstack3,
///     >("test_foo_ns2_ns3")
///     .await
/// }
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns3_ns2() {
///     test_foo::<
///         netstack_testing_common::realms::Netstack3,
///         netstack_testing_common::realms::Netstack2,
///     >("test_foo_ns3_ns2")
///     .await
/// }
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo_ns3_ns3() {
///     test_foo::<
///         netstack_testing_common::realms::Netstack3,
///         netstack_testing_common::realms::Netstack3,
///     >("test_foo_ns3_ns3")
///     .await
/// }
/// ```
///
/// A test with no type parameters is expanded to receive the function name as
/// the first argument.
/// ```
/// #[netstack_test]
/// async fn test_foo(name: &str) {/*...*/}
/// ```
///
/// Expands to
/// ```
/// #[fuchsia_async::run_singlethreaded(test)]
/// async fn test_foo() {
///    async fn test_foo(name: &str){/*...*/}
///    test_foo("test_foo").await
/// }
/// ```
#[proc_macro_attribute]
pub fn netstack_test(attrs: TokenStream, input: TokenStream) -> TokenStream {
    if !attrs.is_empty() {
        return syn::Error::new_spanned(
            proc_macro2::TokenStream::from(attrs),
            "unrecognized attributes",
        )
        .to_compile_error()
        .into();
    }

    let disable_on_riscv: syn::Attribute = syn::parse_quote!(#[cfg(not(target_arch = "riscv64"))]);

    netstack_test_inner(
        input,
        &[
            Variant {
                trait_bound: str_to_syn_path("Netstack"),
                implementations: &[
                    Implementation {
                        type_name: str_to_syn_path("netstack_testing_common::realms::Netstack2"),
                        suffix: "ns2",
                        attrs: vec![disable_on_riscv.clone()],
                    },
                    Implementation {
                        type_name: str_to_syn_path("netstack_testing_common::realms::Netstack3"),
                        suffix: "ns3",
                        attrs: vec![],
                    },
                ],
            },
            Variant {
                trait_bound: str_to_syn_path("DhcpClient"),
                implementations: &[
                    Implementation {
                        type_name: str_to_syn_path("netstack_testing_common::realms::InStack"),
                        suffix: "dhcp_in_stack",
                        attrs: vec![],
                    },
                    Implementation {
                        type_name: str_to_syn_path("netstack_testing_common::realms::OutOfStack"),
                        suffix: "dhcp_out_of_stack",
                        attrs: vec![],
                    },
                ],
            },
            Variant {
                trait_bound: str_to_syn_path("NetstackAndDhcpClient"),
                implementations: &[
                    Implementation {
                        type_name: str_to_syn_path(
                            "netstack_testing_common::realms::Netstack2AndInStackDhcpClient",
                        ),
                        suffix: "ns2_with_dhcp_in_stack",
                        attrs: vec![disable_on_riscv.clone()],
                    },
                    Implementation {
                        type_name: str_to_syn_path(
                            "netstack_testing_common::realms::Netstack2AndOutOfStackDhcpClient",
                        ),
                        suffix: "ns2_with_dhcp_out_of_stack",
                        attrs: vec![disable_on_riscv.clone()],
                    },
                    Implementation {
                        type_name: str_to_syn_path(
                            "netstack_testing_common::realms::Netstack3AndOutOfStackDhcpClient",
                        ),
                        suffix: "ns3_with_dhcp_out_of_stack",
                        attrs: vec![],
                    },
                ],
            },
            Variant {
                trait_bound: str_to_syn_path("Manager"),
                implementations: &[
                    Implementation {
                        type_name: str_to_syn_path("netstack_testing_common::realms::NetCfgBasic"),
                        suffix: "netcfg_basic",
                        attrs: vec![],
                    },
                    Implementation {
                        type_name: str_to_syn_path(
                            "netstack_testing_common::realms::NetCfgAdvanced",
                        ),
                        suffix: "netcfg_advanced",
                        attrs: vec![],
                    },
                ],
            },
            Variant {
                trait_bound: str_to_syn_path("net_types::ip::Ip"),
                implementations: &[
                    Implementation {
                        type_name: str_to_syn_path("net_types::ip::Ipv6"),
                        suffix: "v6",
                        attrs: vec![],
                    },
                    Implementation {
                        type_name: str_to_syn_path("net_types::ip::Ipv4"),
                        suffix: "v4",
                        attrs: vec![],
                    },
                ],
            },
        ],
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use test_case::test_case;

    #[derive(Debug)]
    struct VariantExpectation {
        params: Vec<&'static str>,
        suffix: &'static str,
    }

    impl PartialEq<TestVariation> for VariantExpectation {
        fn eq(&self, other: &TestVariation) -> bool {
            self.suffix == other.suffix
                && self.params
                    == other
                        .params
                        .iter()
                        .map(|p| p.get_ident().unwrap().to_string())
                        .collect::<Vec<_>>()
        }
    }

    #[test_case(vec![] => vec![VariantExpectation {
        params: vec![],
        suffix: "",
    }]; "default")]
    #[test_case(vec!["T: TraitA"] => vec![VariantExpectation {
        params: vec!["ImplA1"],
        suffix: "_a1",
    }, VariantExpectation {
        params: vec!["ImplA2"],
        suffix: "_a2",
    }]; "simple case")]
    #[test_case(vec!["T: TraitA", "S: TraitB"] => vec![VariantExpectation {
        params: vec!["ImplA1", "ImplB1"],
        suffix: "_a1_b1",
    }, VariantExpectation {
        params: vec!["ImplA1", "ImplB2"],
        suffix: "_a1_b2",
    }, VariantExpectation {
        params: vec!["ImplA2", "ImplB1"],
        suffix: "_a2_b1",
    }, VariantExpectation {
        params: vec!["ImplA2", "ImplB2"],
        suffix: "_a2_b2",
    }]; "two traits")]
    #[test_case(vec!["T1: TraitA", "T2: TraitA"] => vec![VariantExpectation {
        params: vec!["ImplA1", "ImplA1"],
        suffix: "_a1_a1",
    }, VariantExpectation {
        params: vec!["ImplA1", "ImplA2"],
        suffix: "_a1_a2",
    }, VariantExpectation {
        params: vec!["ImplA2", "ImplA1"],
        suffix: "_a2_a1",
    }, VariantExpectation {
        params: vec!["ImplA2", "ImplA2"],
        suffix: "_a2_a2",
    }]; "two occurrences of a single trait")]
    fn permutation(generics: impl IntoIterator<Item = &'static str>) -> Vec<TestVariation> {
        let generics = generics
            .into_iter()
            .map(|g| syn::parse_str(g).unwrap())
            .collect::<Vec<syn::TypeParam>>();
        let generics = generics.iter().collect::<Vec<&_>>();

        permutations_over_type_generics(
            &[
                Variant {
                    trait_bound: syn::parse_str("TraitA").unwrap(),
                    implementations: &[
                        Implementation {
                            type_name: syn::parse_str("ImplA1").unwrap(),
                            suffix: "a1",
                            attrs: vec![],
                        },
                        Implementation {
                            type_name: syn::parse_str("ImplA2").unwrap(),
                            suffix: "a2",
                            attrs: vec![],
                        },
                    ],
                },
                Variant {
                    trait_bound: syn::parse_str("TraitB").unwrap(),
                    implementations: &[
                        Implementation {
                            type_name: syn::parse_str("ImplB1").unwrap(),
                            suffix: "b1",
                            attrs: vec![],
                        },
                        Implementation {
                            type_name: syn::parse_str("ImplB2").unwrap(),
                            suffix: "b2",
                            attrs: vec![],
                        },
                    ],
                },
            ],
            &generics,
        )
        .collect()
    }
}
