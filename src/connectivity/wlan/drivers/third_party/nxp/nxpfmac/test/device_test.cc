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

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device.h"

#include <fuchsia/hardware/network/driver/cpp/banjo.h>
#include <fuchsia/hardware/wlanphyimpl/c/banjo.h>
#include <lib/fdio/directory.h>
#include <lib/mock-function/mock-function.h>

#include <fbl/string_buffer.h>
#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device_context.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/mlan_mocks.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/test/mock_bus.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

using wlan::nxpfmac::Device;

namespace {

constexpr size_t kFirmwareVmoSize = 12u;
constexpr fdf_arena_tag_t kArenaTag = 1234567890;

struct TestDevice : public Device {
 public:
  static zx_status_t Create(zx_device_t* parent, sync_completion_t& destructor_completed,
                            TestDevice** out_device) {
    auto device = new TestDevice(parent, destructor_completed);
    const zx_status_t status = device->DdkAdd("TestDevice");
    if (status != ZX_OK) {
      return status;
    }
    *out_device = device;
    return ZX_OK;
  }
  ~TestDevice() {}
  async_dispatcher_t* GetDispatcher() override { return nullptr; }

  wlan::nxpfmac::DeviceContext* GetContext() { return &context_; }
  wlan::nxpfmac::MockBus* GetBus() { return &bus_; }

 private:
  TestDevice(zx_device_t* parent, sync_completion_t& on_destructor)
      : Device(parent), on_destruct_(on_destructor) {}

 protected:
  zx_status_t Init(mlan_device* mlan_dev, wlan::nxpfmac::BusInterface** out_bus) override {
    // We must provide some basic bus level operations for Device to complete its DdkInit. Do
    // nothing.
    mlan_dev->callbacks.moal_read_reg = [](t_void*, t_u32, t_u32*) { return MLAN_STATUS_SUCCESS; };
    mlan_dev->callbacks.moal_write_reg = [](t_void* pmoal, t_u32 reg, t_u32 data) {
      return MLAN_STATUS_SUCCESS;
    };
    mlan_dev->callbacks.moal_read_data_sync = [](t_void* pmoal, pmlan_buffer pmbuf, t_u32 port,
                                                 t_u32 timeout) { return MLAN_STATUS_SUCCESS; };
    mlan_dev->callbacks.moal_write_data_sync = [](t_void* pmoal, pmlan_buffer pmbuf, t_u32 port,
                                                  t_u32 timeout) { return MLAN_STATUS_SUCCESS; };
    mlan_dev->pmoal_handle = &context_;
    *out_bus = &bus_;

    return ZX_OK;
  }
  zx_status_t LoadFirmware(const char* path, zx::vmo* out_fw, size_t* out_size) override {
    // We must provide some data here so that Device can successfully initialize. Just create an
    // empty VMO.
    zx_status_t status = zx::vmo::create(kFirmwareVmoSize, 0, out_fw);
    if (status != ZX_OK) {
      return status;
    }
    return ZX_OK;
  }
  void Shutdown() override {}

  class OnDestruct {
   public:
    explicit OnDestruct(sync_completion_t& completion) : completion_(completion) {}
    ~OnDestruct() { sync_completion_signal(&completion_); }

   private:
    sync_completion_t& completion_;
  };
  // This should be the first data member in the class because it needs to be destroyed last.
  // Otherwise it will signal destruction before the entire destruct sequence is complete.
  OnDestruct on_destruct_;

  wlan::nxpfmac::DeviceContext context_;
  wlan::nxpfmac::MockBus bus_;

