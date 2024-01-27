// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <endian.h>
#include <fidl/fuchsia.hardware.radar/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/fdio.h>
#include <lib/fit/function.h>
#include <lib/sync/completion.h>
#include <lib/zx/channel.h>
#include <stdio.h>

#include <future>

#include <fbl/unique_fd.h>
#include <zxtest/zxtest.h>

namespace {

using BurstReaderProvider = fuchsia_hardware_radar::RadarBurstReaderProvider;
using BurstReader = fuchsia_hardware_radar::RadarBurstReader;
using BurstRequest = fuchsia_hardware_radar::wire::RadarBurstReaderOnBurst2Request;

constexpr size_t kBurstSize = 23247;

class RadarIntegrationTest : public zxtest::Test {
 public:
  RadarIntegrationTest() : loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override { loop_.StartThread("radar-integration-test dispatcher"); }

 protected:
  std::future<void> MakeRadarClient(fidl::WireSharedClient<BurstReader>* out_client) {
    return MakeRadarClient({}, out_client);
  }

  std::future<void> MakeRadarClient(fit::function<void(const BurstRequest&)> burst_handler,
                                    fidl::WireSharedClient<BurstReader>* out_client) {
    std::future<void> out_client_torn_down;
    MakeRadarClient(std::move(burst_handler), out_client, &out_client_torn_down);
    return out_client_torn_down;
  }

  static void CheckBurst(const std::array<uint8_t, kBurstSize>& burst) {
    uint32_t config_id;
    memcpy(&config_id, burst.data(), sizeof(config_id));
    EXPECT_EQ(config_id, 0);

    EXPECT_EQ(burst[4], 30);  // Burst rate in Hz.
    EXPECT_EQ(burst[5], 20);  // Chirps per burst.

    uint16_t chirp_rate_hz;
    memcpy(&chirp_rate_hz, &burst[6], sizeof(chirp_rate_hz));
    EXPECT_EQ(be16toh(chirp_rate_hz), 3000);

    uint16_t samples_per_chirp;
    memcpy(&samples_per_chirp, &burst[8], sizeof(samples_per_chirp));
    EXPECT_EQ(be16toh(samples_per_chirp), 256);

    EXPECT_EQ(burst[10], 0x07);  // RX channel mask.

    uint64_t driver_timestamp, host_timestamp;
    mempcpy(&driver_timestamp, &burst[11], sizeof(driver_timestamp));
    mempcpy(&host_timestamp, &burst[19], sizeof(host_timestamp));
    EXPECT_EQ(driver_timestamp, host_timestamp);
  }

 private:
  void MakeRadarClient(fit::function<void(const BurstRequest&)> burst_handler,
                       fidl::WireSharedClient<BurstReader>* out_client,
                       std::future<void>* out_client_torn_down) {
    auto provider_client_end_or = component::Connect<BurstReaderProvider>();
    ASSERT_OK(provider_client_end_or.status_value());

    fidl::WireSyncClient<BurstReaderProvider> provider_client(
        std::move(provider_client_end_or.value()));

    zx::result endpoints = fidl::CreateEndpoints<BurstReader>();
    ASSERT_OK(endpoints.status_value());
    auto [client_end, server_end] = std::move(*endpoints);

    const auto result = provider_client->Connect(std::move(server_end));
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());

    std::promise<void> client_torn_down_promise;
    *out_client_torn_down = client_torn_down_promise.get_future();
    out_client->Bind(std::move(client_end), loop_.dispatcher(),
                     std::make_unique<EventHandler>(std::move(burst_handler),
                                                    std::move(client_torn_down_promise)));
  }

  class EventHandler : public fidl::WireAsyncEventHandler<BurstReader> {
   public:
    explicit EventHandler(fit::function<void(const BurstRequest&)> burst_handler,
                          std::promise<void> client_torn_down_promise)
        : burst_handler_(std::move(burst_handler)),
          client_torn_down_promise_(std::move(client_torn_down_promise)) {}

    ~EventHandler() override { client_torn_down_promise_.set_value(); }

