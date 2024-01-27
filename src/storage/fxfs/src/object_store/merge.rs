// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::extent_record::{ExtentKey, ExtentValue},
    super::object_record::{AttributeKey, ObjectKey, ObjectKeyData, ObjectValue},
    crate::lsm_tree::{
        merge::{
            ItemOp::{Discard, Keep, Replace},
            MergeLayerIterator, MergeResult,
        },
        types::Item,
    },
};

fn merge_extents(
    object_id: u64,
    attribute_id: u64,
    left: &MergeLayerIterator<'_, ObjectKey, ObjectValue>,
    right: &MergeLayerIterator<'_, ObjectKey, ObjectValue>,
    left_key: &ExtentKey,
    right_key: &ExtentKey,
    left_value: &ExtentValue,
    right_value: &ExtentValue,
) -> MergeResult<ObjectKey, ObjectValue> {
    // For now, we don't support/expect two extents with the same key in one layer.
    // One reason you can't merge deleted extents in non-adjacent layers is because you
    // can't decide which layer the merge should end up in.
    //
    // Consider this scenario:
    //   |X-X-X|
    //      |a-a-a|
    //         |X-X-X|
    // If you merge the deleted extents here, you might end up with:
    //   |X-X-X-X-X-X|
    //      |a-a-a|
    // which is clearly incorrect.
    debug_assert!(right.layer_index != left.layer_index);

    if let (ExtentValue::None, ExtentValue::None) = (left_value, right_value) {
        if (left.layer_index as i32 - right.layer_index as i32).abs() == 1 {
            // Two deletions in adjacent layers can be merged.
            return merge_deleted_extents(
                object_id,
                attribute_id,
                left_key,
                right_key,
                std::cmp::min(left.sequence(), right.sequence()),
            );
        }
    }

    if left_key.range.end <= right_key.range.start {
        // Extents don't overlap.
        return MergeResult::EmitLeft;
    }

    // The start of the left extent is <= the start of the right extent, due to merge key ordering.
    //
    // One of the extents has to win. The way we break this tie is by picking the extent from the
    // newest layer (i.e. the layer with the lowest index).
    //
    // Generally, we'll be doing the following:
    //
    //  Old  |----------|
    //  New          |----------|
    //
    // Turns into
    //
    //  Emit  |------|
    //  Old          |--|
    //  New          |----------|

    if right.layer_index < left.layer_index {
        // Right layer is newer.
        debug_assert!(left_key.range.start < right_key.range.start);
        return MergeResult::Other {
            emit: Some(Item::new_with_sequence(
                ObjectKey::extent(
                    object_id,
                    attribute_id,
                    left_key.range.start..right_key.range.start,
                ),
                ObjectValue::Extent(left_value.shrunk(
                    left_key.range.end - left_key.range.start,
                    right_key.range.start - left_key.range.start,
                )),
                std::cmp::min(left.sequence(), right.sequence()),
            )),
            left: Replace(Item::new_with_sequence(
                ObjectKey::extent(
                    object_id,
                    attribute_id,
                    right_key.range.start..left_key.range.end,
                ),
                ObjectValue::Extent(left_value.offset_by(
                    right_key.range.start - left_key.range.start,
                    left_key.range.end - left_key.range.start,
                )),
                std::cmp::min(left.sequence(), right.sequence()),
            )),
            right: Keep,
        };
    }
    // Left layer is newer.
    if left_key.range.end >= right_key.range.end {
        // The left key entirely contains the right key.
        return MergeResult::Other { emit: None, left: Keep, right: Discard };
    }
    MergeResult::Other {
        emit: None,
        left: Keep,
        right: Replace(Item::new_with_sequence(
            ObjectKey::extent(object_id, attribute_id, left_key.range.end..right_key.range.end),
            ObjectValue::Extent(right_value.offset_by(
                left_key.range.end - right_key.range.start,
                right_key.range.end - right_key.range.start,
            )),
            std::cmp::min(left.sequence(), right.sequence()),
        )),
    }
}

