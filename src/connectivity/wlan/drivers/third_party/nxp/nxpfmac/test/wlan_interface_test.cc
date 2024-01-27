// Copyright (c) 2022 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/wlan_interface.h"

#include <lib/async/cpp/task.h>
#include <stdlib.h>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/data_plane.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device_context.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ioctl_adapter.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/mlan_mocks.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/mock_bus.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/mock_fullmac_ifc.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/test_data_plane.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

using wlan::nxpfmac::DeviceContext;
using wlan::nxpfmac::WlanInterface;

namespace {

constexpr char kFullmacClientDeviceName[] = "test-client-fullmac-ifc";
constexpr char kFullmacSoftApDeviceName[] = "test-softap-fullmac-ifc";
constexpr uint8_t kClientMacAddress[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
constexpr uint8_t kSoftApMacAddress[] = {0x05, 0x04, 0x03, 0x02, 0x01, 0x00};

using wlan::nxpfmac::Device;

// Barebones Device Class (at this time mainly for the dispatcher to handle timers)
struct TestDevice : public Device {
 public:
  static zx_status_t Create(zx_device_t* parent, async_dispatcher_t* dispatcher,
                            sync_completion_t* destruction_compl, TestDevice** out_device) {
    auto device = new TestDevice(parent, dispatcher, destruction_compl);

    *out_device = device;
    return ZX_OK;
  }

  ~TestDevice() {
    if (ifc_) {
      ifc_->Remove([ddk_done = ddk_done_]() mutable {
        if (ddk_done)
          sync_completion_signal(ddk_done);
      });
      ifc_ = nullptr;
    }
  }

  zx_status_t CreateInterface(zx_device_t* parent, const char* name, uint32_t iface_index,
                              wlan_mac_role_t role, DeviceContext* context,
                              const uint8_t mac_address[ETH_ALEN], zx::channel&& mlme_channel) {
    zx_status_t status = WlanInterface::Create(parent, name, iface_index, role, context,
                                               mac_address, std::move(mlme_channel), &ifc_);
    return status;
  }
  WlanInterface* GetInterface() { return ifc_; }

  async_dispatcher_t* GetDispatcher() override { return dispatcher_; }

 private:
  TestDevice(zx_device_t* parent, async_dispatcher_t* dispatcher,
             sync_completion_t* destructor_done)
      : Device(parent) {
    ddk_done_ = destructor_done;
    dispatcher_ = dispatcher;
  }

 protected:
  zx_status_t Init(mlan_device* mlan_dev, wlan::nxpfmac::BusInterface** out_bus) override {
    return ZX_OK;
  }
  zx_status_t LoadFirmware(const char* path, zx::vmo* out_fw, size_t* out_size) override {
    return ZX_OK;
  }
  void Shutdown() override {}

  sync_completion_t* ddk_done_ = nullptr;
  async_dispatcher_t* dispatcher_;
  WlanInterface* ifc_ = nullptr;
  DeviceContext context_;
  wlan::nxpfmac::MockBus bus_;

 public:
};

struct WlanInterfaceTest : public zxtest::Test, public wlan::nxpfmac::DataPlaneIfc {
  void SetUp() override {
    // TODO(fxb/124464): Migrate test to use dispatcher integration.
    parent_ = MockDevice::FakeRootParentNoDispatcherIntegrationDEPRECATED();
    ASSERT_OK(wlan::nxpfmac::TestDataPlane::Create(this, &mock_bus_, mlan_mocks_.GetAdapter(),
                                                   &test_data_plane_));

    // The last device added right after creating the dataplane should be the network device.
    SetupNetDevice(test_data_plane_->GetNetDevice());

    auto ioctl_adapter = wlan::nxpfmac::IoctlAdapter::Create(mlan_mocks_.GetAdapter(), &mock_bus_);
    ASSERT_OK(ioctl_adapter.status_value());
    ioctl_adapter_ = std::move(ioctl_adapter.value());
    dispatcher_loop_ = std::make_unique<::async::Loop>(&kAsyncLoopConfigNeverAttachToThread);
    ASSERT_OK(dispatcher_loop_->StartThread());
    ASSERT_OK(TestDevice::Create(parent_.get(), env_.GetDispatcher(), &device_destructed_,
                                 &test_device_));
    context_.event_handler_ = &event_handler_;
    context_.ioctl_adapter_ = ioctl_adapter_.get();
    context_.data_plane_ = test_data_plane_->GetDataPlane();
    context_.device_ = test_device_;
  }

  void TearDown() override {
    // Destroy the dataplane before the mock device. This ensures a safe destruction before the
    // parent device of the NetworkDeviceImpl device goes away.
    test_data_plane_.reset();
    delete context_.device_;
    context_.device_ = nullptr;
    parent_.reset();
  }

  zx_status_t CreateDeviceInterface(zx_device_t* parent, const char* name, uint32_t iface_index,
                                    wlan_mac_role_t role, DeviceContext* context,
                                    const uint8_t mac_address[ETH_ALEN],
                                    zx::channel&& mlme_channel) {
    return test_device_->CreateInterface(parent, name, iface_index, role, context, mac_address,
                                         std::move(mlme_channel));
  }

  WlanInterface* GetInterface() { return test_device_->GetInterface(); }

  void SetupNetDevice(zx_device* net_device) {
    // Because WlanInterface creates a port for the netdevice we need to provide a limited
    // implementation of the netdevice ifc.
    network_device_impl_protocol_t netdev_proto;
    ASSERT_OK(device_get_protocol(net_device, ZX_PROTOCOL_NETWORK_DEVICE_IMPL, &netdev_proto));
    ASSERT_OK(
        network_device_impl_init(&netdev_proto, netdev_ifc_proto_.ctx, netdev_ifc_proto_.ops));
  }

  static void OnAddPort(void* ctx, uint8_t, const network_port_protocol_t* proto) {
    auto ifc = static_cast<WlanInterfaceTest*>(ctx);
    ifc->net_port_proto_ = *proto;
    EXPECT_NOT_NULL(proto->ctx);
    EXPECT_NOT_NULL(proto->ops);
    sync_completion_signal(&ifc->on_add_port_called_);
  }
  static void OnRemovePort(void* ctx, uint8_t) {
    auto ifc = static_cast<WlanInterfaceTest*>(ctx);
    sync_completion_signal(&ifc->on_remove_port_called_);
    if (ifc->net_port_proto_.ctx && ifc->net_port_proto_.ops) {
      network_port_removed(&ifc->net_port_proto_);
    }
  }

  mlan_status HandleConnectScan(pmlan_ioctl_req req) {
    auto scan = reinterpret_cast<mlan_ds_scan*>(req->pbuf);
    if (scan->sub_command != MLAN_OID_SCAN_USER_CONFIG) {
      // This wasn't a scan request, fail it so the tests can be updated to handle it. If the event
      // is triggered syncrhonously the tests will deadlock.
      return MLAN_STATUS_FAILURE;
    }
    // Asynchronously post an event about scan results.
    async::PostTask(dispatcher_loop_->dispatcher(), [this]() {
      mlan_event event{.event_id = MLAN_EVENT_ID_DRV_SCAN_REPORT};
      event_handler_.OnEvent(&event);
    });
    return MLAN_STATUS_SUCCESS;
  }

  // DataPlaneIfc implementation
  void OnEapolTransmitted(wlan::drivers::components::Frame&& frame, zx_status_t status) override {
    sync_completion_signal(&on_eapol_transmitted_called_);
  }
  void OnEapolReceived(wlan::drivers::components::Frame&& frame) override {
    sync_completion_signal(&on_eapol_received_called_);
  }

  network_device_ifc_protocol_ops_t netdev_ifc_proto_ops_{.add_port = &OnAddPort,
                                                          .remove_port = &OnRemovePort};
  network_device_ifc_protocol_t netdev_ifc_proto_{.ops = &netdev_ifc_proto_ops_, .ctx = this};

  network_port_protocol_t net_port_proto_;
  sync_completion_t on_add_port_called_;
  sync_completion_t on_remove_port_called_;
  sync_completion_t on_eapol_transmitted_called_;
  sync_completion_t on_eapol_received_called_;

  wlan::simulation::Environment env_;
  std::unique_ptr<async::Loop> dispatcher_loop_ = {};
  TestDevice* test_device_ = nullptr;
  sync_completion_t device_destructed_;
  wlan::nxpfmac::EventHandler event_handler_;
  wlan::nxpfmac::MockBus mock_bus_;
  wlan::nxpfmac::MlanMockAdapter mlan_mocks_;
  std::unique_ptr<wlan::nxpfmac::IoctlAdapter> ioctl_adapter_;
  std::unique_ptr<wlan::nxpfmac::TestDataPlane> test_data_plane_;
  DeviceContext context_;
  // This data member MUST BE LAST, because it needs to be destroyed first, ensuring that whatever
  // interface lifetimes are managed by it are destroyed before other data members.
  std::shared_ptr<MockDevice> parent_;
};

TEST_F(WlanInterfaceTest, Construct) {
  // Test that an interface can be constructed and that its lifetime is managed correctly by the
  // mock device parent. It will call release on the interface and destroy it that way.

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, zx::channel()));
}

