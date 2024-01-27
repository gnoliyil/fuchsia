/*
 * Copyright (c) 2019 The Fuchsia Authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/hardware/wlanphyimpl/c/banjo.h>
#include <zircon/errors.h>

#include <algorithm>
#include <memory>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/common.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_device.h"
#include "zircon/system/ulib/sync/include/lib/sync/cpp/completion.h"

namespace wlan {
namespace brcmfmac {
namespace {

std::pair<zx::channel, zx::channel> make_channel() {
  zx::channel local;
  zx::channel remote;
  zx::channel::create(0, &local, &remote);
  return {std::move(local), std::move(remote)};
}

TEST(LifecycleTest, StartStop) {
  auto env = std::make_shared<simulation::Environment>();
  auto dev_mgr = std::make_unique<simulation::FakeDevMgr>();

  SimDevice* device;
  zx_status_t status = SimDevice::Create(dev_mgr->GetRootDevice(), dev_mgr.get(), env, &device);
  ASSERT_EQ(status, ZX_OK);
  status = device->BusInit();
  ASSERT_EQ(status, ZX_OK);
}

TEST(LifecycleTest, StartWithSmeChannel) {
  auto env = std::make_shared<simulation::Environment>();
  auto dev_mgr = std::make_unique<simulation::FakeDevMgr>();

  libsync::Completion completion_;

  // Create PHY.
  SimDevice* device;
  zx_status_t status = SimDevice::Create(dev_mgr->GetRootDevice(), dev_mgr.get(), env, &device);
  ASSERT_EQ(status, ZX_OK);
  status = device->BusInit();
  ASSERT_EQ(status, ZX_OK);
  EXPECT_EQ(dev_mgr->DeviceCountByProtocolId(ZX_PROTOCOL_WLANPHY_IMPL), 1u);

  auto endpoints = fdf::CreateEndpoints<fuchsia_wlan_wlanphyimpl::WlanPhyImpl>();
  ASSERT_FALSE(endpoints.is_error());
  auto dispatcher = fdf::SynchronizedDispatcher::Create(
      {}, __func__, [&](fdf_dispatcher_t*) { completion_.Signal(); });

  ASSERT_FALSE(dispatcher.is_error());
  auto client_dispatcher_ = *std::move(dispatcher);

  auto client_ = fdf::WireSharedClient<fuchsia_wlan_wlanphyimpl::WlanPhyImpl>(
      std::move(endpoints->client), client_dispatcher_.get());

  device->DdkServiceConnect(fidl::DiscoverableProtocolName<fuchsia_wlan_wlanphyimpl::WlanPhyImpl>,
                            endpoints->server.TakeHandle());
  constexpr uint32_t kTag = 'TEST';

  fdf::Arena test_arena_(kTag);

  // Create iface.
  auto [local, _remote] = make_channel();
  zx_handle_t test_mlme_channel = local.get();

  // check that create iface fails when we don't provide role and mlme_channel
  fidl::Arena create_fail_arena;
  auto builder_create_fail =
      fuchsia_wlan_wlanphyimpl::wire::WlanPhyImplCreateIfaceRequest::Builder(create_fail_arena);
  auto result_create_fail =
      client_.sync().buffer(test_arena_)->CreateIface(builder_create_fail.Build());
  EXPECT_TRUE(result_create_fail.ok());
  ASSERT_TRUE(result_create_fail->is_error());
  ASSERT_EQ(result_create_fail->error_value(), ZX_ERR_INVALID_ARGS);

  // create iface successfully
  fidl::Arena create_arena;
  auto builder_create =
      fuchsia_wlan_wlanphyimpl::wire::WlanPhyImplCreateIfaceRequest::Builder(create_arena);
  builder_create.role(fuchsia_wlan_common::wire::WlanMacRole::kClient);
  builder_create.mlme_channel(std::move(local));
  auto result_create = client_.sync().buffer(test_arena_)->CreateIface(builder_create.Build());
  EXPECT_TRUE(result_create.ok());
  ASSERT_FALSE(result_create->is_error());
  uint16_t iface_id = result_create->value()->iface_id();
  EXPECT_EQ(dev_mgr->DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);

  // check that creating another client interface fails with ZX_ERR_NO_RESOURCES
  auto [local_again, _remote_again] = make_channel();
  fidl::Arena create_arena_again;
  auto builder_create_again =
      fuchsia_wlan_wlanphyimpl::wire::WlanPhyImplCreateIfaceRequest::Builder(create_arena_again);
  builder_create_again.role(fuchsia_wlan_common::wire::WlanMacRole::kClient);
  builder_create_again.mlme_channel(std::move(local_again));
  auto result_create_again =
      client_.sync().buffer(test_arena_)->CreateIface(builder_create_again.Build());
  EXPECT_TRUE(result_create_again.ok());
  ASSERT_TRUE(result_create_again->is_error());
  ASSERT_EQ(result_create_again->error_value(), ZX_ERR_NO_RESOURCES);

  // Simulate start call from Fuchsia's generic wlanif-impl driver.
  auto iface = dev_mgr->FindLatestByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL);
  ASSERT_NE(iface, nullptr);
  void* ctx = iface->DevArgs().ctx;
  auto* iface_ops =
      static_cast<const wlan_fullmac_impl_protocol_ops_t*>(iface->DevArgs().proto_ops);
  zx_handle_t mlme_channel = ZX_HANDLE_INVALID;
  wlan_fullmac_impl_ifc_protocol_t ifc_ops{};
  status = iface_ops->start(ctx, &ifc_ops, &mlme_channel);
  EXPECT_EQ(status, ZX_OK);
  EXPECT_EQ(mlme_channel, test_mlme_channel);

  // Verify calling start again will fail with proper error code.
  mlme_channel = ZX_HANDLE_INVALID;
  status = iface_ops->start(ctx, &ifc_ops, &mlme_channel);
  EXPECT_EQ(status, ZX_ERR_ALREADY_BOUND);
  EXPECT_EQ(mlme_channel, ZX_HANDLE_INVALID);

  // check that without specifying iface id, destroy iface fails
  fidl::Arena destroy_fail_arena;
  auto builder_destroy_fail =
      fuchsia_wlan_wlanphyimpl::wire::WlanPhyImplDestroyIfaceRequest::Builder(destroy_fail_arena);
  auto result_destroy_fail =
      client_.sync().buffer(test_arena_)->DestroyIface(builder_destroy_fail.Build());
  EXPECT_TRUE(result_destroy_fail.ok());
  ASSERT_TRUE(result_destroy_fail->is_error());
  ASSERT_EQ(result_destroy_fail->error_value(), ZX_ERR_INVALID_ARGS);

  // Now actually destroy the iface
  fidl::Arena destroy_arena;
  auto builder_destroy =
      fuchsia_wlan_wlanphyimpl::wire::WlanPhyImplDestroyIfaceRequest::Builder(destroy_arena);
  builder_destroy.iface_id(iface_id);
  auto result_destroy = client_.sync().buffer(test_arena_)->DestroyIface(builder_destroy.Build());
  EXPECT_TRUE(result_destroy.ok());
  ASSERT_FALSE(result_destroy->is_error());
  EXPECT_EQ(dev_mgr->DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);

  client_dispatcher_.ShutdownAsync();
  completion_.Wait();
}

}  // namespace
}  // namespace brcmfmac
}  // namespace wlan
