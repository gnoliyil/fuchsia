// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_NODE_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_NODE_H_

#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <lib/ddk/trace/event.h>
#include <lib/fit/defer.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <stdint.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/rights.h>
#include <zircon/types.h>

#include <unordered_set>
#include <vector>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "allocation_result.h"
#include "device.h"
#include "koid_util.h"
#include "logical_buffer_collection.h"
#include "versions.h"

namespace sysmem_driver {

class NodeProperties;
class BufferCollection;
class BufferCollectionToken;
class BufferCollectionTokenGroup;
class OrphanedNode;

// Implemented by BufferCollectionToken, BufferCollectionTokenGroup, BufferCollection, and
// OrphanedNode.
//
// Things that can change when transmuting from BufferCollectionToken to BufferCollection, from
// BufferCollectionToken to OrphanedNode, or from BufferCollection to OrphanedNode, should generally
// go in Node.  Things that don't change when transmuting go in NodeProperties.
class Node : public fbl::RefCounted<Node> {
 public:
  Node(const Node& to_copy) = delete;
  Node(Node&& to_move) = delete;

  Node(fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection,
       NodeProperties* node_properties, zx::unowned_channel server_end);
  // Construction status.
  zx_status_t create_status() const;
  virtual ~Node();

  void Bind(NodeServerEnd server_end);

  void SetErrorHandler(fit::function<void(zx_status_t)> error_handler);

  // The Node must have 0 children to call Fail().
  void Fail(zx_status_t epitaph);

  // Not all Node(s) that are ReadyForAllocation() have buffer_collection_constraints().  In
  // particular an OrphanedNode is always ReadyForAllocation(), but may or may not have
  // buffer_collection_constraints().
  virtual bool ReadyForAllocation() = 0;

  // buffers_logically_allocated() must be false to call this.
  virtual void OnBuffersAllocated(const AllocationResult& allocation_result) = 0;

  // Consider whether to use the sub-class accessors below, or whether to use
  // NodeProperties::is_token(), is_token_group(), is_collection() instead, depending on the
  // situation.  A client Close() + channel close will replace the sub-class of Node with an
  // OrphanedNode, yet the NodeProperties still needs to be treated as a token, collection, or
  // group when it comes to the logical node tree and allocation.  These accessors are for getting a
  // pointer to the current Node sub-class, not for checking the logical node type.  These can also
  // be used to determine if the current Node sub-class indicates that the client still has a
  // connection open to this logical node.
  //
  // If this Node is a BufferCollectionToken, returns the BufferCollectionToken*, else returns
  // nullptr.
  virtual BufferCollectionToken* buffer_collection_token() = 0;
  virtual const BufferCollectionToken* buffer_collection_token() const = 0;
  // If this Node is a BufferCollection, returns the BufferCollection*, else returns nullptr.
  virtual BufferCollection* buffer_collection() = 0;
  virtual const BufferCollection* buffer_collection() const = 0;
  // If this Node is an OrphanedNode, returns the OrphanedNode*, else returns nullptr.
  virtual OrphanedNode* orphaned_node() = 0;
  virtual const OrphanedNode* orphaned_node() const = 0;
  // If this Node is a BufferCollectionTokenGroup, returns the BufferCollectionTokenGroup*, else
  // returns nullptr.
  virtual BufferCollectionTokenGroup* buffer_collection_token_group() = 0;
  virtual const BufferCollectionTokenGroup* buffer_collection_token_group() const = 0;

  // This is a constant per sub-class of Node.  When a "connected" node is no longer connected, the
  // Node sub-class is replaced with OrphanedNode, or deleted as appropriate.
  virtual bool is_connected_type() const = 0;

  // This is dynamic depending on whether the Node sub-class server-side binding is currently bound
  // or in other words whether the node is currently connected.  This will always return false
  // when !is_connected_type(), and can return true or false if is_connected_type().
  virtual bool is_currently_connected() const = 0;
  virtual const char* node_type_string() const = 0;

  LogicalBufferCollection& logical_buffer_collection() const;
  fbl::RefPtr<LogicalBufferCollection> shared_logical_buffer_collection();

  // If the NodeProperties this Node started with is gone, this asserts, including in release.  A
  // hard crash is better than going off in the weeds.
  NodeProperties& node_properties() const;