 public:
};

struct DeviceTest : public zxtest::Test {
  void SetUp() override {
    parent_ = MockDevice::FakeRootParent();

    auto dispatcher = fdf::SynchronizedDispatcher::Create(
        {.value = FDF_DISPATCHER_OPTION_ALLOW_SYNC_CALLS}, "TestDriverDispatcher",
        [&](fdf_dispatcher_t*) { sync_completion_signal(&dispatcher_completion_); });

    ASSERT_FALSE(dispatcher.is_error());
    driver_dispatcher_ = std::move(*dispatcher);
    // Create the device on driver dispatcher because the outgoing directory is required to be
    // accessed by a single dispatcher.
    libsync::Completion created;
    async::PostTask(driver_dispatcher_.async_dispatcher(), [&]() {
      ASSERT_OK(TestDevice::Create(parent_.get(), device_destructed_, &device_));
      created.Signal();
    });
    created.Wait();

    auto outgoing_dir_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_FALSE(outgoing_dir_endpoints.is_error());

    // Serve WlanPhyImplProtocol to the device's outgoing directory on the driver dispatcher.
    libsync::Completion served;
    async::PostTask(driver_dispatcher_.async_dispatcher(), [&]() {
      ASSERT_EQ(ZX_OK,
                device_->ServeWlanPhyImplProtocol(std::move(outgoing_dir_endpoints->server)));
      served.Signal();
    });
    served.Wait();

    wlan::nxpfmac::set_mlan_register_mock_adapter(&mlan_mocks_);
    ddk::InitTxn txn(parent_->GetLatestChild());
    device_->DdkInit(std::move(txn));

    MockDevice* device = parent_->children().front().get();

    device->WaitUntilInitReplyCalled();
    ASSERT_OK(device->InitReplyCallStatus());

    // Connect WlanPhyImpl protocol from this class, this operation mimics the implementation of
    // DdkConnectRuntimeProtocol().
    auto endpoints =
        fdf::CreateEndpoints<fuchsia_wlan_phyimpl::Service::WlanPhyImpl::ProtocolType>();
    ASSERT_FALSE(endpoints.is_error());
    zx::channel client_token, server_token;
    ASSERT_EQ(ZX_OK, zx::channel::create(0, &client_token, &server_token));
    ASSERT_EQ(ZX_OK, fdf::ProtocolConnect(std::move(client_token),
                                          fdf::Channel(endpoints->server.TakeChannel().release())));
    fbl::StringBuffer<fuchsia_io::wire::kMaxPathLength> path;
    path.AppendPrintf("svc/%s/default/%s", fuchsia_wlan_phyimpl::Service::WlanPhyImpl::ServiceName,
                      fuchsia_wlan_phyimpl::Service::WlanPhyImpl::Name);
    // Serve the WlanPhyImpl protocol on `server_token` found at `path` within
    // the outgoing directory.
    ASSERT_EQ(ZX_OK, fdio_service_connect_at(outgoing_dir_endpoints->client.channel().get(),
                                             path.c_str(), server_token.release()));
    wlanphy_client_ =
        fdf::WireSyncClient<fuchsia_wlan_phyimpl::WlanPhyImpl>(std::move(endpoints->client));
    device_->WaitForProtocolConnection();
  }

  void TearDown() override {
    // Remove and release the net device zx_device first, the mock device implementation won't call
    // release after device_async_remove so we have to compensate for that by manually removing it.
    std::shared_ptr<MockDevice> net_device;
    std::shared_ptr<MockDevice> phy_device;

    for (auto child : parent_->children()) {
      network_device_impl_protocol_t proto;
      if (device_get_protocol(child.get(), ZX_PROTOCOL_NETWORK_DEVICE_IMPL, &proto) == ZX_OK) {
        net_device = child;
      } else {
        phy_device = child;
      }
    }

    // Make sure net_device is released first, otherwise it will block the release of phy_device.
    ASSERT_NOT_NULL(net_device.get());
    device_async_remove(net_device.get());
    mock_ddk::ReleaseFlaggedDevices(net_device.get(), driver_dispatcher_.async_dispatcher());
    net_device.reset();

    // Explicitly release phy_device on driver_dispatcher_to ensure the OutgoingDirectory is only
    // accessed from a single dispatcher.
    ASSERT_NOT_NULL(phy_device.get());
    device_async_remove(phy_device.get());
    mock_ddk::ReleaseFlaggedDevices(phy_device.get(), driver_dispatcher_.async_dispatcher());
    phy_device.reset();

    parent_.reset();

    sync_completion_wait(&device_destructed_, ZX_TIME_INFINITE);
    driver_dispatcher_.ShutdownAsync();
    sync_completion_wait(&dispatcher_completion_, ZX_TIME_INFINITE);
  }

