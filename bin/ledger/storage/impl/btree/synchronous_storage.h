// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_IMPL_BTREE_SYNCHRONOUS_STORAGE_H_
#define PERIDOT_BIN_LEDGER_STORAGE_IMPL_BTREE_SYNCHRONOUS_STORAGE_H_

#include <memory>
#include <vector>

#include "peridot/bin/ledger/coroutine/coroutine.h"
#include "peridot/bin/ledger/storage/impl/btree/tree_node.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/bin/ledger/storage/public/types.h"
#include "peridot/lib/callback/waiter.h"

namespace storage {
namespace btree {

// Wrapper for TreeNode and PageStorage that uses coroutines to make
// asynchronous calls look like synchronous ones.
class SynchronousStorage {
 public:
  SynchronousStorage(PageStorage* page_storage,
                     coroutine::CoroutineHandler* handler);

  PageStorage* page_storage() { return page_storage_; }
  coroutine::CoroutineHandler* handler() { return handler_; }

  Status TreeNodeFromDigest(ObjectDigestView object_digest,
                            std::unique_ptr<const TreeNode>* result);

  Status TreeNodesFromDigests(
      std::vector<ObjectDigestView> object_digests,
      std::vector<std::unique_ptr<const TreeNode>>* result);

  Status TreeNodeFromEntries(uint8_t level,
                             const std::vector<Entry>& entries,
                             const std::vector<ObjectDigest>& children,
                             ObjectDigest* result);

 private:
  PageStorage* page_storage_;
  coroutine::CoroutineHandler* handler_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SynchronousStorage);
};

}  // namespace btree
}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_IMPL_BTREE_SYNCHRONOUS_STORAGE_H_