    void OnBurst2(fidl::WireEvent<BurstReader::OnBurst2>* event) override {
      if (burst_handler_) {
        burst_handler_(*event);
      }
    }

    void on_fidl_error(fidl::UnbindInfo info) override {}

   private:
    fit::function<void(const BurstRequest&)> burst_handler_;
    std::promise<void> client_torn_down_promise_;
  };

  async::Loop loop_;
};

TEST_F(RadarIntegrationTest, BurstSize) {
  fidl::WireSharedClient<BurstReader> client;
  ASSERT_NO_FAILURES(MakeRadarClient(&client));

  auto result = client.sync()->GetBurstProperties();
  ASSERT_OK(result.status());
  EXPECT_EQ(result->size, kBurstSize);
}

TEST_F(RadarIntegrationTest, Reconnect) {
  fidl::WireSharedClient<BurstReader> client1;
  std::future<void> client1_torn_down;
  ASSERT_NO_FAILURES(client1_torn_down = MakeRadarClient(&client1));

  {
    const auto result = client1.sync()->GetBurstProperties();
    ASSERT_OK(result.status());
    EXPECT_EQ(result->size, kBurstSize);
  }

  // Unbind and close our end of the channel. We should eventually be able to reconnect, after the
  // driver has cleaned up after the last client.
  client1 = {};
  client1_torn_down.wait();

  fidl::WireSharedClient<BurstReader> client2;
  ASSERT_NO_FAILURES(MakeRadarClient(&client2));

  {
    const auto result = client2.sync()->GetBurstProperties();
    ASSERT_OK(result.status());
    EXPECT_EQ(result->size, kBurstSize);
  }
}

TEST_F(RadarIntegrationTest, BurstFormat) {
  fidl::Arena allocator;

  sync_completion_t completion;
  sync_completion_reset(&completion);

  uint32_t received_id = {};

  fidl::WireSharedClient<BurstReader> client;
  ASSERT_NO_FAILURES(MakeRadarClient(
      [&](const BurstRequest& request) {
        if (request.is_burst()) {
          received_id = request.burst().vmo_id;
          sync_completion_signal(&completion);
        }
      },
      &client));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(kBurstSize, 0, &vmo));

  {
    fidl::VectorView<zx::vmo> vmo_dup(allocator, 1);
    ASSERT_OK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dup[0]));

    fidl::VectorView<uint32_t> vmo_id(allocator, 1);
    vmo_id[0] = 1234;

    const auto result = client.sync()->RegisterVmos(vmo_id, vmo_dup);
    ASSERT_OK(result);
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
  }

  EXPECT_OK(client->StartBursts().status());

  sync_completion_wait(&completion, ZX_TIME_INFINITE);

  EXPECT_OK(client.sync()->StopBursts().status());

  EXPECT_EQ(received_id, 1234);

  std::array<uint8_t, kBurstSize> burst;
  ASSERT_OK(vmo.read(burst.data(), 0, burst.size()));
  ASSERT_NO_FATAL_FAILURE(CheckBurst(burst));

  {
    fidl::VectorView<uint32_t> vmo_id(allocator, 1);
    vmo_id[0] = 1234;

    const auto result = client.sync()->UnregisterVmos(vmo_id);
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
    ASSERT_EQ(result->value()->vmos.count(), 1);
    EXPECT_TRUE(result->value()->vmos[0].is_valid());
  }
}

