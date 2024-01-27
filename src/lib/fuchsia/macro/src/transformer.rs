// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_diagnostics::Severity,
    proc_macro2::TokenStream,
    quote::{quote, quote_spanned, TokenStreamExt},
    syn::{
        parse::{Parse, ParseStream},
        Attribute, Block, Error, Ident, ItemFn, LitBool, LitInt, LitStr, Signature, Token,
        Visibility,
    },
};

#[derive(Clone, Copy)]
enum FunctionType {
    Component,
    Test,
}

// How should code be executed?
#[derive(Clone, Copy)]
enum Executor {
    // Directly by calling it
    None,
    // fasync::run_singlethreaded
    Singlethreaded,
    // fasync::run
    Multithreaded(usize),
    // #[test]
    Test,
    // fasync::run_singlethreaded(test)
    SinglethreadedTest,
    // fasync::run(test)
    MultithreadedTest(usize),
    // fasync::run_until_stalled
    UntilStalledTest,
}

impl Executor {
    fn is_test(&self) -> bool {
        match self {
            Executor::Test
            | Executor::SinglethreadedTest
            | Executor::MultithreadedTest(_)
            | Executor::UntilStalledTest => true,
            Executor::None | Executor::Singlethreaded | Executor::Multithreaded(_) => false,
        }
    }

    fn is_some(&self) -> bool {
        !matches!(self, Executor::Test | Executor::None)
    }
}

impl quote::ToTokens for Executor {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        tokens.extend(match self {
            Executor::None => quote! { ::fuchsia::main_not_async(func) },
            Executor::Test => quote! { ::fuchsia::test_not_async(func) },
            Executor::Singlethreaded => quote! { ::fuchsia::main_singlethreaded(func) },
            Executor::Multithreaded(n) => quote! { ::fuchsia::main_multithreaded(func, #n) },
            Executor::SinglethreadedTest => quote! { ::fuchsia::test_singlethreaded(func) },
            Executor::MultithreadedTest(n) => quote! { ::fuchsia::test_multithreaded(func, #n) },
            Executor::UntilStalledTest => quote! { ::fuchsia::test_until_stalled(func) },
        })
    }
}

// Helper trait for things that can generate the final token stream
trait Finish {
    fn finish(self) -> TokenStream
    where
        Self: Sized;
}

pub struct Transformer {
    executor: Executor,
    attrs: Vec<Attribute>,
    vis: Visibility,
    sig: Signature,
    block: Box<Block>,
    logging: bool,
    logging_tags: LoggingTags,
    interest: Interest,
    add_test_attr: bool,
}

struct Args {
    threads: usize,
    allow_stalls: Option<bool>,
    logging: bool,
    logging_tags: LoggingTags,
    interest: Interest,
    add_test_attr: bool,
}

#[derive(Default)]
struct LoggingTags {
    tags: Vec<String>,
}

impl quote::ToTokens for LoggingTags {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        for tag in &self.tags {
            tag.as_str().to_tokens(tokens);
            tokens.append(proc_macro2::Punct::new(',', proc_macro2::Spacing::Alone));
        }
    }
}

impl Parse for LoggingTags {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        let mut tags = vec![];
        while !input.is_empty() {
            tags.push(input.parse::<LitStr>()?.value());
            if input.is_empty() {
                break;
            }
            input.parse::<Token![,]>()?;
        }
        Ok(Self { tags })
    }
}

#[derive(Default)]
struct Interest {
    min_severity: Option<Severity>,
}

impl Parse for Interest {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        let str_token = input.parse::<LitStr>()?;
        let min_severity = match str_token.value().to_lowercase().as_str() {
            "trace" => Severity::Trace,
            "debug" => Severity::Debug,
            "info" => Severity::Info,
            "warn" => Severity::Warn,
            "error" => Severity::Error,
            "fatal" => Severity::Fatal,
            other => {
                return Err(syn::Error::new(
                    str_token.span(),
                    format!("invalid severity: {}", other),
                ))
            }
        };
        Ok(Interest { min_severity: Some(min_severity) })
    }
}

impl quote::ToTokens for Interest {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        tokens.extend(match self.min_severity {
            None => quote! { ::fuchsia::Interest::default() },
            Some(severity) => match severity {
                Severity::Trace => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Trace),
                        ..Default::default()
                    }
                },
                Severity::Debug => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Debug),
                        ..Default::default()
                    }
                },
                Severity::Info => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Info),
                        ..Default::default()
                    }
                },
                Severity::Warn => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Warn),
                        ..Default::default()
                    }
                },
                Severity::Error => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Error),
                        ..Default::default()
                    }
                },
                Severity::Fatal => quote! {
                    ::fuchsia::Interest {
                        min_severity: Some(::fuchsia::Severity::Fatal),
                        ..Default::default()
                    }
                },
            },
        });
    }
}

