// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/cpp/message.h>
#include <lib/fidl/cpp/wire/transaction.h>
#include <lib/fidl/txn_header.h>
#include <lib/stdcompat/span.h>
#include <zircon/fidl.h>
#include <zircon/types.h>

#include "src/devices/bin/driver_manager/constants.h"
#include "src/devices/bin/driver_manager/tests/multiple_device_test.h"

namespace {

class FidlTransaction : public fidl::Transaction {
 public:
  FidlTransaction(FidlTransaction&&) = default;
  explicit FidlTransaction(zx_txid_t transaction_id, zx::unowned_channel channel)
      : txid_(transaction_id), channel_(channel) {}

  std::unique_ptr<fidl::Transaction> TakeOwnership() override {
    return std::make_unique<FidlTransaction>(std::move(*this));
  }

  zx_status_t Reply(fidl::OutgoingMessage* message, fidl::WriteOptions write_options) override {
    ZX_ASSERT(txid_ != 0);
    message->set_txid(txid_);
    txid_ = 0;
    message->Write(channel_, std::move(write_options));
    return message->status();
  }

  void Close(zx_status_t epitaph) override {}

  void InternalError(fidl::UnbindInfo info, fidl::ErrorOrigin origin) override {
    detected_error_ = info;
  }

  ~FidlTransaction() override = default;

  const std::optional<fidl::UnbindInfo>& detected_error() const { return detected_error_; }

 private:
  zx_txid_t txid_;
  zx::unowned_channel channel_;
  std::optional<fidl::UnbindInfo> detected_error_;
};

class FakeDriverHost : public fidl::WireServer<fdm::DriverHostController> {
 public:
  FakeDriverHost(
      const char* expected_driver,
      fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client,
      fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server)
      : expected_driver_(expected_driver),
        device_coordinator_client_(device_coordinator_client),
        device_controller_server_(device_controller_server) {}

  void CreateDevice(CreateDeviceRequestView request,
                    CreateDeviceCompleter::Sync& completer) override {
    if (request->type.is_proxy()) {
      auto& proxy = request->type.proxy();
      if (strncmp(expected_driver_, proxy.driver_path.data(), proxy.driver_path.size()) == 0) {
        *device_coordinator_client_ = std::move(request->coordinator);
        *device_controller_server_ = std::move(request->device_controller);
        completer.Reply(ZX_OK);
        return;
      }
    }
    completer.Reply(ZX_ERR_INTERNAL);
  }

  void Restart(RestartCompleter::Sync& completer) override {}
  void Start(StartRequestView request, StartCompleter::Sync& completer) override {}

 private:
  const char* expected_driver_;
  fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client_;
  fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server_;
};

}  // namespace

