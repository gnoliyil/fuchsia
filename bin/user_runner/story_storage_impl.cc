// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/modular/src/user_runner/story_storage_impl.h"

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/modular/lib/fidl/array_to_string.h"
#include "apps/modular/lib/fidl/operation.h"

namespace modular {

namespace {

class ReadLinkDataCall : public Operation {
 public:
  using Result = StoryStorageImpl::ReadLinkDataCallback;

  ReadLinkDataCall(OperationContainer* const container,
                   ledger::Page* const page,
                   const fidl::String& link_id,
                   Result result)
      : Operation(container),
        page_(page),
        link_id_(link_id),
        result_(result) {
    Ready();
  }

  void Run() override {
    page_->GetSnapshot(
        page_snapshot_.NewRequest(),  nullptr, [this](ledger::Status status) {
          page_snapshot_->Get(
              to_array(link_id_),
              [this](ledger::Status status, ledger::ValuePtr value) {
                if (value) {
                  data_ = LinkData::New();
                  data_->Deserialize(value->get_bytes().data(),
                                     value->get_bytes().size());
                }
                result_(std::move(data_));
                Done();
              });
        });
  }

 private:
  ledger::Page* const page_;  // not owned
  ledger::PageSnapshotPtr page_snapshot_;
  const fidl::String link_id_;
  LinkDataPtr data_;
  Result result_;

  FTL_DISALLOW_COPY_AND_ASSIGN(ReadLinkDataCall);
};

class WriteLinkDataCall : public Operation {
 public:
  using Result = StoryStorageImpl::WriteLinkDataCallback;

  WriteLinkDataCall(OperationContainer* const container,
                    ledger::Page* const page,
                    const fidl::String& link_id,
                    LinkDataPtr data,
                    Result result)
      : Operation(container),
        page_(page),
        link_id_(link_id),
        data_(std::move(data)),
        result_(result) {
    Ready();
  }

  void Run() override {
    fidl::Array<uint8_t> bytes;
    bytes.resize(data_->GetSerializedSize());
    data_->Serialize(bytes.data(), bytes.size());

    page_->Put(to_array(link_id_), std::move(bytes),
               [this](ledger::Status status) {
                 result_();
                 Done();
               });
  }

 private:
  ledger::Page* const page_;  // not owned
  ledger::PageSnapshotPtr page_snapshot_;
  const fidl::String link_id_;
  LinkDataPtr data_;
  Result result_;

  FTL_DISALLOW_COPY_AND_ASSIGN(WriteLinkDataCall);
};

class SyncCall : public Operation {
 public:
  using Result = StoryStorageImpl::SyncCallback;

  SyncCall(OperationContainer* const container, Result result)
      : Operation(container),
        result_(result) {
    Ready();
  }

  void Run() override {
    result_();
    Done();
  }

 private:
  Result result_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SyncCall);
};

}  // namespace

StoryStorageImpl::StoryStorageImpl(std::shared_ptr<Storage> storage,
                                   ledger::PagePtr story_page,
                                   const fidl::String& key,
                                   fidl::InterfaceRequest<StoryStorage> request)
    : page_watcher_binding_(this),
      key_(key),
      storage_(storage),
      // Comment out this initializer in order to switch to in-memory storage.
      story_page_(std::move(story_page)) {
  bindings_.AddBinding(this, std::move(request));

  if (story_page_.is_bound()) {
    // TODO(mesch): We get the initial state from a different query. This
    // leaves the possibility that the next OnChange is against a
    // different base state. We should get the initial state from this page
    // snapshot instead.
    ledger::PageSnapshotPtr snapshot_unused;
    story_page_->GetSnapshot(snapshot_unused.NewRequest(),
                       page_watcher_binding_.NewBinding(),
                       [](ledger::Status status) {});
  }
}

StoryStorageImpl::~StoryStorageImpl() {}

// |StoryStorage|
void StoryStorageImpl::ReadLinkData(const fidl::String& link_id,
                                    const ReadLinkDataCallback& callback) {
  if (story_page_.is_bound()) {
    new ReadLinkDataCall(&operation_queue_, story_page_.get(), link_id,
                         callback);

  } else {
    auto& story_data = (*storage_)[key_];
    auto i = story_data.find(link_id);
    if (i != story_data.end()) {
      callback(i->second->Clone());
    } else {
      callback(nullptr);
    }
  }
}

// |StoryStorage|
void StoryStorageImpl::WriteLinkData(const fidl::String& link_id,
                                     LinkDataPtr data,
                                     const WriteLinkDataCallback& callback) {
  if (story_page_.is_bound()) {
    new WriteLinkDataCall(&operation_queue_, story_page_.get(), link_id,
                          std::move(data), callback);

  } else {
    (*storage_)[key_][link_id] = std::move(data);
    callback();
  }
}

// |StoryStorage|
void StoryStorageImpl::WatchLink(
    const fidl::String& link_id,
    fidl::InterfaceHandle<StoryStorageLinkWatcher> watcher) {
  watchers_.emplace_back(std::make_pair(
      link_id, StoryStorageLinkWatcherPtr::Create(std::move(watcher))));
}

// |StoryStorage|
void StoryStorageImpl::Dup(fidl::InterfaceRequest<StoryStorage> request) {
  bindings_.AddBinding(this, std::move(request));
}

// |PageWatcher|
void StoryStorageImpl::OnChange(ledger::PageChangePtr page,
                                const OnChangeCallback& callback) {
  if (!page.is_null() && !page->changes.is_null()) {
    for (auto& entry : page->changes) {
      const fidl::String link_id = to_string(entry->key);
      for (auto& watcher_entry : watchers_) {
        if (link_id == watcher_entry.first) {
          auto data = LinkData::New();
          data->Deserialize(entry->value->get_bytes().data(),
                            entry->value->get_bytes().size());
          watcher_entry.second->OnChange(std::move(data));
        }
      }
    }
  }
  callback(nullptr);
}

// |StoryStorage|
void StoryStorageImpl::Sync(const SyncCallback& callback) {
  new SyncCall(&operation_queue_, callback);
}

}  // namespace modular
