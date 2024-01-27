// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_

#include "src/connectivity/bluetooth/core/bt-host/gatt/fake_client.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt/gatt.h"

namespace bt::gatt::testing {

// This is a fake version of the root GATT object that can be injected in unit
// tests.
class FakeLayer final : public GATT {
 public:
  struct Update {
    IdType chrc_id;
    std::vector<uint8_t> value;
    IndicationCallback indicate_cb;
    std::optional<PeerId> peer;
  };

  struct LocalService {
    std::unique_ptr<Service> service;
    ReadHandler read_handler;
    WriteHandler write_handler;
    ClientConfigCallback ccc_callback;
    std::vector<Update> updates;
  };

  FakeLayer() = default;
  ~FakeLayer() override = default;

  // Create a new peer GATT service. Creates a peer entry if it doesn't already exist.
  // Replaces an existing service with the same handle if it exists.
  // Notifies the remote service watcher if |notify| is true.
  //
  // Returns the fake remote service and a handle to the fake object.
  //
  // NOTE: the remote service watcher can also get triggered by calling InitializeClient().
  std::pair<RemoteService::WeakPtr, FakeClient::WeakPtr> AddPeerService(PeerId peer_id,
                                                                        const ServiceData& info,
                                                                        bool notify = false);

  // Removes the service with start handle of |handle| and notifies service watcher.
  void RemovePeerService(PeerId peer_id, att::Handle handle);

  // Assign a callback to be notified when a request is made to initialize the client.
  using InitializeClientCallback = fit::function<void(PeerId, std::vector<UUID>)>;
  void SetInitializeClientCallback(InitializeClientCallback cb);

  // Assign the status that will be returned by the ListServices callback.
  void set_list_services_status(att::Result<>);

  // Ignore future calls to ListServices().
  void stop_list_services() { pause_list_services_ = true; }

  // Assign a callback to be notified when the persist service changed CCC callback is set.
  using SetPersistServiceChangedCCCCallbackCallback = fit::function<void()>;
  void SetSetPersistServiceChangedCCCCallbackCallback(
      SetPersistServiceChangedCCCCallbackCallback cb);

  // Assign a callback to be notified when the retrieve service changed CCC callback is set.
  using SetRetrieveServiceChangedCCCCallbackCallback = fit::function<void()>;
  void SetSetRetrieveServiceChangedCCCCallbackCallback(
      SetRetrieveServiceChangedCCCCallbackCallback cb);

  // Directly force the fake layer to call the persist service changed CCC callback, to test the
  // GAP adapter and peer cache.
  void CallPersistServiceChangedCCCCallback(PeerId peer_id, bool notify, bool indicate);

  // Directly force the fake layer to call the retrieve service changed CCC callback, to test the
  // GAP adapter and peer cache.
  std::optional<ServiceChangedCCCPersistedData> CallRetrieveServiceChangedCCCCallback(
      PeerId peer_id);

  Service* FindLocalServiceById(IdType id) {
    return local_services_.count(id) ? local_services_[id].service.get() : nullptr;
  }

  std::map<IdType, LocalService>& local_services() { return local_services_; }

  // If true, cause all calls to RegisterService() to fail.
  void set_register_service_fails(bool fails) { register_service_fails_ = fails; }

  // GATT overrides:
  void AddConnection(PeerId peer_id, std::unique_ptr<Client> client,
                     Server::FactoryFunction server_factory) override;
  void RemoveConnection(PeerId peer_id) override;
  PeerMtuListenerId RegisterPeerMtuListener(PeerMtuListener listener) override;
  bool UnregisterPeerMtuListener(PeerMtuListenerId listener_id) override;
  void RegisterService(ServicePtr service, ServiceIdCallback callback, ReadHandler read_handler,
                       WriteHandler write_handler, ClientConfigCallback ccc_callback) override;
  void UnregisterService(IdType service_id) override;
  void SendUpdate(IdType service_id, IdType chrc_id, PeerId peer_id, ::std::vector<uint8_t> value,
                  IndicationCallback indicate_cb) override;
  void UpdateConnectedPeers(IdType service_id, IdType chrc_id, ::std::vector<uint8_t> value,
                            IndicationCallback indicate_cb) override;
  void SetPersistServiceChangedCCCCallback(PersistServiceChangedCCCCallback callback) override;
  void SetRetrieveServiceChangedCCCCallback(RetrieveServiceChangedCCCCallback callback) override;
  void InitializeClient(PeerId peer_id, std::vector<UUID> services_to_discover) override;
  RemoteServiceWatcherId RegisterRemoteServiceWatcherForPeer(PeerId peer_id,
                                                             RemoteServiceWatcher watcher) override;
  bool UnregisterRemoteServiceWatcher(RemoteServiceWatcherId watcher_id) override;
  void ListServices(PeerId peer_id, std::vector<UUID> uuids, ServiceListCallback callback) override;
  RemoteService::WeakPtr FindService(PeerId peer_id, IdType service_id) override;

  using WeakPtr = WeakSelf<FakeLayer>::WeakPtr;
  FakeLayer::WeakPtr GetFakePtr() { return weak_fake_.GetWeakPtr(); }

 private:
  IdType next_local_service_id_ = 100;  // Start at a random large ID to help catch bugs (e.g.
                                        // FIDL IDs mixed up with internal IDs).
  std::map<IdType, LocalService> local_services_;

  bool register_service_fails_ = false;

  // Test callbacks
  InitializeClientCallback initialize_client_cb_;
  SetPersistServiceChangedCCCCallbackCallback set_persist_service_changed_ccc_cb_cb_;
  SetRetrieveServiceChangedCCCCallbackCallback set_retrieve_service_changed_ccc_cb_cb_;

  // Emulated callbacks
  std::unordered_map<PeerId, RemoteServiceWatcher> remote_service_watchers_;

  PersistServiceChangedCCCCallback persist_service_changed_ccc_cb_;
  RetrieveServiceChangedCCCCallback retrieve_service_changed_ccc_cb_;

  att::Result<> list_services_status_ = fit::ok();
  bool pause_list_services_ = false;

  // Emulated GATT peer.
  struct TestPeer {
    TestPeer();

    FakeClient fake_client;
    std::unordered_map<IdType, std::unique_ptr<RemoteService>> services;

    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(TestPeer);
  };
  std::unordered_map<PeerId, TestPeer> peers_;

  WeakSelf<FakeLayer> weak_fake_{this};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeLayer);
};

}  // namespace bt::gatt::testing

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_
