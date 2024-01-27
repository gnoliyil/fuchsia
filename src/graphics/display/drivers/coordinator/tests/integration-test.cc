// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.sysmem2/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-testing/test_loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/wire/array.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <zircon/pixelformat.h>
#include <zircon/types.h>

#include <cstdint>
#include <memory>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <gtest/gtest.h>

#include "src/graphics/display/drivers/coordinator/client.h"
#include "src/graphics/display/drivers/coordinator/controller.h"
#include "src/graphics/display/drivers/coordinator/tests/base.h"
#include "src/graphics/display/drivers/coordinator/tests/fidl_client.h"
#include "src/graphics/display/drivers/coordinator/util.h"
#include "src/graphics/display/drivers/fake/fake-display.h"
#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/testing/predicates/status.h"

namespace sysmem = fuchsia_sysmem;

namespace display {

class IntegrationTest : public TestBase, public testing::WithParamInterface<bool> {
 public:
  fbl::RefPtr<display::DisplayInfo> display_info(uint64_t id) __TA_REQUIRES(controller()->mtx()) {
    auto iter = controller()->displays_.find(id);
    if (iter.IsValid()) {
      return iter.CopyPointer();
    } else {
      return nullptr;
    }
  }

  bool primary_client_connected() {
    fbl::AutoLock l(controller()->mtx());
    if (!controller()->primary_client_) {
      return false;
    }
    fbl::AutoLock cl(&controller()->primary_client_->mtx_);
    return (controller()->primary_client_ == controller()->active_client_ &&
            // DC processed the EnableVsync request. We can now expect vsync events.
            controller()->primary_client_->enable_vsync_);
  }

  bool virtcon_client_connected() {
    fbl::AutoLock l(controller()->mtx());
    return (controller()->vc_client_ != nullptr &&
            controller()->vc_client_ == controller()->active_client_);
  }

  bool vsync_acknowledge_delivered(uint64_t cookie) {
    fbl::AutoLock l(controller()->mtx());
    fbl::AutoLock cl(&controller()->primary_client_->mtx_);
    return controller()->primary_client_->handler_.LatestAckedCookie() == cookie;
  }

  size_t get_gamma_table_size() {
    fbl::AutoLock l(controller()->mtx());
    fbl::AutoLock cl(&controller()->primary_client_->mtx_);
    return controller()->primary_client_->handler_.GetGammaTableSize();
  }

  void SendVsyncAfterUnbind(std::unique_ptr<TestFidlClient> client, uint64_t display_id) {
    fbl::AutoLock l(controller()->mtx());
    // Reseting client will *start* client tear down.
    client.reset();
    ClientProxy* client_ptr = controller()->active_client_;
    EXPECT_OK(sync_completion_wait(client_ptr->handler_.fidl_unbound(), zx::sec(1).get()));
    // EnableVsync(false) has not completed here, because we are still holding controller()->mtx()
    client_ptr->OnDisplayVsync(display_id, 0, kInvalidConfigStampBanjo);
  }

  bool primary_client_dead() {
    fbl::AutoLock l(controller()->mtx());
    return controller()->primary_client_ == nullptr;
  }

  bool virtcon_client_dead() {
    fbl::AutoLock l(controller()->mtx());
    return controller()->vc_client_ == nullptr;
  }

  void client_proxy_send_vsync() {
    fbl::AutoLock l(controller()->mtx());
    controller()->active_client_->OnDisplayVsync(0, 0, kInvalidConfigStampBanjo);
  }

  void SendDisplayVsync() { display()->SendVsync(); }

  // |TestBase|
  void SetUp() override {
    TestBase::SetUp();
    zx::result<fidl::Endpoints<sysmem::Allocator>> endpoints =
        fidl::CreateEndpoints<sysmem::Allocator>();
    ASSERT_OK(endpoints.status_value());
    auto& [client, server] = endpoints.value();
    EXPECT_TRUE(sysmem_fidl()->ConnectV1(std::move(server)).ok());
    sysmem_ = fidl::WireSyncClient(std::move(client));
    const fidl::OneWayStatus status = sysmem_->SetDebugClientInfo(
        fidl::StringView::FromExternal(fsl::GetCurrentProcessName()), fsl::GetCurrentProcessKoid());
    EXPECT_OK(status.status());
  }

  // |TestBase|
  void TearDown() override {
    // Wait until the display core has processed all client disconnections before sending the last
    // vsync.
    EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
        [this]() { return primary_client_dead() && virtcon_client_dead(); }));

    // Send one last vsync, to make sure any blank configs take effect.
    SendDisplayVsync();
    EXPECT_EQ(0u, controller()->TEST_imported_images_count());
    TestBase::TearDown();
  }

  fidl::WireSyncClient<sysmem::Allocator> sysmem_;
};