  std::shared_ptr<MockDevice> parent_;
  sync_completion_t device_destructed_;
  TestDevice* device_ = nullptr;
  sync_completion_t dispatcher_completion_;
  fdf::Dispatcher driver_dispatcher_;
  wlan::nxpfmac::MlanMockAdapter mlan_mocks_;
  fdf::WireSyncClient<fuchsia_wlan_phyimpl::WlanPhyImpl> wlanphy_client_;
};

// Since we use a zero byte vmo for the files, none of the power file related ioctls will be
// issued during this test.
TEST_F(DeviceTest, SetCountry) {
  fdf::Arena arena(kArenaTag);

  auto request = ::fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'U', 'S'});

  bool ioctl_called = false;
  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    ioctl_called = true;
    EXPECT_EQ(MLAN_ACT_SET, req->action);
    EXPECT_EQ(MLAN_IOCTL_MISC_CFG, req->req_id);
    auto cfg = reinterpret_cast<const mlan_ds_misc_cfg*>(req->pbuf);
    EXPECT_EQ(MLAN_OID_MISC_COUNTRY_CODE, cfg->sub_command);
    EXPECT_BYTES_EQ(request.alpha2().data(), cfg->param.country_code.country_code,
                    request.alpha2().size());
    return MLAN_STATUS_SUCCESS;
  });

  auto result = wlanphy_client_.buffer(arena)->SetCountry(request);
  ASSERT_OK(result.status());
  ASSERT_TRUE(result.value().is_ok());
  ASSERT_TRUE(ioctl_called);
}

TEST_F(DeviceTest, SetCountryCodeFails) {
  fdf::Arena arena(kArenaTag);

  auto request = ::fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'U', 'S'});

  bool ioctl_called = false;
  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    ioctl_called = true;
    EXPECT_EQ(MLAN_ACT_SET, req->action);
    EXPECT_EQ(MLAN_IOCTL_MISC_CFG, req->req_id);
    auto cfg = reinterpret_cast<const mlan_ds_misc_cfg*>(req->pbuf);
    EXPECT_EQ(MLAN_OID_MISC_COUNTRY_CODE, cfg->sub_command);
    return MLAN_STATUS_FAILURE;
  });

  auto result = wlanphy_client_.buffer(arena)->SetCountry(request);
  ASSERT_OK(result.status());
  ASSERT_TRUE(result.value().is_error());
  ASSERT_EQ(ZX_ERR_IO, result.value().error_value());
  ASSERT_TRUE(ioctl_called);
}

TEST_F(DeviceTest, GetCountry) {
  std::array<uint8_t, 3> country_code_set_by_ioctl;

  auto set_request = ::fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'U', 'S'});

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    EXPECT_EQ(MLAN_IOCTL_MISC_CFG, req->req_id);
    auto cfg = reinterpret_cast<mlan_ds_misc_cfg*>(req->pbuf);
    EXPECT_EQ(MLAN_OID_MISC_COUNTRY_CODE, cfg->sub_command);
    if (req->action == MLAN_ACT_SET) {
      // Store the country code from the set operation.
      memcpy(country_code_set_by_ioctl.data(), cfg->param.country_code.country_code,
             std::min(sizeof(country_code_set_by_ioctl),
                      sizeof(cfg->param.country_code.country_code)));
      return MLAN_STATUS_SUCCESS;
    }
    if (req->action == MLAN_ACT_GET) {
      // Return the country code that was previously set.
      memcpy(cfg->param.country_code.country_code, country_code_set_by_ioctl.data(),
             std::min(sizeof(country_code_set_by_ioctl),
                      sizeof(cfg->param.country_code.country_code)));
      return MLAN_STATUS_SUCCESS;
    }
    ADD_FAILURE("Unexpected ioctl");
    return MLAN_STATUS_FAILURE;
  });

  fdf::Arena arena_for_set(kArenaTag);
  auto set_result = wlanphy_client_.buffer(arena_for_set)->SetCountry(set_request);
  ASSERT_OK(set_result.status());
  ASSERT_TRUE(set_result.value().is_ok());

  fdf::Arena arena_for_get(kArenaTag + 1);
  auto get_result = wlanphy_client_.buffer(arena_for_get)->GetCountry();
  ASSERT_OK(get_result.status());
  ASSERT_TRUE(get_result.value().is_ok());

  ASSERT_TRUE(get_result->value()->is_alpha2());
  EXPECT_BYTES_EQ(set_request.alpha2().data(), get_result->value()->alpha2().data(),
                  set_request.alpha2().size());
}

