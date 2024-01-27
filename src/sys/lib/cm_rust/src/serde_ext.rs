// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(feature = "serde")]

use {
    crate::{CapabilityPath, DictionaryValue},
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_io as fio,
    serde::{
        de::{self, Visitor},
        Deserialize, Deserializer, Serialize, Serializer,
    },
    std::{fmt, str::FromStr},
};

/// Reflect fidl_fuchsia_sys2::StorageId for serialization/deserialization.
#[derive(Serialize, Deserialize)]
#[serde(remote = "fdecl::StorageId")]
pub enum StorageId {
    StaticInstanceId = 1,
    StaticInstanceIdOrMoniker = 2,
}

/// Custom deserialization for Option<fidl_fuchsia_io::Operations> bitflags.
pub fn deserialize_opt_fio_operations<'de, D>(
    deserializer: D,
) -> Result<Option<fio::Operations>, D::Error>
where
    D: Deserializer<'de>,
{
    deserializer.deserialize_option(OptionFio2OperationsVisitor)
}

/// Deserialization visitor pattern for for Option<fidl_fuchsia_io::Operations> bitflags.
struct OptionFio2OperationsVisitor;

impl<'de> Visitor<'de> for OptionFio2OperationsVisitor {
    type Value = Option<fio::Operations>;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "u64 bits of fio::Operations")
    }

    fn visit_some<D>(self, deserializer: D) -> Result<Self::Value, D::Error>
    where
        D: Deserializer<'de>,
    {
        Ok(Some(deserialize_fio_operations(deserializer)?))
    }

    fn visit_none<E>(self) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        Ok(None)
    }

    fn visit_unit<E>(self) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        Ok(None)
    }
}

/// Custom serialization for Option<fidl_fuchsia_io::Operations> bitflags.
pub fn serialize_opt_fio_operations<S>(
    operations: &Option<fio::Operations>,
    serializer: S,
) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    match operations {
        Some(operations) => serializer.serialize_some(&operations.bits()),
        None => serializer.serialize_none(),
    }
}

/// Custom deserialization for fidl_fuchsia_io::Operations bitflags.
pub fn deserialize_fio_operations<'de, D>(deserializer: D) -> Result<fio::Operations, D::Error>
where
    D: Deserializer<'de>,
{
    deserializer.deserialize_u64(Fio2OperationsVisitor)
}

/// Deserialization visitor pattern for for Option<fidl_fuchsia_io::Operations> bitflags.
struct Fio2OperationsVisitor;

impl<'de> Visitor<'de> for Fio2OperationsVisitor {
    type Value = fio::Operations;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "u64 bits of fio::Operations")
    }

    fn visit_u64<E>(self, bits: u64) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        fio::Operations::from_bits(bits)
            .ok_or_else(|| E::custom("Expected u64 bits of fio::Operations"))
    }
}

/// Custom serialization for fidl_fuchsia_io::Operations bitflags.
pub fn serialize_fio_operations<S>(
    operations: &fio::Operations,
    serializer: S,
) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    serializer.serialize_u64(operations.bits())
}

impl Serialize for CapabilityPath {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let path = if self.dirname == "/" {
            format!("/{}", self.basename)
        } else {
            format!("{}/{}", self.dirname, self.basename)
        };
        serializer.serialize_str(&path)
    }
}

struct CapabilityPathVisitor;

impl<'de> Visitor<'de> for CapabilityPathVisitor {
    type Value = CapabilityPath;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("A capability path")
    }

    fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        CapabilityPath::from_str(value)
            .map_err(|_| E::custom(format!("Expected capability path, but found \"{}\"", value)))
    }
}

impl<'de> Deserialize<'de> for CapabilityPath {
    fn deserialize<D>(deserializer: D) -> Result<CapabilityPath, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_str(CapabilityPathVisitor)
    }
}

impl Serialize for DictionaryValue {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            DictionaryValue::Str(s) => serializer.serialize_str(s),
            DictionaryValue::StrVec(v) => v.serialize(serializer),
            DictionaryValue::Null => serializer.serialize_none(),
        }
    }
}

struct DictionaryValueVisitor;