TEST_F(IntegrationTest, DISABLED_ClientsCanBail) {
  for (size_t i = 0; i < 100; i++) {
    RunLoopWithTimeoutOrUntil([this]() { return !primary_client_connected(); }, zx::sec(1));
    TestFidlClient client(sysmem_);
    ASSERT_TRUE(client.CreateChannel(display_fidl(), false));
    ASSERT_TRUE(client.Bind(dispatcher()));
  }
}

TEST_F(IntegrationTest, MustUseUniqueEvenIDs) {
  TestFidlClient client(sysmem_);
  ASSERT_TRUE(client.CreateChannel(display_fidl(), false));
  ASSERT_TRUE(client.Bind(dispatcher()));
  zx::event event_a, event_b, event_c;
  ASSERT_OK(zx::event::create(0, &event_a));
  ASSERT_OK(zx::event::create(0, &event_b));
  ASSERT_OK(zx::event::create(0, &event_c));
  {
    fbl::AutoLock lock(client.mtx());
    EXPECT_OK(client.dc_->ImportEvent(std::move(event_a), 123).status());
    // ImportEvent is one way. Expect the next call to fail.
    EXPECT_OK(client.dc_->ImportEvent(std::move(event_b), 123).status());
    // This test passes if it closes without deadlocking.
  }
  // TODO: Use LLCPP epitaphs when available to detect ZX_ERR_PEER_CLOSED.
}

TEST_F(IntegrationTest, SendVsyncsAfterEmptyConfig) {
  TestFidlClient vc_client(sysmem_);
  ASSERT_TRUE(vc_client.CreateChannel(display_fidl(), /*is_vc=*/true));
  {
    fbl::AutoLock lock(vc_client.mtx());
    EXPECT_EQ(ZX_OK, vc_client.dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, vc_client.dc_->ApplyConfig().status());
  }

  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // Present an image
  EXPECT_OK(primary_client->PresentLayers());
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, id = primary_client->display_id()]() {
        fbl::AutoLock lock(controller()->mtx());
        auto info = display_info(id);
        return info->layer_count == 1;
      },
      zx::sec(1)));
  auto count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), count]() { return p->vsync_count() > count; }, zx::sec(1)));

  // Set an empty config
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_OK(primary_client->dc_->SetDisplayLayers(primary_client->display_id(), {}).status());
    EXPECT_OK(primary_client->dc_->ApplyConfig().status());
  }
  config_stamp_t empty_config_stamp = controller()->TEST_controller_stamp();
  // Wait for it to apply
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, id = primary_client->displays_[0].id_]() {
        fbl::AutoLock lock(controller()->mtx());
        auto info = display_info(id);
        return info->layer_count == 0;
      },
      zx::sec(1)));

  // The old client disconnects
  primary_client.reset();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return primary_client_dead(); }));

  // A new client connects
  primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }));
  // ... and presents before the previous client's empty vsync
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers());
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, id = primary_client->display_id()]() {
        fbl::AutoLock lock(controller()->mtx());
        auto info = display_info(id);
        return info->layer_count == 1;
      },
      zx::sec(1)));

  // Empty vsync for last client. Nothing should be sent to the new client.
  controller()->DisplayControllerInterfaceOnDisplayVsync(primary_client->display_id(), 0u,
                                                         &empty_config_stamp);

  // Send a second vsync, using the config the client applied.
  count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [count, p = primary_client.get()]() { return p->vsync_count() > count; }, zx::sec(1)));
}

TEST_F(IntegrationTest, DISABLED_SendVsyncsAfterClientsBail) {
  TestFidlClient vc_client(sysmem_);
  ASSERT_TRUE(vc_client.CreateChannel(display_fidl(), /*is_vc=*/true));
  {
    fbl::AutoLock lock(vc_client.mtx());
    EXPECT_EQ(ZX_OK, vc_client.dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, vc_client.dc_->ApplyConfig().status());
  }

  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // Present an image
  EXPECT_OK(primary_client->PresentLayers());
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, id = primary_client->display_id()]() {
        fbl::AutoLock lock(controller()->mtx());
        auto info = display_info(id);
        return info->layer_count == 1;
      },
      zx::sec(1)));

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return p->vsync_count() == 1; }, zx::sec(1)));
  // Send the controller a vsync for an image / a config it won't recognize anymore.
  config_stamp_t invalid_config_stamp = {.value = controller()->TEST_controller_stamp().value - 1};
  controller()->DisplayControllerInterfaceOnDisplayVsync(primary_client->display_id(), 0u,
                                                         &invalid_config_stamp);

  // Send a second vsync, using the config the client applied.
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return p->vsync_count() == 2; }, zx::sec(1)));
  EXPECT_EQ(2u, primary_client->vsync_count());
}

