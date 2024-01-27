// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
pub use macros::*;

use {
    std::collections::{BTreeMap, BTreeSet, HashMap, HashSet},
    std::ops::Range,
    std::option::Option,
    std::string::String,
};

/// A TypeFingerprint is able to return a string that represents the layout of a type.
/// It is intended to capture any structure that will affect serialiation via Serde.
pub trait TypeFingerprint {
    fn fingerprint() -> String;
}

// Basic types just return themselves as a string.
macro_rules! impl_fprint_simple {
    ($($($type: ident)::*),*) => {
        $(
            impl TypeFingerprint for $($type)::* {
                fn fingerprint() -> String { stringify!($($type)::*).to_string() }
            }
        )*
    };
}

impl_fprint_simple!(
    bool, i8, i16, i32, i64, i128, isize, u8, u16, u32, u64, u128, usize, f32, f64, str, String
);

// Arrays return their inner type and length (up to a maximum).
macro_rules! impl_fprint_array {
    ($n: literal) => {
        impl<T: TypeFingerprint> TypeFingerprint for [T; $n] {
            fn fingerprint() -> String {
                "[".to_string() + &T::fingerprint() + ";" + stringify!($n) + "]"
            }
        }
    };
}

impl_fprint_array!(0);
impl_fprint_array!(1);
impl_fprint_array!(2);
impl_fprint_array!(3);
impl_fprint_array!(4);
impl_fprint_array!(5);
impl_fprint_array!(6);
impl_fprint_array!(7);
impl_fprint_array!(8);
impl_fprint_array!(9);
impl_fprint_array!(10);
impl_fprint_array!(11);
impl_fprint_array!(12);
impl_fprint_array!(13);
impl_fprint_array!(14);
impl_fprint_array!(15);
impl_fprint_array!(16);
impl_fprint_array!(17);
impl_fprint_array!(18);
impl_fprint_array!(19);
impl_fprint_array!(20);
impl_fprint_array!(21);
impl_fprint_array!(22);
impl_fprint_array!(23);
impl_fprint_array!(24);
impl_fprint_array!(25);
impl_fprint_array!(26);
impl_fprint_array!(27);
impl_fprint_array!(28);
impl_fprint_array!(29);
impl_fprint_array!(30);
impl_fprint_array!(31);
impl_fprint_array!(32);

macro_rules! impl_fprint_one_generic {
    ($($($type: ident)::*),*) => {
        $(
            impl<T: TypeFingerprint> TypeFingerprint for $($type)::*<T> {
                fn fingerprint() -> String {
                    "".to_owned() + stringify!($($type)::*) + "<" + &T::fingerprint() + ">"
                }
            }
        )*
    };
}

impl_fprint_one_generic!(BTreeSet, HashSet, Range, Option, Vec);

macro_rules! impl_fprint_two_generic {
    ($($($type: ident)::*),*) => {
        $(
            impl<A: TypeFingerprint, B: TypeFingerprint> TypeFingerprint for $($type)::*<A,B> {
                fn fingerprint() -> String {
                    "".to_owned() + stringify!($($type)::*) +
                        "<" + &A::fingerprint() + "," + &B::fingerprint() + ">"
                }
            }
        )*
    };
}

impl_fprint_two_generic!(BTreeMap, HashMap);

macro_rules! impl_fprint_tuple {
    (($($type: ident,)*)) => {
        impl<$($type: TypeFingerprint),*> TypeFingerprint for ($($type,)*) {
            fn fingerprint() -> String {
                "(".to_owned() + $(&$type::fingerprint() + "," +)* ")"
            }
        }
    };
}

impl_fprint_tuple!(());
impl_fprint_tuple!((A,));
impl_fprint_tuple!((A, B,));
impl_fprint_tuple!((A, B, C,));
impl_fprint_tuple!((A, B, C, D,));
impl_fprint_tuple!((A, B, C, D, E,));
impl_fprint_tuple!((A, B, C, D, E, F,));
impl_fprint_tuple!((A, B, C, D, E, F, G,));
impl_fprint_tuple!((A, B, C, D, E, F, G, H,));
impl_fprint_tuple!((A, B, C, D, E, F, G, H, I,));

impl<'a, T: TypeFingerprint + ?Sized> TypeFingerprint for &'a T {
    fn fingerprint() -> String {
        "&".to_owned() + &T::fingerprint()
    }
}

impl<'a, T: TypeFingerprint + ?Sized> TypeFingerprint for &'a mut T {
    fn fingerprint() -> String {
        "&mut".to_owned() + &T::fingerprint()
    }
}

impl<T: TypeFingerprint> TypeFingerprint for [T] {
    fn fingerprint() -> String {
        "[".to_owned() + &T::fingerprint() + "]"
    }
}

impl<T: TypeFingerprint + ?Sized> TypeFingerprint for Box<T> {
    fn fingerprint() -> String {
        "Box<".to_owned() + &T::fingerprint() + ">"
    }
}

#[cfg(test)]
mod tests {
    use crate::*;
    struct Foo {}
    impl TypeFingerprint for Foo {
        fn fingerprint() -> String {
            "Foo".to_string()
        }
    }

    #[derive(TypeFingerprint)]
    struct Bar(Foo);

    #[derive(TypeFingerprint)]
    struct Baz {
        foo: Foo,
        bar: Bar,
        bizz: u64, //std::collections::BTreeMap<String, (u64, u32, u16, u8, usize)>,
    }

    #[derive(TypeFingerprint)]
    enum Buzz {
        A(Foo),
        B(Bar),
        C(Baz),
    }

    #[test]
    fn test_simple() {
        assert_eq!(u32::fingerprint(), "u32");
    }

    #[test]
    fn test_array() {
        assert_eq!(<[u32; 3]>::fingerprint(), "[u32;3]");
    }

    #[test]
    fn test_vec() {
        assert_eq!(Vec::<[u32; 3]>::fingerprint(), "std :: vec :: Vec<[u32;3]>");
    }

    #[test]
    fn test_hashmap_and_tuple() {
        assert_eq!(
            std::collections::HashMap::<[u32; 3], (bool, [u64; 8])>::fingerprint(),
            "std :: collections :: HashMap<[u32;3],(bool,[u64;8],)>"
        );
    }

    #[test]
    fn test_hand_implemented() {
        assert_eq!(Foo::fingerprint(), "Foo");
    }

    #[test]
    fn test_struct() {
        assert_eq!(Bar::fingerprint(), "struct Bar {Foo,}");
        assert_eq!(Baz::fingerprint(), "struct Baz {foo:Foo,bar:struct Bar {Foo,},bizz:u64,}");
    }

    #[test]
    fn test_enum() {
        assert_eq!(Buzz::fingerprint(), "enum Buzz { A(Foo,),B(struct Bar {Foo,},),C(struct Baz {foo:Foo,bar:struct Bar {Foo,},bizz:u64,},),}");
    }
}
