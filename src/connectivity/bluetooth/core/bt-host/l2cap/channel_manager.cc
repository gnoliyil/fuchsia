// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "channel_manager.h"

#include <lib/async/default.h>

#include "logical_link.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"

namespace bt::l2cap {

namespace {

constexpr const char* kInspectServicesNodeName = "services";
constexpr const char* kInspectServiceNodePrefix = "service_";
constexpr const char* kInspectLogicalLinksNodeName = "logical_links";
constexpr const char* kInspectLogicalLinkNodePrefix = "logical_link_";
constexpr const char* kInspectPsmPropertyName = "psm";

}  // namespace

class ChannelManagerImpl final : public ChannelManager {
 public:
  using LinkErrorCallback = fit::closure;

  ChannelManagerImpl(hci::AclDataChannel* acl_data_channel, hci::CommandChannel* cmd_channel,
                     bool random_channel_ids);
  ~ChannelManagerImpl() override;

  void AddACLConnection(hci_spec::ConnectionHandle handle, hci_spec::ConnectionRole role,
                        LinkErrorCallback link_error_cb,
                        SecurityUpgradeCallback security_cb) override;

  [[nodiscard]] LEFixedChannels AddLEConnection(hci_spec::ConnectionHandle handle,
                                                hci_spec::ConnectionRole role,
                                                LinkErrorCallback link_error_cb,
                                                LEConnectionParameterUpdateCallback conn_param_cb,
                                                SecurityUpgradeCallback security_cb) override;

  void RemoveConnection(hci_spec::ConnectionHandle handle) override;

  void AssignLinkSecurityProperties(hci_spec::ConnectionHandle handle,
                                    sm::SecurityProperties security) override;

  Channel::WeakPtr OpenFixedChannel(hci_spec::ConnectionHandle connection_handle,
                                    ChannelId channel_id) override;

  void OpenL2capChannel(hci_spec::ConnectionHandle handle, PSM psm, ChannelParameters params,
                        ChannelCallback cb) override;

  bool RegisterService(PSM psm, ChannelParameters params, ChannelCallback cb) override;
  void UnregisterService(PSM psm) override;

  void RequestConnectionParameterUpdate(
      hci_spec::ConnectionHandle handle, hci_spec::LEPreferredConnectionParameters params,
      ConnectionParameterUpdateRequestCallback request_cb) override;

  void AttachInspect(inspect::Node& parent, std::string name) override;

  internal::LogicalLink::WeakPtr LogicalLinkForTesting(hci_spec::ConnectionHandle handle) override;

 private:
  // Returns a handler for data packets received from the Bluetooth controller bound to this object.
  hci::ACLPacketHandler MakeInboundDataHandler();

  // Called when an ACL data packet is received from the controller. This method
  // is responsible for routing the packet to the corresponding LogicalLink.
  void OnACLDataReceived(hci::ACLDataPacketPtr data_packet);

  // Called by the various Register functions. Returns a pointer to the newly
  // added link.
  internal::LogicalLink* RegisterInternal(hci_spec::ConnectionHandle handle, bt::LinkType ll_type,
                                          hci_spec::ConnectionRole role, size_t max_payload_size);

  // If a service (identified by |psm|) requested has been registered, return a ServiceInfo object
  // containing preferred channel parameters and a callback that passes an inbound channel to the
  // registrant. The callback may be called repeatedly to pass multiple channels for |psm|, but
  // should not be stored because the service may be unregistered at a later time. Calls for
  // unregistered services return null.
  std::optional<ServiceInfo> QueryService(hci_spec::ConnectionHandle handle, PSM psm);

  // Maximum sizes for data packet payloads from host to controller.
  size_t max_acl_payload_size_;
  size_t max_le_payload_size_;

  hci::AclDataChannel* acl_data_channel_;
  hci::CommandChannel* cmd_channel_;

  using LinkMap =
      std::unordered_map<hci_spec::ConnectionHandle, std::unique_ptr<internal::LogicalLink>>;
  LinkMap ll_map_;
  inspect::Node ll_node_;

