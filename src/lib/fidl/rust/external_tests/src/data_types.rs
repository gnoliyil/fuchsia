// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This file tests the public APIs of FIDL data types.

use assert_matches::assert_matches;
use fidl_fidl_rust_test_external::{
    FlexibleAnimal, FlexibleButtons, FlexibleEmptyEnum, FlexibleResourceThing, FlexibleValueThing,
    ResourceRecord, StrictAnimal, StrictButtons, StrictResourceThing, StrictValueThing,
    ValueRecord,
};

#[test]
fn strict_bits() {
    assert_eq!(StrictButtons::from_bits(0b001), Some(StrictButtons::PLAY));
    assert_eq!(StrictButtons::from_bits(0b1100), None);
    assert_eq!(StrictButtons::from_bits_truncate(0b1010), StrictButtons::PAUSE);
    assert_eq!(StrictButtons::from_bits_truncate(u32::MAX), StrictButtons::all());
    assert_eq!(StrictButtons::STOP.bits(), 0b100);

    // You can use the flexible methods on strict types, but it produces a
    // deprecation warning.
    #[allow(deprecated)]
    let has_unknown_bits = StrictButtons::PLAY.has_unknown_bits();
    assert_eq!(has_unknown_bits, false);
    #[allow(deprecated)]
    let get_unknown_bits = StrictButtons::PLAY.get_unknown_bits();
    assert_eq!(get_unknown_bits, 0);
}

#[test]
fn flexible_bits() {
    assert_eq!(FlexibleButtons::from_bits(0b001), Some(FlexibleButtons::PLAY));
    assert_eq!(FlexibleButtons::from_bits(0b1100), None);
    assert_eq!(
        FlexibleButtons::from_bits_allow_unknown(0b1010) & FlexibleButtons::all(),
        FlexibleButtons::PAUSE
    );
    assert_eq!(FlexibleButtons::STOP.bits(), 0b100);
    assert_eq!(FlexibleButtons::from_bits_allow_unknown(0b1010).bits(), 0b1010);
    assert_eq!(FlexibleButtons::from_bits_allow_unknown(u32::MAX).bits(), u32::MAX);

    assert_eq!(FlexibleButtons::PLAY.has_unknown_bits(), false);
    assert_eq!(FlexibleButtons::PLAY.get_unknown_bits(), 0);
    assert_eq!(FlexibleButtons::from_bits_allow_unknown(0b1010).has_unknown_bits(), true);
    assert_eq!(FlexibleButtons::from_bits_allow_unknown(0b1010).get_unknown_bits(), 0b1000);
    assert_eq!(FlexibleButtons::from_bits_allow_unknown(u32::MAX).has_unknown_bits(), true);
    assert_eq!(
        FlexibleButtons::from_bits_allow_unknown(u32::MAX).get_unknown_bits(),
        u32::MAX & !0b111
    );

    // Negation ANDs with the mask.
    assert_ne!(
        FlexibleButtons::from_bits_allow_unknown(0b101000101),
        FlexibleButtons::PLAY | FlexibleButtons::STOP
    );
    assert_eq!(!FlexibleButtons::from_bits_allow_unknown(0b101000101), FlexibleButtons::PAUSE);
    assert_eq!(
        !!FlexibleButtons::from_bits_allow_unknown(0b101000101),
        FlexibleButtons::PLAY | FlexibleButtons::STOP
    );
}

#[test]
fn strict_enum() {
    assert_eq!(StrictAnimal::from_primitive(0), Some(StrictAnimal::Dog));
    assert_eq!(StrictAnimal::from_primitive(3), None);
    assert_eq!(StrictAnimal::Cat.into_primitive(), 1);

    // You can use the flexible methods on strict types, but it produces a
    // deprecation warning.
    #[allow(deprecated)]
    let is_unknown = StrictAnimal::Cat.is_unknown();
    assert_eq!(is_unknown, false);
}