fn get_arg<T: Parse>(p: &ParseStream<'_>) -> syn::Result<T> {
    p.parse::<Token![=]>()?;
    p.parse()
}

fn get_base10_arg<T>(p: &ParseStream<'_>) -> syn::Result<T>
where
    T: std::str::FromStr,
    T::Err: std::fmt::Display,
{
    get_arg::<LitInt>(p)?.base10_parse()
}

fn get_bool_arg(p: &ParseStream<'_>, if_present: bool) -> syn::Result<bool> {
    if p.peek(Token![=]) {
        Ok(get_arg::<LitBool>(p)?.value)
    } else {
        Ok(if_present)
    }
}

fn get_logging_tags(p: &ParseStream<'_>) -> syn::Result<LoggingTags> {
    p.parse::<Token![=]>()?;
    let content;
    syn::bracketed!(content in p);
    let logging_tags = content.parse::<LoggingTags>()?;
    Ok(logging_tags)
}

fn get_interest_arg(input: &ParseStream<'_>) -> syn::Result<Interest> {
    input.parse::<Token![=]>()?;
    input.parse::<Interest>()
}

impl Parse for Args {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        let mut args = Self {
            threads: 1,
            allow_stalls: None,
            logging: true,
            logging_tags: LoggingTags::default(),
            interest: Interest::default(),
            add_test_attr: true,
        };

        loop {
            if input.is_empty() {
                break;
            }
            let ident: Ident = input.parse()?;
            let err = |message| Err(Error::new(ident.span(), message));
            match ident.to_string().as_ref() {
                "threads" => args.threads = get_base10_arg(&input)?,
                "allow_stalls" => args.allow_stalls = Some(get_bool_arg(&input, true)?),
                "logging" => args.logging = get_bool_arg(&input, true)?,
                "logging_tags" => args.logging_tags = get_logging_tags(&input)?,
                "logging_minimum_severity" => args.interest = get_interest_arg(&input)?,
                "add_test_attr" => args.add_test_attr = get_bool_arg(&input, true)?,
                x => return err(format!("unknown argument: {}", x)),
            }
            if input.is_empty() {
                break;
            }
            input.parse::<Token![,]>()?;
        }

        Ok(args)
    }
}

impl Transformer {
    pub fn parse_main(args: TokenStream, input: TokenStream) -> Result<Self, Error> {
        Self::parse(FunctionType::Component, args, input)
    }

    pub fn parse_test(args: TokenStream, input: TokenStream) -> Result<Self, Error> {
        Self::parse(FunctionType::Test, args, input)
    }

    pub fn finish(self) -> TokenStream {
        Finish::finish(self)
    }

    // Construct a new Transformer, verifying correctness.
    fn parse(
        function_type: FunctionType,
        args: TokenStream,
        input: TokenStream,
    ) -> Result<Transformer, Error> {
        let args: Args = syn::parse2(args)?;
        let ItemFn { attrs, vis, sig, block } = syn::parse2(input)?;
        let is_async = sig.asyncness.is_some();

        let err = |message| Err(Error::new(sig.ident.span(), message));

        let executor = match (args.threads, args.allow_stalls, is_async, function_type) {
            (0, _, _, _) => return err("need at least one thread"),
            (_, Some(_), _, FunctionType::Component) => {
                return err("allow_stalls only applies to tests")
            }
            (1, _, false, FunctionType::Component) => Executor::None,
            (1, None, true, FunctionType::Component) => Executor::Singlethreaded,
            (n, None, true, FunctionType::Component) => Executor::Multithreaded(n),
            (1, Some(_), false, FunctionType::Test) => {
                return err("allow_stalls only applies to async tests")
            }
            (1, None, false, FunctionType::Test) => Executor::Test,
            (1, Some(true) | None, true, FunctionType::Test) => Executor::SinglethreadedTest,
            (n, Some(true) | None, true, FunctionType::Test) => Executor::MultithreadedTest(n),
            (1, Some(false), true, FunctionType::Test) => Executor::UntilStalledTest,
            (_, Some(false), _, FunctionType::Test) => {
                return err("allow_stalls=false tests must be single threaded")
            }
            (_, Some(true) | None, false, _) => return err("must be async to use >1 thread"),
        };

        Ok(Transformer {
            executor,
            attrs,
            vis,
            sig,
            block,
            logging: args.logging,
            logging_tags: args.logging_tags,
            interest: args.interest,
            add_test_attr: args.add_test_attr,
        })
    }
}