  // Stores packets received on a connection handle before a link for it has
  // been created.
  using PendingPacketMap =
      std::unordered_map<hci_spec::ConnectionHandle, std::queue<hci::ACLDataPacketPtr>>;
  PendingPacketMap pending_packets_;

  // Store information required to create and forward channels for locally-
  // hosted services.
  struct ServiceData {
    void AttachInspect(inspect::Node& parent);
    ServiceInfo info;
    PSM psm;
    inspect::Node node;
    inspect::StringProperty psm_property;
  };
  using ServiceMap = std::unordered_map<PSM, ServiceData>;
  ServiceMap services_;
  inspect::Node services_node_;
  inspect::Node node_;

  // Stored info on whether random channel ids are requested.
  bool random_channel_ids_;

  WeakSelf<ChannelManagerImpl> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ChannelManagerImpl);
};

ChannelManagerImpl::ChannelManagerImpl(hci::AclDataChannel* acl_data_channel,
                                       hci::CommandChannel* cmd_channel, bool random_channel_ids)
    : acl_data_channel_(acl_data_channel),
      cmd_channel_(cmd_channel),
      random_channel_ids_(random_channel_ids),
      weak_self_(this) {
  BT_ASSERT(acl_data_channel_);
  max_acl_payload_size_ = acl_data_channel_->GetBufferInfo().max_data_length();
  max_le_payload_size_ = acl_data_channel_->GetLeBufferInfo().max_data_length();
  acl_data_channel_->SetDataRxHandler(MakeInboundDataHandler());
  bt_log(DEBUG, "l2cap", "initialized");
}

ChannelManagerImpl::~ChannelManagerImpl() {
  bt_log(DEBUG, "l2cap", "shutting down");

  // Explicitly shut down all links to force associated L2CAP channels to
  // release their strong references.
  for (auto& [handle, link] : ll_map_) {
    link->Close();
  }
}

hci::ACLPacketHandler ChannelManagerImpl::MakeInboundDataHandler() {
  return [self = weak_self_.GetWeakPtr()](auto packet) {
    if (self.is_alive()) {
      self->OnACLDataReceived(std::move(packet));
    }
  };
}

void ChannelManagerImpl::AddACLConnection(hci_spec::ConnectionHandle handle,
                                          hci_spec::ConnectionRole role,
                                          LinkErrorCallback link_error_cb,
                                          SecurityUpgradeCallback security_cb) {
  bt_log(DEBUG, "l2cap", "register ACL link (handle: %#.4x)", handle);

  auto* ll = RegisterInternal(handle, bt::LinkType::kACL, role, max_acl_payload_size_);
  ll->set_error_callback(std::move(link_error_cb));
  ll->set_security_upgrade_callback(std::move(security_cb));
}

ChannelManagerImpl::LEFixedChannels ChannelManagerImpl::AddLEConnection(
    hci_spec::ConnectionHandle handle, hci_spec::ConnectionRole role,
    LinkErrorCallback link_error_cb, LEConnectionParameterUpdateCallback conn_param_cb,
    SecurityUpgradeCallback security_cb) {
  bt_log(DEBUG, "l2cap", "register LE link (handle: %#.4x)", handle);

  auto* ll = RegisterInternal(handle, bt::LinkType::kLE, role, max_le_payload_size_);
  ll->set_error_callback(std::move(link_error_cb));
  ll->set_security_upgrade_callback(std::move(security_cb));
  ll->set_connection_parameter_update_callback(std::move(conn_param_cb));

  Channel::WeakPtr att = OpenFixedChannel(handle, kATTChannelId);
  Channel::WeakPtr smp = OpenFixedChannel(handle, kLESMPChannelId);
  BT_ASSERT(att.is_alive());
  BT_ASSERT(smp.is_alive());
  return LEFixedChannels{.att = std::move(att), .smp = std::move(smp)};
}