TEST_F(WlanInterfaceTest, WlanFullmacImplStartClient) {
  // Test that WlanFullmacImplStart works and correctly passes the MLME channel back.

  zx::channel in_client_mlme_channel;
  zx::channel unused;
  WlanInterface* ifc;
  ASSERT_OK(zx::channel::create(0, &in_client_mlme_channel, &unused));

  const zx_handle_t client_mlme_channel_handle = in_client_mlme_channel.get();

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, std::move(in_client_mlme_channel)));
  ifc = GetInterface();

  const wlan_fullmac_impl_ifc_protocol_t fullmac_ifc{.ops = nullptr, .ctx = this};
  zx::channel out_client_mlme_channel;
  ifc->WlanFullmacImplStart(&fullmac_ifc, &out_client_mlme_channel);

  // Verify that the channel we get back from starting is the same we passed in during construction.
  // The one passed in during construction will be the one passed through wlanphy and we have to
  // pass it back here.
  ASSERT_EQ(client_mlme_channel_handle, out_client_mlme_channel.get());
}

TEST_F(WlanInterfaceTest, WlanFullmacImplStartSoftAp) {
  zx::channel in_softap_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_softap_mlme_channel, &unused));

  const zx_handle_t softap_mlme_channel_handle = in_softap_mlme_channel.get();

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacSoftApDeviceName, 0, WLAN_MAC_ROLE_AP,
                                  &context_, kSoftApMacAddress, std::move(in_softap_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  const wlan_fullmac_impl_ifc_protocol_t fullmac_ifc{.ops = nullptr, .ctx = this};
  zx::channel out_softap_mlme_channel;
  ifc->WlanFullmacImplStart(&fullmac_ifc, &out_softap_mlme_channel);

  // Verify that the channel we get back from starting is the same we passed in during construction.
  // The one passed in during construction will be the one passed through wlanphy and we have to
  // pass it back here.
  ASSERT_EQ(softap_mlme_channel_handle, out_softap_mlme_channel.get());
}