TEST_F(IntegrationTest, SendVsyncsAfterClientDies) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));
  auto id = primary_client->display_id();
  SendVsyncAfterUnbind(std::move(primary_client), id);
}

TEST_F(IntegrationTest, AcknowledgeVsync) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));
  EXPECT_EQ(0u, primary_client->vsync_count());
  EXPECT_EQ(0u, primary_client->get_cookie());

  // send vsyncs upto watermark level
  for (uint32_t i = 0; i < ClientProxy::kVsyncMessagesWatermark; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return (p->get_cookie() != 0); }, zx::sec(3)));
  EXPECT_EQ(ClientProxy::kVsyncMessagesWatermark, primary_client->vsync_count());

  // acknowledge
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(primary_client->get_cookie());
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));
}

TEST_F(IntegrationTest, AcknowledgeVsyncAfterQueueFull) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // send vsyncs until max vsync
  uint32_t vsync_count = ClientProxy::kMaxVsyncMessages;
  while (vsync_count--) {
    client_proxy_send_vsync();
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return (p->vsync_count() == ClientProxy::kMaxVsyncMessages); },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());
  EXPECT_NE(0u, primary_client->get_cookie());

  // At this point, display will not send any more vsync events. Let's confirm by sending a few
  constexpr uint32_t kNumVsync = 5;
  for (uint32_t i = 0; i < kNumVsync; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());

  // now let's acknowledge vsync
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(primary_client->get_cookie());
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // After acknowledge, we should expect to get all the stored messages + the latest vsync
  client_proxy_send_vsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() == ClientProxy::kMaxVsyncMessages + kNumVsync + 1);
      },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages + kNumVsync + 1, primary_client->vsync_count());
}

TEST_F(IntegrationTest, AcknowledgeVsyncAfterLongTime) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // send vsyncs until max vsyncs
  for (uint32_t i = 0; i < ClientProxy::kMaxVsyncMessages; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return (p->vsync_count() == ClientProxy::kMaxVsyncMessages); },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());
  EXPECT_NE(0u, primary_client->get_cookie());

  // At this point, display will not send any more vsync events. Let's confirm by sending a lot
  constexpr uint32_t kNumVsync = ClientProxy::kVsyncBufferSize * 10;
  for (uint32_t i = 0; i < kNumVsync; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());

  // now let's acknowledge vsync
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(primary_client->get_cookie());
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // After acknowledge, we should expect to get all the stored messages + the latest vsync
  client_proxy_send_vsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() ==
                ClientProxy::kMaxVsyncMessages + ClientProxy::kVsyncBufferSize + 1);
      },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages + ClientProxy::kVsyncBufferSize + 1,
            primary_client->vsync_count());
}

TEST_F(IntegrationTest, InvalidVSyncCookie) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // send vsyncs until max vsync
  for (uint32_t i = 0; i < ClientProxy::kMaxVsyncMessages; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return (p->vsync_count() == ClientProxy::kMaxVsyncMessages); },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());
  EXPECT_NE(0u, primary_client->get_cookie());

  // At this point, display will not send any more vsync events. Let's confirm by sending a few
  constexpr uint32_t kNumVsync = 5;
  for (uint32_t i = 0; i < kNumVsync; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());

  // now let's acknowledge vsync with invalid cookie
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(0xdeadbeef);
  }
  EXPECT_FALSE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // We should still not receive vsync events since acknowledge did not use valid cookie
  client_proxy_send_vsync();
  EXPECT_FALSE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() == ClientProxy::kMaxVsyncMessages + kNumVsync + 1);
      },
      zx::sec(1)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());
}