// Assumes that the two extents to be merged are on adjacent layers (i.e. layers N, N+1).
fn merge_deleted_extents(
    object_id: u64,
    attribute_id: u64,
    left_key: &ExtentKey,
    right_key: &ExtentKey,
    sequence: u64,
) -> MergeResult<ObjectKey, ObjectValue> {
    if left_key.range.end < right_key.range.start {
        // The extents are not adjacent or overlapping.
        return MergeResult::EmitLeft;
    }
    // Both of these are deleted extents which are either adjacent or overlapping, which means
    // we can coalece the records.
    if left_key.range.end >= right_key.range.end {
        // The left deletion eclipses the right, so just keep the left.
        return MergeResult::Other { emit: None, left: Keep, right: Discard };
    }
    MergeResult::Other {
        emit: None,
        left: Discard,
        right: Replace(Item::new_with_sequence(
            ObjectKey::extent(object_id, attribute_id, left_key.range.start..right_key.range.end),
            ObjectValue::deleted_extent(),
            sequence,
        )),
    }
}

/// Merge function for items in the object store.
///
/// The most interesting behaviour in this merge function is how extents are handled. Since extents
/// can overlap and replace one another, the merge function generally builds up the most
/// recent view of the extents in the tree, so that the output of a full merge contains no
/// overlapping extents. You can imagine looking down at the extents from the top-most layer.
///
/// A brief example:
///
/// Layer 0   |a-a-a-a|     |b-b-b|
/// Layer 1   |c-c-c-c-c|
/// Layer 2                     |d-d-d-d|
///
/// Merged    |a-a-a-a|c|   |b-b-b|d-d-d|
///
/// Adjacent or overlapping extent deletions in two adjacent layers can be merged into single
/// records (since they do not have a physical offset, so there's no need to keep the physical
/// extents contiguous). We can't merge deletions from non-adjacent layers, since that would
/// cause issues in situations like this:
///
/// Layer 0         |X-X-X|
/// Layer 1   |a-a-a-a-a-a|
/// Layer 2   |X-X-X|
///
/// Merging the two deletions in layers 0 and 2 would either result in the middle extent being
/// fully occluded or not at all (depending on whether we replaced on the left or right layer).
pub fn merge(
    left: &MergeLayerIterator<'_, ObjectKey, ObjectValue>,
    right: &MergeLayerIterator<'_, ObjectKey, ObjectValue>,
) -> MergeResult<ObjectKey, ObjectValue> {
    if left.key().object_id != right.key().object_id {
        return MergeResult::EmitLeft;
    }
    match (left.key(), right.key(), left.value(), right.value()) {
        (
            ObjectKey {
                object_id,
                data: ObjectKeyData::Attribute(left_attr_id, AttributeKey::Extent(left_extent_key)),
            },
            ObjectKey {
                object_id: _,
                data:
                    ObjectKeyData::Attribute(right_attr_id, AttributeKey::Extent(right_extent_key)),
            },
            ObjectValue::Extent(left_extent),
            ObjectValue::Extent(right_extent),
        ) if left_attr_id == right_attr_id => {
            return merge_extents(
                *object_id,
                *left_attr_id,
                left,
                right,
                left_extent_key,
                right_extent_key,
                left_extent,
                right_extent,
            );
        }
        (
            ObjectKey {
                object_id: _,
                data: ObjectKeyData::ProjectUsage { project_id: left_project_id },
            },
            ObjectKey {
                object_id: _,
                data: ObjectKeyData::ProjectUsage { project_id: right_project_id },
            },
            ObjectValue::BytesAndNodes { bytes: left_bytes, nodes: left_nodes },
            ObjectValue::BytesAndNodes { bytes: right_bytes, nodes: right_nodes },
        ) if left_project_id == right_project_id => {
            let bytes = left_bytes + right_bytes;
            let nodes = left_nodes + right_nodes;
            // Tombstone the tracking when it goes to zero.
            match (bytes, nodes) {
                (0, 0) => MergeResult::Other { emit: None, left: Discard, right: Discard },
                _ => MergeResult::Other {
                    emit: None,
                    left: Discard,
                    right: Replace(Item {
                        key: right.key().clone(),
                        value: ObjectValue::BytesAndNodes { bytes, nodes },
                        sequence: right.sequence(),
                    }),
                },
            }
        }
        // Tombstones (ObjectKeyData::Object) compare before others, so always appear on left.
        (ObjectKey { data: ObjectKeyData::Object, .. }, _, ObjectValue::None, _) => {
            debug_assert!(left.layer_index < right.layer_index);
            MergeResult::Other { emit: None, left: Keep, right: Discard }
        }
        // Note that identical keys are sorted by layer_index, so left is always newer.
        (left_key, right_key, _, _) if left_key == right_key => {
            debug_assert!(left.layer_index < right.layer_index);
            MergeResult::Other { emit: None, left: Keep, right: Discard }
        }
        _ => MergeResult::EmitLeft,
    }
}