TEST_F(WlanInterfaceTest, WlanFullmacImplQuery) {
  // Test that WlanFullmacImplQuery returns some reasonable values

  constexpr uint8_t kChannels[] = {1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
                                   36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108, 112, 116,
                                   120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 255};
  constexpr size_t kNum2gChannels = 13;
  constexpr size_t kNum5gChannels = 26;

  constexpr wlan_mac_role_t kRole = WLAN_MAC_ROLE_AP;
  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, kRole, &context_,
                                  kSoftApMacAddress, zx::channel()));
  WlanInterface* ifc = GetInterface();

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->req_id == MLAN_IOCTL_BSS && req->action == MLAN_ACT_GET) {
      // Get the supported channel list.
      auto bss = reinterpret_cast<mlan_ds_bss*>(req->pbuf);
      EXPECT_EQ(MLAN_OID_BSS_CHANNEL_LIST, bss->sub_command);
      chan_freq* chan = bss->param.chanlist.cf;
      for (auto channel : kChannels) {
        (chan++)->channel = channel;
      }
      bss->param.chanlist.num_of_chan = std::size(kChannels);
    }
    return MLAN_STATUS_SUCCESS;
  });

  wlan_fullmac_query_info_t info{};
  ifc->WlanFullmacImplQuery(&info);

  // Should match the role we provided at construction
  ASSERT_EQ(kRole, info.role);

  // Should support both bands
  ASSERT_EQ(2u, info.band_cap_count);
  EXPECT_EQ(WLAN_BAND_TWO_GHZ, info.band_cap_list[0].band);
  EXPECT_EQ(WLAN_BAND_FIVE_GHZ, info.band_cap_list[1].band);

  // Should support a non-zero number of rates and channels for 2.4 GHz
  EXPECT_NE(0, info.band_cap_list[0].basic_rate_count);
  EXPECT_EQ(kNum2gChannels, info.band_cap_list[0].operating_channel_count);
  EXPECT_BYTES_EQ(kChannels, info.band_cap_list[0].operating_channel_list, kNum2gChannels);

  // Should support a non-zero number of rates and channels for 5 GHz
  EXPECT_NE(0, info.band_cap_list[1].basic_rate_count);
  EXPECT_EQ(kNum5gChannels, info.band_cap_list[1].operating_channel_count);
  EXPECT_BYTES_EQ(kChannels + kNum2gChannels, info.band_cap_list[1].operating_channel_list,
                  kNum5gChannels);
}

TEST_F(WlanInterfaceTest, WlanFullmacImplQueryMacSublayerSupport) {
  // Test that the most important values are configured in the mac sublayer support.

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, zx::channel()));
  WlanInterface* ifc = GetInterface();

  mac_sublayer_support_t sublayer_support;
  ifc->WlanFullmacImplQueryMacSublayerSupport(&sublayer_support);

  // Data plan must be network device
  ASSERT_EQ(DATA_PLANE_TYPE_GENERIC_NETWORK_DEVICE, sublayer_support.data_plane.data_plane_type);
  // This is a fullmac device.
  ASSERT_EQ(MAC_IMPLEMENTATION_TYPE_FULLMAC, sublayer_support.device.mac_implementation_type);
}

TEST_F(WlanInterfaceTest, WlanFullmacImplStartScan) {
  // Test that calling start scan will eventually issue a scan IOCTL, more detailed scan tests exist
  // in the dedicated scanner tests.

  constexpr uint64_t kScanTxnId = 0x34435457;

  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_SCAN) {
      // Start scan, has to be completed asynchronously.
      ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
      return MLAN_STATUS_PENDING;
    }
    if (req->action == MLAN_ACT_GET && req->req_id == MLAN_IOCTL_SCAN) {
      // Get scan results
      auto scan_req = reinterpret_cast<mlan_ds_scan*>(req->pbuf);
      // Make it easy for ourselves by saying there are no scan results.
      scan_req->param.scan_resp.num_in_scan_table = 0;
      return MLAN_STATUS_SUCCESS;
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  const wlan_fullmac_scan_req_t scan_request{
      .txn_id = kScanTxnId,
      .scan_type = WLAN_SCAN_TYPE_ACTIVE,
  };
  ifc->WlanFullmacImplStartScan(&scan_request);

  // Because there are no scan results we just expect a scan end. We really only need to verify that
  // the scanner was called as a result of our StartScan call.
  sync_completion_t scan_end_called;
  mock_fullmac_ifc.on_scan_end.ExpectCallWithMatcher([&](const wlan_fullmac_scan_end_t* scan_end) {
    EXPECT_EQ(kScanTxnId, scan_end->txn_id);
    sync_completion_signal(&scan_end_called);
  });

  mlan_event scan_report_event{.event_id = MLAN_EVENT_ID_DRV_SCAN_REPORT};

  // Send a report indicating there's a scan report.
  event_handler_.OnEvent(&scan_report_event);

  ASSERT_OK(sync_completion_wait(&scan_end_called, ZX_TIME_INFINITE));

  mock_fullmac_ifc.on_scan_end.VerifyAndClear();
}

