// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_DYNAMIC_CHANNEL_REGISTRY_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_DYNAMIC_CHANNEL_REGISTRY_H_

#include <lib/fit/function.h>

#include <random>
#include <unordered_map>

#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/common/weak_self.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/dynamic_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/signaling_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/types.h"

namespace bt::l2cap::internal {

// Base class for registries of dynamic L2CAP channels. It serves both as the
// factory and owner of dynamic channels created on a logical link, including
// assigning and tracking dynamic channel IDs.
//
// Registry entries are DynamicChannels that do not implement the l2cap::Channel
// interface used for user data transfer.
//
// This class is not thread-safe and is intended to be created and run on the
// L2CAP thread for each logical link connected.
class DynamicChannelRegistry : public WeakSelf<DynamicChannelRegistry> {
 public:
  // Used to pass an optional channel to clients of the registry. |channel| may
  // be nullptr upon failure to open. Otherwise, it points to an instance owned
  // by the registry and should not be retained by the callee.
  using DynamicChannelCallback = fit::function<void(const DynamicChannel* channel)>;
  using ServiceInfo = ServiceInfo<DynamicChannelCallback>;

  // Used to query the upper layers for the presence of a service that is
  // accepting channels. If the service exists, it should return a callback
  // that accepts the inbound dynamic channel opened.
  using ServiceRequestCallback = fit::function<std::optional<ServiceInfo>(PSM psm)>;

  virtual ~DynamicChannelRegistry();

  // Create and connect a dynamic channel. The result will be returned by
  // calling |open_cb| on the L2CAP thread the channel is ready for data
  // transfer, with a nullptr if unsuccessful. The DynamicChannel passed will
  // contain the local and remote channel IDs to be used for user data transfer
  // over the new channel. Preferred channel parameters can be set in |params|.
  void OpenOutbound(PSM psm, ChannelParameters params, DynamicChannelCallback open_cb);

  // Disconnect and remove the channel identified by |local_cid|. After this call completes,
  // incoming PDUs with |local_cid| should be discarded as in error or considered to belong to a
  // subsequent channel with that ID. Any outbound PDUs passed to the Channel interface for this
  // channel should be discarded. When the close operation completes, |close_cb| will be called, the
  // internal channel will be destroyed, and |local_cid| may be recycled for another dynamic
  // channel. |close_cb| will be called immediately if the channel doesn't exist.
  void CloseChannel(ChannelId local_cid, fit::closure close_cb);

 protected:
  // |max_num_channels| is the number of dynamic channel IDs that can be
  // allocated on this link, and must be non-zero and less than 65473.
  //
  // |close_cb| will be called upon a remote-initiated closure of an open
  // channel. The registry's internal channel is passed as a parameter, and it
  // will be closed for user data transfer before the callback fires. When the
  // callback returns, the channel is destroyed and its ID may be recycled for
  // another dynamic channel. Channels that fail to open due to error or are
  // closed using CloseChannel will not trigger this callback.
  //
  // |service_request_cb| will be called upon remote-initiated channel requests.
  // For services accepting channels, it shall return a callback to accept the
  // opened channel, which only be called if the channel successfully opens. To
  // deny the channel creation, |service_request_cb| should return a nullptr.
  //
  // If |random_channel_ids| is true then the channel IDs assigned will be randomized.
  // Otherwise, they will be assigned starting at the lowest available dynamic channel id.
  DynamicChannelRegistry(uint16_t max_num_channels, DynamicChannelCallback close_cb,
                         ServiceRequestCallback service_request_cb, bool random_channel_ids);

  // Factory method for a DynamicChannel implementation that represents an
  // outbound channel with an endpoint on this device identified by |local_cid|.
  virtual DynamicChannelPtr MakeOutbound(PSM psm, ChannelId local_cid,
                                         ChannelParameters params) = 0;

  // Factory method for a DynamicChannel implementation that represents an
  // inbound channel from a remote endpoint identified by |remote_cid| to an
  // endpoint on this device identified by |local_cid|.
  virtual DynamicChannelPtr MakeInbound(PSM psm, ChannelId local_cid, ChannelId remote_cid,
                                        ChannelParameters params) = 0;

  // Open an inbound channel for a service |psm| from the remote endpoint
  // identified by |remote_cid| to the local endpoint by |local_cid|.
  DynamicChannel* RequestService(PSM psm, ChannelId local_cid, ChannelId remote_cid);

  // In the range starting at kFirstDynamicChannelId with |max_num_channels_|, pick a dynamic
  // channel ID that is available on this link.
  // Returns kInvalidChannelId if all IDs have been exhausted.
  ChannelId FindAvailableChannelId();

  // Return the number of alive channels on this link.
  size_t AliveChannelCount() const;

  // Returns null if not found. Can be downcast to the derived DynamicChannel
  // created by MakeOutbound.
  DynamicChannel* FindChannelByLocalId(ChannelId local_cid) const;

  // Searches for alive dynamic channel with given remote channel id.
  // Returns null if not found.
  DynamicChannel* FindChannelByRemoteId(ChannelId remote_cid) const;

  // Iterates over all channels, running |f| on each entry synchronously.
  void ForEach(fit::function<void(DynamicChannel*)> f) const;

 private:
  friend class DynamicChannel;

  // Open a newly-created channel. If |pass_failed| is true, always invoke
  // |open_cb| with the result of the operation, including with nullptr if the
  // channel failed to open. Otherwise if |pass_failed| is false, only invoke
  // |open_cb| for successfully-opened channels.
  void ActivateChannel(DynamicChannel* channel, DynamicChannelCallback open_cb, bool pass_failed);

  // Signal a remote-initiated closure of a channel owned by this registry, then
  // delete it. |close_cb_| is invoked if the channel was ever open (see
  // |DynamicChannel::opened|).
  void OnChannelDisconnected(DynamicChannel* channel);

  // Delete a channel owned by this registry. Then, after this returns,
  // |local_cid| may be recycled for another dynamic channel.
  void RemoveChannel(DynamicChannel* channel);

  // Greatest dynamic channel ID that can be assigned on the kind of logical
  // link associated to this registry.
  const uint16_t max_num_channels_;

  // Called only for channels that were already open (see
  // |DynamicChannel::opened|).
  DynamicChannelCallback close_cb_;
  ServiceRequestCallback service_request_cb_;

  // Maps local CIDs to alive dynamic channels on this logical link.
  using ChannelMap = std::unordered_map<ChannelId, DynamicChannelPtr>;
  ChannelMap channels_;

  std::optional<std::default_random_engine> rng_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(DynamicChannelRegistry);
};

}  // namespace bt::l2cap::internal

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_DYNAMIC_CHANNEL_REGISTRY_H_