impl<'de> Visitor<'de> for DictionaryValueVisitor {
    type Value = DictionaryValue;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("A dictionary value")
    }

    fn visit_str<E>(self, s: &str) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        Ok(DictionaryValue::Str(s.to_owned()))
    }

    fn visit_string<E>(self, s: String) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        Ok(DictionaryValue::Str(s))
    }

    fn visit_seq<A>(self, mut seq: A) -> Result<Self::Value, A::Error>
    where
        A: de::SeqAccess<'de>,
    {
        let mut v: Vec<String> = Vec::new();
        while let Some(s) = seq.next_element()? {
            v.push(s)
        }
        Ok(DictionaryValue::StrVec(v))
    }

    fn visit_none<E>(self) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        Ok(DictionaryValue::Null)
    }
}

impl<'de> Deserialize<'de> for DictionaryValue {
    fn deserialize<D>(deserializer: D) -> Result<DictionaryValue, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_any(DictionaryValueVisitor)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{
            deserialize_fio_operations, deserialize_opt_fio_operations, serialize_fio_operations,
            serialize_opt_fio_operations,
        },
        crate::CapabilityPath,
        fidl_fuchsia_io as fio,
        serde_json::{self, Deserializer, Serializer},
        std::str::{from_utf8, FromStr},
    };

    #[test]
    fn capability_path_symmetry() {
        let cps_to_serialize = vec!["/path", "/multi/path"]
            .into_iter()
            .map(CapabilityPath::from_str)
            .collect::<Result<Vec<CapabilityPath>, _>>()
            .unwrap();
        for cp_to_serialize in cps_to_serialize.into_iter() {
            let cp_json_str = serde_json::to_string_pretty(&cp_to_serialize).unwrap();
            let cp_deserialized: CapabilityPath = serde_json::from_str(&cp_json_str).unwrap();
            assert_eq!(cp_to_serialize, cp_deserialized);
        }
    }

    #[test]
    fn test_deserialize_opt_fio_operations_some() {
        let connect_str = fio::Operations::CONNECT.bits().to_string();
        let mut deserializer = Deserializer::from_str(&connect_str);
        assert_eq!(
            deserialize_opt_fio_operations(&mut deserializer).unwrap(),
            Some(fio::Operations::CONNECT)
        );
    }

    #[test]
    fn test_deserialize_opt_fio_operations_none() {
        let null_str = "null";
        let mut deserializer = Deserializer::from_str(null_str);
        assert_eq!(deserialize_opt_fio_operations(&mut deserializer).unwrap(), None);
    }

    #[test]
    fn test_serialize_opt_fio_operations_some() {
        let some_ops: Option<fio::Operations> = Some(fio::Operations::CONNECT);
        let mut data = Vec::new();
        let mut serializer = Serializer::new(&mut data);
        serialize_opt_fio_operations(&some_ops, &mut serializer).unwrap();
        assert_eq!(from_utf8(&data).unwrap(), &fio::Operations::CONNECT.bits().to_string());
    }

    #[test]
    fn test_serialize_opt_fio_operations_none() {
        let none_ops: Option<fio::Operations> = None;
        let mut data = Vec::new();
        let mut serializer = Serializer::new(&mut data);
        serialize_opt_fio_operations(&none_ops, &mut serializer).unwrap();
        assert_eq!(from_utf8(&data).unwrap(), "null");
    }

    #[test]
    fn test_deserialize_fio_operations() {
        let connect_str = fio::Operations::CONNECT.bits().to_string();
        let mut deserializer = Deserializer::from_str(&connect_str);
        assert_eq!(
            deserialize_fio_operations(&mut deserializer).unwrap(),
            fio::Operations::CONNECT
        );
    }

    #[test]
    fn test_serialize_fio_operations() {
        let ops = fio::Operations::CONNECT;
        let mut data = Vec::new();
        let mut serializer = Serializer::new(&mut data);
        serialize_fio_operations(&ops, &mut serializer).unwrap();
        assert_eq!(from_utf8(&data).unwrap(), &fio::Operations::CONNECT.bits().to_string());
    }
}
