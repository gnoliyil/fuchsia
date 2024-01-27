// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    cm_types::{LongName, Name},
    core::cmp::{Ord, Ordering},
    moniker::{ChildMoniker, ChildMonikerBase, MonikerError},
    std::fmt,
};

#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

/// An instanced child moniker locally identifies a child component instance using the name assigned by
/// its parent and its collection (if present). It is a building block for more complex monikers.
///
/// Display notation: "[collection:]name:instance_id".
#[cfg_attr(feature = "serde", derive(Deserialize, Serialize))]
#[derive(Eq, PartialEq, Clone, Hash)]
pub struct InstancedChildMoniker {
    name: LongName,
    collection: Option<Name>,
    instance: IncarnationId,
}

pub type IncarnationId = u32;

impl ChildMonikerBase for InstancedChildMoniker {
    /// Parses an `ChildMoniker` from a string.
    ///
    /// Input strings should be of the format `(<collection>:)?<name>:<instance_id>`, e.g. `foo:42`
    /// or `coll:foo:42`.
    fn parse<T: AsRef<str>>(rep: T) -> Result<Self, MonikerError> {
        let rep = rep.as_ref();
        let parts: Vec<&str> = rep.split(":").collect();
        // An instanced moniker is either just a name (static instance), or
        // collection:name:instance_id.
        let (coll, name, instance) = match parts.len() {
            2 => {
                let instance = parts[1]
                    .parse::<IncarnationId>()
                    .map_err(|_| MonikerError::invalid_moniker(rep))?;
                (None, parts[0], instance)
            }
            3 => {
                let instance = parts[2]
                    .parse::<IncarnationId>()
                    .map_err(|_| MonikerError::invalid_moniker(rep))?;
                (Some(parts[0]), parts[1], instance)
            }
            _ => return Err(MonikerError::invalid_moniker(rep)),
        };
        Self::try_new(name, coll, instance)
    }

    fn name(&self) -> &str {
        self.name.as_str()
    }

    fn collection(&self) -> Option<&str> {
        self.collection.as_ref().map(|c| c.as_str())
    }
}

impl InstancedChildMoniker {
    pub fn try_new<S>(
        name: S,
        collection: Option<S>,
        instance: IncarnationId,
    ) -> Result<Self, MonikerError>
    where
        S: AsRef<str> + Into<String>,
    {
        let name = LongName::new(name)?;
        let collection = match collection {
            Some(coll) => {
                let coll_name = Name::new(coll)?;
                Some(coll_name)
            }
            None => None,
        };
        Ok(Self { name, collection, instance })
    }

    /// Returns a moniker for a static child.
    ///
    /// The returned value will have no `collection`, and will have an `instance_id` of 0.
    pub fn static_child(name: &str) -> Result<Self, MonikerError> {
        Self::try_new(name, None, 0)
    }

    /// Converts this child moniker into an instanced moniker.
    pub fn from_child_moniker(m: &ChildMoniker, instance: IncarnationId) -> Self {
        Self::try_new(m.name(), m.collection(), instance)
            .expect("child moniker is guaranteed to be valid")
    }

    /// Convert an InstancedChildMoniker to an allocated ChildMoniker
    /// without an InstanceId
    pub fn without_instance_id(&self) -> ChildMoniker {
        ChildMoniker::try_new(self.name(), self.collection())
            .expect("moniker is guaranteed to be valid")
    }

    pub fn instance(&self) -> IncarnationId {
        self.instance
    }

    pub fn format(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(coll) = &self.collection {
            write!(f, "{}:{}:{}", coll, self.name, self.instance)
        } else {
            write!(f, "{}:{}", self.name, self.instance)
        }
    }
}

impl TryFrom<&str> for InstancedChildMoniker {
    type Error = MonikerError;

    fn try_from(rep: &str) -> Result<Self, MonikerError> {
        InstancedChildMoniker::parse(rep)
    }
}

impl Ord for InstancedChildMoniker {
    fn cmp(&self, other: &Self) -> Ordering {
        (&self.collection, &self.name, &self.instance).cmp(&(
            &other.collection,
            &other.name,
            &other.instance,
        ))
    }
}

impl PartialOrd for InstancedChildMoniker {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl fmt::Display for InstancedChildMoniker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.format(f)
    }
}