void ChannelManagerImpl::RemoveConnection(hci_spec::ConnectionHandle handle) {
  bt_log(DEBUG, "l2cap", "unregister link (handle: %#.4x)", handle);

  pending_packets_.erase(handle);
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(DEBUG, "l2cap", "attempt to unregister unknown link (handle: %#.4x)", handle);
    return;
  }

  // Explicitly shut down the link to force associated L2CAP channels to release
  // their strong references.
  iter->second->Close();
  ll_map_.erase(iter);
}

void ChannelManagerImpl::AssignLinkSecurityProperties(hci_spec::ConnectionHandle handle,
                                                      sm::SecurityProperties security) {
  bt_log(DEBUG, "l2cap", "received new security properties (handle: %#.4x)", handle);

  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(DEBUG, "l2cap", "ignoring new security properties on unknown link");
    return;
  }

  iter->second->AssignSecurityProperties(security);
}

Channel::WeakPtr ChannelManagerImpl::OpenFixedChannel(hci_spec::ConnectionHandle handle,
                                                      ChannelId channel_id) {
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(ERROR, "l2cap", "cannot open fixed channel on unknown connection handle: %#.4x", handle);
    return Channel::WeakPtr();
  }

  return iter->second->OpenFixedChannel(channel_id);
}

void ChannelManagerImpl::OpenL2capChannel(hci_spec::ConnectionHandle handle, PSM psm,
                                          ChannelParameters params, ChannelCallback cb) {
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(ERROR, "l2cap", "Cannot open channel on unknown connection handle: %#.4x", handle);
    cb(Channel::WeakPtr());
    return;
  }

  iter->second->OpenChannel(psm, params, std::move(cb));
}

bool ChannelManagerImpl::RegisterService(PSM psm, ChannelParameters params, ChannelCallback cb) {
  // v5.0 Vol 3, Part A, Sec 4.2: PSMs shall be odd and the least significant
  // bit of the most significant byte shall be zero
  if (((psm & 0x0001) != 0x0001) || ((psm & 0x0100) != 0x0000)) {
    return false;
  }

  auto iter = services_.find(psm);
  if (iter != services_.end()) {
    return false;
  }

  ServiceData service{.info = ServiceInfo(params, std::move(cb)), .psm = psm};
  service.AttachInspect(services_node_);
  services_.emplace(psm, std::move(service));
  return true;
}

void ChannelManagerImpl::UnregisterService(PSM psm) { services_.erase(psm); }

void ChannelManagerImpl::RequestConnectionParameterUpdate(
    hci_spec::ConnectionHandle handle, hci_spec::LEPreferredConnectionParameters params,
    ConnectionParameterUpdateRequestCallback request_cb) {
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    bt_log(DEBUG, "l2cap", "ignoring Connection Parameter Update request on unknown link");
    return;
  }

  iter->second->SendConnectionParameterUpdateRequest(params, std::move(request_cb));
}

void ChannelManagerImpl::AttachInspect(inspect::Node& parent, std::string name) {
  if (!parent) {
    return;
  }

  node_ = parent.CreateChild(name);

  services_node_ = node_.CreateChild(kInspectServicesNodeName);
  for (auto& [psm, service] : services_) {
    service.AttachInspect(services_node_);
  }

  ll_node_ = node_.CreateChild(kInspectLogicalLinksNodeName);
  for (auto& [_, ll] : ll_map_) {
    ll->AttachInspect(ll_node_, ll_node_.UniqueName(kInspectLogicalLinkNodePrefix));
  }
}

internal::LogicalLink::WeakPtr ChannelManagerImpl::LogicalLinkForTesting(
    hci_spec::ConnectionHandle handle) {
  auto iter = ll_map_.find(handle);
  if (iter == ll_map_.end()) {
    return internal::LogicalLink::WeakPtr();
  }
  return iter->second->GetWeakPtr();
}

