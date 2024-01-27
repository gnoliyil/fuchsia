// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//!

use {
    crate::reader::{
        error::ReaderError, DiagnosticsHierarchy, MissingValueReason, PartialNodeHierarchy,
        ReadableTree, Snapshot,
    },
    fuchsia_async::{DurationExt, TimeoutExt},
    futures::{
        future::{self, BoxFuture},
        prelude::*,
    },
    inspect_format::LinkNodeDisposition,
    std::{
        collections::BTreeMap,
        convert::{TryFrom, TryInto},
        time::Duration,
    },
};

/// Contains the snapshot of the hierarchy and snapshots of all the lazy nodes in the hierarchy.
#[derive(Debug)]
pub struct SnapshotTree {
    snapshot: Snapshot,
    children: SnapshotTreeMap,
}

impl SnapshotTree {
    /// Loads a snapshot tree from the given inspect tree.
    #[cfg(target_os = "fuchsia")]
    pub async fn try_from(
        tree: &fidl_fuchsia_inspect::TreeProxy,
    ) -> Result<SnapshotTree, ReaderError> {
        load_snapshot_tree(tree, None).await
    }

    pub async fn try_from_with_timeout<T: ReadableTree + Send + Sync>(
        tree: &T,
        lazy_child_timeout: Duration,
    ) -> Result<SnapshotTree, ReaderError> {
        load_snapshot_tree(tree, Some(lazy_child_timeout)).await
    }
}

type SnapshotTreeMap = BTreeMap<String, Result<SnapshotTree, ReaderError>>;

impl TryInto<DiagnosticsHierarchy> for SnapshotTree {
    type Error = ReaderError;

    fn try_into(mut self) -> Result<DiagnosticsHierarchy, Self::Error> {
        let partial = PartialNodeHierarchy::try_from(self.snapshot)?;
        Ok(expand(partial, &mut self.children))
    }
}

fn expand(
    partial: PartialNodeHierarchy,
    snapshot_children: &mut SnapshotTreeMap,
) -> DiagnosticsHierarchy {
    // TODO(miguelfrde): remove recursion or limit depth.
    let children =
        partial.children.into_iter().map(|child| expand(child, snapshot_children)).collect();
    let mut hierarchy = DiagnosticsHierarchy::new(partial.name, partial.properties, children);
    for link_value in partial.links {
        let result = snapshot_children.remove(&link_value.content);
        if result.is_none() {
            hierarchy.add_missing(MissingValueReason::LinkNotFound, link_value.name);
            continue;
        }
        // TODO(miguelfrde): remove recursion or limit depth.
        let result: Result<DiagnosticsHierarchy, ReaderError> =
            result.unwrap().and_then(|snapshot_tree| snapshot_tree.try_into());
        match result {
            Err(ReaderError::TreeTimedOut) => {
                hierarchy.add_missing(MissingValueReason::Timeout, link_value.name);
            }
            Err(_) => {
                hierarchy.add_missing(MissingValueReason::LinkParseFailure, link_value.name);
            }
            Ok(mut child_hierarchy) => match link_value.disposition {
                LinkNodeDisposition::Child => {
                    child_hierarchy.name = link_value.name;
                    hierarchy.children.push(child_hierarchy);
                }
                LinkNodeDisposition::Inline => {
                    hierarchy.children.extend(child_hierarchy.children.into_iter());
                    hierarchy.properties.extend(child_hierarchy.properties.into_iter());
                    hierarchy.missing.extend(child_hierarchy.missing.into_iter());
                }
            },
        }
    }
    hierarchy
}

/// Reads the given `ReadableTree` into a DiagnosticsHierarchy.
/// This reads versions 1 and 2 of the Inspect Format.
pub async fn read<T>(tree: &T) -> Result<DiagnosticsHierarchy, ReaderError>
where
    T: ReadableTree + Send + Sync,
{
    load_snapshot_tree(tree, None).await?.try_into()
}

/// Reads the given `ReadableTree` into a DiagnosticsHierarchy with Lazy Node timeout.
/// This reads versions 1 and 2 of the Inspect Format.
pub async fn read_with_timeout<T>(
    tree: &T,
    lazy_node_timeout: Duration,
) -> Result<DiagnosticsHierarchy, ReaderError>
where
    T: ReadableTree + Send + Sync,
{
    load_snapshot_tree(tree, Some(lazy_node_timeout)).await?.try_into()
}

fn load_snapshot_tree<T>(
    tree: &T,
    lazy_child_timeout: Option<Duration>,
) -> BoxFuture<'_, Result<SnapshotTree, ReaderError>>
where
    T: ReadableTree + Send + Sync,
{
    async move {
        let results = if let Some(t) = lazy_child_timeout {
            future::join(tree.vmo(), tree.tree_names())
                .on_timeout(t.after_now(), || {
                    (Err(ReaderError::TreeTimedOut), Err(ReaderError::TreeTimedOut))
                })
                .await
        } else {
            future::join(tree.vmo(), tree.tree_names()).await
        };

        let vmo = results.0?;
        let children_names = results.1?;
        let mut children = BTreeMap::new();
        // TODO(miguelfrde): remove recursion or limit depth.
        for child_name in children_names {
            let result = if let Some(t) = lazy_child_timeout {
                tree.read_tree(&child_name)
                    .on_timeout(t.after_now(), || Err(ReaderError::TreeTimedOut))
                    .and_then(|child_tree| load(child_tree, lazy_child_timeout))
                    .await
            } else {
                tree.read_tree(&child_name).and_then(|child_tree| load(child_tree, None)).await
            };
            children.insert(child_name, result);
        }
        Ok(SnapshotTree { snapshot: Snapshot::try_from(&vmo)?, children })
    }
    .boxed()
}