TEST_F(WlanInterfaceTest, WlanFullmacImplConnectDisconnectReq) {
  // Test that a connect request results in a successful connection. Also verify that a signal
  // report indication is received after a successful connection. And verify the client
  // disconnects when a local disconnect request is injected. More detailed tests exist in
  // the dedicated ClientConnection tests.

  constexpr int8_t kTestRssi = -64;
  constexpr int8_t kTestSnr = 28;
  constexpr zx::duration kSignalLogTimeout = zx::sec(30);
  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_BSS) {
      // Connect request, must complete asynchronously.
      ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
      return MLAN_STATUS_PENDING;
    }
    if (req->action == MLAN_ACT_GET && req->req_id == MLAN_IOCTL_GET_INFO) {
      auto signal_info = reinterpret_cast<mlan_ds_get_info*>(req->pbuf);
      if (signal_info->sub_command == MLAN_OID_GET_SIGNAL) {
        // Get signal request, must return asynchronously.
        signal_info->param.signal.data_snr_avg = kTestSnr;
        signal_info->param.signal.data_rssi_avg = kTestRssi;
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        return MLAN_STATUS_PENDING;
      }
    }
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_SCAN) {
      // As part of connecting WlanInterface will perform a connect scan, make it succeed.
      return HandleConnectScan(req);
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  sync_completion_t on_connect_conf_called;
  mock_fullmac_ifc.on_connect_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_connect_confirm_t* resp) {
        sync_completion_signal(&on_connect_conf_called);
      });

  // Prepare and send the connect request.
  constexpr uint8_t kIesWithSsid[] = {"\x00\x04Test"};
  constexpr uint8_t kTestChannel = 1;
  const wlan_fullmac_connect_req_t connect_request = {
      .selected_bss{.ies_list = kIesWithSsid,
                    .ies_count = sizeof(kIesWithSsid),
                    .channel{.primary = kTestChannel}},
      .auth_type = WLAN_AUTH_TYPE_OPEN_SYSTEM};

  ifc->WlanFullmacImplConnectReq(&connect_request);

  sync_completion_t on_signal_ind_called;
  mock_fullmac_ifc.on_signal_report.ExpectCallWithMatcher(
      [&](const wlan_fullmac_signal_report_indication_t* report) {
        EXPECT_EQ(report->rssi_dbm, kTestRssi);
        EXPECT_EQ(report->snr_db, kTestSnr);
        sync_completion_signal(&on_signal_ind_called);
      });

  // Wait until the timer has been scheduled.
  while (1) {
    if (env_.GetLatestEventTime().get() >= kSignalLogTimeout.to_nsecs()) {
      break;
    }
  }
  // Let the timer run
  env_.Run(kSignalLogTimeout);

  // Check to see if connect confirmation and signal indication were received.
  ASSERT_OK(sync_completion_wait(&on_connect_conf_called, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&on_signal_ind_called, ZX_TIME_INFINITE));

  // Prepare and send a deauth request and verify deauth confirmation was received.
  sync_completion_t on_deauth_conf_called;
  mock_fullmac_ifc.on_deauth_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_deauth_confirm_t* conf) {
        sync_completion_signal(&on_deauth_conf_called);
      });

  const wlan_fullmac_deauth_req deauth_req = {
      .peer_sta_address = {1, 2, 3, 4, 5, 6},
      .reason_code = 0,
  };
  ifc->WlanFullmacImplDeauthReq(&deauth_req);
  ASSERT_OK(sync_completion_wait(&on_deauth_conf_called, ZX_TIME_INFINITE));
}

