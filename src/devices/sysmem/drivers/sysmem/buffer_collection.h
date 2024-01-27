// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_

#include <fidl/fuchsia.sysmem/cpp/fidl.h>
#include <fidl/fuchsia.sysmem2/cpp/fidl.h>
#include <lib/fidl/internal.h>

#include <list>

#include "lib/fidl/cpp/wire/wire_messaging_declarations.h"
#include "lib/zx/channel.h"
#include "logging.h"
#include "logical_buffer_collection.h"
#include "node.h"
#include "src/devices/sysmem/drivers/sysmem/device.h"

namespace sysmem_driver {

// This class indirectly implements both V1 and V2 BufferCollection server.
//
// This class can't directly implement both servers because FIDL completers for one-way messages
// from client to server end up using the same type, which makes the overrride(s) ambiguous for any
// one-way message with no parameters.
class BufferCollection : public Node {
 public:
  // Use EmplaceInTree() instead of Create() (until we switch to llcpp when we can have a new
  // Create() that does what EmplaceInTree() currently does).  The returned reference is valid while
  // this Node is in the tree under root_.
  static BufferCollection& EmplaceInTree(
      fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection, BufferCollectionToken* token,
      zx::unowned_channel server_end);
  ~BufferCollection() override;

  //
  // LogicalBufferCollection uses these:
  //

  bool is_set_constraints_seen() const { return is_set_constraints_seen_; }
  bool has_constraints();

  // has_constraints() must be true to call this.
  const fuchsia_sysmem2::BufferCollectionConstraints& constraints();

  // has_constraints() must be true to call this, and will stay true after calling this.
  fuchsia_sysmem2::BufferCollectionConstraints CloneConstraints();

  fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection_shared();

  bool should_propagate_failure_to_parent_node() const;

  // Node interface
  bool ReadyForAllocation() override;
  void OnBuffersAllocated(const AllocationResult& allocation_result) override;
  BufferCollectionToken* buffer_collection_token() override;
  const BufferCollectionToken* buffer_collection_token() const override;
  BufferCollection* buffer_collection() override;
  const BufferCollection* buffer_collection() const override;
  BufferCollectionTokenGroup* buffer_collection_token_group() override;
  const BufferCollectionTokenGroup* buffer_collection_token_group() const override;
  OrphanedNode* orphaned_node() override;
  const OrphanedNode* orphaned_node() const override;
  bool is_connected_type() const override;
  bool is_currently_connected() const override;
  const char* node_type_string() const override;

 protected:
  void BindInternalV1(zx::channel collection_request,
                      ErrorHandlerWrapper error_handler_wrapper) override;
  void BindInternalV2(zx::channel collection_request,
                      ErrorHandlerWrapper error_handler_wrapper) override;

 private:
  friend class FidlServer;

  struct V1 : public fidl::Server<fuchsia_sysmem::BufferCollection> {
    explicit V1(BufferCollection& parent) : parent_(parent) {}

    //
    // V1:
    //
    // FIDL "compose Node" "interface" (identical among BufferCollection, BufferCollectionToken,
    // BufferCollectionTokenGroup)
    //
    void Sync(SyncCompleter::Sync& completer) override;
    void DeprecatedSync(DeprecatedSyncCompleter::Sync& completer) override;
    void Close(CloseCompleter::Sync& completer) override;
    void DeprecatedClose(DeprecatedCloseCompleter::Sync& completer) override;
    void GetNodeRef(GetNodeRefCompleter::Sync& completer) override;
    void IsAlternateFor(IsAlternateForRequest& request,
                        IsAlternateForCompleter::Sync& completer) override;
    void SetName(SetNameRequest& request, SetNameCompleter::Sync& completer) override;
    void DeprecatedSetName(DeprecatedSetNameRequest& request,
                           DeprecatedSetNameCompleter::Sync& completer) override;
    void SetDebugClientInfo(SetDebugClientInfoRequest& request,
                            SetDebugClientInfoCompleter::Sync& completer) override;
    void DeprecatedSetDebugClientInfo(
        DeprecatedSetDebugClientInfoRequest& request,
        DeprecatedSetDebugClientInfoCompleter::Sync& completer) override;
    void SetDebugTimeoutLogDeadline(SetDebugTimeoutLogDeadlineRequest& request,
                                    SetDebugTimeoutLogDeadlineCompleter::Sync& completer) override;
    void SetVerboseLogging(SetVerboseLoggingCompleter::Sync& completer) override;

    //
    // V1:
    //
    // fuchsia.sysmem.BufferCollection interface methods (see also "compose Node" methods above)
    //
    void SetConstraints(SetConstraintsRequest& request,
                        SetConstraintsCompleter::Sync& completer) override;
    void WaitForBuffersAllocated(WaitForBuffersAllocatedCompleter::Sync& completer) override;
    void CheckBuffersAllocated(CheckBuffersAllocatedCompleter::Sync& completer) override;
    void SetConstraintsAuxBuffers(SetConstraintsAuxBuffersRequest& request,
                                  SetConstraintsAuxBuffersCompleter::Sync& completer) override;
    void GetAuxBuffers(GetAuxBuffersCompleter::Sync& completer) override;
    void AttachToken(AttachTokenRequest& request, AttachTokenCompleter::Sync& completer) override;
    void AttachLifetimeTracking(AttachLifetimeTrackingRequest& request,
                                AttachLifetimeTrackingCompleter::Sync& completer) override;

    BufferCollection& parent_;
  };

