// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[macro_use]
extern crate quote;

use net_types::ip::{IpAddress, SubnetError};
use proc_macro2::TokenStream;
use std::fmt::Formatter;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV4, SocketAddrV6};
use std::num::ParseIntError;
use std::str::FromStr;

/// Declares a proc_macro with `name` using `generator` to generate any of `ty`.
macro_rules! declare_macro {
    ($name:ident, $generator:ident, $($ty:path),+) => {
        #[proc_macro]
        pub fn $name(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
            Emitter::<$generator, $($ty),+>::emit(input).into()
        }
    }
}

/// Empty slot in an [`Emitter`].
struct Skip;

/// The mock error returned by [`Skip`].
#[derive(Debug)]
struct SkipError;

impl std::fmt::Display for SkipError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        std::fmt::Debug::fmt(self, f)
    }
}
impl std::error::Error for SkipError {}

impl FromStr for Skip {
    type Err = SkipError;

    fn from_str(_s: &str) -> Result<Self, Self::Err> {
        Err(SkipError {})
    }
}

/// Generates a [`TokenStream`] representation of `T`.
trait Generator<T> {
    /// Generates the [`TokenStream`] for `input`.
    fn generate(input: T) -> TokenStream;

    /// Get an optional string representation of type `T`.
    ///
    /// Used in error reporting when parsing fails.
    fn type_str() -> Option<&'static str> {
        Some(std::any::type_name::<T>())
    }
}

impl<O> Generator<Skip> for O {
    fn generate(_input: Skip) -> TokenStream {
        unreachable!()
    }

    fn type_str() -> Option<&'static str> {
        None
    }
}

/// Attempts to emit the resulting [`TokenStream`] for `input` parsed as `T`.
fn try_emit<G, T>(input: &str) -> Result<TokenStream, T::Err>
where
    G: Generator<T>,
    T: FromStr,
{
    Ok(G::generate(T::from_str(input)?))
}

/// Provides common structor for parsing types from [`TokenStream`]s and
/// emitting the resulting [`TokenStream`].
///
/// `Emitter` can parse any of `Tn` and pass into a [`Generator`] `G`, encoding
/// the common logic for declarations build from string parsing at compile-time.
struct Emitter<G, T1 = Skip, T2 = Skip, T3 = Skip, T4 = Skip> {
    _g: std::marker::PhantomData<G>,
    _t1: std::marker::PhantomData<T1>,
    _t2: std::marker::PhantomData<T2>,
    _t3: std::marker::PhantomData<T3>,
    _t4: std::marker::PhantomData<T4>,
}

impl<G, T1, T2, T3, T4> Emitter<G, T1, T2, T3, T4>
where
    G: Generator<T1> + Generator<T2> + Generator<T3> + Generator<T4>,
    T1: FromStr,
    T2: FromStr,
    T3: FromStr,
    T4: FromStr,
    T1::Err: std::error::Error,
    T2::Err: std::error::Error,
    T3::Err: std::error::Error,
    T4::Err: std::error::Error,
{
    /// Emits the resulting [`TokenStream`] (or an error one) after attempting
    /// to parse `input` into all of the `Emitter`'s types sequentially.
    fn emit(input: proc_macro::TokenStream) -> TokenStream {
        // Always require a string literal.
        let input = proc_macro2::TokenStream::from(input);
        let s = match syn::parse2::<syn::LitStr>(input.clone()) {
            Ok(s) => s.value(),
            Err(e) => return e.to_compile_error().into(),
        };
        match try_emit::<G, T1>(&s)
            .or_else(|e1| try_emit::<G, T2>(&s).map_err(|e2| (e1, e2)))
            .or_else(|(e1, e2)| try_emit::<G, T3>(&s).map_err(|e3| (e1, e2, e3)))
            .or_else(|(e1, e2, e3)| try_emit::<G, T4>(&s).map_err(|e4| (e1, e2, e3, e4)))
        {
            Ok(ts) => ts,
            Err((e1, e2, e3, e4)) => syn::Error::new_spanned(
                input,
                format!("failed to parse as {}", Self::error_str(&e1, &e2, &e3, &e4)),
            )
            .to_compile_error()
            .into(),
        }
    }

    /// Get the error string reported to the compiler when parsing fails with
    /// this `Emitter`.
    fn error_str(
        e1: &dyn std::error::Error,
        e2: &dyn std::error::Error,
        e3: &dyn std::error::Error,
        e4: &dyn std::error::Error,
    ) -> String {
        [
            (<G as Generator<T1>>::type_str(), e1),
            (<G as Generator<T2>>::type_str(), e2),
            (<G as Generator<T3>>::type_str(), e3),
            (<G as Generator<T4>>::type_str(), e4),
        ]
        .iter()
        .filter_map(|(ts, e)| ts.map(|t| format!("{}: \"{}\"", t, e)))
        .collect::<Vec<_>>()
        .join(", or ")
    }
}

