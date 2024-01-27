// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_VFS_CPP_PSEUDO_DIR_H_
#define LIB_VFS_CPP_PSEUDO_DIR_H_

#include <lib/vfs/cpp/internal/directory.h>
#include <lib/vfs/cpp/internal/node.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <functional>
#include <map>
#include <mutex>
#include <string_view>

namespace vfs {

// A pseudo-directory is a directory-like object whose entries are constructed
// by a program at runtime.  The client can lookup, enumerate, and watch(not yet
// implemented) these directory entries but it cannot create, remove, or rename
// them.
//
// This class is thread-hostile, as are the |Nodes| it manages.
//
//  # Simple usage
//
// Instances of this class should be owned and managed on the same thread
// that services their connections.
//
// # Advanced usage
//
// You can use a background thread to service connections provided: (a) the
// contents of the directory are configured prior to starting to service
// connections, (b) all modifications to the directory occur while the
// async_dispatcher_t for the background thread is stopped or suspended, and
// (c) async_dispatcher_t for the background thread is stopped or suspended
// prior to destroying the directory.
class PseudoDir : public vfs::internal::Directory {
 public:
  // Creates a directory which is initially empty.
  PseudoDir();

  // Destroys the directory and releases the nodes it contains.
  ~PseudoDir() override;

  // Adds a directory entry associating the given |name| with |vn|.
  // It is ok to add the same Node multiple times with different names.
  //
  // Returns |ZX_OK| on success.
  // Returns |ZX_ERR_ALREADY_EXISTS| if there is already a node with the given
  // name.
  zx_status_t AddSharedEntry(std::string name, std::shared_ptr<Node> vn);

  // Adds a directory entry associating the given |name| with |vn|.
  //
  // Returns |ZX_OK| on success.
  // Returns |ZX_ERR_ALREADY_EXISTS| if there is already a node with the given
  // name.
  zx_status_t AddEntry(std::string name, std::unique_ptr<Node> vn);

  // Removes a directory entry with the given |name|.
  //
  // Returns |ZX_OK| on success.
  // Returns |ZX_ERR_NOT_FOUND| if there is no node with the given name.
  zx_status_t RemoveEntry(const std::string& name);

  // Removes a directory entry with the given |name| and |node|.
  //
  // Returns |ZX_OK| on success.
  // Returns |ZX_ERR_NOT_FOUND| if there is no node with the given |name| and
  // matching |node| pointer.
  zx_status_t RemoveEntry(const std::string& name, Node* node);

  // Checks if directory is empty.
  // Be careful while using this function if using this Dir in multiple
  // threads.
  bool IsEmpty() const;

  // |Directory| implementation:
  zx_status_t Lookup(std::string_view name, vfs::internal::Node** out_node) const final;

  zx_status_t Readdir(uint64_t offset, void* data, uint64_t len, uint64_t* out_offset,
                      uint64_t* out_actual) override;

 private:
  class Entry {
   public:
    Entry(uint64_t id, std::string name);
    virtual ~Entry();

    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    virtual Node* node() const = 0;

   private:
    uint64_t const id_;
    std::string name_;
  };

  class SharedEntry : public Entry {
   public:
    SharedEntry(uint64_t id, std::string name, std::shared_ptr<Node> node);
    ~SharedEntry() override;

    Node* node() const override;

   private:
    std::shared_ptr<Node> node_;
  };

  class UniqueEntry : public Entry {
   public:
    UniqueEntry(uint64_t id, std::string name, std::unique_ptr<Node> node);
    ~UniqueEntry() override;

    Node* node() const override;

   private:
    std::unique_ptr<Node> node_;
  };

  zx_status_t AddEntry(std::unique_ptr<Entry> entry);

  static constexpr uint64_t kDotId = 1u;

  mutable std::mutex mutex_;

  std::atomic_uint64_t next_node_id_;

  // for enumeration
  std::map<uint64_t, std::unique_ptr<Entry>> entries_by_id_ __TA_GUARDED(mutex_);

  // for lookup
  std::map<std::string, Entry*, std::less<>> entries_by_name_ __TA_GUARDED(mutex_);
};

}  // namespace vfs
#endif  // LIB_VFS_CPP_PSEUDO_DIR_H_