TEST_F(DeviceTest, ClearCountry) {
  std::array<uint8_t, 3> country_code_set_by_ioctl;

  auto set_request = ::fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'U', 'S'});

  mlan_mocks_.SetOnMlanIoctl([&](t_void*, pmlan_ioctl_req req) -> mlan_status {
    EXPECT_EQ(MLAN_IOCTL_MISC_CFG, req->req_id);
    auto cfg = reinterpret_cast<mlan_ds_misc_cfg*>(req->pbuf);
    EXPECT_EQ(MLAN_OID_MISC_COUNTRY_CODE, cfg->sub_command);
    if (req->action == MLAN_ACT_SET) {
      // Store the country code from the set operation.
      memcpy(country_code_set_by_ioctl.data(), cfg->param.country_code.country_code,
             std::min(sizeof(country_code_set_by_ioctl),
                      sizeof(cfg->param.country_code.country_code)));
      return MLAN_STATUS_SUCCESS;
    }
    if (req->action == MLAN_ACT_GET) {
      // Return the country code that was previously set.
      memcpy(cfg->param.country_code.country_code, country_code_set_by_ioctl.data(),
             std::min(sizeof(country_code_set_by_ioctl),
                      sizeof(cfg->param.country_code.country_code)));
      return MLAN_STATUS_SUCCESS;
    }
    ADD_FAILURE("Unexpected ioctl");
    return MLAN_STATUS_FAILURE;
  });

  // Set country code to US
  fdf::Arena arena_for_set(kArenaTag);
  auto set_result = wlanphy_client_.buffer(arena_for_set)->SetCountry(set_request);
  ASSERT_OK(set_result.status());
  ASSERT_TRUE(set_result.value().is_ok());

  // Get country should return US
  fdf::Arena arena_for_get(kArenaTag + 1);
  auto get_result = wlanphy_client_.buffer(arena_for_get)->GetCountry();
  ASSERT_OK(get_result.status());
  ASSERT_TRUE(get_result.value().is_ok());

  ASSERT_TRUE(get_result->value()->is_alpha2());
  EXPECT_BYTES_EQ(set_request.alpha2().data(), get_result->value()->alpha2().data(),
                  set_request.alpha2().size());
  // Clear country should reset it to WW
  fdf::Arena arena_for_clear(kArenaTag + 2);
  auto clear_result = wlanphy_client_.buffer(arena_for_clear)->ClearCountry();
  ASSERT_OK(clear_result.status());
  ASSERT_TRUE(clear_result.value().is_ok());

  // Get should return WW
  get_result = wlanphy_client_.buffer(arena_for_get)->GetCountry();
  ASSERT_OK(get_result.status());
  ASSERT_TRUE(get_result->value()->is_alpha2());
  EXPECT_BYTES_EQ("WW", get_result->value()->alpha2().data(), 2);
}

TEST_F(DeviceTest, DeferredRxWork) {
  // Test that the deferred processing event triggers a call to the bus to perform processing.

  wlan::nxpfmac::EventHandler* event_handler = device_->GetContext()->event_handler_;

  sync_completion_t mlan_rx_process_called;
  mlan_mocks_.SetOnMlanRxProcess([&](t_void*, t_u8*) -> mlan_status {
    sync_completion_signal(&mlan_rx_process_called);
    return MLAN_STATUS_SUCCESS;
  });

  mlan_event event{.event_id = MLAN_EVENT_ID_DRV_DEFER_RX_WORK};
  event_handler->OnEvent(&event);

  ASSERT_OK(sync_completion_wait(&mlan_rx_process_called, ZX_TIME_INFINITE));
}

TEST_F(DeviceTest, DeferredProcessing) {
  // Test that the deferred processing event triggers a call to the bus to perform processing.

  wlan::nxpfmac::EventHandler* event_handler = device_->GetContext()->event_handler_;
  wlan::nxpfmac::MockBus* mock_bus = device_->GetBus();

  sync_completion_t trigger_main_process_called;
  mock_bus->SetTriggerMainProcess([&]() -> zx_status_t {
    sync_completion_signal(&trigger_main_process_called);
    return ZX_OK;
  });

  mlan_event event{.event_id = MLAN_EVENT_ID_DRV_DEFER_HANDLING};
  event_handler->OnEvent(&event);

  sync_completion_wait(&trigger_main_process_called, ZX_TIME_INFINITE);
}

}  // namespace