TEST_F(WlanInterfaceTest, WlanFullmacImplConnectRemoteDisconnectReq) {
  // Test that a connect request results in a successful connection. Also verify that a signal
  // report indication is received after a successful connection. And verify the client
  // disconnects when a FW disconnect event is injected. More detailed tests exist in
  // the dedicated ClientConnection tests.

  constexpr int8_t kTestRssi = -64;
  constexpr int8_t kTestSnr = 28;
  constexpr uint8_t kBssIndex = 0;
  constexpr zx::duration kSignalLogTimeout = zx::sec(30);
  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_BSS) {
      // Connect request, must complete asynchronously.
      ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
      return MLAN_STATUS_PENDING;
    }
    if (req->action == MLAN_ACT_GET && req->req_id == MLAN_IOCTL_GET_INFO) {
      auto signal_info = reinterpret_cast<mlan_ds_get_info*>(req->pbuf);
      if (signal_info->sub_command == MLAN_OID_GET_SIGNAL) {
        // Get signal request, must return asynchronously.
        signal_info->param.signal.data_snr_avg = kTestSnr;
        signal_info->param.signal.data_rssi_avg = kTestRssi;
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        return MLAN_STATUS_PENDING;
      }
    }
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_SCAN) {
      // As part of connecting WlanInterface will perform a connect scan, make it succeed.
      return HandleConnectScan(req);
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, kBssIndex,
                                  WLAN_MAC_ROLE_CLIENT, &context_, kClientMacAddress,
                                  std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  // Prepare and send the connect request.
  sync_completion_t on_connect_conf_called;
  mock_fullmac_ifc.on_connect_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_connect_confirm_t* resp) {
        sync_completion_signal(&on_connect_conf_called);
      });

  constexpr uint8_t kIesWithSsid[] = {"\x00\x04Test"};
  constexpr uint8_t kTestChannel = 1;
  const wlan_fullmac_connect_req_t connect_request = {
      .selected_bss{.ies_list = kIesWithSsid,
                    .ies_count = sizeof(kIesWithSsid),
                    .channel{.primary = kTestChannel}},
      .auth_type = WLAN_AUTH_TYPE_OPEN_SYSTEM};

  ifc->WlanFullmacImplConnectReq(&connect_request);

  sync_completion_t on_signal_ind_called;
  mock_fullmac_ifc.on_signal_report.ExpectCallWithMatcher(
      [&](const wlan_fullmac_signal_report_indication_t* report) {
        EXPECT_EQ(report->rssi_dbm, kTestRssi);
        EXPECT_EQ(report->snr_db, kTestSnr);
        sync_completion_signal(&on_signal_ind_called);
      });

  // Wait until the timer has been scheduled.
  while (1) {
    if (env_.GetLatestEventTime().get() >= kSignalLogTimeout.to_nsecs()) {
      break;
    }
  }
  // Let the timer run
  env_.Run(kSignalLogTimeout);

  // Verify connect confirmation and signal indication were received.
  ASSERT_OK(sync_completion_wait(&on_connect_conf_called, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&on_signal_ind_called, ZX_TIME_INFINITE));

  // Simulate a remote disconnect by injecting a FW DISCONNECTED event.
  sync_completion_t on_deauth_ind_called;
  mock_fullmac_ifc.on_deauth_ind_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_deauth_indication_t* conf) {
        sync_completion_signal(&on_deauth_ind_called);
      });

  constexpr uint16_t kDisconnectReasonCode = 1;
  uint8_t event_buf[sizeof(mlan_event) + ETH_ALEN + 2];
  auto event = reinterpret_cast<pmlan_event>(event_buf);
  event->event_id = MLAN_EVENT_ID_FW_DISCONNECTED;
  memcpy(event->event_buf, &kDisconnectReasonCode, sizeof(kDisconnectReasonCode));
  event->event_len = sizeof(kDisconnectReasonCode);
  event->bss_index = kBssIndex;
  event_handler_.OnEvent(event);

  // confirm that the deauth indication was received.
  ASSERT_OK(sync_completion_wait(&on_deauth_ind_called, ZX_TIME_INFINITE));
}

TEST_F(WlanInterfaceTest, MacSetMode) {
  // Test that MacSetMode actually sets the mac mode.

  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;

  constexpr uint8_t kMacMulticastFilters[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                              0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};

  std::atomic<mode_t> mac_mode;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->action == MLAN_ACT_SET && req->req_id == MLAN_IOCTL_BSS) {
      auto bss = reinterpret_cast<const mlan_ds_bss*>(req->pbuf);
      if (bss->sub_command == MLAN_OID_BSS_MULTICAST_LIST) {
        switch (mac_mode.load()) {
          case MODE_MULTICAST_FILTER:
            EXPECT_EQ(MLAN_MULTICAST_MODE, bss->param.multicast_list.mode);
            EXPECT_EQ(sizeof(kMacMulticastFilters) / ETH_ALEN,
                      bss->param.multicast_list.num_multicast_addr);
            EXPECT_BYTES_EQ(kMacMulticastFilters, bss->param.multicast_list.mac_list,
                            sizeof(kMacMulticastFilters));
            break;
          case MODE_MULTICAST_PROMISCUOUS:
            EXPECT_EQ(MLAN_ALL_MULTI_MODE, bss->param.multicast_list.mode);
            EXPECT_EQ(0, bss->param.multicast_list.num_multicast_addr);
            break;
          case MODE_PROMISCUOUS:
            EXPECT_EQ(MLAN_PROMISC_MODE, bss->param.multicast_list.mode);
            EXPECT_EQ(0, bss->param.multicast_list.num_multicast_addr);
            break;
          default:
            ADD_FAILURE("Unexpected mac mode: %u", mac_mode.load());
            break;
        }
        return MLAN_STATUS_SUCCESS;
      }
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacClientDeviceName, 0, WLAN_MAC_ROLE_CLIENT,
                                  &context_, kClientMacAddress, std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  constexpr mode_t kMacModes[] = {MODE_MULTICAST_FILTER, MODE_MULTICAST_PROMISCUOUS,
                                  MODE_PROMISCUOUS};
  for (auto mode : kMacModes) {
    mac_mode = mode;
    ifc->MacSetMode(mode, kMacMulticastFilters);
  }
}

