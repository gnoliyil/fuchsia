// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_PROFILE_SERVER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_PROFILE_SERVER_H_

#include <fuchsia/bluetooth/bredr/cpp/fidl.h>

#include <fbl/ref_counted.h>

#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/fidl/server_base.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_connection_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/sdp/server.h"
#include "src/connectivity/bluetooth/core/bt-host/socket/socket_factory.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace bthost {

// Implements the bredr::Profile FIDL interface.
class ProfileServer : public ServerBase<fuchsia::bluetooth::bredr::Profile> {
 public:
  ProfileServer(bt::gap::Adapter::WeakPtr adapter,
                fidl::InterfaceRequest<fuchsia::bluetooth::bredr::Profile> request);
  ~ProfileServer() override;

 private:
  class AudioDirectionExt;

  class L2capParametersExt final
      : public ServerBase<fuchsia::bluetooth::bredr::L2capParametersExt> {
   public:
    L2capParametersExt(
        fidl::InterfaceRequest<fuchsia::bluetooth::bredr::L2capParametersExt> request,
        fxl::WeakPtr<bt::l2cap::Channel> channel)
        : ServerBase(this, std::move(request)),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)) {}

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    void RequestParameters(fuchsia::bluetooth::bredr::ChannelParameters requested,
                           RequestParametersCallback callback) override;

   private:
    bt::l2cap::Channel::UniqueId unique_id_;
    fxl::WeakPtr<bt::l2cap::Channel> channel_;
  };

  class AudioOffloadExt final : public ServerBase<fuchsia::bluetooth::bredr::AudioOffloadExt> {
   public:
    AudioOffloadExt(fidl::InterfaceRequest<fuchsia::bluetooth::bredr::AudioOffloadExt> request,
                    fxl::WeakPtr<bt::l2cap::Channel> channel, bt::gap::Adapter::WeakPtr adapter)
        : ServerBase(this, std::move(request)),
          channel_(std::move(channel)),
          adapter_(std::move(adapter)) {}
    void GetSupportedFeatures(GetSupportedFeaturesCallback callback) override;
    void StartAudioOffload(fuchsia::bluetooth::bredr::AudioOffloadConfiguration configuration,
                           fidl::InterfaceRequest<fuchsia::bluetooth::bredr::AudioOffloadController>
                               controller) override {}

   private:
    fxl::WeakPtr<bt::l2cap::Channel> channel_;
    bt::gap::Adapter::WeakPtr adapter_;
  };

  class ScoConnectionServer final : public ServerBase<fuchsia::bluetooth::bredr::ScoConnection> {
   public:
    ScoConnectionServer(fidl::InterfaceRequest<fuchsia::bluetooth::bredr::ScoConnection> request,
                        fxl::WeakPtr<bt::sco::ScoConnection> connection);
    ~ScoConnectionServer() override;
    void Activate(fit::callback<void()> on_closed);
    void Read(ReadCallback callback) override;
    void Write(std::vector<uint8_t> data, WriteCallback callback) override;

   private:
    void TryRead();
    void Close(zx_status_t epitaph);
    fxl::WeakPtr<bt::sco::ScoConnection> connection_;
    fit::callback<void()> on_closed_;
    // Non-null when a read request is waiting for an inbound packet.
    fit::callback<void(fuchsia::bluetooth::bredr::RxPacketStatus, std::vector<uint8_t>)> read_cb_;
  };

  struct ScoRequest : public fbl::RefCounted<ScoRequest> {
    std::optional<bt::gap::BrEdrConnectionManager::ScoRequestHandle> request_handle;
    fuchsia::bluetooth::bredr::ScoConnectionReceiverPtr receiver;
    std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters> parameters;
  };

  // fuchsia::bluetooth::bredr::Profile overrides:
  void Advertise(std::vector<fuchsia::bluetooth::bredr::ServiceDefinition> definitions,
                 fuchsia::bluetooth::bredr::ChannelParameters parameters,
                 fidl::InterfaceHandle<fuchsia::bluetooth::bredr::ConnectionReceiver> receiver,
                 AdvertiseCallback callback) override;
  void Search(fuchsia::bluetooth::bredr::ServiceClassProfileIdentifier service_uuid,
              std::vector<uint16_t> attr_ids,
              fidl::InterfaceHandle<fuchsia::bluetooth::bredr::SearchResults> results) override;
  void Connect(fuchsia::bluetooth::PeerId peer_id,
               fuchsia::bluetooth::bredr::ConnectParameters connection,
               ConnectCallback callback) override;
  void ConnectSco(
      fuchsia::bluetooth::PeerId fidl_peer_id, bool initiator,
      std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters> fidl_params,
      fidl::InterfaceHandle<fuchsia::bluetooth::bredr::ScoConnectionReceiver> receiver) override;

  // Callback when clients close their connection targets
  void OnConnectionReceiverError(uint64_t ad_id);

  // Callback when clients close their search results
  void OnSearchResultError(uint64_t search_id, zx_status_t status);

  // Callback for incoming connections
  void OnChannelConnected(uint64_t ad_id, fxl::WeakPtr<bt::l2cap::Channel> channel,
                          const bt::sdp::DataElement& protocol_list);

  // Callback for services found on connected device
  void OnServiceFound(uint64_t search_id, bt::PeerId peer_id,
                      const std::map<bt::sdp::AttributeId, bt::sdp::DataElement>& attributes);

  // Callback for SCO connections requests.
  void OnScoConnectionResult(fbl::RefPtr<ScoRequest> request,
                             bt::sco::ScoConnectionManager::AcceptConnectionResult);

  // Callback when clients close their audio direction extension.
  void OnAudioDirectionExtError(AudioDirectionExt* ext_server, zx_status_t status);

  // Create an AudioDirectionExt server for the given channel and set up callbacks.
  // Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::AudioDirectionExt> BindAudioDirectionExtServer(
      fxl::WeakPtr<bt::l2cap::Channel> channel);

  // Callback when clients close their l2cap parameters extension.
  void OnL2capParametersExtError(L2capParametersExt* ext_server, zx_status_t status);

  // Create an L2capParametersExt server for the given channel and set up callbacks.
  // Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::L2capParametersExt> BindL2capParametersExtServer(
      fxl::WeakPtr<bt::l2cap::Channel> channel);

  // Callback when clients close their audio offload extension.
  void OnAudioOffloadExtError(AudioOffloadExt* ext_server, zx_status_t status);

  // Create an AudioOffloadExt server for the given channel and set up callbacks.
  // Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::AudioOffloadExt> BindAudioOffloadExtServer(
      fxl::WeakPtr<bt::l2cap::Channel> channel);

  // Create a FIDL Channel from an l2cap::Channel. A socket channel relay is created from |channel|
  // and returned in the FIDL Channel.
  fuchsia::bluetooth::bredr::Channel ChannelToFidl(fxl::WeakPtr<bt::l2cap::Channel> channel);

  const bt::gap::Adapter::WeakPtr& adapter() const { return adapter_; }

  // Advertised Services
  struct AdvertisedService {
    AdvertisedService(fidl::InterfacePtr<fuchsia::bluetooth::bredr::ConnectionReceiver> receiver,
                      bt::sdp::Server::RegistrationHandle registration_handle,
                      AdvertiseCallback disconnection_cb)
        : receiver(std::move(receiver)),
          registration_handle(registration_handle),
          disconnection_cb(std::move(disconnection_cb)) {}
    fidl::InterfacePtr<fuchsia::bluetooth::bredr::ConnectionReceiver> receiver;
    bt::sdp::Server::RegistrationHandle registration_handle;
    AdvertiseCallback disconnection_cb;
  };

  uint64_t advertised_total_;
  std::map<uint64_t, AdvertisedService> current_advertised_;

  // Searches registered
  struct RegisteredSearch {
    RegisteredSearch(fidl::InterfacePtr<fuchsia::bluetooth::bredr::SearchResults> results,
                     bt::gap::BrEdrConnectionManager::SearchId search_id)
        : results(std::move(results)), search_id(search_id) {}
    fidl::InterfacePtr<fuchsia::bluetooth::bredr::SearchResults> results;
    bt::gap::BrEdrConnectionManager::SearchId search_id;
  };

  uint64_t searches_total_;
  std::map<uint64_t, RegisteredSearch> searches_;

  class AudioDirectionExt final : public ServerBase<fuchsia::bluetooth::bredr::AudioDirectionExt> {
   public:
    // Calls to SetPriority() are forwarded to |priority_cb|.
    AudioDirectionExt(fidl::InterfaceRequest<fuchsia::bluetooth::bredr::AudioDirectionExt> request,
                      fxl::WeakPtr<bt::l2cap::Channel> channel)
        : ServerBase(this, std::move(request)),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)){};

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    // fuchsia::bluetooth::bredr::AudioDirectionExt overrides:
    void SetPriority(fuchsia::bluetooth::bredr::A2dpDirectionPriority priority,
                     SetPriorityCallback callback) override;

   private:
    bt::l2cap::Channel::UniqueId unique_id_;
    fxl::WeakPtr<bt::l2cap::Channel> channel_;
  };
  std::unordered_map<bt::l2cap::Channel::UniqueId, std::unique_ptr<AudioDirectionExt>>
      audio_direction_ext_servers_;

  std::unordered_map<bt::l2cap::Channel::UniqueId, std::unique_ptr<L2capParametersExt>>
      l2cap_parameters_ext_servers_;

  std::unordered_map<AudioOffloadExt*, std::unique_ptr<AudioOffloadExt>> audio_offload_ext_servers_;

  bt::gap::Adapter::WeakPtr adapter_;

  // Creates sockets that bridge L2CAP channels to profile processes.
  bt::socket::SocketFactory<bt::l2cap::Channel> l2cap_socket_factory_;

  std::unordered_map<bt::sco::ScoConnection*, std::unique_ptr<ScoConnectionServer>>
      sco_connection_servers_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  fxl::WeakPtrFactory<ProfileServer> weak_ptr_factory_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ProfileServer);
};

}  // namespace bthost

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_PROFILE_SERVER_H_
