// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow,
    clonable_error::ClonableError,
    component_id_index, fidl,
    fidl::unpersist,
    fidl_fuchsia_component_internal as fcomponent_internal,
    moniker::{AbsoluteMoniker, MonikerError},
    std::{
        collections::{HashMap, HashSet},
        path::PathBuf,
        str::FromStr,
    },
    thiserror::Error,
};

#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

pub const INSTANCE_ID_LEN: usize = 64;

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct ComponentInstanceId([u8; INSTANCE_ID_LEN]);

#[derive(Debug)]
pub enum ComponentInstanceIdParseError {
    BadLength,
}

impl std::str::FromStr for ComponentInstanceId {
    type Err = ComponentInstanceIdParseError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(ComponentInstanceId(
            s.as_bytes().try_into().map_err(|_| ComponentInstanceIdParseError::BadLength)?,
        ))
    }
}

impl std::fmt::Display for ComponentInstanceId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match String::from_utf8(self.0.iter().map(|u| *u).collect()) {
            Ok(s) => write!(f, "{}", s),
            Err(_) => {
                write!(f, "string conversion error, bytes: ")?;
                for b in &self.0 {
                    write!(f, "{}", b)?;
                }
                Ok(())
            }
        }
    }
}

impl Into<PathBuf> for &ComponentInstanceId {
    fn into(self) -> PathBuf {
        let copy: Vec<u8> = (*self.0.iter().map(|u| *u).collect::<Vec<u8>>()).to_vec();
        String::from_utf8(copy).unwrap().into()
    }
}

impl Into<PathBuf> for ComponentInstanceId {
    fn into(self) -> PathBuf {
        (&self).into()
    }
}

#[cfg_attr(feature = "serde", derive(Deserialize, Serialize), serde(rename_all = "snake_case"))]
#[derive(Debug, Clone, Error)]
pub enum ComponentIdIndexError {
    // The capability routing static analyzer does not report this error subtype as part of a
    // routing verification result, so we don't need to serialize it.
    #[cfg_attr(feature = "serde", serde(skip))]
    #[error("could not read index file {}", .path)]
    IndexUnreadable {
        #[source]
        err: ClonableError,
        path: String,
    },
    #[error("Index error")]
    IndexError(#[from] component_id_index::IndexError),
    #[error("invalid moniker")]
    MonikerError(#[from] MonikerError),
    #[error("id invalid")]
    InvalidId,
}

impl ComponentIdIndexError {
    pub fn index_unreadable(
        index_file_path: impl Into<String>,
        err: impl Into<anyhow::Error>,
    ) -> Self {
        ComponentIdIndexError::IndexUnreadable {
            path: index_file_path.into(),
            err: err.into().into(),
        }
    }
}

// Custom implementation of PartialEq in which two ComponentIdIndexError::IndexUnreadable errors are
// never equal.
impl PartialEq for ComponentIdIndexError {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::IndexError(self_err), Self::IndexError(other_err)) => self_err.eq(other_err),
            (Self::MonikerError(self_err), Self::MonikerError(other_err)) => self_err.eq(other_err),
            (Self::IndexUnreadable { .. }, Self::IndexUnreadable { .. }) => false,
            (Self::InvalidId, Self::InvalidId) => true,
            _ => false,
        }
    }
}

/// ComponentIdIndex parses a given index and provides methods to look up instance IDs.
#[derive(Debug, Default)]
pub struct ComponentIdIndex {
    /// Map of a moniker from the index to its instance ID.
    ///
    /// The moniker does not contain instances, i.e. all of the ChildMonikers in the
    /// path have the (moniker, not index) instance ID set to zero.
    moniker_to_instance_id: HashMap<AbsoluteMoniker, ComponentInstanceId>,

    /// Stores all instance IDs from the index.
    /// This is used by StorageAdmin for methods that operate directly on instance IDs.
    ///
    /// This set will currently contain all storage IDs, even of components registered with appmgr
    /// instead of component_manager. This is desired because it allows the StorageAdmin protocol
    /// to handle all storage on the system.
    all_instance_ids: HashSet<ComponentInstanceId>,
}

impl ComponentIdIndex {
    pub fn new(index_file_path: &str) -> Result<Self, ComponentIdIndexError> {
        let raw_content = std::fs::read(index_file_path)
            .map_err(|err| ComponentIdIndexError::index_unreadable(index_file_path, err))?;

        let fidl_index = unpersist::<fcomponent_internal::ComponentIdIndex>(&raw_content)
            .map_err(|err| ComponentIdIndexError::index_unreadable(index_file_path, err))?;

        let index = component_id_index::Index::from_fidl(fidl_index)?;
        Self::new_from_index(index)
    }