  void EnsureDetachedFromNodeProperties();

  // Returns server end of the channel serving this node.  At least for now, this must only be
  // called when it's known that the binding is still valid.  We check this using
  // is_currently_connected().
  zx::unowned_channel channel() const;

  bool is_done() const;

  bool has_client_koid() const;
  zx_koid_t client_koid() const;
  bool has_server_koid() const;
  zx_koid_t server_koid() const;

  void set_unfound_node() { was_unfound_node_ = true; }
  bool was_unfound_node() const { return was_unfound_node_; }

  Device* parent_device() const;

  void SetDebugClientInfoInternal(std::string name, uint64_t id);

 protected:
  // Called during Bind() to perform the sub-class protocol-specific bind itself.
  using ErrorHandlerWrapper = fit::function<void(fidl::UnbindInfo info)>;
  virtual void BindInternalV1(zx::channel server_end,
                              ErrorHandlerWrapper error_handler_wrapper) = 0;
  virtual void BindInternalV2(zx::channel server_end,
                              ErrorHandlerWrapper error_handler_wrapper) = 0;
  virtual void BindInternalCombinedV1AndV2(zx::channel server_end,
                                           ErrorHandlerWrapper error_handler_wrapper) = 0;

  template <typename Completer>
  void FailSync(Location location, Completer& completer, zx_status_t status, const char* format,
                ...) {
    va_list args;
    va_start(args, format);
    logical_buffer_collection().VLogClientError(location, &node_properties(), format, args);
    va_end(args);

    completer.Close(status);
    async_failure_result_ = status;
  }

  template <class SyncCompleterSync>
  void SyncImpl(SyncCompleterSync& completer) {
    TRACE_DURATION("gfx", "Node::SyncImpl", "this", this, "logical_buffer_collection",
                   &logical_buffer_collection());
    if (is_done_) {
      // Probably a Close() followed by Sync(), which is illegal and
      // causes the whole LogicalBufferCollection to fail.
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "Sync() after Close()");
      return;
    }

    completer.Reply();
  }