TEST_F(WlanInterfaceTest, WlanFullmacImplStartReq) {
  // Test that a SoftAP Start request results in the right set of ioctls and parameters.

  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;
  constexpr uint8_t kSoftApSsid[] = {"Test_SoftAP"};
  constexpr uint8_t kTestChannel = 6;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->req_id == MLAN_IOCTL_BSS) {
      auto bss = reinterpret_cast<const mlan_ds_bss*>(req->pbuf);
      if (bss->sub_command == MLAN_OID_UAP_BSS_CONFIG) {
        if (req->action == MLAN_ACT_SET) {
          // BSS config set. Ensure SSID, channel and Band are correctly set.
          EXPECT_EQ(bss->param.bss_config.ssid.ssid_len, sizeof(kSoftApSsid));
          EXPECT_BYTES_EQ(bss->param.bss_config.ssid.ssid, kSoftApSsid,
                          bss->param.bss_config.ssid.ssid_len);
          EXPECT_EQ(bss->param.bss_config.channel, kTestChannel);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanBand, BAND_2GHZ);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanWidth, CHAN_BW_20MHZ);
        }
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        return MLAN_STATUS_PENDING;
      } else if (bss->sub_command == MLAN_OID_BSS_START ||
                 bss->sub_command == MLAN_OID_UAP_BSS_RESET) {
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        if (bss->sub_command == MLAN_OID_BSS_START) {
          uint8_t event_buf[sizeof(mlan_event)];
          auto event = reinterpret_cast<pmlan_event>(event_buf);
          event->event_id = MLAN_EVENT_ID_UAP_FW_BSS_START;
          event->bss_index = 0;
          event->event_len = 0;
          event_handler_.OnEvent(event);
        }
        return MLAN_STATUS_PENDING;
      }
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacSoftApDeviceName, 0, WLAN_MAC_ROLE_AP,
                                  &context_, kSoftApMacAddress, std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  sync_completion_t on_start_conf_called;
  mock_fullmac_ifc.on_start_conf.ExpectCallWithMatcher([&](const wlan_fullmac_start_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_START_RESULT_SUCCESS);
    sync_completion_signal(&on_start_conf_called);
  });

  // Start the SoftAP
  wlan_fullmac_start_req start_req = {
      .bss_type = BSS_TYPE_INFRASTRUCTURE,
      .channel = kTestChannel,
  };
  memcpy(start_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  start_req.ssid.len = sizeof(kSoftApSsid);

  ifc->WlanFullmacImplStartReq(&start_req);
  ASSERT_OK(sync_completion_wait(&on_start_conf_called, ZX_TIME_INFINITE));

  // And now ensure SoftAP Stop works ok.
  sync_completion_t on_stop_conf_called;
  mock_fullmac_ifc.on_stop_conf.ExpectCallWithMatcher([&](const wlan_fullmac_stop_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_STOP_RESULT_SUCCESS);
    sync_completion_signal(&on_stop_conf_called);
  });
  wlan_fullmac_stop_req stop_req = {};
  memcpy(stop_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  stop_req.ssid.len = sizeof(kSoftApSsid);
  ifc->WlanFullmacImplStopReq(&stop_req);
  ASSERT_OK(sync_completion_wait(&on_stop_conf_called, ZX_TIME_INFINITE));
}