TEST_F(IntegrationTest, AcknowledgeVsyncWithOldCookie) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // send vsyncs until max vsync
  for (uint32_t i = 0; i < ClientProxy::kMaxVsyncMessages; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return (p->vsync_count() == ClientProxy::kMaxVsyncMessages); },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());
  EXPECT_NE(0u, primary_client->get_cookie());

  // At this point, display will not send any more vsync events. Let's confirm by sending a few
  constexpr uint32_t kNumVsync = 5;
  for (uint32_t i = 0; i < kNumVsync; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages, primary_client->vsync_count());

  // now let's acknowledge vsync
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(primary_client->get_cookie());
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // After acknowledge, we should expect to get all the stored messages + the latest vsync
  client_proxy_send_vsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() == ClientProxy::kMaxVsyncMessages + kNumVsync + 1);
      },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages + kNumVsync + 1, primary_client->vsync_count());

  // save old cookie
  uint64_t old_cookie = primary_client->get_cookie();

  // send vsyncs until max vsync
  for (uint32_t i = 0; i < ClientProxy::kMaxVsyncMessages; i++) {
    client_proxy_send_vsync();
  }

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() == ClientProxy::kMaxVsyncMessages * 2);
      },
      zx::sec(3)));
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages * 2, primary_client->vsync_count());
  EXPECT_NE(0u, primary_client->get_cookie());

  // At this point, display will not send any more vsync events. Let's confirm by sending a few
  for (uint32_t i = 0; i < ClientProxy::kVsyncBufferSize; i++) {
    client_proxy_send_vsync();
  }
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages * 2, primary_client->vsync_count());

  // now let's acknowledge vsync with old cookie
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(old_cookie);
  }
  EXPECT_FALSE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // Since we did not acknowledge with most recent cookie, we should not get any vsync events back
  client_proxy_send_vsync();
  EXPECT_FALSE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() == (ClientProxy::kMaxVsyncMessages * 2) + kNumVsync + 1);
      },
      zx::sec(1)));
  // count should still remain the same
  EXPECT_EQ(ClientProxy::kMaxVsyncMessages * 2, primary_client->vsync_count());

  // now let's acknowledge with valid cookie
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->AcknowledgeVsync(primary_client->get_cookie());
  }
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, p = primary_client.get()]() { return vsync_acknowledge_delivered(p->get_cookie()); },
      zx::sec(1)));

  // After acknowledge, we should expect to get all the stored messages + the latest vsync
  client_proxy_send_vsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() {
        return (p->vsync_count() ==
                (ClientProxy::kMaxVsyncMessages * 2) + ClientProxy::kVsyncBufferSize + 1);
      },
      zx::sec(3)));
  EXPECT_EQ((ClientProxy::kMaxVsyncMessages * 2) + ClientProxy::kVsyncBufferSize + 1,
            primary_client->vsync_count());
}

TEST_F(IntegrationTest, ImportGammaTable) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  uint64_t gamma_table_id = 3;
  ::fidl::Array<float, 256> gamma_red = {{0.1f}};
  ::fidl::Array<float, 256> gamma_green = {{0.2f}};
  ::fidl::Array<float, 256> gamma_blue = {{0.3f}};
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ImportGammaTable(gamma_table_id, gamma_red, gamma_green, gamma_blue);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 1; }, zx::sec(1)));
  }
}

TEST_F(IntegrationTest, ReleaseGammaTable) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  uint64_t gamma_table_id = 3;
  ::fidl::Array<float, 256> gamma_red = {{0.1f}};
  ::fidl::Array<float, 256> gamma_green = {{0.2f}};
  ::fidl::Array<float, 256> gamma_blue = {{0.3f}};
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ImportGammaTable(gamma_table_id, gamma_red, gamma_green, gamma_blue);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 1; }, zx::sec(1)));
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ReleaseGammaTable(gamma_table_id);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 0; }, zx::sec(1)));
  }
}

TEST_F(IntegrationTest, ReleaseInvalidGammaTable) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  uint64_t gamma_table_id = 3;
  ::fidl::Array<float, 256> gamma_red = {{0.1f}};
  ::fidl::Array<float, 256> gamma_green = {{0.2f}};
  ::fidl::Array<float, 256> gamma_blue = {{0.3f}};
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ImportGammaTable(gamma_table_id, gamma_red, gamma_green, gamma_blue);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 1; }, zx::sec(1)));
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ReleaseGammaTable(gamma_table_id + 5);
    EXPECT_FALSE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 0; }, zx::sec(1)));
  }
}

TEST_F(IntegrationTest, SetGammaTable) {
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  uint64_t gamma_table_id = 3;
  ::fidl::Array<float, 256> gamma_red = {{0.1f}};
  ::fidl::Array<float, 256> gamma_green = {{0.2f}};
  ::fidl::Array<float, 256> gamma_blue = {{0.3f}};
  {
    fbl::AutoLock lock(primary_client->mtx());
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->ImportGammaTable(gamma_table_id, gamma_red, gamma_green, gamma_blue);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return get_gamma_table_size() == 1; }, zx::sec(1)));
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->SetDisplayGammaTable(primary_client->display_id(), gamma_table_id);
  }
}