async fn load<T>(tree: T, lazy_node_timeout: Option<Duration>) -> Result<SnapshotTree, ReaderError>
where
    T: ReadableTree + Send + Sync,
{
    load_snapshot_tree(&tree, lazy_node_timeout).await
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{assert_data_tree, assert_json_diff, reader, Inspector, InspectorConfig},
    };

    #[fuchsia::test]
    async fn test_read() -> Result<(), anyhow::Error> {
        let inspector = test_inspector();
        let hierarchy = read(&inspector).await?;
        assert_data_tree!(hierarchy, root: {
            int: 3i64,
            "lazy-node": {
                a: "test",
                child: {
                    double: 3.25,
                },
            }
        });
        Ok(())
    }

    #[fuchsia::test]
    async fn test_load_snapshot_tree() -> Result<(), anyhow::Error> {
        let inspector = test_inspector();
        let mut snapshot_tree = load_snapshot_tree(&inspector, None).await?;

        let root_hierarchy: DiagnosticsHierarchy =
            PartialNodeHierarchy::try_from(snapshot_tree.snapshot)?.into();
        assert_eq!(snapshot_tree.children.keys().collect::<Vec<&String>>(), vec!["lazy-node-0"]);
        assert_data_tree!(root_hierarchy, root: {
            int: 3i64,
        });

        let mut lazy_node = snapshot_tree.children.remove("lazy-node-0").unwrap().unwrap();
        let lazy_node_hierarchy: DiagnosticsHierarchy =
            PartialNodeHierarchy::try_from(lazy_node.snapshot)?.into();
        assert_eq!(lazy_node.children.keys().collect::<Vec<&String>>(), vec!["lazy-values-0"]);
        assert_data_tree!(lazy_node_hierarchy, root: {
            a: "test",
            child: {},
        });

        let lazy_values = lazy_node.children.remove("lazy-values-0").unwrap().unwrap();
        let lazy_values_hierarchy = PartialNodeHierarchy::try_from(lazy_values.snapshot)?;
        assert_eq!(lazy_values.children.keys().len(), 0);
        assert_data_tree!(lazy_values_hierarchy, root: {
            double: 3.25,
        });

        Ok(())
    }

    #[fuchsia::test]
    async fn read_with_hanging_lazy_node() -> Result<(), anyhow::Error> {
        let inspector = Inspector::default();
        let root = inspector.root();
        root.record_string("child", "value");

        root.record_lazy_values("lazy-node-always-hangs", || {
            async move {
                fuchsia_async::Timer::new(Duration::from_secs(30 * 60).after_now()).await;
                Ok(Inspector::default())
            }
            .boxed()
        });

        root.record_int("int", 3);

        let hierarchy = read_with_timeout(&inspector, Duration::from_secs(2)).await?;
        assert_json_diff!(hierarchy, root: {
            child: "value",
            int: 3i64,
        });

        Ok(())
    }

    #[fuchsia::test]
    async fn missing_value_parse_failure() -> Result<(), anyhow::Error> {
        let inspector = Inspector::default();
        let _lazy_child = inspector.root().create_lazy_child("lazy", || {
            async move {
                // For the sake of the test, force an invalid vmo.
                Ok(Inspector::new(InspectorConfig::default().no_op()))
            }
            .boxed()
        });
        let hierarchy = reader::read(&inspector).await?;
        assert_eq!(hierarchy.missing.len(), 1);
        assert_eq!(hierarchy.missing[0].reason, MissingValueReason::LinkParseFailure);
        assert_data_tree!(hierarchy, root: {});
        Ok(())
    }

    #[fuchsia::test]
    async fn missing_value_not_found() -> Result<(), anyhow::Error> {
        let inspector = Inspector::default();
        inspector.state().map(|state| {
            let mut state = state.try_lock().expect("lock state");
            state
                .allocate_link(
                    "missing".into(),
                    "missing-404".into(),
                    LinkNodeDisposition::Child,
                    0.into(),
                )
                .unwrap();
        });
        let hierarchy = reader::read(&inspector).await?;
        assert_eq!(hierarchy.missing.len(), 1);
        assert_eq!(hierarchy.missing[0].reason, MissingValueReason::LinkNotFound);
        assert_eq!(hierarchy.missing[0].name, "missing");
        assert_data_tree!(hierarchy, root: {});
        Ok(())
    }

    fn test_inspector() -> Inspector {
        let inspector = Inspector::default();
        let root = inspector.root();
        root.record_int("int", 3);
        root.record_lazy_child("lazy-node", || {
            async move {
                let inspector = Inspector::default();
                inspector.root().record_string("a", "test");
                let child = inspector.root().create_child("child");
                child.record_lazy_values("lazy-values", || {
                    async move {
                        let inspector = Inspector::default();
                        inspector.root().record_double("double", 3.25);
                        Ok(inspector)
                    }
                    .boxed()
                });
                inspector.root().record(child);
                Ok(inspector)
            }
            .boxed()
        });
        inspector
    }
}