// Check to see SoftAP auth and assoc indications are received when a STA connects to it.
// Also verify deauth and disassoc indications are received when a STA disconnects from it.
TEST_F(WlanInterfaceTest, SoftApStaConnectDisconnect) {
  // Test that STA connect, disconnect indications are received correctly.

  constexpr uint8_t kTestSoftApClient[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5};
  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;
  constexpr uint8_t kSoftApSsid[] = {"Test_SoftAP"};
  constexpr uint8_t kTestChannel = 6;
  constexpr uint8_t kBssIndex = 1;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->req_id == MLAN_IOCTL_BSS) {
      auto bss = reinterpret_cast<const mlan_ds_bss*>(req->pbuf);
      if (bss->sub_command == MLAN_OID_UAP_BSS_CONFIG) {
        if (req->action == MLAN_ACT_SET) {
          // BSS config set. Ensure SSID, channel and Band are correctly set.
          EXPECT_EQ(bss->param.bss_config.ssid.ssid_len, sizeof(kSoftApSsid));
          EXPECT_BYTES_EQ(bss->param.bss_config.ssid.ssid, kSoftApSsid,
                          bss->param.bss_config.ssid.ssid_len);
          EXPECT_EQ(bss->param.bss_config.channel, kTestChannel);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanBand, BAND_2GHZ);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanWidth, CHAN_BW_20MHZ);
        }
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        return MLAN_STATUS_PENDING;
      } else if (bss->sub_command == MLAN_OID_BSS_START ||
                 bss->sub_command == MLAN_OID_UAP_BSS_RESET) {
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        if (bss->sub_command == MLAN_OID_BSS_START) {
          uint8_t event_buf[sizeof(mlan_event)];
          auto event = reinterpret_cast<pmlan_event>(event_buf);
          event->event_id = MLAN_EVENT_ID_UAP_FW_BSS_START;
          event->bss_index = kBssIndex;
          event->event_len = 0;
          event_handler_.OnEvent(event);
        }
        return MLAN_STATUS_PENDING;
      }
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacSoftApDeviceName, kBssIndex,
                                  WLAN_MAC_ROLE_AP, &context_, kSoftApMacAddress,
                                  std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  sync_completion_t on_start_conf_called;
  mock_fullmac_ifc.on_start_conf.ExpectCallWithMatcher([&](const wlan_fullmac_start_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_START_RESULT_SUCCESS);
    sync_completion_signal(&on_start_conf_called);
  });

  // Start the SoftAP
  wlan_fullmac_start_req start_req = {
      .bss_type = BSS_TYPE_INFRASTRUCTURE,
      .channel = kTestChannel,
  };
  memcpy(start_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  start_req.ssid.len = sizeof(kSoftApSsid);

  ifc->WlanFullmacImplStartReq(&start_req);
  ASSERT_OK(sync_completion_wait(&on_start_conf_called, ZX_TIME_INFINITE));

  // Setup the auth_ind and assoc_ind callbacks.
  sync_completion_t on_auth_ind;
  mock_fullmac_ifc.on_auth_ind_conf.ExpectCallWithMatcher([&](const wlan_fullmac_auth_ind_t* ind) {
    EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
    EXPECT_EQ(ind->auth_type, WLAN_AUTH_TYPE_OPEN_SYSTEM);
    sync_completion_signal(&on_auth_ind);
  });

  sync_completion_t on_assoc_ind;
  mock_fullmac_ifc.on_assoc_ind_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_assoc_ind_t* ind) {
        EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
        EXPECT_EQ(ind->listen_interval, 0);
        sync_completion_signal(&on_assoc_ind);
      });

  // Send a STA connect event
  uint8_t event_buf[sizeof(mlan_event) + ETH_ALEN + 2];
  auto event = reinterpret_cast<pmlan_event>(event_buf);
  event->event_id = MLAN_EVENT_ID_UAP_FW_STA_CONNECT;
  memcpy(event->event_buf, kTestSoftApClient, sizeof(kTestSoftApClient));
  event->event_len = sizeof(kTestSoftApClient);
  event->bss_index = kBssIndex;
  event_handler_.OnEvent(event);

  // Wait for auth and assoc indications.
  ASSERT_OK(sync_completion_wait(&on_auth_ind, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&on_assoc_ind, ZX_TIME_INFINITE));

  // Setup the deauth_ind and disassoc_ind callbacks.
  sync_completion_t on_deauth_ind;
  mock_fullmac_ifc.on_deauth_ind_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_deauth_indication* ind) {
        EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
        sync_completion_signal(&on_deauth_ind);
      });

  sync_completion_t on_disassoc_ind;
  mock_fullmac_ifc.on_disassoc_ind_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_disassoc_indication* ind) {
        EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
        sync_completion_signal(&on_disassoc_ind);
      });

  // Followed by the STA disconnect event.
  event->event_id = MLAN_EVENT_ID_UAP_FW_STA_DISCONNECT;
  memcpy(event->event_buf + 2, kTestSoftApClient, sizeof(kTestSoftApClient));
  event->event_len = sizeof(kTestSoftApClient) + 2;
  event->bss_index = kBssIndex;
  event_handler_.OnEvent(event);

  // Wait for deauth and disassoc indications.
  ASSERT_OK(sync_completion_wait(&on_auth_ind, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&on_assoc_ind, ZX_TIME_INFINITE));

  // And now ensure SoftAP Stop works ok.
  sync_completion_t on_stop_conf_called;
  mock_fullmac_ifc.on_stop_conf.ExpectCallWithMatcher([&](const wlan_fullmac_stop_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_STOP_RESULT_SUCCESS);
    sync_completion_signal(&on_stop_conf_called);
  });
  wlan_fullmac_stop_req stop_req = {};
  memcpy(stop_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  stop_req.ssid.len = sizeof(kSoftApSsid);
  ifc->WlanFullmacImplStopReq(&stop_req);
  ASSERT_OK(sync_completion_wait(&on_stop_conf_called, ZX_TIME_INFINITE));
}