impl Finish for Transformer {
    // Build the transformed code, knowing that everything is ok because we proved that in parse.
    fn finish(self) -> TokenStream {
        let ident = self.sig.ident;
        let span = ident.span();
        let ret_type = self.sig.output;
        let attrs = self.attrs;
        let visibility = self.vis;
        let asyncness = self.sig.asyncness;
        let block = self.block;
        let inputs = self.sig.inputs;
        let logging_tags = self.logging_tags;
        let interest = self.interest;

        let mut func_attrs = Vec::new();

        // Initialize logging
        let init_logging = if !self.logging {
            quote! { func }
        } else if self.executor.is_test() {
            let test_name = LitStr::new(&format!("{}", ident), ident.span());
            if self.executor.is_some() {
                quote! {
                    ::fuchsia::init_logging_for_test_with_executor(
                        func, #test_name, &[#logging_tags], #interest)
                }
            } else {
                quote! {
                    ::fuchsia::init_logging_for_test_with_threads(
                        func, #test_name, &[#logging_tags], #interest)
                }
            }
        } else {
            if self.executor.is_some() {
                quote! {
                    ::fuchsia::init_logging_for_component_with_executor(
                        func, &[#logging_tags], #interest)
                }
            } else {
                quote! {
                    ::fuchsia::init_logging_for_component_with_threads(
                        func, &[#logging_tags], #interest)
                }
            }
        };

        if self.executor.is_test() && self.add_test_attr {
            // Add test attribute to outer function.
            func_attrs.push(quote!(#[test]));
        }

        let func = if self.executor.is_test() {
            quote! { test_entry_point }
        } else {
            quote! { component_entry_point }
        };

        // Adapt the runner function based on whether it's a test and argument count
        // by providing needed arguments.
        let adapt_main = match (self.executor.is_test(), inputs.len()) {
            // Main function, no arguments - no adaption needed.
            (false, 0) => quote! { #func },
            // Main function, one arguemnt - adapt by parsing command line arguments.
            (false, 1) => quote! { ::fuchsia::adapt_to_parse_arguments(#func) },
            // Test function, no arguments - adapt by taking the run number and discarding it.
            (true, 0) => quote! { ::fuchsia::adapt_to_take_test_run_number(#func) },
            // Test function, one argument - no adaption needed.
            (true, 1) => quote! { #func },
            // Anything with more than one argument: error.
            (_, n) => panic!("Too many ({}) arguments to function", n),
        };
        let tokenized_executor = self.executor;
        let is_nonempty_ret_type = !matches!(ret_type, syn::ReturnType::Default);

        // Select executor
        let (run_executor, modified_ret_type) = match (self.logging, is_nonempty_ret_type) {
            (true, false) | (false, false) => (quote!(#tokenized_executor), quote!(#ret_type)),
            (false, true) => (quote!(#tokenized_executor), quote!(#ret_type)),
            _ => (
                quote! {
                    let result = #tokenized_executor;
                    match result {
                        std::result::Result::Ok(res) => {
                            std::result::Result::Ok(res)
                        }
                        std::result::Result::Err(e) => {
                            ::fuchsia::error!("{:?}", e);
                            std::result::Result::Err(e)
                        }
                     }
                },
                quote!(#ret_type),
            ),
        };

        // Finally build output.
        let output = quote_spanned! {span =>
            #(#attrs)* #(#func_attrs)*
            #visibility fn #ident () #modified_ret_type {
                // Note: `ItemFn::block` includes the function body braces. Do
                // not add additional braces (will break source code coverage
                // analysis).
                // TODO(fxbug.dev/77212): Try to improve the Rust compiler to
                // ease this restriction.
                #asyncness fn #func(#inputs) #ret_type #block
                let func = #adapt_main;
                let func = #init_logging;
                #run_executor
            }
        };
        output.into()
    }
}

impl Finish for Error {
    fn finish(self) -> TokenStream {
        self.to_compile_error().into()
    }
}

impl<R: Finish, E: Finish> Finish for Result<R, E> {
    fn finish(self) -> TokenStream {
        match self {
            Ok(r) => r.finish(),
            Err(e) => e.finish(),
        }
    }
}