TEST_F(IntegrationTest, ImportImage_InvalidCollection) {
  TestFidlClient client(sysmem_);
  ASSERT_TRUE(client.CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(client.Bind(dispatcher()));

  fbl::AutoLock lock(client.mtx());
  auto cl_reply = client.dc_->CreateLayer();
  ASSERT_TRUE(cl_reply.ok());
  ASSERT_OK(cl_reply.value().res);
  // Importing an image from a non-existent collection should fail.
  auto ii_reply =
      client.dc_->ImportImage(client.displays_[0].image_config_, 0xffeeeedd, /* image_id= */ 1, 0);
  ASSERT_NE(ii_reply.value().res, ZX_OK);
}

TEST_F(IntegrationTest, ClampRgb) {
  // Create vc client
  TestFidlClient vc_client(sysmem_);
  ASSERT_TRUE(vc_client.CreateChannel(display_fidl(), /*is_vc=*/true));
  {
    fbl::AutoLock lock(vc_client.mtx());
    // set mode to Fallback
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)vc_client.dc_->SetVirtconMode(1);
    EXPECT_TRUE(
        RunLoopWithTimeoutOrUntil([this]() { return virtcon_client_connected(); }, zx::sec(1)));
    // Clamp RGB to a minimum value
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)vc_client.dc_->SetMinimumRgb(32);
    EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return display()->GetClampRgbValue() == 32; },
                                          zx::sec(1)));
  }

  // Create a primary client
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));
  {
    fbl::AutoLock lock(primary_client->mtx());
    // Clamp RGB to a new value
    // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
    (void)primary_client->dc_->SetMinimumRgb(1);
    EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return display()->GetClampRgbValue() == 1; },
                                          zx::sec(1)));
  }
  // close client and wait for virtcon to become active again
  primary_client.reset(nullptr);
  // Apply a config for virtcon client to become active.
  {
    fbl::AutoLock lock(vc_client.mtx());
    EXPECT_EQ(ZX_OK, vc_client.dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, vc_client.dc_->ApplyConfig().status());
  }
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return virtcon_client_connected(); }, zx::sec(1)));
  SendDisplayVsync();
  // make sure clamp value was restored
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return display()->GetClampRgbValue() == 32; },
                                        zx::sec(1)));
}

TEST_F(IntegrationTest, EmptyConfigIsNotApplied) {
  // Create and bind virtcon client.
  TestFidlClient vc_client(sysmem_);
  ASSERT_TRUE(vc_client.CreateChannel(display_fidl(), /*is_vc=*/true));
  {
    fbl::AutoLock lock(vc_client.mtx());
    EXPECT_EQ(ZX_OK, vc_client.dc_
                         ->SetVirtconMode(static_cast<uint8_t>(
                             fuchsia_hardware_display::wire::VirtconMode::kFallback))
                         .status());
  }
  ASSERT_TRUE(vc_client.Bind(dispatcher()));
  {
    fbl::AutoLock lock(vc_client.mtx());
    EXPECT_EQ(ZX_OK, vc_client.dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, vc_client.dc_->ApplyConfig().status());
  }
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return virtcon_client_connected(); }, zx::sec(1)));

  // Create and bind primary client.
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(1)));

  // Virtcon client should remain active until primary client has set a config.
  auto vc_vsync_count = vc_client.vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = &vc_client, vc_vsync_count]() { return p->vsync_count() > vc_vsync_count; },
      zx::sec(1)));
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get()]() { return p->vsync_count() == 0; }, zx::sec(1)));

  // Present an image from the primary client.
  EXPECT_OK(primary_client->PresentLayers());
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [this, id = primary_client->display_id()]() {
        fbl::AutoLock lock(controller()->mtx());
        auto info = display_info(id);
        return info->layer_count == 1;
      },
      zx::sec(1)));

  // Primary client should have become active after a config was set.
  auto primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(1)));
}