// Called when an ACL data packet is received from the controller. This method
// is responsible for routing the packet to the corresponding LogicalLink.
void ChannelManagerImpl::OnACLDataReceived(hci::ACLDataPacketPtr packet) {
  auto handle = packet->connection_handle();
  TRACE_DURATION("bluetooth", "ChannelManagerImpl::OnDataReceived", "handle", handle);

  auto iter = ll_map_.find(handle);
  PendingPacketMap::iterator pp_iter;

  // If a LogicalLink does not exist, we set up a queue for its packets to be
  // delivered when the LogicalLink gets created.
  if (iter == ll_map_.end()) {
    pp_iter = pending_packets_.emplace(handle, std::queue<hci::ACLDataPacketPtr>()).first;
  } else {
    // A logical link exists. |pp_iter| will be valid only if the drain task has
    // not run yet (see ChannelManagerImpl::RegisterInternal()).
    pp_iter = pending_packets_.find(handle);
  }

  if (pp_iter != pending_packets_.end()) {
    packet->set_trace_id(TRACE_NONCE());
    TRACE_FLOW_BEGIN("bluetooth", "ChannelMaager::OnDataReceived queued", packet->trace_id());
    pp_iter->second.push(std::move(packet));
    bt_log(TRACE, "l2cap", "queued rx packet on handle: %#.4x", handle);
    return;
  }

  iter->second->HandleRxPacket(std::move(packet));
}

internal::LogicalLink* ChannelManagerImpl::RegisterInternal(hci_spec::ConnectionHandle handle,
                                                            bt::LinkType ll_type,
                                                            hci_spec::ConnectionRole role,
                                                            size_t max_payload_size) {
  TRACE_DURATION("bluetooth", "ChannelManagerImpl::RegisterInternal", "handle", handle);

  // TODO(armansito): Return nullptr instead of asserting. Callers shouldn't
  // assume this will succeed.
  auto iter = ll_map_.find(handle);
  BT_DEBUG_ASSERT_MSG(iter == ll_map_.end(), "connection handle re-used! (handle=%#.4x)", handle);

  auto ll = std::make_unique<internal::LogicalLink>(
      handle, ll_type, role, max_payload_size,
      fit::bind_member<&ChannelManagerImpl::QueryService>(this), acl_data_channel_, cmd_channel_,
      random_channel_ids_);
  if (ll_node_) {
    ll->AttachInspect(ll_node_, ll_node_.UniqueName(kInspectLogicalLinkNodePrefix));
  }

  // Route all pending packets to the link.
  auto pp_iter = pending_packets_.find(handle);
  if (pp_iter != pending_packets_.end()) {
    auto& packets = pp_iter->second;
    while (!packets.empty()) {
      auto packet = std::move(packets.front());
      packets.pop();
      TRACE_FLOW_END("bluetooth", "ChannelManagerImpl::OnDataReceived queued", packet->trace_id());
      ll->HandleRxPacket(std::move(packet));
    }
    pending_packets_.erase(pp_iter);
  }

  auto* ll_raw = ll.get();
  ll_map_[handle] = std::move(ll);

  return ll_raw;
}

std::optional<ChannelManager::ServiceInfo> ChannelManagerImpl::QueryService(
    hci_spec::ConnectionHandle handle, PSM psm) {
  auto iter = services_.find(psm);
  if (iter == services_.end()) {
    return std::nullopt;
  }

  // |channel_cb| will be called in LogicalLink. Each callback in |services_| already trampolines
  // to the appropriate dispatcher (passed to RegisterService).
  return ServiceInfo(iter->second.info.channel_params, iter->second.info.channel_cb.share());
}

void ChannelManagerImpl::ServiceData::AttachInspect(inspect::Node& parent) {
  if (!parent) {
    return;
  }
  node = parent.CreateChild(parent.UniqueName(kInspectServiceNodePrefix));
  psm_property = node.CreateString(kInspectPsmPropertyName, PsmToString(psm));
}

std::unique_ptr<ChannelManager> ChannelManager::Create(hci::AclDataChannel* acl_data_channel,
                                                       hci::CommandChannel* cmd_channel,
                                                       bool random_channel_ids) {
  return std::make_unique<ChannelManagerImpl>(acl_data_channel, cmd_channel, random_channel_ids);
}

}  // namespace bt::l2cap
