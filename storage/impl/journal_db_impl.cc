// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/storage/impl/journal_db_impl.h"

#include <functional>
#include <string>

#include "apps/ledger/storage/impl/btree/btree_builder.h"
#include "apps/ledger/storage/impl/commit_impl.h"
#include "apps/ledger/storage/impl/db.h"
#include "apps/ledger/storage/public/commit.h"

namespace storage {

JournalDBImpl::JournalDBImpl(PageStorageImpl* page_storage,
                             DB* db,
                             const JournalId& id,
                             const CommitId& base)
    : page_storage_(page_storage),
      db_(db),
      id_(id),
      base_(base),
      object_store_(page_storage),
      valid_(true) {}

JournalDBImpl::~JournalDBImpl() {
  FTL_DCHECK(!valid_)
      << "Journal should be committed or rolled back before being destroyed.";
}

std::unique_ptr<Journal> JournalDBImpl::Simple(PageStorageImpl* page_storage,
                                               DB* db,
                                               const JournalId& id,
                                               const CommitId& base) {
  return std::unique_ptr<Journal>(
      new JournalDBImpl(page_storage, db, id, base));
}

std::unique_ptr<Journal> JournalDBImpl::Merge(PageStorageImpl* page_storage,
                                              DB* db,
                                              const JournalId& id,
                                              const CommitId& base,
                                              const CommitId& other) {
  JournalDBImpl* db_journal = new JournalDBImpl(page_storage, db, id, base);
  db_journal->other_ = std::unique_ptr<CommitId>(new std::string(other));
  std::unique_ptr<Journal> journal(db_journal);
  return journal;
}

JournalId JournalDBImpl::GetId() const {
  return id_;
}

Status JournalDBImpl::Put(convert::ExtendedStringView key,
                          ObjectIdView object_id,
                          KeyPriority priority) {
  if (!valid_) {
    return Status::ILLEGAL_STATE;
  }
  return db_->AddJournalEntry(id_, key, object_id, priority);
}

Status JournalDBImpl::Delete(convert::ExtendedStringView key) {
  if (!valid_) {
    return Status::ILLEGAL_STATE;
  }
  return db_->RemoveJournalEntry(id_, key);
}

void JournalDBImpl::Commit(
    std::function<void(Status, const CommitId&)> callback) {
  if (!valid_) {
    callback(Status::ILLEGAL_STATE, "");
    return;
  }
  std::unique_ptr<Iterator<const EntryChange>> entries;
  Status status = db_->GetJournalEntries(id_, &entries);
  if (status != Status::OK) {
    callback(status, "");
    return;
  }

  std::unique_ptr<storage::Commit> base_commit;
  status = page_storage_->GetCommit(base_, &base_commit);
  if (status != Status::OK) {
    callback(status, "");
    return;
  }

  size_t node_size;
  status = db_->GetNodeSize(&node_size);
  if (status != Status::OK) {
    callback(status, "");
    return;
  }

  BTreeBuilder::ApplyChanges(
      &object_store_, base_commit->GetRootId(), node_size, std::move(entries),
      [this, callback](Status status, ObjectId object_id) {
        if (status != Status::OK) {
          callback(status, "");
          return;
        }

        std::vector<CommitId> parents({base_});
        if (other_) {
          parents.push_back(*other_);
        }

        std::unique_ptr<storage::Commit> commit =
            CommitImpl::FromContentAndParents(&object_store_, object_id,
                                              std::move(parents));
        ObjectId id = commit->GetId();
        status = page_storage_->AddCommitFromLocal(std::move(commit));
        db_->RemoveJournal(id_);
        valid_ = false;
        if (status != Status::OK) {
          callback(status, "");
        } else {
          callback(status, id);
        }
      });
}

Status JournalDBImpl::Rollback() {
  if (!valid_) {
    return Status::ILLEGAL_STATE;
  }
  Status s = db_->RemoveJournal(id_);
  if (s == Status::OK) {
    valid_ = false;
  }
  return s;
}

}  // namespace storage