// This tests the basic behavior of ApplyConfig() and OnVsync() events.
// We test applying configurations with images without wait fences, so they are
// guaranteed to be ready when client calls ApplyConfig().
//
// In this case, the new configuration stamp is guaranteed to appear in the next
// coming OnVsync() event.
//
// Here we test the following case:
//
//  * ApplyConfig({layerA: img0}) ==> config_stamp_1
//  - Vsync now should have config_stamp_1
//  * ApplyConfig({layerA: img1}) ==> config_stamp_2
//  - Vsync now should have config_stamp_2
//  * ApplyConfig({}) ==> config_stamp_3
//  - Vsync now should have config_stamp_3
//
// Both images are ready at ApplyConfig() time, i.e. no fences are provided.
TEST_F(IntegrationTest, VsyncEvent) {
  // Create and bind primary client.
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  // Apply a config for client to become active.
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_EQ(ZX_OK, primary_client->dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, primary_client->dc_->ApplyConfig().status());
  }
  auto apply_config_stamp_0 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_0);
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(2)));

  auto primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));

  auto present_config_stamp_0 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_0 == present_config_stamp_0);
  EXPECT_NE(0u, present_config_stamp_0.value);

  auto create_default_layer_result = primary_client->CreateLayer();
  auto create_image_0_result = primary_client->CreateImage();
  auto create_image_1_result = primary_client->CreateImage();

  EXPECT_EQ(ZX_OK, create_default_layer_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_0_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_result.status_value());

  auto default_layer_id = create_default_layer_result.value();
  auto image_0_id = create_image_0_result.value();
  auto image_1_id = create_image_1_result.value();

  // Present one single image without wait.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_0_id,
                        .image_ready_wait_event_id = std::nullopt},
                   }));
  auto apply_config_stamp_1 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_1);
  EXPECT_TRUE(apply_config_stamp_1 > apply_config_stamp_0);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_1 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_1 == present_config_stamp_1);

  // Present another image layer without wait.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_1_id,
                        .image_ready_wait_event_id = std::nullopt},
                   }));
  auto apply_config_stamp_2 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_2);
  EXPECT_TRUE(apply_config_stamp_2 > apply_config_stamp_1);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_2 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_2 == present_config_stamp_2);

  // Hide the existing layer.
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_EQ(ZX_OK, primary_client->dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, primary_client->dc_->ApplyConfig().status());
  }
  auto apply_config_stamp_3 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_3);
  EXPECT_TRUE(apply_config_stamp_3 > apply_config_stamp_2);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(0u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_3 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_3 == present_config_stamp_3);
}

// This tests the behavior of ApplyConfig() and OnVsync() events when images
// come with wait fences, which is a common use case in Scenic when using GPU
// composition.
//
// When applying configurations with pending images, the config_stamp returned
// from OnVsync() should not be updated unless the image becomes ready and
// triggers a ReapplyConfig().
//
// Here we test the following case:
//
//  * ApplyConfig({layerA: img0}) ==> config_stamp_1
//  - Vsync now should have config_stamp_1
//  * ApplyConfig({layerA: img1, wait on fence1}) ==> config_stamp_2
//  - Vsync now should have config_stamp_1
//  * Signal fence1
//  - Vsync now should have config_stamp_2
//
TEST_F(IntegrationTest, VsyncWaitForPendingImages) {
  // Create and bind primary client.
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  // Apply a config for client to become active.
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_EQ(ZX_OK, primary_client->dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, primary_client->dc_->ApplyConfig().status());
  }
  auto apply_config_stamp_0 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_0);
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(2)));

  auto primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));

  auto present_config_stamp_0 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_0 == present_config_stamp_0);
  EXPECT_NE(0u, present_config_stamp_0.value);

  auto create_default_layer_result = primary_client->CreateLayer();
  auto create_image_0_result = primary_client->CreateImage();
  auto create_image_1_result = primary_client->CreateImage();
  auto create_image_1_ready_fence_result = primary_client->CreateEvent();

  EXPECT_EQ(ZX_OK, create_default_layer_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_0_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_ready_fence_result.status_value());

  auto default_layer_id = create_default_layer_result.value();
  auto image_0_id = create_image_0_result.value();
  auto image_1_id = create_image_1_result.value();
  auto image_1_ready_fence = std::move(create_image_1_ready_fence_result.value());

  // Present one single image without wait.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_0_id,
                        .image_ready_wait_event_id = std::nullopt},
                   }));
  auto apply_config_stamp_1 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_1);
  EXPECT_TRUE(apply_config_stamp_1 > apply_config_stamp_0);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_1 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_1 == present_config_stamp_1);

  // Present another image layer; but the image is not ready yet. So the
  // configuration applied to display device will be still the old one. On Vsync
  // the |presented_config_stamp| is still |config_stamp_1|.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_1_id,
                        .image_ready_wait_event_id = std::make_optional(image_1_ready_fence.id)},
                   }));
  auto apply_config_stamp_2 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_2);
  EXPECT_TRUE(apply_config_stamp_2 >= apply_config_stamp_1);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_2 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_2 == present_config_stamp_1);

  // Signal the event. Display Fence callback will be signaled, and new
  // configuration with new config stamp (config_stamp_2) will be used.
  // On next Vsync, the |presented_config_stamp| will be updated.
  auto old_controller_stamp = controller()->TEST_controller_stamp();
  image_1_ready_fence.event.signal(0u, ZX_EVENT_SIGNALED);
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [controller = controller(), old_controller_stamp]() {
        return controller->TEST_controller_stamp() > old_controller_stamp;
      },
      zx::sec(2)));

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_3 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_3 == apply_config_stamp_2);
}