/// Generator for `std` types.
enum StdGen {}

impl Generator<IpAddr> for StdGen {
    fn generate(input: IpAddr) -> TokenStream {
        let (t, inner) = match input {
            IpAddr::V4(v4) => (quote! { V4 }, Self::generate(v4)),
            IpAddr::V6(v6) => (quote! { V6 }, Self::generate(v6)),
        };
        quote! {
            std::net::IpAddr::#t(#inner)
        }
    }
}

impl Generator<Ipv4Addr> for StdGen {
    fn generate(input: Ipv4Addr) -> TokenStream {
        let octets = input.octets();
        quote! {
            std::net::Ipv4Addr::new(#(#octets),*)
        }
    }
}

impl Generator<Ipv6Addr> for StdGen {
    fn generate(input: Ipv6Addr) -> TokenStream {
        let segments = input.segments();
        quote! {
            std::net::Ipv6Addr::new(#(#segments),*)
        }
    }
}

impl Generator<SocketAddr> for StdGen {
    fn generate(input: SocketAddr) -> TokenStream {
        let (t, inner) = match input {
            SocketAddr::V4(v4) => (quote! { V4 }, Self::generate(v4)),
            SocketAddr::V6(v6) => (quote! { V6 }, Self::generate(v6)),
        };
        quote! {
            std::net::SocketAddr::#t(#inner)
        }
    }
}

impl Generator<SocketAddrV4> for StdGen {
    fn generate(input: SocketAddrV4) -> TokenStream {
        let addr = Self::generate(input.ip().clone());
        let port = input.port();
        quote! {
            std::net::SocketAddrV4::new(#addr, #port)
        }
    }
}

impl Generator<SocketAddrV6> for StdGen {
    fn generate(input: SocketAddrV6) -> TokenStream {
        let addr = Self::generate(input.ip().clone());
        let port = input.port();
        let flowinfo = input.flowinfo();
        let scope_id = input.scope_id();
        quote! {
            std::net::SocketAddrV6::new(#addr, #port, #flowinfo, #scope_id)
        }
    }
}

declare_macro!(std_ip, StdGen, IpAddr);
declare_macro!(std_ip_v4, StdGen, Ipv4Addr);
declare_macro!(std_ip_v6, StdGen, Ipv6Addr);
declare_macro!(std_socket_addr, StdGen, SocketAddr, SocketAddrV4);
declare_macro!(std_socket_addr_v4, StdGen, SocketAddrV4);
declare_macro!(std_socket_addr_v6, StdGen, SocketAddrV6);

/// Generator for FIDL types.
enum FidlGen {}

impl Generator<IpAddr> for FidlGen {
    fn generate(input: IpAddr) -> TokenStream {
        let (t, inner) = match input {
            IpAddr::V4(v4) => (quote! { Ipv4 }, Self::generate(v4)),
            IpAddr::V6(v6) => (quote! { Ipv6 }, Self::generate(v6)),
        };
        quote! {
            fidl_fuchsia_net::IpAddress::#t(#inner)
        }
    }
}

impl Generator<Ipv4Addr> for FidlGen {
    fn generate(input: Ipv4Addr) -> TokenStream {
        let octets = input.octets();
        quote! {
            fidl_fuchsia_net::Ipv4Address{ addr: [#(#octets),*]}
        }
    }
}

impl Generator<Ipv6Addr> for FidlGen {
    fn generate(input: Ipv6Addr) -> TokenStream {
        let octets = input.octets();
        quote! {
            fidl_fuchsia_net::Ipv6Address{ addr: [#(#octets),*]}
        }
    }
}

impl Generator<SocketAddr> for FidlGen {
    fn generate(input: SocketAddr) -> TokenStream {
        let (t, inner) = match input {
            SocketAddr::V4(v4) => (quote! { Ipv4 }, Self::generate(v4)),
            SocketAddr::V6(v6) => (quote! { Ipv6 }, Self::generate(v6)),
        };
        quote! {
            fidl_fuchsia_net::SocketAddress::#t(#inner)
        }
    }
}

impl Generator<SocketAddrV4> for FidlGen {
    fn generate(input: SocketAddrV4) -> TokenStream {
        let addr = Self::generate(input.ip().clone());
        let port = input.port();
        quote! {
            fidl_fuchsia_net::Ipv4SocketAddress {
                address: #addr,
                port: #port
            }
        }
    }
}

impl Generator<SocketAddrV6> for FidlGen {
    fn generate(input: SocketAddrV6) -> TokenStream {
        let addr = Self::generate(input.ip().clone());
        let port = input.port();
        let scope_id = u64::from(input.scope_id());
        quote! {
            fidl_fuchsia_net::Ipv6SocketAddress {
                address: #addr,
                port: #port,
                zone_index: #scope_id
            }
        }
    }
}

/// Helper struct to parse Mac addresses from string.
#[derive(Default)]
struct MacAddress([u8; 6]);

#[derive(thiserror::Error, Debug)]
enum MacParseError {
    #[error("invalid length for MacAddress, should be 6")]
    InvalidLength,
    #[error("invalid byte length (\"{0}\") in MacAddress, should be 2")]
    InvalidByte(String),
    #[error("failed to parse byte \"{0}\": {1}")]
    IntError(String, ParseIntError),
}

impl FromStr for MacAddress {
    type Err = MacParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut mac = Self::default();
        let Self(octets) = &mut mac;
        let mut parse_iter = s.split(':');
        let mut save_iter = octets.iter_mut();
        loop {
            match (parse_iter.next(), save_iter.next()) {
                (Some(s), Some(b)) => {
                    if s.len() != 2 {
                        return Err(MacParseError::InvalidByte(s.to_string()));
                    }
                    *b = u8::from_str_radix(s, 16)
                        .map_err(|e| MacParseError::IntError(s.to_string(), e))?;
                }
                (None, Some(_)) | (Some(_), None) => break Err(MacParseError::InvalidLength),
                (None, None) => break Ok(mac),
            }
        }
    }
}

impl Generator<MacAddress> for FidlGen {
    fn generate(input: MacAddress) -> TokenStream {
        let MacAddress(octets) = input;
        quote! {
            fidl_fuchsia_net::MacAddress {
                octets: [#(#octets),*]
            }
        }
    }
}

#[derive(PartialEq, Debug)]
/// Helper struct to parse Cidr addresses from string.
struct IpAddressWithPrefix<A> {
    address: A,
    prefix: u8,
}

#[derive(thiserror::Error, PartialEq, Debug)]
enum IpAddressWithPrefixParseError {
    #[error("missing address")]
    MissingIp,
    #[error("missing prefix length")]
    MissingPrefix,
    #[error("unexpected trailing data \"{0}\"")]
    TrailingInformation(String),
    #[error("failed to parse IP \"{0}\": {1}")]
    IpParseError(String, std::net::AddrParseError),
    #[error("failed to parse prefix \"{0}\": {1}")]
    PrefixParseError(String, std::num::ParseIntError),
    #[error("bad prefix value {0} for {1}")]
    BadPrefix(u8, std::net::IpAddr),
}

impl FromStr for IpAddressWithPrefix<IpAddr> {
    type Err = IpAddressWithPrefixParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut parse_iter = s.split('/');
        let ip_str = parse_iter.next().ok_or(IpAddressWithPrefixParseError::MissingIp)?;
        let prefix_str = parse_iter.next().ok_or(IpAddressWithPrefixParseError::MissingPrefix)?;
        if let Some(trailing) = parse_iter.next() {
            return Err(IpAddressWithPrefixParseError::TrailingInformation(trailing.to_string()));
        }
        let address = std::net::IpAddr::from_str(ip_str)
            .map_err(|e| IpAddressWithPrefixParseError::IpParseError(ip_str.to_string(), e))?;
        let prefix = u8::from_str_radix(prefix_str, 10).map_err(|e| {
            IpAddressWithPrefixParseError::PrefixParseError(prefix_str.to_string(), e)
        })?;
        let addr_len = 8 * match address {
            std::net::IpAddr::V4(a) => a.octets().len(),
            std::net::IpAddr::V6(a) => a.octets().len(),
        };
        if usize::from(prefix) > addr_len {
            return Err(IpAddressWithPrefixParseError::BadPrefix(prefix, address));
        }
        Ok(Self { address, prefix })
    }
}

impl Generator<IpAddressWithPrefix<IpAddr>> for FidlGen {
    fn generate(input: IpAddressWithPrefix<IpAddr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let address = Self::generate(address);
        quote! {
            fidl_fuchsia_net::Subnet {
                addr: #address,
                prefix_len: #prefix
            }
        }
    }
}

impl Generator<IpAddressWithPrefix<IpAddr>> for NetGen {
    fn generate(input: IpAddressWithPrefix<IpAddr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let address = Self::generate(address);
        // SAFETY: AddrSubnetEither's invariants were already checked.
        quote! {
            unsafe {
                net_types::ip::AddrSubnetEither::new_unchecked(#address, #prefix)
            }
        }
    }
}

impl Generator<IpAddressWithPrefix<Ipv4Addr>> for NetGen {
    fn generate(input: IpAddressWithPrefix<Ipv4Addr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let address = Self::generate(address);
        // SAFETY: AddrSubnet's invariants were already checked.
        quote! {
            unsafe {
                net_types::ip::AddrSubnet::<net_types::ip::Ipv4Addr>::new_unchecked(
                    #address, #prefix,
                )
            }
        }
    }
}

impl Generator<IpAddressWithPrefix<Ipv6Addr>> for NetGen {
    fn generate(input: IpAddressWithPrefix<Ipv6Addr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let address = Self::generate(address);
        // SAFETY: AddrSubnet's invariants were already checked.
        quote! {
            unsafe {
                net_types::ip::AddrSubnet::<net_types::ip::Ipv6Addr>::new_unchecked(
                    #address, #prefix,
                )
            }
        }
    }
}

#[derive(thiserror::Error, PartialEq, Debug)]
enum VersionedIpPrefixError {
    #[error("wrong IP version: {0}")]
    WrongVersion(IpAddr),
    #[error("failed to parse")]
    IpError(#[from] IpAddressWithPrefixParseError),
}

impl FromStr for IpAddressWithPrefix<Ipv4Addr> {
    type Err = VersionedIpPrefixError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let IpAddressWithPrefix { address, prefix } = s.parse::<IpAddressWithPrefix<IpAddr>>()?;
        match address {
            IpAddr::V4(address) => Ok(Self { address, prefix }),
            IpAddr::V6(address) => Err(VersionedIpPrefixError::WrongVersion(address.into())),
        }
    }
}

impl FromStr for IpAddressWithPrefix<Ipv6Addr> {
    type Err = VersionedIpPrefixError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let IpAddressWithPrefix { address, prefix } = s.parse::<IpAddressWithPrefix<IpAddr>>()?;
        match address {
            IpAddr::V6(address) => Ok(Self { address, prefix }),
            IpAddr::V4(address) => Err(VersionedIpPrefixError::WrongVersion(address.into())),
        }
    }
}

impl Generator<IpAddressWithPrefix<Ipv4Addr>> for FidlGen {
    fn generate(input: IpAddressWithPrefix<Ipv4Addr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let addr = Self::generate(address);
        quote! {
            fidl_fuchsia_net::Ipv4AddressWithPrefix {
                addr: #addr,
                prefix_len: #prefix,
            }
        }
    }
}

impl Generator<IpAddressWithPrefix<Ipv6Addr>> for FidlGen {
    fn generate(input: IpAddressWithPrefix<Ipv6Addr>) -> TokenStream {
        let IpAddressWithPrefix { address, prefix } = input;
        let addr = Self::generate(address);
        quote! {
            fidl_fuchsia_net::Ipv6AddressWithPrefix {
                addr: #addr,
                prefix_len: #prefix,
            }
        }
    }
}

declare_macro!(fidl_ip, FidlGen, IpAddr);
declare_macro!(fidl_ip_v4, FidlGen, Ipv4Addr);
declare_macro!(fidl_ip_v4_with_prefix, FidlGen, IpAddressWithPrefix<Ipv4Addr>);
declare_macro!(fidl_ip_v6, FidlGen, Ipv6Addr);
declare_macro!(fidl_ip_v6_with_prefix, FidlGen, IpAddressWithPrefix<Ipv6Addr>);
declare_macro!(fidl_socket_addr, FidlGen, SocketAddr);
declare_macro!(fidl_socket_addr_v4, FidlGen, SocketAddrV4);
declare_macro!(fidl_socket_addr_v6, FidlGen, SocketAddrV6);
declare_macro!(fidl_mac, FidlGen, MacAddress);
declare_macro!(fidl_subnet, FidlGen, IpAddressWithPrefix<IpAddr>);

#[derive(PartialEq, Debug)]
/// Helper struct to parse Cidr addresses from string.
struct StrictSubnet<A> {
    address: A,
    prefix: u8,
}

#[derive(thiserror::Error, PartialEq, Debug)]
enum SubnetParseError {
    #[error(transparent)]
    IpError(#[from] VersionedIpPrefixError),
    #[error("host bits set in {0}/{1}")]
    HostBitsSet(IpAddr, u8),
}

impl FromStr for StrictSubnet<Ipv4Addr> {
    type Err = SubnetParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let IpAddressWithPrefix { address, prefix } = s.parse::<IpAddressWithPrefix<Ipv4Addr>>()?;
        match net_types::ip::Subnet::<net_types::ip::Ipv4Addr>::new(address.into(), prefix) {
            Ok(_) => Ok(StrictSubnet { address, prefix }),
            Err(SubnetError::HostBitsSet) => {
                Err(SubnetParseError::HostBitsSet(address.into(), prefix))
            }
            Err(SubnetError::PrefixTooLong) => {
                unreachable!("checked by IpAddressWithPrefix")
            }
        }
    }
}

impl FromStr for StrictSubnet<Ipv6Addr> {
    type Err = SubnetParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let IpAddressWithPrefix { address, prefix } = s.parse::<IpAddressWithPrefix<Ipv6Addr>>()?;
        match net_types::ip::Subnet::<net_types::ip::Ipv6Addr>::new(address.into(), prefix) {
            Ok(_) => Ok(StrictSubnet { address, prefix }),
            Err(SubnetError::HostBitsSet) => {
                Err(SubnetParseError::HostBitsSet(address.into(), prefix))
            }
            Err(SubnetError::PrefixTooLong) => {
                unreachable!("checked by IpAddressWithPrefix")
            }
        }
    }
}

/// Generator for net-types types.
enum NetGen {}

impl Generator<IpAddr> for NetGen {
    fn generate(input: IpAddr) -> TokenStream {
        let (t, inner) = match input {
            IpAddr::V4(v4) => (quote! { V4 }, Self::generate(v4)),
            IpAddr::V6(v6) => (quote! { V6 }, Self::generate(v6)),
        };
        quote! {
            net_types::ip::IpAddr::#t(#inner)
        }
    }
}

impl Generator<Ipv4Addr> for NetGen {
    fn generate(input: Ipv4Addr) -> TokenStream {
        let octets = input.octets();
        quote! {
            net_types::ip::Ipv4Addr::new([#(#octets),*])
        }
    }
}

impl Generator<Ipv6Addr> for NetGen {
    fn generate(input: Ipv6Addr) -> TokenStream {
        let octets = input.octets();
        quote! {
            net_types::ip::Ipv6Addr::from_bytes([#(#octets),*])
        }
    }
}

impl Generator<MacAddress> for NetGen {
    fn generate(input: MacAddress) -> TokenStream {
        let MacAddress(octets) = input;
        quote! {
            net_types::ethernet::Mac::new([#(#octets),*])
        }
    }
}

impl<I> Generator<StrictSubnet<I>> for NetGen
where
    Self: Generator<I>,
{
    fn generate(input: StrictSubnet<I>) -> TokenStream {
        let StrictSubnet { address, prefix } = input;
        let network = Self::generate(address);
        // SAFETY: Subnet's invariants were already checked.
        quote! {
            unsafe { net_types::ip::Subnet::new_unchecked(#network, #prefix) }
        }
    }
}

declare_macro!(net_ip, NetGen, IpAddr);
declare_macro!(net_ip_v4, NetGen, Ipv4Addr);
declare_macro!(net_ip_v6, NetGen, Ipv6Addr);
declare_macro!(net_mac, NetGen, MacAddress);
declare_macro!(net_subnet_v4, NetGen, StrictSubnet<Ipv4Addr>);
declare_macro!(net_subnet_v6, NetGen, StrictSubnet<Ipv6Addr>);
declare_macro!(net_addr_subnet_v4, NetGen, IpAddressWithPrefix<Ipv4Addr>);
declare_macro!(net_addr_subnet_v6, NetGen, IpAddressWithPrefix<Ipv6Addr>);
declare_macro!(net_addr_subnet, NetGen, IpAddressWithPrefix<IpAddr>);

fn net_prefix_length_impl<I: net_types::ip::Ip>(
    input: proc_macro::TokenStream,
) -> proc_macro::TokenStream {
    let input = proc_macro2::TokenStream::from(input);
    let n = match syn::parse2::<syn::LitInt>(input.clone()).and_then(|n| n.base10_parse::<u8>()) {
        Ok(n) => n,
        Err(e) => return e.to_compile_error().into(),
    };
    if n > I::Addr::BYTES * 8 {
        return syn::Error::new_spanned(
            input,
            format!("{n} is too long to be the prefix length for an {} address", I::NAME),
        )
        .to_compile_error()
        .into();
    }

    let ip_version = match I::VERSION {
        net_types::ip::IpVersion::V4 => quote! { net_types::ip::Ipv4 },
        net_types::ip::IpVersion::V6 => quote! { net_types::ip::Ipv6 },
    };

    quote! {
        // SAFETY: already checked that #n <= I::Addr::BYTES * 8.
        unsafe {
            net_types::ip::PrefixLength::<#ip_version>::new_unchecked(#n)
        }
    }
    .into()
}

#[proc_macro]
pub fn net_prefix_length_v4(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    net_prefix_length_impl::<net_types::ip::Ipv4>(input)
}

#[proc_macro]
pub fn net_prefix_length_v6(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    net_prefix_length_impl::<net_types::ip::Ipv6>(input)
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;

    #[test]
    fn ip_address_with_prefix_valid() {
        assert_eq!(
            "28.19.8.91/16".parse::<IpAddressWithPrefix<IpAddr>>(),
            Ok(IpAddressWithPrefix { address: [28, 19, 8, 91].into(), prefix: 16 })
        );
        assert_eq!(
            "184f:139:84::56:3456/38".parse::<IpAddressWithPrefix<IpAddr>>(),
            Ok(IpAddressWithPrefix {
                address: [0x184f, 0x0139, 0x0084, 0, 0, 0, 0x56, 0x3456].into(),
                prefix: 38
            })
        );

        assert_eq!(
            "1.2.3.4/5".parse::<IpAddressWithPrefix<Ipv4Addr>>(),
            Ok(IpAddressWithPrefix { address: 0x01020304.into(), prefix: 5 })
        );

        assert_eq!(
            "5a:e8:6695::78:4521/108".parse::<IpAddressWithPrefix<Ipv6Addr>>(),
            Ok(IpAddressWithPrefix {
                address: [0x5a, 0xe8, 0x6695, 0, 0, 0, 0x78, 0x4521].into(),
                prefix: 108
            })
        );
    }

    #[test]
    fn ip_address_with_prefix_invalid() {
        assert_matches!(
            "5.6.7.8".parse::<IpAddressWithPrefix<IpAddr>>(),
            Err(IpAddressWithPrefixParseError::MissingPrefix)
        );
        assert_matches!(
            "192.168.0.1/12/x".parse::<IpAddressWithPrefix<IpAddr>>(),
            Err(IpAddressWithPrefixParseError::TrailingInformation(_))
        );
        assert_matches!(
            "12.9.9/6".parse::<IpAddressWithPrefix<IpAddr>>(),
            Err(IpAddressWithPrefixParseError::IpParseError(_, _))
        );
        assert_matches!(
            "192.168.0.1/ff".parse::<IpAddressWithPrefix<IpAddr>>(),
            Err(IpAddressWithPrefixParseError::PrefixParseError(_, _))
        );
        assert_matches!(
            "192.168.0.1/55".parse::<IpAddressWithPrefix<IpAddr>>(),
            Err(IpAddressWithPrefixParseError::BadPrefix(55, _))
        );
    }

    #[test]
    fn ip_address_with_prefix_wrong_type() {
        assert_eq!(
            "28.19.8.91/16".parse::<IpAddressWithPrefix<Ipv6Addr>>(),
            Err(VersionedIpPrefixError::WrongVersion([28, 19, 8, 91].into())),
        );
        assert_eq!(
            "184f:139:84::56:3456/38".parse::<IpAddressWithPrefix<Ipv4Addr>>(),
            Err(VersionedIpPrefixError::WrongVersion(
                [0x184f, 0x0139, 0x0084, 0, 0, 0, 0x56, 0x3456].into()
            ))
        );
    }

    #[test]
    fn test_strict_subnet_prefix_valid() {
        assert_eq!(
            "192.168.0.1/32".parse::<StrictSubnet<Ipv4Addr>>(),
            Ok(StrictSubnet { prefix: 32, address: [192, 168, 0, 1].into() })
        );
        assert_eq!(
            "192.168.0.0/24".parse::<StrictSubnet<Ipv4Addr>>(),
            Ok(StrictSubnet { prefix: 24, address: [192, 168, 0, 0].into() })
        );
    }

    #[test]
    fn test_strict_subnet_prefix_too_large() {
        assert_eq!(
            "192.168.0.0/33".parse::<StrictSubnet<Ipv4Addr>>(),
            Err(SubnetParseError::IpError(VersionedIpPrefixError::IpError(
                IpAddressWithPrefixParseError::BadPrefix(33, [192, 168, 0, 0].into())
            )))
        );
        assert_eq!(
            "ff:00:ff::/130".parse::<StrictSubnet<Ipv6Addr>>(),
            Err(SubnetParseError::IpError(VersionedIpPrefixError::IpError(
                IpAddressWithPrefixParseError::BadPrefix(130, "ff:00:ff::".parse().unwrap())
            )))
        );
    }

    #[test]
    fn test_strict_subnet_prefix_host_bits_set() {
        assert_eq!(
            "192.168.0.0/8".parse::<StrictSubnet<Ipv4Addr>>(),
            Err(SubnetParseError::HostBitsSet([192, 168, 0, 0].into(), 8))
        );
        assert_eq!(
            "ff:00:ff::/14".parse::<StrictSubnet<Ipv6Addr>>(),
            Err(SubnetParseError::HostBitsSet("ff:00:ff::".parse().unwrap(), 14))
        );
    }
}