    pub fn new_from_index(index: component_id_index::Index) -> Result<Self, ComponentIdIndexError> {
        let mut moniker_to_instance_id = HashMap::<AbsoluteMoniker, ComponentInstanceId>::new();
        let mut all_instance_ids = HashSet::<ComponentInstanceId>::new();
        for entry in index.instances {
            let instance_id = entry
                .instance_id
                .as_ref()
                .ok_or_else(|| {
                    ComponentIdIndexError::IndexError(
                        component_id_index::IndexError::ValidationError(
                            component_id_index::ValidationError::MissingInstanceIds {
                                entries: vec![entry.clone()],
                            },
                        ),
                    )
                })?
                .clone();
            all_instance_ids.insert(
                ComponentInstanceId::from_str(&instance_id)
                    .map_err(|_| ComponentIdIndexError::InvalidId)?,
            );
            if let Some(absolute_moniker) = entry.moniker {
                moniker_to_instance_id.insert(
                    absolute_moniker,
                    ComponentInstanceId::from_str(&instance_id)
                        .map_err(|_| ComponentIdIndexError::InvalidId)?,
                );
            }
        }
        Ok(Self { moniker_to_instance_id, all_instance_ids })
    }

    pub fn look_up_moniker(&self, moniker: &AbsoluteMoniker) -> Option<&ComponentInstanceId> {
        self.moniker_to_instance_id.get(&moniker)
    }

    pub fn look_up_instance_id(&self, id: &ComponentInstanceId) -> bool {
        self.all_instance_ids.contains(id)
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use moniker::{AbsoluteMonikerBase, ChildMoniker};
    use routing_test_helpers::component_id_index::make_index_file;

    #[fuchsia::test]
    async fn look_up_moniker_no_exists() {
        let index_file = make_index_file(component_id_index::Index::default()).unwrap();
        let index = ComponentIdIndex::new(index_file.path().to_str().unwrap()).unwrap();
        assert!(index.look_up_moniker(&AbsoluteMoniker::parse_str("/a/b/c").unwrap()).is_none());
    }

    #[fuchsia::test]
    async fn look_up_moniker_exists() {
        let iid = "0".repeat(64);
        let index_file = make_index_file(component_id_index::Index {
            instances: vec![component_id_index::InstanceIdEntry {
                instance_id: Some(iid.clone()),
                appmgr_moniker: None,
                moniker: Some(AbsoluteMoniker::parse_str("/a/b/c").unwrap()),
            }],
            ..component_id_index::Index::default()
        })
        .unwrap();
        let index = ComponentIdIndex::new(index_file.path().to_str().unwrap()).unwrap();
        assert_eq!(
            Some(iid),
            index
                .look_up_moniker(&AbsoluteMoniker::parse_str("/a/b/c").unwrap())
                .map(|id| id.to_string())
        );
    }

    #[fuchsia::test]
    async fn look_up_moniker_with_instances_exists() {
        let iid = "0".repeat(64);
        let index_file = make_index_file(component_id_index::Index {
            instances: vec![component_id_index::InstanceIdEntry {
                instance_id: Some(iid.clone()),
                appmgr_moniker: None,
                moniker: Some(AbsoluteMoniker::parse_str("/a/coll:name").unwrap()),
            }],
            ..component_id_index::Index::default()
        })
        .unwrap();
        let index = ComponentIdIndex::new(index_file.path().to_str().unwrap()).unwrap();
        assert_eq!(
            Some(iid),
            index
                .look_up_moniker(&AbsoluteMoniker::new(vec![
                    ChildMoniker::try_new("a", None).unwrap(),
                    ChildMoniker::try_new("name", Some("coll")).unwrap(),
                ]))
                .map(|id| id.to_string())
        );
    }

    #[fuchsia::test]
    fn new_from_index() {
        let iid = "0".repeat(64);
        let inner_index = component_id_index::Index {
            instances: vec![component_id_index::InstanceIdEntry {
                instance_id: Some(iid.clone()),
                appmgr_moniker: None,
                moniker: Some(AbsoluteMoniker::parse_str("/a/b/c").unwrap()),
            }],
            ..component_id_index::Index::default()
        };
        let index = ComponentIdIndex::new_from_index(inner_index)
            .expect("failed to create component id index from inner index");
        assert_eq!(
            Some(iid),
            index
                .look_up_moniker(&AbsoluteMoniker::parse_str("/a/b/c").unwrap())
                .map(|id| id.to_string())
        );
    }

    #[fuchsia::test]
    async fn index_unreadable() {
        let result = ComponentIdIndex::new("/this/path/doesnt/exist");
        assert!(matches!(result, Err(ComponentIdIndexError::IndexUnreadable { path: _, err: _ })));
    }
}