// This tests the behavior of ApplyConfig() and OnVsync() events when images
// that comes with wait fences are hidden in subsequent configurations.
//
// If a pending image never becomes ready, the config_stamp returned from
// OnVsync() should not be updated unless the image layer has been removed from
// the display in a subsequent configuration.
//
// Here we test the following case:
//
//  * ApplyConfig({layerA: img0}) ==> config_stamp_1
//  - Vsync now should have config_stamp_1
//  * ApplyConfig({layerA: img1, waiting on fence1}) ==> config_stamp_2
//  - Vsync now should have config_stamp_1
//  * ApplyConfig({}) ==> config_stamp_3
//  - Vsync now should have config_stamp_3
//
// Note that fence1 is never signaled.
//
TEST_F(IntegrationTest, VsyncHidePendingLayer) {
  // Create and bind primary client.
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));
  // Apply a config for client to become active.
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_EQ(ZX_OK, primary_client->dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, primary_client->dc_->ApplyConfig().status());
  }
  auto apply_config_stamp_0 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_0);
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(2)));

  auto primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));

  auto present_config_stamp_0 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_0 == present_config_stamp_0);
  EXPECT_NE(0u, present_config_stamp_0.value);

  auto create_default_layer_result = primary_client->CreateLayer();
  auto create_image_0_result = primary_client->CreateImage();
  auto create_image_1_result = primary_client->CreateImage();
  auto create_image_1_ready_fence_result = primary_client->CreateEvent();

  EXPECT_EQ(ZX_OK, create_default_layer_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_0_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_ready_fence_result.status_value());

  auto default_layer_id = create_default_layer_result.value();
  auto image_0_id = create_image_0_result.value();
  auto image_1_id = create_image_1_result.value();
  auto image_1_ready_fence = std::move(create_image_1_ready_fence_result.value());

  // Present an image layer.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_0_id,
                        .image_ready_wait_event_id = std::nullopt},
                   }));
  auto apply_config_stamp_1 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_1);
  EXPECT_TRUE(apply_config_stamp_1 > apply_config_stamp_0);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_1 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_1 == present_config_stamp_1);

  // Present another image layer; but the image is not ready yet. Display
  // controller will wait on the fence and Vsync will return the previous
  // configuration instead.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_1_id,
                        .image_ready_wait_event_id = image_1_ready_fence.id},
                   }));
  auto apply_config_stamp_2 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_2);
  EXPECT_TRUE(apply_config_stamp_2 > apply_config_stamp_1);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_2 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_2 == present_config_stamp_1);

  // Hide the image layer. Display controller will not care about the fence
  // and thus use the latest configuration stamp.
  {
    fbl::AutoLock lock(primary_client->mtx());
    EXPECT_EQ(ZX_OK, primary_client->dc_->SetDisplayLayers(1, {}).status());
    EXPECT_EQ(ZX_OK, primary_client->dc_->ApplyConfig().status());
  }
  auto apply_config_stamp_3 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_3);
  EXPECT_TRUE(apply_config_stamp_3 >= apply_config_stamp_2);

  // On Vsync, the configuration stamp client receives on Vsync event message
  // will be the latest one applied to the display controller, since the pending
  // image has been removed from the configuration.
  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(0u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_3 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_3 == apply_config_stamp_3);
}