#[test]
fn flexible_enum() {
    assert_eq!(FlexibleAnimal::from_primitive(0), Some(FlexibleAnimal::Dog));
    assert_eq!(FlexibleAnimal::from_primitive(3), None);
    assert_eq!(FlexibleAnimal::from_primitive_allow_unknown(0), FlexibleAnimal::Dog);

    #[allow(deprecated)] // allow referencing __Unknown
    let unknown3 = FlexibleAnimal::__Unknown(3);
    assert_eq!(FlexibleAnimal::from_primitive_allow_unknown(3), unknown3);

    assert_eq!(FlexibleAnimal::Cat.into_primitive(), 1);
    assert_eq!(FlexibleAnimal::from_primitive_allow_unknown(3).into_primitive(), 3);
    assert_eq!(FlexibleAnimal::unknown().into_primitive(), i32::MAX);

    assert_eq!(FlexibleAnimal::Cat.is_unknown(), false);
    assert_eq!(FlexibleAnimal::from_primitive_allow_unknown(3).is_unknown(), true);
    assert_eq!(FlexibleAnimal::unknown().is_unknown(), true);
}

#[test]
fn flexible_empty_enum() {
    assert_eq!(FlexibleEmptyEnum::from_primitive(3), None);

    #[allow(deprecated)] // allow referencing __Unknown
    let unknown3 = FlexibleEmptyEnum::__Unknown(3);
    assert_eq!(FlexibleEmptyEnum::from_primitive_allow_unknown(3), unknown3);

    assert_eq!(FlexibleEmptyEnum::unknown().into_primitive(), i32::MAX);

    assert_eq!(FlexibleEmptyEnum::from_primitive_allow_unknown(3).is_unknown(), true);
    assert_eq!(FlexibleEmptyEnum::unknown().is_unknown(), true);
}

#[test]
fn strict_value_union() {
    // You can use the flexible methods on strict types, but it produces a
    // deprecation warning.
    #[allow(deprecated)]
    let is_unknown = StrictValueThing::Number(42).is_unknown();
    assert_eq!(is_unknown, false);
}

#[test]
fn flexible_value_union() {
    assert_eq!(FlexibleValueThing::Number(42).is_unknown(), false);
    assert_eq!(FlexibleValueThing::Name("hello".to_owned()).is_unknown(), false);
    assert_eq!(FlexibleValueThing::unknown_variant_for_testing().is_unknown(), true);
}

#[test]
fn strict_resource_union() {
    // You can use the flexible methods on strict types, but it produces a
    // deprecation warning.
    #[allow(deprecated)]
    let is_unknown = StrictResourceThing::Number(42).is_unknown();
    assert_eq!(is_unknown, false);
}

#[test]
fn flexible_resource_union() {
    assert_eq!(FlexibleResourceThing::Number(42).is_unknown(), false);
    assert_eq!(FlexibleResourceThing::Name("hello".to_owned()).is_unknown(), false);
    assert_eq!(FlexibleResourceThing::unknown_variant_for_testing().is_unknown(), true);
}

#[test]
fn value_table() {
    assert_matches!(ValueRecord::EMPTY, ValueRecord { name: None, age: None, .. });

    let table = ValueRecord { age: Some(30), ..ValueRecord::EMPTY };
    assert_eq!(table.name, None);
    assert_eq!(table.age, Some(30));

    let ValueRecord { name, .. } = table;
    assert_eq!(name, None);
    let ValueRecord { age, .. } = table;
    assert_eq!(age, Some(30));
}

#[test]
fn resource_table() {
    assert_matches!(ResourceRecord::EMPTY, ResourceRecord { name: None, age: None, .. });

    let table = ResourceRecord { age: Some(30), ..ResourceRecord::EMPTY };
    assert_eq!(table.name, None);
    assert_eq!(table.age, Some(30));

    let ResourceRecord { name, .. } = table;
    assert_eq!(name, None);
    let ResourceRecord { age, .. } = table;
    assert_eq!(age, Some(30));
}