  struct V2 : public fidl::Server<fuchsia_sysmem2::BufferCollection>, public fbl::Recyclable<V2> {
    explicit V2(BufferCollection& parent) : parent_(parent) {}

    //
    // FIDL "compose Node" "interface" (identical among BufferCollection, BufferCollectionToken,
    // BufferCollectionTokenGroup)
    //
    void Sync(SyncCompleter::Sync& completer) override;
    void Close(CloseCompleter::Sync& completer) override;
    void GetNodeRef(GetNodeRefCompleter::Sync& completer) override;
    void IsAlternateFor(IsAlternateForRequest& request,
                        IsAlternateForCompleter::Sync& completer) override;
    void SetName(SetNameRequest& request, SetNameCompleter::Sync& completer) override;
    void SetDebugClientInfo(SetDebugClientInfoRequest& request,
                            SetDebugClientInfoCompleter::Sync& completer) override;
    void SetDebugTimeoutLogDeadline(SetDebugTimeoutLogDeadlineRequest& request,
                                    SetDebugTimeoutLogDeadlineCompleter::Sync& completer) override;
    void SetVerboseLogging(SetVerboseLoggingCompleter::Sync& completer) override;

    //
    // fuchsia.sysmem.BufferCollection interface methods (see also "compose Node" methods above)
    //
    void SetConstraints(SetConstraintsRequest& request,
                        SetConstraintsCompleter::Sync& completer) override;
    void WaitForAllBuffersAllocated(WaitForAllBuffersAllocatedCompleter::Sync& completer) override;
    void CheckAllBuffersAllocated(CheckAllBuffersAllocatedCompleter::Sync& completer) override;
    void AttachToken(AttachTokenRequest& request, AttachTokenCompleter::Sync& completer) override;
    void AttachLifetimeTracking(AttachLifetimeTrackingRequest& request,
                                AttachLifetimeTrackingCompleter::Sync& completer) override;

    BufferCollection& parent_;
  };

  explicit BufferCollection(fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection,
                            const BufferCollectionToken& token, zx::unowned_channel server_end);

  void CloseServerBinding(zx_status_t epitaph) override;

  // The rights attenuation mask driven by usage, so that read-only usage
  // doesn't get write, etc.
  uint32_t GetUsageBasedRightsAttenuation();

  uint32_t GetClientVmoRights();
  uint32_t GetClientAuxVmoRights();
  void MaybeCompleteWaitForBuffersAllocated();
  void MaybeFlushPendingLifetimeTracking();

  void FailAsync(Location location, zx_status_t status, const char* format, ...) __PRINTFLIKE(4, 5);
  // FailSync must be used instead of FailAsync if the current method has a completer that needs a
  // reply.
  template <typename Completer>
  void FailSync(Location location, Completer& completer, zx_status_t status, const char* format,
                ...) __PRINTFLIKE(5, 6);

  fpromise::result<fuchsia_sysmem2::BufferCollectionInfo> CloneResultForSendingV2(
      const fuchsia_sysmem2::BufferCollectionInfo& buffer_collection_info);

  fpromise::result<fuchsia_sysmem::BufferCollectionInfo2> CloneResultForSendingV1(
      const fuchsia_sysmem2::BufferCollectionInfo& buffer_collection_info);
  fpromise::result<fuchsia_sysmem::BufferCollectionInfo2> CloneAuxBuffersResultForSendingV1(
      const fuchsia_sysmem2::BufferCollectionInfo& buffer_collection_info);

  template <typename Completer>
  bool CommonSetConstraintsStage1(Completer& completer);

  template <typename Completer>
  bool CommonWaitForAllBuffersAllocatedStage1(Completer& completer, trace_async_id_t* out_event_id);

  template <typename Completer>
  bool CommonCheckAllBuffersAllocatedStage1(Completer& completer, zx_status_t* result);

  template <typename Completer>
  bool CommonAttachTokenStage1(uint32_t rights_attenuation_mask, Completer& completer,
                               NodeProperties** out_node_properties);

  template <typename Completer>
  void CommonAttachLifetimeTracking(zx::eventpair server_end, uint32_t buffers_remaining,
                                    Completer& completer);

  std::optional<V1> v1_server_;
  std::optional<V2> v2_server_;

  // Temporarily holds fuchsia.sysmem.BufferCollectionConstraintsAuxBuffers until SetConstraints()
  // arrives.
  std::optional<fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers> constraints_aux_buffers_;

  // FIDL protocol enforcement.
  bool is_set_constraints_seen_ = false;
  bool is_set_constraints_aux_buffers_seen_ = false;

  std::list<std::pair</*async_id*/ uint64_t, V1::WaitForBuffersAllocatedCompleter::Async>>
      pending_wait_for_buffers_allocated_v1_;
  std::list<std::pair</*async_id*/ uint64_t, V2::WaitForAllBuffersAllocatedCompleter::Async>>
      pending_wait_for_buffers_allocated_v2_;

  bool is_done_ = false;

  std::optional<fidl::ServerBindingRef<fuchsia_sysmem::BufferCollection>> server_binding_v1_;
  std::optional<fidl::ServerBindingRef<fuchsia_sysmem2::BufferCollection>> server_binding_v2_;

  // Becomes set when OnBuffersAllocated() is called, and stays set after that.
  std::optional<AllocationResult> logical_allocation_result_;

  struct PendingLifetimeTracking {
    zx::eventpair server_end;
    uint32_t buffers_remaining;
  };
  std::vector<PendingLifetimeTracking> pending_lifetime_tracking_;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_