// Check to see SoftAP auth and assoc indications are received when a STA connects to it.
// Also verify a connected STA can be deauth'd by the SoftAP explicitly.
TEST_F(WlanInterfaceTest, SoftApStaLocalDisconnect) {
  // Test that STA connect, disconnect indications are received correctly.

  constexpr uint8_t kTestSoftApClient[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5};
  wlan::nxpfmac::MockFullmacIfc mock_fullmac_ifc;
  constexpr uint8_t kSoftApSsid[] = {"Test_SoftAP"};
  constexpr uint8_t kTestChannel = 6;
  constexpr uint8_t kBssIndex = 1;

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    if (req->req_id == MLAN_IOCTL_BSS) {
      auto bss = reinterpret_cast<const mlan_ds_bss*>(req->pbuf);
      if (bss->sub_command == MLAN_OID_UAP_BSS_CONFIG) {
        if (req->action == MLAN_ACT_SET) {
          // BSS config set. Ensure SSID, channel and Band are correctly set.
          EXPECT_EQ(bss->param.bss_config.ssid.ssid_len, sizeof(kSoftApSsid));
          EXPECT_BYTES_EQ(bss->param.bss_config.ssid.ssid, kSoftApSsid,
                          bss->param.bss_config.ssid.ssid_len);
          EXPECT_EQ(bss->param.bss_config.channel, kTestChannel);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanBand, BAND_2GHZ);
          EXPECT_EQ(bss->param.bss_config.bandcfg.chanWidth, CHAN_BW_20MHZ);
        }
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        return MLAN_STATUS_PENDING;
      } else if (bss->sub_command == MLAN_OID_BSS_START ||
                 bss->sub_command == MLAN_OID_UAP_BSS_RESET) {
        ioctl_adapter_->OnIoctlComplete(req, wlan::nxpfmac::IoctlStatus::Success);
        if (bss->sub_command == MLAN_OID_BSS_START) {
          uint8_t event_buf[sizeof(mlan_event)];
          auto event = reinterpret_cast<pmlan_event>(event_buf);
          event->event_id = MLAN_EVENT_ID_UAP_FW_BSS_START;
          event->bss_index = kBssIndex;
          event->event_len = 0;
          event_handler_.OnEvent(event);
        }
        return MLAN_STATUS_PENDING;
      }
    }
    // Return success for everything else.
    return MLAN_STATUS_SUCCESS;
  });

  zx::channel in_mlme_channel;
  zx::channel unused;
  ASSERT_OK(zx::channel::create(0, &in_mlme_channel, &unused));

  ASSERT_OK(CreateDeviceInterface(parent_.get(), kFullmacSoftApDeviceName, kBssIndex,
                                  WLAN_MAC_ROLE_AP, &context_, kSoftApMacAddress,
                                  std::move(in_mlme_channel)));
  WlanInterface* ifc = GetInterface();

  zx::channel out_mlme_channel;
  ifc->WlanFullmacImplStart(mock_fullmac_ifc.proto(), &out_mlme_channel);

  sync_completion_t on_start_conf_called;
  mock_fullmac_ifc.on_start_conf.ExpectCallWithMatcher([&](const wlan_fullmac_start_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_START_RESULT_SUCCESS);
    sync_completion_signal(&on_start_conf_called);
  });

  // Start the SoftAP
  wlan_fullmac_start_req start_req = {
      .bss_type = BSS_TYPE_INFRASTRUCTURE,
      .channel = kTestChannel,
  };
  memcpy(start_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  start_req.ssid.len = sizeof(kSoftApSsid);

  ifc->WlanFullmacImplStartReq(&start_req);
  ASSERT_OK(sync_completion_wait(&on_start_conf_called, ZX_TIME_INFINITE));

  // Setup the auth_ind and assoc_ind callbacks.
  sync_completion_t on_auth_ind;
  mock_fullmac_ifc.on_auth_ind_conf.ExpectCallWithMatcher([&](const wlan_fullmac_auth_ind_t* ind) {
    EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
    EXPECT_EQ(ind->auth_type, WLAN_AUTH_TYPE_OPEN_SYSTEM);
    sync_completion_signal(&on_auth_ind);
  });

  sync_completion_t on_assoc_ind;
  mock_fullmac_ifc.on_assoc_ind_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_assoc_ind_t* ind) {
        EXPECT_BYTES_EQ(ind->peer_sta_address, kTestSoftApClient, ETH_ALEN);
        EXPECT_EQ(ind->listen_interval, 0);
        sync_completion_signal(&on_assoc_ind);
      });

  // Send a STA connect event
  uint8_t event_buf[sizeof(mlan_event) + ETH_ALEN + 2];
  auto event = reinterpret_cast<pmlan_event>(event_buf);
  event->event_id = MLAN_EVENT_ID_UAP_FW_STA_CONNECT;
  memcpy(event->event_buf, kTestSoftApClient, sizeof(kTestSoftApClient));
  event->event_len = sizeof(kTestSoftApClient);
  event->bss_index = kBssIndex;
  event_handler_.OnEvent(event);

  // Wait for auth and assoc indications.
  ASSERT_OK(sync_completion_wait(&on_auth_ind, ZX_TIME_INFINITE));
  ASSERT_OK(sync_completion_wait(&on_assoc_ind, ZX_TIME_INFINITE));

  sync_completion_t on_deauth_conf;
  mock_fullmac_ifc.on_deauth_conf.ExpectCallWithMatcher(
      [&](const wlan_fullmac_deauth_confirm_t* conf) {
        EXPECT_BYTES_EQ(conf->peer_sta_address, kTestSoftApClient, ETH_ALEN);
        sync_completion_signal(&on_deauth_conf);
      });

  // Send a deauth request to disconnect the STA
  wlan_fullmac_deauth_req deauth_req = {
      .reason_code = 0,
  };
  memcpy(deauth_req.peer_sta_address, kTestSoftApClient, ETH_ALEN);

  env_.ScheduleNotification([&]() { ifc->WlanFullmacImplDeauthReq(&deauth_req); }, zx::msec(10));
  // We currently do not have a way to send the deauth event at the appropriate time. So let the
  // deauth request timeout waiting for the event and send a deauth conf.

  env_.Run(zx::sec(2));

  ASSERT_OK(sync_completion_wait(&on_deauth_conf, ZX_TIME_INFINITE));

  // And now ensure SoftAP Stop works ok.
  sync_completion_t on_stop_conf_called;
  mock_fullmac_ifc.on_stop_conf.ExpectCallWithMatcher([&](const wlan_fullmac_stop_confirm* resp) {
    EXPECT_EQ(resp->result_code, WLAN_STOP_RESULT_SUCCESS);
    sync_completion_signal(&on_stop_conf_called);
  });
  wlan_fullmac_stop_req stop_req = {};
  memcpy(stop_req.ssid.data, kSoftApSsid, sizeof(kSoftApSsid));
  stop_req.ssid.len = sizeof(kSoftApSsid);
  ifc->WlanFullmacImplStopReq(&stop_req);
  ASSERT_OK(sync_completion_wait(&on_stop_conf_called, ZX_TIME_INFINITE));
}

}  // namespace
