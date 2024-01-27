// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{Error, Inner, InnerType, InspectType, State};
use inspect_format::BlockIndex;

#[cfg(test)]
use inspect_format::{Block, Container};

/// Inspect Lazy Node data type.
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct LazyNode {
    inner: Inner<InnerLazyNodeType>,
}

impl InspectType for LazyNode {}

crate::impl_inspect_type_internal!(LazyNode);

#[derive(Default, Debug)]
struct InnerLazyNodeType;

impl InnerType for InnerLazyNodeType {
    type Data = ();
    fn free(state: &State, block_index: BlockIndex) -> Result<(), Error> {
        let mut state_lock = state.try_lock()?;
        state_lock
            .free_lazy_node(block_index)
            .map_err(|err| Error::free("lazy node", block_index, err))
    }
}

#[cfg(test)]
impl LazyNode {
    /// Returns the [`Block`][Block] associated with this value.
    pub fn get_block(&self) -> Option<Block<Container>> {
        self.inner.inner_ref().and_then(|inner_ref| {
            inner_ref
                .state
                .try_lock()
                .and_then(|state| state.heap().get_block(inner_ref.block_index))
                .ok()
        })
    }
}

#[cfg(test)]
mod tests {
    use crate::writer::types::Inspector;
    use futures::FutureExt;
    use inspect_format::{BlockType, LinkNodeDisposition};

    #[fuchsia::test]
    fn lazy_values() {
        let inspector = Inspector::default();
        let node = inspector.root().create_child("node");
        let node_block = node.get_block().unwrap();
        {
            let lazy_node =
                node.create_lazy_values("lazy", || async move { Ok(Inspector::default()) }.boxed());
            let lazy_node_block = lazy_node.get_block().unwrap();
            assert_eq!(lazy_node_block.block_type(), BlockType::LinkValue);
            assert_eq!(
                lazy_node_block.link_node_disposition().unwrap(),
                LinkNodeDisposition::Inline
            );
            assert_eq!(*lazy_node_block.link_content_index().unwrap(), 6);
            assert_eq!(node_block.child_count().unwrap(), 1);
        }
        assert_eq!(node_block.child_count().unwrap(), 0);
    }

    #[fuchsia::test]
    fn lazy_node() {
        let inspector = Inspector::default();
        let node = inspector.root().create_child("node");
        let node_block = node.get_block().unwrap();
        {
            let lazy_node =
                node.create_lazy_child("lazy", || async move { Ok(Inspector::default()) }.boxed());
            let lazy_node_block = lazy_node.get_block().unwrap();
            assert_eq!(lazy_node_block.block_type(), BlockType::LinkValue);
            assert_eq!(
                lazy_node_block.link_node_disposition().unwrap(),
                LinkNodeDisposition::Child
            );
            assert_eq!(*lazy_node_block.link_content_index().unwrap(), 6);
            assert_eq!(node_block.child_count().unwrap(), 1);
        }
        assert_eq!(node_block.child_count().unwrap(), 0);
    }
}