impl fmt::Debug for InstancedChildMoniker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.format(f)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        cm_types::{MAX_LONG_NAME_LENGTH, MAX_NAME_LENGTH},
    };

    #[test]
    fn instanced_child_monikers() {
        let m = InstancedChildMoniker::try_new("test", None, 42).unwrap();
        assert_eq!("test", m.name());
        assert_eq!(None, m.collection());
        assert_eq!(42, m.instance());
        assert_eq!("test:42", format!("{}", m));
        assert_eq!(m, InstancedChildMoniker::try_from("test:42").unwrap());
        assert_eq!("test", m.without_instance_id().to_string());
        assert_eq!(m, InstancedChildMoniker::from_child_moniker(&"test".try_into().unwrap(), 42));

        let m = InstancedChildMoniker::try_new("test", Some("coll"), 42).unwrap();
        assert_eq!("test", m.name());
        assert_eq!(Some("coll"), m.collection());
        assert_eq!(42, m.instance());
        assert_eq!("coll:test:42", format!("{}", m));
        assert_eq!(m, InstancedChildMoniker::try_from("coll:test:42").unwrap());
        assert_eq!("coll:test", m.without_instance_id().to_string());
        assert_eq!(
            m,
            InstancedChildMoniker::from_child_moniker(&"coll:test".try_into().unwrap(), 42)
        );

        let max_coll_length_part = "f".repeat(MAX_NAME_LENGTH);
        let max_name_length_part: LongName = "f".repeat(MAX_LONG_NAME_LENGTH).parse().unwrap();
        let m = InstancedChildMoniker::parse(format!(
            "{}:{}:42",
            max_coll_length_part, max_name_length_part
        ))
        .expect("valid moniker");
        assert_eq!(max_name_length_part, m.name());
        assert_eq!(Some(max_coll_length_part.as_str()), m.collection());
        assert_eq!(42, m.instance());

        assert!(InstancedChildMoniker::parse("").is_err(), "cannot be empty");
        assert!(InstancedChildMoniker::parse(":").is_err(), "cannot be empty with colon");
        assert!(InstancedChildMoniker::parse("::").is_err(), "cannot be empty with double colon");
        assert!(
            InstancedChildMoniker::parse("f:").is_err(),
            "second part cannot be empty with colon"
        );
        assert!(
            InstancedChildMoniker::parse(":1").is_err(),
            "first part cannot be empty with colon"
        );
        assert!(
            InstancedChildMoniker::parse("f:f:").is_err(),
            "third part cannot be empty with colon"
        );
        assert!(
            InstancedChildMoniker::parse("f::1").is_err(),
            "second part cannot be empty with colon"
        );
        assert!(
            InstancedChildMoniker::parse(":f:1").is_err(),
            "first part cannot be empty with colon"
        );
        assert!(
            InstancedChildMoniker::parse("f:f:1:1").is_err(),
            "more than three colons not allowed"
        );
        assert!(InstancedChildMoniker::parse("f:f").is_err(), "second part must be int");
        assert!(InstancedChildMoniker::parse("f:f:f").is_err(), "third part must be int");
        assert!(InstancedChildMoniker::parse("@:1").is_err(), "invalid character in name");
        assert!(InstancedChildMoniker::parse("@:f:1").is_err(), "invalid character in collection");
        assert!(
            InstancedChildMoniker::parse("f:@:1").is_err(),
            "invalid character in name with collection"
        );
        assert!(
            InstancedChildMoniker::parse(&format!("f:{}", "x".repeat(MAX_LONG_NAME_LENGTH + 1)))
                .is_err(),
            "name too long"
        );
        assert!(
            InstancedChildMoniker::parse(&format!("{}:x", "f".repeat(MAX_NAME_LENGTH + 1)))
                .is_err(),
            "collection too long"
        );
    }

    #[test]
    fn instanced_child_moniker_compare() {
        let a = InstancedChildMoniker::try_new("a", None, 1).unwrap();
        let a2 = InstancedChildMoniker::try_new("a", None, 2).unwrap();
        let aa = InstancedChildMoniker::try_new("a", Some("a"), 1).unwrap();
        let aa2 = InstancedChildMoniker::try_new("a", Some("a"), 2).unwrap();
        let ab = InstancedChildMoniker::try_new("a", Some("b"), 1).unwrap();
        let ba = InstancedChildMoniker::try_new("b", Some("a"), 1).unwrap();
        let bb = InstancedChildMoniker::try_new("b", Some("b"), 1).unwrap();
        let aa_same = InstancedChildMoniker::try_new("a", Some("a"), 1).unwrap();

        assert_eq!(Ordering::Less, a.cmp(&a2));
        assert_eq!(Ordering::Greater, a2.cmp(&a));
        assert_eq!(Ordering::Less, a2.cmp(&aa));
        assert_eq!(Ordering::Greater, aa.cmp(&a2));
        assert_eq!(Ordering::Less, a.cmp(&ab));
        assert_eq!(Ordering::Greater, ab.cmp(&a));
        assert_eq!(Ordering::Less, a.cmp(&ba));
        assert_eq!(Ordering::Greater, ba.cmp(&a));
        assert_eq!(Ordering::Less, a.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&a));

        assert_eq!(Ordering::Less, aa.cmp(&aa2));
        assert_eq!(Ordering::Greater, aa2.cmp(&aa));
        assert_eq!(Ordering::Less, aa.cmp(&ab));
        assert_eq!(Ordering::Greater, ab.cmp(&aa));
        assert_eq!(Ordering::Less, aa.cmp(&ba));
        assert_eq!(Ordering::Greater, ba.cmp(&aa));
        assert_eq!(Ordering::Less, aa.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&aa));
        assert_eq!(Ordering::Equal, aa.cmp(&aa_same));
        assert_eq!(Ordering::Equal, aa_same.cmp(&aa));

        assert_eq!(Ordering::Greater, ab.cmp(&ba));
        assert_eq!(Ordering::Less, ba.cmp(&ab));
        assert_eq!(Ordering::Less, ab.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&ab));

        assert_eq!(Ordering::Less, ba.cmp(&bb));
        assert_eq!(Ordering::Greater, bb.cmp(&ba));
    }
}