// Reads a CreateDevice from remote, checks expectations, and sends a ZX_OK
// response.
void MultipleDeviceTestCase::CheckCreateDeviceReceived(
    const fidl::ServerEnd<fdm::DriverHostController>& devhost_controller,
    const char* expected_driver,
    fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client,
    fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server) {
  uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl_channel_handle_metadata_t handle_metadata[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl::IncomingHeaderAndMessage msg = fidl::MessageRead(
      devhost_controller.channel(), fidl::ChannelMessageStorageView{
                                        .bytes = fidl::BufferSpan(bytes, std::size(bytes)),
                                        .handles = handles,
                                        .handle_metadata = handle_metadata,
                                        .handle_capacity = ZX_CHANNEL_MAX_MSG_HANDLES,
                                    });
  ASSERT_TRUE(msg.ok());

  auto* header = msg.header();
  FidlTransaction txn(header->txid, zx::unowned(devhost_controller.channel()));

  FakeDriverHost fake(expected_driver, device_coordinator_client, device_controller_server);
  fidl::WireDispatch(&fake, std::move(msg), &txn);
  ASSERT_FALSE(txn.detected_error());
  ASSERT_TRUE(device_coordinator_client->is_valid());
  ASSERT_TRUE(device_controller_server->is_valid());
}

bool DeviceState::HasPendingMessages() {
  return controller_server.channel().wait_one(ZX_CHANNEL_READABLE, zx::time(0), nullptr) == ZX_OK;
}

void DeviceState::Dispatch() {
  uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl_channel_handle_metadata_t handle_metadata[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl::IncomingHeaderAndMessage msg = fidl::MessageRead(
      controller_server.channel(), fidl::ChannelMessageStorageView{
                                       .bytes = fidl::BufferSpan(bytes, std::size(bytes)),
                                       .handles = handles,
                                       .handle_metadata = handle_metadata,
                                       .handle_capacity = ZX_CHANNEL_MAX_MSG_HANDLES,
                                   });
  ASSERT_TRUE(msg.ok());

  auto* header = msg.header();
  FidlTransaction txn(header->txid, zx::unowned(controller_server.channel()));

  fidl::WireDispatch<fdm::DeviceController>(this, std::move(msg), &txn);
  ASSERT_FALSE(txn.detected_error());
}

void DeviceState::CheckBindDriverReceivedAndReply(std::string_view expected_driver_name) {
  Dispatch();
  ASSERT_TRUE(bind_completer_.has_value());
  ASSERT_EQ(expected_driver_name, bind_driver_path_);
  bind_completer_->Reply(ZX_OK, zx::channel{});
  bind_completer_.reset();
}

void DeviceState::CheckInitReceived() {
  Dispatch();
  ASSERT_TRUE(init_completer_.has_value());
}

void DeviceState::SendInitReply(zx_status_t return_status) {
  init_completer_->Reply(return_status);
  init_completer_.reset();
}

void DeviceState::CheckInitReceivedAndReply(zx_status_t return_status) {
  CheckInitReceived();
  SendInitReply(return_status);
}

void DeviceState::CheckUnbindReceived() {
  Dispatch();
  ASSERT_TRUE(unbind_completer_.has_value());
}

void DeviceState::SendUnbindReply() {
  unbind_completer_->ReplySuccess();
  unbind_completer_.reset();
}

void DeviceState::CheckUnbindReceivedAndReply() {
  CheckUnbindReceived();
  SendUnbindReply();
}

void DeviceState::CheckRemoveReceived() {
  Dispatch();
  ASSERT_TRUE(remove_completer_.has_value());
}

void DeviceState::SendRemoveReply() {
  remove_completer_->ReplySuccess();
  remove_completer_.reset();
}

void DeviceState::CheckRemoveReceivedAndReply() {
  CheckRemoveReceived();
  SendRemoveReply();
}

void DeviceState::CheckSuspendReceived(uint32_t expected_flags) {
  Dispatch();
  ASSERT_TRUE(suspend_completer_.has_value());
  ASSERT_EQ(suspend_flags_, expected_flags);
}

void DeviceState::SendSuspendReply(zx_status_t return_status) {
  suspend_completer_->Reply(return_status);
  suspend_completer_.reset();
}

void DeviceState::CheckSuspendReceivedAndReply(uint32_t expected_flags, zx_status_t return_status) {
  CheckSuspendReceived(expected_flags);
  SendSuspendReply(return_status);
}

void DeviceState::CheckResumeReceived(SystemPowerState target_state) {
  Dispatch();
  ASSERT_TRUE(resume_completer_.has_value());
  ASSERT_EQ(SystemPowerState(resume_target_state_), target_state);
}

void DeviceState::SendResumeReply(zx_status_t return_status) {
  resume_completer_->Reply(return_status);
  resume_completer_.reset();
}

void DeviceState::CheckResumeReceivedAndReply(SystemPowerState target_state,
                                              zx_status_t return_status) {
  CheckResumeReceived(target_state);
  SendResumeReply(return_status);
}

void MultipleDeviceTestCase::SetUp() {
  // Start the mock server thread.
  ASSERT_OK(mock_server_loop_.StartThread("mock-admin-server"));

  ASSERT_NO_FATAL_FAILURE(InitializeCoordinator(&coordinator()));

  coordinator().driver_loader().LoadDriverUrl(std::string{fdf::kFragmentDriverUrl});
  {
    auto client_end = fidl::CreateEndpoints(&driver_host_server_);
    ASSERT_OK(client_end.status_value());
    driver_host_ =
        fbl::MakeRefCounted<DriverHost>(&coordinator(), std::move(*client_end),
                                        fidl::ClientEnd<fuchsia_io::Directory>(), zx::process{});
  }

  // Set up the root device proxy, inside of the driver_host
  ASSERT_OK(coordinator().PrepareProxy(coordinator().root_device(), driver_host_));
  coordinator_loop_.RunUntilIdle();
  ASSERT_NO_FATAL_FAILURE(CheckCreateDeviceReceived(driver_host_server_, kSystemDriverPath,
                                                    &root_proxy()->coordinator_client,
                                                    &root_proxy()->controller_server));
  coordinator_loop_.RunUntilIdle();

  // Create a child of the root device (an equivalent of the platform bus)
  {
    auto device_controller = fidl::CreateEndpoints(&platform_bus_.controller_server);
    ASSERT_OK(device_controller.status_value());

    auto coordinator_request = fidl::CreateEndpoints(&platform_bus_.coordinator_client);
    ASSERT_OK(coordinator_request.status_value());

    auto status = coordinator().device_manager()->AddDevice(
        coordinator().root_device()->proxy(), std::move(*device_controller),
        std::move(*coordinator_request),
        /* props_data */ nullptr, /* props_count */ 0, /* str_props_data */ nullptr,
        /* str_props_count */ 0, "platform-bus", 0,
        /* driver_path */ {},
        /* args */ {}, /* add_device_config */ {}, /* has_init */ false,
        /* always_init */ true,
        /*inspect*/ zx::vmo(),
        /* outgoing_dir */ fidl::ClientEnd<fio::Directory>(), &platform_bus_.device);
    ASSERT_OK(status);
    coordinator_loop_.RunUntilIdle();

    ASSERT_NO_FATAL_FAILURE(platform_bus()->CheckInitReceivedAndReply());
    coordinator_loop()->RunUntilIdle();
  }
}

void MultipleDeviceTestCase::TearDown() {
  // Stop any threads, so we're serialized here.
  if (coordinator_loop_thread_running()) {
    coordinator_loop_.Shutdown();
  }

  coordinator_loop_.RunUntilIdle();

  // Remove the devices in the opposite order that we added them
  while (!devices_.is_empty()) {
    devices_.pop_back();
    coordinator_loop_.RunUntilIdle();
  }

  coordinator().device_manager()->RemoveDevice(std::move(platform_bus_.device), /* forced */ false);
  coordinator_loop_.RunUntilIdle();

  // We need to explicitly remove this proxy device, because it holds a reference to devhost_.
  // Other devices will be removed via the DeviceState dtor.
  fbl::RefPtr<Device> sys_proxy = coordinator().root_device()->proxy();
  if (sys_proxy) {
    coordinator().device_manager()->RemoveDevice(std::move(sys_proxy), /* forced */ false);
    coordinator_loop_.RunUntilIdle();
  }

  coordinator().device_manager()->RemoveDevice(coordinator().root_device(), /* forced */ false);
  coordinator_loop_.RunUntilIdle();

  // We no longer need the async loop.
  // If we do not shutdown here, the destructor
  // could be cleaning up the vfs, before the loop clears the
  // connections.
  coordinator_loop_.Shutdown();
}

void MultipleDeviceTestCase::AddDevice(const fbl::RefPtr<Device>& parent, const char* name,
                                       uint32_t protocol_id, fbl::String driver, bool has_init,
                                       bool reply_to_init, bool always_init,
                                       fdm::AddDeviceConfig add_device_config,
                                       fidl::ClientEnd<fio::Directory> outgoing_dir,
                                       zx::vmo inspect, size_t* index) {
  DeviceState state;

  auto coordinator_server = fidl::CreateEndpoints(&state.coordinator_client);
  ASSERT_OK(coordinator_server.status_value());

  auto controller_client = fidl::CreateEndpoints(&state.controller_server);
  ASSERT_OK(controller_client.status_value());

  auto status = coordinator().device_manager()->AddDevice(
      parent, std::move(*controller_client), std::move(*coordinator_server),
      /* props_data */ nullptr,
      /* props_count */ 0, /* str_props_data */ nullptr,
      /* str_props_count */ 0, name, protocol_id, /* driver_path */ driver.data(), /* args */ {},
      /* add_device_config */ add_device_config, /* has_init */ has_init,
      /* always_init */ always_init, std::move(inspect), /* outgoing_dir */ std::move(outgoing_dir),
      &state.device);
  state.device->flags |= DEV_CTX_ALLOW_MULTI_COMPOSITE;
  ASSERT_OK(status);
  coordinator_loop_.RunUntilIdle();

  devices_.push_back(std::move(state));
  *index = devices_.size() - 1;

  if (reply_to_init) {
    ASSERT_NO_FATAL_FAILURE(device(*index)->CheckInitReceivedAndReply());
    coordinator_loop()->RunUntilIdle();
  }
}

void MultipleDeviceTestCase::AddDevice(const fbl::RefPtr<Device>& parent, const char* name,
                                       uint32_t protocol_id, fbl::String driver, bool has_init,
                                       bool reply_to_init, bool always_init, zx::vmo inspect,
                                       size_t* index) {
  AddDevice(parent, name, protocol_id, driver, has_init, reply_to_init, always_init,
            fdm::AddDeviceConfig{}, fidl::ClientEnd<fio::Directory>(), std::move(inspect), index);
}

void MultipleDeviceTestCase::AddDevice(const fbl::RefPtr<Device>& parent, const char* name,
                                       uint32_t protocol_id, fbl::String driver, size_t* index) {
  AddDevice(parent, name, protocol_id, driver, /* has_init */ false,
            /* reply_to_init */ true, /* always_init */ true, /* inspect */ zx::vmo(), index);
}

void MultipleDeviceTestCase::RemoveDevice(size_t device_index) {
  auto& state = devices_[device_index];
  ASSERT_OK(coordinator().device_manager()->RemoveDevice(state.device, false));
  state.device.reset();
  state.controller_server.reset();
  state.coordinator_client.reset();
  coordinator_loop_.RunUntilIdle();
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags,
                                       fit::function<void(uint32_t flags)> suspend_cb) {
  suspend_cb(flags);
  if (!coordinator_loop_thread_running()) {
    coordinator_loop()->RunUntilIdle();
  }
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags) {
  DoSuspend(flags,
            [this](uint32_t flags) { coordinator().suspend_resume_manager().Suspend(flags); });
}

void MultipleDeviceTestCase::DoSuspendWithCallback(
    uint32_t flags, fit::function<void(zx_status_t status)> suspend_complete_cb) {
  DoSuspend(flags, [this, suspend_cb = std::move(suspend_complete_cb)](uint32_t flags) mutable {
    coordinator().suspend_resume_manager().Suspend(flags, std::move(suspend_cb));
  });
}

void MultipleDeviceTestCase::DoResume(
    SystemPowerState target_state, fit::function<void(SystemPowerState target_state)> resume_cb) {
  resume_cb(target_state);
  if (!coordinator_loop_thread_running()) {
    coordinator_loop()->RunUntilIdle();
  }
}

void MultipleDeviceTestCase::DoResume(SystemPowerState target_state, ResumeCallback callback) {
  DoResume(target_state, [this, callback = std::move(callback)](SystemPowerState target_state) {
    coordinator().suspend_resume_manager().Resume(target_state, callback);
  });
}