// This tests the behavior of ApplyConfig() and OnVsync() events when images
// that comes with wait fences are overridden in subsequent configurations.
//
// If a client applies a configuration (#1) with a pending image, while display
// controller waits for the image to be ready, the client may apply another
// configuration (#2) with a different image. If the image in configuration #2
// becomes available earlier than #1, the layer configuration in #1 should be
// overridden, and signaling wait fences in #1 should not trigger a
// ReapplyConfig().
//
// Here we test the following case:
//
//  * ApplyConfig({layerA: img0}) ==> config_stamp_1
//  - Vsync now should have config_stamp_1
//  * ApplyConfig({layerA: img1, waiting on fence1}) ==> config_stamp_2
//  - Vsync now should have config_stamp_1 since img1 is not ready yet
//  * ApplyConfig({layerA: img2, waiting on fence2}) ==> config_stamp_3
//  - Vsync now should have config_stamp_1 since img1 and img2 are not ready
//  * Signal fence2
//  - Vsync now should have config_stamp_3.
//  * Signal fence1
//  - Vsync .
//
// Note that fence1 is never signaled.
TEST_F(IntegrationTest, VsyncSkipOldPendingConfiguration) {
  // Create and bind primary client.
  auto primary_client = std::make_unique<TestFidlClient>(sysmem_);
  ASSERT_TRUE(primary_client->CreateChannel(display_fidl(), /*is_vc=*/false));
  ASSERT_TRUE(primary_client->Bind(dispatcher()));

  auto create_default_layer_result = primary_client->CreateLayer();
  auto create_image_0_result = primary_client->CreateImage();
  auto create_image_1_result = primary_client->CreateImage();
  auto create_image_2_result = primary_client->CreateImage();
  auto create_image_1_ready_fence_result = primary_client->CreateEvent();
  auto create_image_2_ready_fence_result = primary_client->CreateEvent();

  EXPECT_EQ(ZX_OK, create_default_layer_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_0_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_2_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_1_ready_fence_result.status_value());
  EXPECT_EQ(ZX_OK, create_image_2_ready_fence_result.status_value());

  auto default_layer_id = create_default_layer_result.value();
  auto image_0_id = create_image_0_result.value();
  auto image_1_id = create_image_1_result.value();
  auto image_2_id = create_image_2_result.value();
  auto image_1_ready_fence = std::move(create_image_1_ready_fence_result.value());
  auto image_2_ready_fence = std::move(create_image_2_ready_fence_result.value());

  // Apply a config for client to become active; Present an image layer.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_0_id,
                        .image_ready_wait_event_id = std::nullopt},
                   }));
  auto apply_config_stamp_0 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_0);
  EXPECT_TRUE(
      RunLoopWithTimeoutOrUntil([this]() { return primary_client_connected(); }, zx::sec(2)));

  auto primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_0 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(apply_config_stamp_0 == present_config_stamp_0);
  EXPECT_NE(0u, present_config_stamp_0.value);

  // Present another image layer (image #1, wait_event #0); but the image is not
  // ready yet. Display controller will wait on the fence and Vsync will return
  // the previous configuration instead.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_1_id,
                        .image_ready_wait_event_id = image_1_ready_fence.id},
                   }));
  auto apply_config_stamp_1 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_1);
  EXPECT_TRUE(apply_config_stamp_1 > apply_config_stamp_0);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));

  auto present_config_stamp_1 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_1 == present_config_stamp_0);

  // Present another image layer (image #2, wait_event #1); the image is not
  // ready as well. We should still see current |presented_config_stamp| to be
  // equal to |present_config_stamp_0|.
  EXPECT_EQ(ZX_OK, primary_client->PresentLayers({
                       {.layer_id = default_layer_id,
                        .image_id = image_2_id,
                        .image_ready_wait_event_id = image_2_ready_fence.id},
                   }));
  auto apply_config_stamp_2 = primary_client->GetRecentAppliedConfigStamp();
  EXPECT_TRUE(kInvalidConfigStampFidl != apply_config_stamp_2);
  EXPECT_TRUE(apply_config_stamp_2 > apply_config_stamp_1);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));

  auto present_config_stamp_2 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_2 == present_config_stamp_1);

  // Signal the event #1. Display Fence callback will be signaled, and
  // configuration with new config stamp (apply_config_stamp_2) will be used.
  // On next Vsync, the |presented_config_stamp| will be updated.
  auto old_controller_stamp = controller()->TEST_controller_stamp();
  image_2_ready_fence.event.signal(0u, ZX_EVENT_SIGNALED);
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [controller = controller(), old_controller_stamp]() {
        return controller->TEST_controller_stamp() > old_controller_stamp;
      },
      zx::sec(2)));

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_3 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_3 == apply_config_stamp_2);

  // Signal the event #0. Since we have displayed a newer image, signaling the
  // old event associated with the old image shouldn't trigger ReapplyConfig().
  // We should still see |apply_config_stamp_2| as the latest presented config
  // stamp in the client.
  old_controller_stamp = controller()->TEST_controller_stamp();
  image_1_ready_fence.event.signal(0u, ZX_EVENT_SIGNALED);

  primary_vsync_count = primary_client->vsync_count();
  SendDisplayVsync();
  EXPECT_TRUE(RunLoopWithTimeoutOrUntil(
      [p = primary_client.get(), primary_vsync_count]() {
        return p->vsync_count() > primary_vsync_count;
      },
      zx::sec(2)));
  {
    fbl::AutoLock lock(controller()->mtx());
    EXPECT_EQ(1u, display_info(primary_client->display_id())->layer_count);
  }

  auto present_config_stamp_4 = primary_client->recent_presented_config_stamp();
  EXPECT_TRUE(present_config_stamp_4 == apply_config_stamp_2);
}

// TODO(fxbug.dev/90423): Currently the fake-display driver only supports one
// primary layer. In order to better test ApplyConfig() / OnVsync() behavior,
// we should make fake-display driver support multi-layer configurations and
// then we could add more multi-layer tests.

}  // namespace display