TEST_F(RadarIntegrationTest, ReadManyBursts) {
  constexpr uint32_t kVmoCount = 10;
  constexpr uint32_t kBurstCount = 303;  // Read for about 10 seconds.

  fidl::Arena allocator;

  sync_completion_t completion;
  sync_completion_reset(&completion);

  uint32_t received_burst_count = 0;

  fidl::WireSharedClient<BurstReader> client;
  ASSERT_NO_FAILURES(MakeRadarClient(
      [&](const BurstRequest& request) {
        if (request.is_burst()) {
          [[maybe_unused]] auto call_result = client->UnlockVmo(request.burst().vmo_id);
          if (++received_burst_count >= kBurstCount) {
            sync_completion_signal(&completion);
          }
        }
      },
      &client));

  std::vector<zx::vmo> vmos(kVmoCount);

  {
    fidl::VectorView<zx::vmo> vmo_dups(allocator, kVmoCount);
    fidl::VectorView<uint32_t> vmo_ids(allocator, kVmoCount);

    for (uint32_t i = 0; i < kVmoCount; i++) {
      ASSERT_OK(zx::vmo::create(kBurstSize, 0, &vmos[i]));
      ASSERT_OK(vmos[i].duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dups[i]));
      vmo_ids[i] = i;
    }

    const auto result = client.sync()->RegisterVmos(vmo_ids, vmo_dups);
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
  }

  EXPECT_OK(client->StartBursts().status());

  sync_completion_wait(&completion, ZX_TIME_INFINITE);

  EXPECT_OK(client.sync()->StopBursts().status());

  EXPECT_GE(received_burst_count, kBurstCount);

  {
    fidl::VectorView<uint32_t> vmo_ids(allocator, kVmoCount);
    for (uint32_t i = 0; i < kVmoCount; i++) {
      vmo_ids[i] = i;
    }

    const auto result = client.sync()->UnregisterVmos(vmo_ids);
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
    ASSERT_EQ(result->value()->vmos.count(), kVmoCount);
    for (size_t i = 0; i < kVmoCount; i++) {
      EXPECT_TRUE(result->value()->vmos[i].is_valid());
    }
  }
}

TEST_F(RadarIntegrationTest, ReadManyBurstsMultipleClients) {
  constexpr uint32_t kVmoCount = 10;
  constexpr uint32_t kBurstCount = 303;  // Read for about 10 seconds.

  fidl::Arena allocator;

  struct {
    fidl::WireSharedClient<BurstReader> client;
    sync_completion_t completion;
    uint32_t received_burst_count = 0;
  } clients[3] = {};

  for (auto& client : clients) {
    sync_completion_reset(&client.completion);
    ASSERT_NO_FAILURES(MakeRadarClient(
        [&](const BurstRequest& request) {
          if (request.is_burst()) {
            [[maybe_unused]] auto call_result = client.client->UnlockVmo(request.burst().vmo_id);
            if (++client.received_burst_count >= kBurstCount) {
              sync_completion_signal(&client.completion);
            }
          }
        },
        &client.client));

    std::vector<zx::vmo> vmos(kVmoCount);

    fidl::VectorView<zx::vmo> vmo_dups(allocator, kVmoCount);
    fidl::VectorView<uint32_t> vmo_ids(allocator, kVmoCount);

    for (uint32_t i = 0; i < kVmoCount; i++) {
      ASSERT_OK(zx::vmo::create(kBurstSize, 0, &vmos[i]));
      ASSERT_OK(vmos[i].duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dups[i]));
      vmo_ids[i] = i;
    }

    const auto result = client.client.sync()->RegisterVmos(vmo_ids, vmo_dups);
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
  }

  for (auto& client : clients) {
    EXPECT_OK(client.client->StartBursts().status());
  }

  for (auto& client : clients) {
    sync_completion_wait(&client.completion, ZX_TIME_INFINITE);
  }

  for (auto& client : clients) {
    EXPECT_OK(client.client.sync()->StopBursts().status());
  }

  for (auto& client : clients) {
    EXPECT_GE(client.received_burst_count, kBurstCount);
  }

  for (auto& client : clients) {
    fidl::VectorView<uint32_t> vmo_ids(allocator, kVmoCount);
    for (uint32_t i = 0; i < kVmoCount; i++) {
      vmo_ids[i] = i;
    }

    const auto result = client.client.sync()->UnregisterVmos(vmo_ids);
    ASSERT_OK(result.status());
    ASSERT_TRUE(result->is_ok(), "%d", result->error_value());
    ASSERT_EQ(result->value()->vmos.count(), kVmoCount);
    for (size_t i = 0; i < kVmoCount; i++) {
      EXPECT_TRUE(result->value()->vmos[i].is_valid());
    }
  }
}

}  // namespace