#[cfg(test)]
mod tests {
    use {
        super::merge,
        crate::{
            lsm_tree::{
                types::{Item, LayerIterator, MergeableKey, Value},
                LSMTree,
            },
            object_store::extent_record::{Checksums, ExtentValue},
            object_store::object_record::{AttributeKey, ObjectKey, ObjectValue, Timestamp},
        },
        anyhow::Error,
        std::ops::Bound,
    };

    async fn test_merge<K: MergeableKey, V: Value + PartialEq>(
        tree: &LSMTree<K, V>,
        layer0: &[Item<K, V>],
        layer1: &[Item<K, V>],
        expected: &[Item<K, V>],
    ) {
        for item in layer1 {
            tree.insert(item.clone()).await.expect("insert error");
        }
        tree.seal().await;
        for item in layer0 {
            tree.insert(item.clone()).await.expect("insert error");
        }
        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(Bound::Unbounded).await.expect("seek failed");
        for e in expected {
            assert_eq!(iter.get().expect("get failed"), e.as_item_ref());
            iter.advance().await.expect("advance failed");
        }
        assert!(iter.get().is_none());
    }

    #[fuchsia::test]
    async fn test_merge_extents_non_overlapping() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::Extent(ExtentValue::new(16384)),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_rewrite_right() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::Extent(ExtentValue::new(16384)),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(0)));
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(16384)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_rewrite_left() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::with_checksum(0, Checksums::Fletcher(vec![1, 2]))),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::Extent(ExtentValue::with_checksum(16384, Checksums::Fletcher(vec![3]))),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::Extent(ExtentValue::with_checksum(16384, Checksums::Fletcher(vec![3])))
        );
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::Extent(ExtentValue::with_checksum(512, Checksums::Fletcher(vec![2])))
        );
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_rewrite_middle() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..2048),
            ObjectValue::Extent(ExtentValue::with_checksum(
                0,
                Checksums::Fletcher(vec![1, 2, 3, 4]),
            )),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::Extent(ExtentValue::with_checksum(16384, Checksums::Fletcher(vec![5]))),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1024));
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::Extent(ExtentValue::with_checksum(0, Checksums::Fletcher(vec![1, 2])))
        );
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1024..1536));
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::Extent(ExtentValue::with_checksum(16384, Checksums::Fletcher(vec![5])))
        );
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1536..2048));
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::Extent(ExtentValue::with_checksum(1536, Checksums::Fletcher(vec![4])))
        );
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_rewrite_eclipses() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..2048),
            ObjectValue::Extent(ExtentValue::new(16384)),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(16384)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_delete_left() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(512)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_delete_right() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(0)));
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_delete_middle() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..2048),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(0)));
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1024..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1536..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(1536)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_extents_delete_eclipses() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..2048),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_new_layer_joins_two_deletions() -> Result<(), Error> {
        // Old layer:  [----]    [----]
        // New layer:       [----]
        // Merged:     [--------------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_new_layer_joined_by_old_deletion() -> Result<(), Error> {
        // Old layer:       [----]
        // New layer:  [----]    [----]
        // Merged:     [--------------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_overlapping_newest_on_right() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_overlapping_newest_on_left() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_new_layer_contained_in_old() -> Result<(), Error> {
        // Old layer:  [--------------]
        // New layer:       [----]
        // Merged:     [--------------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_new_layer_eclipses_old() -> Result<(), Error> {
        // Old layer:       [----]
        // New layer:  [--------------]
        // Merged:     [--------------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1536),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_does_not_coalesce_if_not_adjacent_layers(
    ) -> Result<(), Error> {
        // Layer 0:  [XXXXX]
        // Layer 1:  [--------------]
        // Layer 2:        [XXXXXXXX]
        //  Merged:  [XXXXX|--------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::<ObjectKey, ObjectValue>::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(512)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extents_does_not_coalesce_if_not_adjacent_deletions(
    ) -> Result<(), Error> {
        // Layer 0:  [XXXXX|--------]
        // Layer 1:           [XXXXX]
        //  Merged:  [XXXXX|--------]
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::<ObjectKey, ObjectValue>::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..1536),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.seal().await;

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..512),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 512..1536),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(0)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extent_into_overwrites_extents() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::<ObjectKey, ObjectValue>::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::Extent(ExtentValue::new(0)),
        ))
        .await
        .expect("insert error");
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..2048),
            ObjectValue::Extent(ExtentValue::new(16384)),
        ))
        .await
        .expect("insert error");
        let key = ObjectKey::extent(object_id, attr_id, 512..1536);
        tree.merge_into(
            Item::new(key.clone(), ObjectValue::deleted_extent()),
            &key.key_for_merge_into(),
        )
        .await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(0)));
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..1536));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1536..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(16896)));
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_deleted_extent_into_merges_with_other_deletions() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::<ObjectKey, ObjectValue>::new(merge);

        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 0..1024),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");
        tree.insert(Item::new(
            ObjectKey::extent(object_id, attr_id, 1024..2048),
            ObjectValue::deleted_extent(),
        ))
        .await
        .expect("insert error");

        let key = ObjectKey::extent(object_id, attr_id, 512..1536);
        tree.merge_into(
            Item::new(key.clone(), ObjectValue::deleted_extent()),
            &key.key_for_merge_into(),
        )
        .await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_size_records() {
        let left =
            &[Item::new(ObjectKey::attribute(1, 0, AttributeKey::Size), ObjectValue::attribute(5))];
        let right = &[Item::new(
            ObjectKey::attribute(1, 0, AttributeKey::Size),
            ObjectValue::attribute(10),
        )];
        let tree = LSMTree::new(merge);
        test_merge(&tree, left, right, left).await;
    }

    #[fuchsia::test]
    async fn test_different_attributes_not_merged() {
        let left =
            Item::new(ObjectKey::attribute(1, 0, AttributeKey::Size), ObjectValue::attribute(5));
        let right =
            Item::new(ObjectKey::attribute(1, 1, AttributeKey::Size), ObjectValue::attribute(10));
        let tree = LSMTree::new(merge);
        test_merge(&tree, &[left.clone()], &[right.clone()], &[left, right]).await;

        let left =
            Item::new(ObjectKey::extent(1, 0, 0..100), ObjectValue::Extent(ExtentValue::new(0)));
        let right =
            Item::new(ObjectKey::extent(1, 1, 0..100), ObjectValue::Extent(ExtentValue::new(1)));
        let tree = LSMTree::new(merge);
        test_merge(&tree, &[left.clone()], &[right.clone()], &[left, right]).await;
    }

    #[fuchsia::test]
    async fn test_tombstone_discards_all_other_records() {
        let tombstone = Item::new(ObjectKey::object(1), ObjectValue::None);
        let other_object = Item::new(
            ObjectKey::object(2),
            ObjectValue::file(1, 0, Timestamp::default(), Timestamp::default(), 0),
        );
        let tree = LSMTree::new(merge);
        test_merge(
            &tree,
            &[tombstone.clone()],
            &[
                Item::new(
                    ObjectKey::object(1),
                    ObjectValue::file(1, 100, Timestamp::default(), Timestamp::default(), 0),
                ),
                Item::new(
                    ObjectKey::attribute(1, 0, AttributeKey::Size),
                    ObjectValue::attribute(100),
                ),
                other_object.clone(),
            ],
            &[tombstone, other_object],
        )
        .await;
    }

    #[fuchsia::test]
    async fn test_merge_preserves_sequences() -> Result<(), Error> {
        let object_id = 0;
        let attr_id = 0;
        let tree = LSMTree::<ObjectKey, ObjectValue>::new(merge);

        tree.insert(Item {
            key: ObjectKey::extent(object_id, attr_id, 0..1024),
            value: ObjectValue::Extent(ExtentValue::new(0u64)),
            sequence: 1u64,
        })
        .await
        .expect("insert error");
        tree.seal().await;
        tree.insert(Item {
            key: ObjectKey::extent(object_id, attr_id, 0..512),
            value: ObjectValue::deleted_extent(),
            sequence: 2u64,
        })
        .await
        .expect("insert error");
        tree.insert(Item {
            key: ObjectKey::extent(object_id, attr_id, 1536..2048),
            value: ObjectValue::Extent(ExtentValue::new(1536)),
            sequence: 3u64,
        })
        .await
        .expect("insert error");
        tree.insert(Item {
            key: ObjectKey::extent(object_id, attr_id, 768..1024),
            value: ObjectValue::Extent(ExtentValue::new(12345)),
            sequence: 4u64,
        })
        .await
        .expect("insert error");

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 0..512));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::deleted_extent());
        assert_eq!(iter.get().unwrap().sequence, 2u64);
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 512..768));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(512)));
        assert_eq!(iter.get().unwrap().sequence, 1u64);
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 768..1024));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(12345)));
        assert_eq!(iter.get().unwrap().sequence, 4u64);
        iter.advance().await?;
        assert_eq!(iter.get().unwrap().key, &ObjectKey::extent(object_id, attr_id, 1536..2048));
        assert_eq!(iter.get().unwrap().value, &ObjectValue::Extent(ExtentValue::new(1536)));
        assert_eq!(iter.get().unwrap().sequence, 3u64);
        iter.advance().await?;
        assert!(iter.get().is_none());
        Ok(())
    }

    #[fuchsia::test]
    async fn test_merge_project_usage() {
        let tree = LSMTree::new(merge);
        let key = ObjectKey::project_usage(5, 6);

        tree.insert(Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 100, nodes: 1000 }))
            .await
            .expect("insert error");
        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 4, nodes: 8 }),
            &key,
        )
        .await;
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: -1, nodes: -2 }),
            &key,
        )
        .await;
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 16, nodes: 32 }),
            &key,
        )
        .await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await.unwrap();
        assert_eq!(iter.get().unwrap().key, &key);
        assert_eq!(
            iter.get().unwrap().value,
            &ObjectValue::BytesAndNodes { bytes: 119, nodes: 1038 }
        );
        iter.advance().await.unwrap();
        assert!(iter.get().is_none());
    }

    #[fuchsia::test]
    async fn test_merge_project_usage_to_zero() {
        let tree = LSMTree::new(merge);
        let key = ObjectKey::project_usage(5, 6);

        tree.insert(Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 4, nodes: 8 }))
            .await
            .expect("insert error");
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: -4, nodes: -8 }),
            &key,
        )
        .await;
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let iter = merger.seek(std::ops::Bound::Unbounded).await.unwrap();
        assert!(iter.get().is_none());
    }

    #[fuchsia::test]
    async fn test_merge_project_usage_recover_from_zero() {
        let tree = LSMTree::new(merge);
        let key = ObjectKey::project_usage(5, 6);

        tree.insert(Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 4, nodes: 8 }))
            .await
            .expect("insert error");
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: -4, nodes: -8 }),
            &key,
        )
        .await;
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 20, nodes: 40 }),
            &key,
        )
        .await;
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await.unwrap();
        assert_eq!(iter.get().unwrap().key, &key);
        assert_eq!(iter.get().unwrap().value, &ObjectValue::BytesAndNodes { bytes: 20, nodes: 40 });
        iter.advance().await.unwrap();
        assert!(iter.get().is_none());
    }

    #[fuchsia::test]
    async fn test_merge_project_usage_layer_merge_to_negative() {
        let tree = LSMTree::new(merge);
        let key = ObjectKey::project_usage(5, 6);

        tree.insert(Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 4, nodes: 8 }))
            .await
            .expect("insert error");
        tree.seal().await;

        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: 16, nodes: 32 }),
            &key,
        )
        .await;
        tree.seal().await;

        // As we merge from the newest layer down this will drop bytes below zero and nodes to
        // exactly zero during the merge process.
        tree.merge_into(
            Item::new(key.clone(), ObjectValue::BytesAndNodes { bytes: -18, nodes: -32 }),
            &key,
        )
        .await;
        tree.seal().await;

        let layer_set = tree.layer_set();
        let mut merger = layer_set.merger();
        let mut iter = merger.seek(std::ops::Bound::Unbounded).await.unwrap();
        assert_eq!(iter.get().unwrap().key, &key);
        assert_eq!(iter.get().unwrap().value, &ObjectValue::BytesAndNodes { bytes: 2, nodes: 8 });
        iter.advance().await.unwrap();
        assert!(iter.get().is_none());
    }
}