  template <class CloseCompleterSync>
  void CloseImpl(CloseCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "Close() after Close()");
      return;
    }
    // We still want to enforce that the client doesn't send any other messages
    // between Close() and closing the channel, so we just set is_done_ here and
    // do a FailSync() if is_done_ is seen to be set while handling any other
    // message.
    is_done_ = true;
  }

  template <class SetNameRequestView, class SetNameCompleterSync>
  void SetNameImplV1(SetNameRequestView request, SetNameCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetName() after Close()");
      return;
    }
    logical_buffer_collection().SetName(request.priority(),
                                        std::string(request.name().begin(), request.name().end()));
  }

  template <class SetNameRequest, class SetNameCompleterSync>
  void SetNameImplV2(SetNameRequest& request, SetNameCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetName() after Close()");
      return;
    }
    if (!request.priority().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetName() requires priority set");
      return;
    }
    if (!request.name().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetName() requires name set");
      return;
    }
    logical_buffer_collection().SetName(
        request.priority().value(),
        std::string(request.name().value().begin(), request.name().value().end()));
  }

  template <class SetDebugClientInfoRequestView, class SetDebugClientInfoCompleterSync>
  void SetDebugClientInfoImplV1(SetDebugClientInfoRequestView request,
                                SetDebugClientInfoCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetDebugClientInfo() after Close()");
      return;
    }
    SetDebugClientInfoInternal(std::string(request.name().begin(), request.name().end()),
                               request.id());
  }

  template <class SetDebugClientInfoRequest, class SetDebugClientInfoCompleterSync>
  void SetDebugClientInfoImplV2(SetDebugClientInfoRequest& request,
                                SetDebugClientInfoCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetDebugClientInfo() after Close()");
      return;
    }
    if (!request.name().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetDebugClientInfo() requires name set");
      return;
    }
    if (!request.id().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetDebugClientInfo() requires id set");
      return;
    }
    SetDebugClientInfoInternal(
        std::string(request.name().value().begin(), request.name().value().end()),
        request.id().value());
  }

  template <class SetDebugTimeoutLogDeadlineRequestView,
            class SetDebugTimeoutLogDeadlineCompleterSync>
  void SetDebugTimeoutLogDeadlineImplV1(SetDebugTimeoutLogDeadlineRequestView request,
                                        SetDebugTimeoutLogDeadlineCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE,
               "SetDebugTimeoutLogDeadline() after Close()");
      return;
    }
    logical_buffer_collection().SetDebugTimeoutLogDeadline(request.deadline());
  }

  template <class SetDebugTimeoutLogDeadlineRequest, class SetDebugTimeoutLogDeadlineCompleterSync>
  void SetDebugTimeoutLogDeadlineImplV2(SetDebugTimeoutLogDeadlineRequest& request,
                                        SetDebugTimeoutLogDeadlineCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE,
               "SetDebugTimeoutLogDeadline() after Close()");
      return;
    }
    if (!request.deadline().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE,
               "SetDebugTimeoutLogDeadline() requires deadline set");
      return;
    }
    logical_buffer_collection().SetDebugTimeoutLogDeadline(request.deadline().value());
  }

  template <class SetVerboseLoggingCompleterSync>
  void SetVerboseLoggingImpl(SetVerboseLoggingCompleterSync& completer) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "SetVerboseLogging() after Close()");
      return;
    }
    logical_buffer_collection_->SetVerboseLogging();
  }

  template <typename GetNodeRefCompleterSync>
  bool CommonGetNodeRefImplStage1(GetNodeRefCompleterSync& completer, zx::event* out_to_vend) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "GetNodeRef() after Close()");
      return false;
    }
    // No process actually needs to wait on or signal this event.  It's just a generic handle that
    // needs get_info to work so we can check the koid.
    zx_status_t status =
        node_properties_->node_ref()->duplicate(ZX_RIGHTS_BASIC & ~(ZX_RIGHT_WAIT), out_to_vend);
    if (status != ZX_OK) {
      // We treat this similarly to a code page-in that fails due to low memory.
      ZX_PANIC("node_ref_vend_.duplicate() failed - sysmem terminating");
    }
    return true;
  }

  template <class GetNodeRefCompleterSync>
  void GetNodeRefImplV1(GetNodeRefCompleterSync& completer) {
    zx::event to_vend;
    if (!CommonGetNodeRefImplStage1(completer, &to_vend)) {
      return;
    }
    completer.Reply(std::move(to_vend));
  }

  template <class GetNodeRefCompleterSync, class Response = fuchsia_sysmem2::NodeGetNodeRefResponse>
  void GetNodeRefImplV2(GetNodeRefCompleterSync& completer) {
    zx::event to_vend;
    if (!CommonGetNodeRefImplStage1(completer, &to_vend)) {
      return;
    }
    Response response;
    response.node_ref().emplace(std::move(to_vend));
    completer.Reply(std::move(response));
  }

  template <typename Completer>
  bool CommonIsAlternateFor(zx::event node_ref, Completer& completer, bool* out_result) {
    if (is_done_) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "IsAlternateFor() after Close()");
      return false;
    }
    zx_koid_t node_ref_koid;
    zx_koid_t not_used;
    zx_status_t status = get_handle_koids(node_ref, &node_ref_koid, &not_used, ZX_OBJ_TYPE_EVENT);
    if (status != ZX_OK) {
      completer.Reply(fit::error(ZX_ERR_INVALID_ARGS));
      return false;
    }
    auto maybe_other_node_properties =
        logical_buffer_collection_->FindNodePropertiesByNodeRefKoid(node_ref_koid);
    if (!maybe_other_node_properties.has_value()) {
      completer.Reply(fit::error(ZX_ERR_NOT_FOUND));
      return false;
    }
    auto* other_node_properties = maybe_other_node_properties.value();

    for (auto* iter = &node_properties(); iter; iter = iter->parent()) {
      iter->set_marked(true);
    }
    // Ensure we set_marked(false), even if we add an early return.
    auto clear_marked = fit::defer([this] {
      for (auto* iter = &node_properties(); iter; iter = iter->parent()) {
        iter->set_marked(false);
      }
    });
    NodeProperties* common_parent = nullptr;
    for (auto* iter = other_node_properties; iter; iter = iter->parent()) {
      if (iter->is_marked()) {
        common_parent = iter;
        break;
      }
    }
    clear_marked.call();

    ZX_DEBUG_ASSERT(common_parent);
    // The common_parent can presently be associated with an OrphanedNode, but
    // common_parent->is_token_group() will still return true if the original Node sub-class was
    // BufferCollectionTokenGroup (even after the client has done a Close() + channel close on
    // common_parent).
    bool is_alternate_for = common_parent->is_token_group();
    *out_result = is_alternate_for;
    return true;
  }

  template <class IsAlternateForRequest, class IsAlternateForCompleterSync,
            class Response = fuchsia_sysmem::NodeIsAlternateForResponse>
  void IsAlternateForImplV1(IsAlternateForRequest& request,
                            IsAlternateForCompleterSync& completer) {
    bool is_alternate_for;
    if (!CommonIsAlternateFor(std::move(request.node_ref()), completer, &is_alternate_for)) {
      return;
    }
    Response response;
    response.is_alternate() = is_alternate_for;
    completer.Reply(fit::ok(std::move(response)));
  }

  template <class IsAlternateForRequest, class IsAlternateForCompleterSync,
            class Response = fuchsia_sysmem2::NodeIsAlternateForResponse>
  void IsAlternateForImplV2(IsAlternateForRequest& request,
                            IsAlternateForCompleterSync& completer) {
    if (!request.node_ref().has_value()) {
      FailSync(FROM_HERE, completer, ZX_ERR_BAD_STATE, "IsAlternateFor() requires node_ref set");
      return;
    }
    bool is_alernate_for;
    if (!CommonIsAlternateFor(std::move(request.node_ref().value()), completer, &is_alernate_for)) {
      return;
    }
    Response response;
    response.is_alternate() = is_alernate_for;
    completer.Reply(fit::ok(std::move(response)));
  }

  void CloseChannel(zx_status_t epitaph);

  virtual void CloseServerBinding(zx_status_t epitaph) = 0;

  // Becomes true on the first Close() (or BindSharedCollection(), in the case of
  // BufferCollectionToken).  This being true means a channel close is not fatal to the node's
  // sub-tree.  However, if the client sends a redundant Close(), that is fatal to the node's
  // sub-tree.
  bool is_done_ = false;

  std::optional<zx_status_t> async_failure_result_;

  // Used by all Node subclasses except OrphanedNode.
  fit::function<void(zx_status_t)> error_handler_;

  inspect::Node inspect_node_;
  inspect::UintProperty debug_id_property_;
  inspect::StringProperty debug_name_property_;
  inspect::ValueList properties_;

 private:
  // Construction status.
  zx_status_t create_status_ = ZX_ERR_INTERNAL;
  // At least one call to status() needs to happen before ~Node, typically shortly after
  // construction (and if status is failed, typically that one check of status() will also be
  // shortly before destruction).
  mutable bool create_status_was_checked_ = false;

  // This is in Node instead of NodeProperties because when BufferCollectionToken or
  // BufferCollection becomes an OrphanedNode, we no longer reference LogicalBufferCollection.
  fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection_;

  // The Node is co-owned by the NodeProperties, so the Node has a raw pointer back to
  // NodeProperties.
  //
  // This pointer is set to nullptr during ~NodeProperties(), so if we attempt to access via
  // node_properties_ after that, we'll get a hard crash instead of going off in the weeds.
  //
  // The main way we avoid accessing NodeProperties beyond when it goes away is the setting of
  // error_handler_ = {} in the two CloseChannel() methods.  We rely on sub-class's error_handler_
  // not running after CloseChannel(), and we rely on LLCPP not calling protocol message handlers
  // after server binding Close() (other than completion of any currently-in-progress message
  // handler), since we're running Close() on the same dispatcher.
  NodeProperties* node_properties_ = nullptr;

  // We keep server_end_ around
  zx::unowned_channel server_end_;
  zx_koid_t client_koid_ = ZX_KOID_INVALID;
  zx_koid_t server_koid_ = ZX_KOID_INVALID;

  // If true, this node was looked up by koid at some previous time, but at that time the koid
  // wasn't found.  When true, we log info later if/when the koid shows up and/or debug information
  // shows up.
  bool was_unfound_node_ = false;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_NODE_H_
