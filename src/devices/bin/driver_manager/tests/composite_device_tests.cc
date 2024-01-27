// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/driver.h>

#include <utility>
#include <vector>

#include <fbl/vector.h>
#include <zxtest/zxtest.h>

#include "src/devices/bin/driver_manager/tests/fake_driver_index.h"
#include "src/devices/bin/driver_manager/tests/multiple_device_test.h"
#include "src/lib/storage/vfs/cpp/synchronous_vfs.h"

namespace fio = fuchsia_io;

namespace {

const std::string kFakeDriverUrl = "#driver/mock-device.so";

// Matches |args| to the fake driver if it follows the bind rules:
// fuchsia.BIND_PROTOCOL == fuchsia.test.BIND_PROTOCOL.DEVICE.
zx::result<FakeDriverIndex::MatchResult> MatchFakeDriver(
    fuchsia_driver_index::wire::MatchDriverArgs args) {
  for (auto& prop : args.properties()) {
    if (prop.key().is_int_value() && prop.key().int_value() == BIND_PROTOCOL &&
        prop.value().is_int_value() && prop.value().int_value() == ZX_PROTOCOL_TEST) {
      return zx::ok(FakeDriverIndex::MatchResult{
          .url = kFakeDriverUrl,
          .colocate = true,
      });
    }
  }

  return zx::error(ZX_ERR_NOT_FOUND);
}

class FidlTransaction : public fidl::Transaction {
 public:
  FidlTransaction(FidlTransaction&&) = default;
  explicit FidlTransaction(zx_txid_t transaction_id, zx::unowned_channel channel)
      : txid_(transaction_id), channel_(std::move(channel)) {}

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

  void Close(zx_status_t epitaph) override { ZX_ASSERT(false); }

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

// A fake DriverHostController server that checks that it receives a
// CreateDevice message for a composite device.
class FakeCompositeDevhost : public fidl::WireServer<fuchsia_device_manager::DriverHostController> {
 public:
  FakeCompositeDevhost(
      const char* expected_name, size_t expected_fragments_count,
      fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client,
      fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server)
      : expected_name_(expected_name),
        expected_fragments_count_(expected_fragments_count),
        device_coordinator_client_(device_coordinator_client),
        device_controller_server_(device_controller_server) {}

  void CreateDevice(CreateDeviceRequestView request,
                    CreateDeviceCompleter::Sync& completer) override {
    if (request->type.is_composite()) {
      auto& composite = request->type.composite();
      if (strncmp(expected_name_, composite.name.data(), composite.name.size()) == 0 &&
          composite.fragments.count() == expected_fragments_count_) {
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
  const char* expected_name_;
  size_t expected_fragments_count_;
  fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client_;
  fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server_;
};

// A fake DriverHostController server that checks that it receives a
// CreateDevice message for a FIDL proxy device.
class FakeFidlProxyDevhost : public fidl::WireServer<fuchsia_device_manager::DriverHostController> {
 public:
  FakeFidlProxyDevhost(
      fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client,
      fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server)
      : device_coordinator_client_(device_coordinator_client),
        device_controller_server_(device_controller_server) {}

  void CreateDevice(CreateDeviceRequestView request,
                    CreateDeviceCompleter::Sync& completer) override {
    if (request->type.is_fidl_proxy()) {
      *device_coordinator_client_ = std::move(request->coordinator);
      *device_controller_server_ = std::move(request->device_controller);
      completer.Reply(ZX_OK);
      return;
    }
    completer.Reply(ZX_ERR_INTERNAL);
  }
  void Start(StartRequestView request, StartCompleter::Sync& completer) override {}

  void Restart(RestartCompleter::Sync& completer) override {}

 private:
  fidl::ClientEnd<fuchsia_device_manager::Coordinator>* device_coordinator_client_;
  fidl::ServerEnd<fuchsia_device_manager::DeviceController>* device_controller_server_;
};

}  // namespace

// Reads a CreateDevice from remote, checks expectations, and sends
// a ZX_OK response.
void CheckCreateDeviceReceived(
    fidl::WireServer<fuchsia_device_manager::DriverHostController>* fake_dev_host,
    const fidl::ServerEnd<fdm::DriverHostController>& controller, DeviceState* composite) {
  // Manually read a FIDL message off the DriverHostController channel.
  uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl_channel_handle_metadata_t handle_metadata[ZX_CHANNEL_MAX_MSG_HANDLES];
  fidl::IncomingHeaderAndMessage msg = fidl::MessageRead(
      controller.channel(), fidl::ChannelMessageStorageView{
                                .bytes = fidl::BufferSpan(bytes, std::size(bytes)),
                                .handles = handles,
                                .handle_metadata = handle_metadata,
                                .handle_capacity = ZX_CHANNEL_MAX_MSG_HANDLES,
                            });
  ASSERT_TRUE(msg.ok());

  auto* header = msg.header();
  FidlTransaction txn(header->txid, zx::unowned(controller.channel()));

  // Dispatch the message to the fake server.
  fidl::WireDispatch(fake_dev_host, std::move(msg), &txn);
  ASSERT_FALSE(txn.detected_error());

  // The fake server holds a reference to the composite which it should populate
  // with the channels. Check that that happened.
  ASSERT_TRUE(composite->coordinator_client.is_valid());
  ASSERT_TRUE(composite->controller_server.is_valid());
}

void CheckCreateCompositeDeviceReceived(
    const fidl::ServerEnd<fdm::DriverHostController>& controller, const char* expected_name,
    size_t expected_fragments_count, DeviceState* composite) {
  FakeCompositeDevhost fake(expected_name, expected_fragments_count, &composite->coordinator_client,
                            &composite->controller_server);
  CheckCreateDeviceReceived(&fake, controller, composite);
}

void CheckCreateFidlProxyDeviceReceived(
    const fidl::ServerEnd<fdm::DriverHostController>& controller, DeviceState* fidl_proxy) {
  FakeFidlProxyDevhost fake(&fidl_proxy->coordinator_client, &fidl_proxy->controller_server);
  CheckCreateDeviceReceived(&fake, controller, fidl_proxy);
}

// Helper for BindComposite for issuing an AddComposite for a composite with the
// given fragments.  It's assumed that these fragments are children of
// the platform_bus and have the given protocol_id
void BindCompositeDefineComposite(const fbl::RefPtr<Device>& platform_bus,
                                  const uint32_t* protocol_ids, size_t fragment_count,
                                  const char* name, zx_status_t expected_status = ZX_OK,
                                  const device_metadata_t* metadata = nullptr,
                                  size_t metadata_count = 0) {
  fidl::Arena allocator;
  std::vector<fuchsia_device_manager::wire::DeviceFragment> fragments = {};
  for (size_t i = 0; i < fragment_count; ++i) {
    // Define a union type to avoid violating the strict aliasing rule.

    zx_bind_inst_t always = BI_MATCH();
    zx_bind_inst_t protocol = BI_MATCH_IF(EQ, BIND_PROTOCOL, protocol_ids[i]);

    fuchsia_device_manager::wire::DeviceFragment fragment;  // = &fragments[i];
    fragment.name = ::fidl::StringView("unnamed-fragment");
    fragment.parts.Allocate(allocator, 1);
    fragment.parts[0].match_program.Allocate(allocator, 1);
    fragment.parts[0].match_program[0] = fuchsia_device_manager::wire::BindInstruction{
        .op = protocol.op,
        .arg = protocol.arg,
        .debug = always.debug,
    };
    fragments.push_back(fragment);
  }

  std::vector<fuchsia_device_manager::wire::DeviceProperty> props_list = {
      fuchsia_device_manager::wire::DeviceProperty{
          .id = BIND_PROTOCOL,
          .reserved = 0,
          .value = ZX_PROTOCOL_TEST,
      }};

  std::vector<fuchsia_device_manager::wire::DeviceMetadata> metadata_list = {};
  for (size_t i = 0; i < metadata_count; i++) {
    auto meta = fuchsia_device_manager::wire::DeviceMetadata{
        .key = metadata[i].type,
        .data = ::fidl::VectorView<uint8_t>::FromExternal(
            reinterpret_cast<uint8_t*>(const_cast<void*>(metadata[i].data)), metadata[i].length)};
    metadata_list.emplace_back(meta);
  }

  fuchsia_device_manager::wire::CompositeDeviceDescriptor comp_desc = {
      .props = ::fidl::VectorView<fuchsia_device_manager::wire::DeviceProperty>::FromExternal(
          props_list),
      .fragments =
          ::fidl::VectorView<fuchsia_device_manager::wire::DeviceFragment>::FromExternal(fragments),
      .primary_fragment_index = 0,
      .spawn_colocated = true,
      .metadata = ::fidl::VectorView<fuchsia_device_manager::wire::DeviceMetadata>::FromExternal(
          metadata_list),
  };

  Coordinator* coordinator = platform_bus->coordinator;
  ASSERT_EQ(coordinator->device_manager()->AddCompositeDevice(platform_bus, name, comp_desc),
            expected_status);
}

class CompositeTestCase : public MultipleDeviceTestCase {
 public:
  ~CompositeTestCase() override = default;

  // Adds the devices to construct the composite out of. It adds a device for each protocol id
  // and outputs the device's index into |device_indexes|. |fragment_count| must match the size
  // of |protocol_ids| and |device_indexes|.
  void AddCompositeFragmentDevices(cpp20::span<const uint32_t> protocol_ids, size_t fragment_count,
                                   size_t* device_indexes);

  void CheckCompositeFragments(const char* composite_name, const size_t* device_indexes,
                               size_t device_indexes_count, size_t* fragment_indexes_out);

  void CheckCompositeCreation(const char* composite_name, const size_t* device_indexes,
                              size_t device_indexes_count, size_t* fragment_indexes_out,
                              DeviceState* composite_state);

  fbl::RefPtr<Device> GetCompositeDevice(std::string_view composite_name);

 protected:
  CoordinatorConfig CreateConfig(async_dispatcher_t* bootargs_dispatcher,
                                 mock_boot_arguments::Server* boot_args,
                                 fidl::WireSyncClient<fuchsia_boot::Arguments>* client) override {
    auto config = DefaultConfig(bootargs_dispatcher, boot_args, client);
    ZX_ASSERT(driver_index_loop_.StartThread("test-thread") == ZX_OK);

    driver_index_ = std::make_unique<FakeDriverIndex>(
        driver_index_loop_.dispatcher(),
        [&](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
          return MatchFakeDriver(args);
        });

    config.driver_index = fidl::WireSharedClient<fdi::DriverIndex>(
        std::move(driver_index_->Connect().value()), mock_server_loop_.dispatcher());
    return config;
  }

  void SetUp() override {
    MultipleDeviceTestCase::SetUp();
    ASSERT_NOT_NULL(coordinator().LoadFragmentDriver());
    ASSERT_NOT_NULL(coordinator().driver_loader().LoadDriverUrl(kFakeDriverUrl));
  }

  void TearDown() override {
    driver_index_loop_.Quit();
    driver_index_loop_.JoinThreads();
    driver_index_loop_.ResetQuit();
    MultipleDeviceTestCase::TearDown();
  }

  std::unique_ptr<FakeDriverIndex> driver_index_;

 private:
  async::Loop driver_index_loop_{&kAsyncLoopConfigNeverAttachToThread};
};

void CompositeTestCase::AddCompositeFragmentDevices(cpp20::span<const uint32_t> protocol_ids,
                                                    size_t fragment_count, size_t* device_indexes) {
  for (size_t i = 0; i < fragment_count; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURE(
        AddDevice(platform_bus()->device, name, protocol_ids[i], "", &device_indexes[i]));
  }
}

fbl::RefPtr<Device> CompositeTestCase::GetCompositeDevice(std::string_view composite_name) {
  for (Device& device : coordinator().device_manager()->devices()) {
    if (!device.is_composite()) {
      continue;
    }
    if (std::string_view(device.composite()->get().name()) == composite_name) {
      return fbl::RefPtr(&device);
    }
  }
  return nullptr;
}

void CompositeTestCase::CheckCompositeFragments(const char* composite_name,
                                                const size_t* device_indexes,
                                                size_t device_indexes_count,
                                                size_t* fragment_indexes_out) {
  for (size_t i = 0; i < device_indexes_count; ++i) {
    auto device_state = device(device_indexes[i]);
    // Check that the fragments got bound
    fbl::String driver = coordinator().LoadFragmentDriver()->libname;
    ASSERT_NO_FATAL_FAILURE(device_state->CheckBindDriverReceivedAndReply(driver.data()));
    coordinator_loop()->RunUntilIdle();

    // Synthesize the AddDevice request the fragment driver would send
    char name[32];
    snprintf(name, sizeof(name), "%s-comp-device-%zu", composite_name, i);
    ASSERT_NO_FATAL_FAILURE(
        AddDevice(device_state->device, name, 0, driver, &fragment_indexes_out[i]));
  }
}

void CompositeTestCase::CheckCompositeCreation(const char* composite_name,
                                               const size_t* device_indexes,
                                               size_t device_indexes_count,
                                               size_t* fragment_indexes_out,
                                               DeviceState* composite) {
  CheckCompositeFragments(composite_name, device_indexes, device_indexes_count,
                          fragment_indexes_out);

  // Make sure the composite comes up
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(driver_host_server(), composite_name,
                                                             device_indexes_count, composite));
  composite->CheckBindDriverReceivedAndReply(kFakeDriverUrl);
}

class CompositeAddOrderTestCase : public CompositeTestCase {
 public:
  enum class AddLocation {
    // Add the composite before any fragments
    BEFORE,
    // Add the composite after some fragments
    MIDDLE,
    // Add the composite after all fragments
    AFTER,
  };
  void ExecuteTest(AddLocation add);
};

class CompositeAddOrderSharedFragmentTestCase : public CompositeAddOrderTestCase {
 public:
  void ExecuteSharedFragmentTest(AddLocation dev1_add, AddLocation dev2_add) {
    uint32_t protocol_id[] = {
        ZX_PROTOCOL_GPIO,
        ZX_PROTOCOL_I2C,
        ZX_PROTOCOL_ETHERNET,
    };
    size_t device_indexes[std::size(protocol_id)];

    const char* kCompositeDev1Name = "composite-dev1";
    const char* kCompositeDev2Name = "composite-dev2";
    auto do_add = [&](const char* devname) {
      ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                           std::size(protocol_id), devname));
    };

    if (dev1_add == AddLocation::BEFORE) {
      ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev1Name));
    }

    if (dev2_add == AddLocation::BEFORE) {
      ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev2Name));
    }
    // Add the devices to construct the composite out of.
    for (size_t i = 0; i < std::size(device_indexes); ++i) {
      char name[32];
      snprintf(name, sizeof(name), "device-%zu", i);
      ASSERT_NO_FATAL_FAILURE(
          AddDevice(platform_bus()->device, name, protocol_id[i], "", &device_indexes[i]));
      if (i == 0 && dev1_add == AddLocation::MIDDLE) {
        ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev1Name));
      }
      if (i == 0 && dev2_add == AddLocation::MIDDLE) {
        ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev2Name));
      }
    }

    if (dev1_add == AddLocation::AFTER) {
      ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev1Name));
    }

    DeviceState composite1, composite2;
    size_t fragment_device1_indexes[std::size(device_indexes)];
    size_t fragment_device2_indexes[std::size(device_indexes)];
    ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDev1Name, device_indexes,
                                                   std::size(device_indexes),
                                                   fragment_device1_indexes, &composite1));
    if (dev2_add == AddLocation::AFTER) {
      ASSERT_NO_FATAL_FAILURE(do_add(kCompositeDev2Name));
    }
    ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDev2Name, device_indexes,
                                                   std::size(device_indexes),
                                                   fragment_device2_indexes, &composite2));
  }
};

void CompositeAddOrderTestCase::ExecuteTest(AddLocation add) {
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
      ZX_PROTOCOL_ETHERNET,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  auto do_add = [&]() {
    ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(
        platform_bus()->device, protocol_id, std::size(protocol_id), kCompositeDevName));
  };

  if (add == AddLocation::BEFORE) {
    ASSERT_NO_FATAL_FAILURE(do_add());
  }

  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < std::size(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURE(
        AddDevice(platform_bus()->device, name, protocol_id[i], "", &device_indexes[i]));
    if (i == 0 && add == AddLocation::MIDDLE) {
      ASSERT_NO_FATAL_FAILURE(do_add());
    }
  }

  if (add == AddLocation::AFTER) {
    ASSERT_NO_FATAL_FAILURE(do_add());
  }

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
}

zx::result<Devnode*> resolve_path(Devnode* dn, std::string_view path) {
  // This function does not work with /dev/class entries due to the downcast to Devnode::VnodeImpl.
  if (cpp20::starts_with(path, "class/")) {
    ADD_FAILURE("resolve_path does not work with /dev/class entry: %.*s",
                static_cast<int>(path.size()), path.data());
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  while (!path.empty()) {
    const size_t i = path.find('/');
    if (i == 0) {
      return zx::error(ZX_ERR_BAD_PATH);
    }
    std::string_view name = path;
    if (i != std::string::npos) {
      name = path.substr(0, i);
      path = path.substr(i + 1);
    } else {
      path = {};
    }
    fbl::RefPtr<fs::Vnode> out;
    if (const zx_status_t status = dn->children().Lookup(name, &out); status != ZX_OK) {
      return zx::error(status);
    }
    dn = &fbl::RefPtr<Devnode::VnodeImpl>::Downcast(out)->holder_;
  }
  return zx::ok(dn);
}

TEST_F(CompositeAddOrderTestCase, DefineBeforeDevices) {
  ASSERT_NO_FATAL_FAILURE(ExecuteTest(AddLocation::BEFORE));
}

TEST_F(CompositeAddOrderTestCase, DefineAfterDevices) {
  ASSERT_NO_FATAL_FAILURE(ExecuteTest(AddLocation::AFTER));
}

TEST_F(CompositeAddOrderTestCase, DefineInbetweenDevices) {
  ASSERT_NO_FATAL_FAILURE(ExecuteTest(AddLocation::MIDDLE));
}

TEST_F(CompositeAddOrderSharedFragmentTestCase, DefineDevice1BeforeDevice2Before) {
  ASSERT_NO_FATAL_FAILURE(ExecuteSharedFragmentTest(AddLocation::BEFORE, AddLocation::BEFORE));
}

TEST_F(CompositeAddOrderSharedFragmentTestCase, DefineDevice1BeforeDevice2After) {
  ASSERT_NO_FATAL_FAILURE(ExecuteSharedFragmentTest(AddLocation::BEFORE, AddLocation::AFTER));
}

TEST_F(CompositeAddOrderSharedFragmentTestCase, DefineDevice1MiddleDevice2Before) {
  ASSERT_NO_FATAL_FAILURE(ExecuteSharedFragmentTest(AddLocation::BEFORE, AddLocation::MIDDLE));
}

TEST_F(CompositeAddOrderSharedFragmentTestCase, DefineDevice1MiddleDevice2After) {
  ASSERT_NO_FATAL_FAILURE(ExecuteSharedFragmentTest(AddLocation::MIDDLE, AddLocation::AFTER));
}

TEST_F(CompositeAddOrderSharedFragmentTestCase, DefineDevice1AfterDevice2After) {
  ASSERT_NO_FATAL_FAILURE(ExecuteSharedFragmentTest(AddLocation::AFTER, AddLocation::AFTER));
}

TEST_F(CompositeTestCase, AddCompositeWithoutMatchingDriver) {
  // Disable the driver index from matching the fake driver to the composite.
  driver_index_->set_match_callback([&](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
    return zx::error(ZX_ERR_NOT_FOUND);
  });

  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_ETHERNET,
  };

  size_t device_indexes[std::size(protocol_id)];
  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  size_t fragment_device_indexes[std::size(device_indexes)];
  CheckCompositeFragments(kCompositeDevName, device_indexes, std::size(device_indexes),
                          fragment_device_indexes);

  // Check that the composite device doesn't exist. The driver host should not receive a
  // message to add a device.
  ASSERT_NE(ZX_OK,
            driver_host_server().channel().wait_one(ZX_CHANNEL_READABLE, zx::time(0), nullptr));

  // Update driver index so that the fake driver will now match the composite.
  driver_index_->set_match_callback(
      [&](auto args) -> zx::result<FakeDriverIndex::MatchResult> { return MatchFakeDriver(args); });
  coordinator().bind_driver_manager().BindAllDevices(DriverLoader::MatchDeviceConfig{});

  // Make sure that the composite comes up.
  DeviceState composite;
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(
      driver_host_server(), kCompositeDevName, std::size(device_indexes), &composite));
}

TEST_F(CompositeTestCase, AddMultipleSharedFragmentCompositeDevices) {
  zx_status_t status = ZX_OK;
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  for (size_t i = 1; i <= 5; i++) {
    char composite_dev_name[32];
    snprintf(composite_dev_name, sizeof(composite_dev_name), "composite-dev-%zu", i);
    ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(
        platform_bus()->device, protocol_id, std::size(protocol_id), composite_dev_name));
  }

  DeviceState composite[5];
  size_t fragment_device_indexes[5][std::size(device_indexes)];
  for (size_t i = 1; i <= 5; i++) {
    char composite_dev_name[32];
    snprintf(composite_dev_name, sizeof(composite_dev_name), "composite-dev-%zu", i);
    ASSERT_NO_FATAL_FAILURE(
        CheckCompositeCreation(composite_dev_name, device_indexes, std::size(device_indexes),
                               fragment_device_indexes[i - 1], &composite[i - 1]));
  }
  auto device1 = device(device_indexes[1])->device;
  size_t count = 0;
  for (auto& child : device1->children()) {
    count++;
    char name[32];
    snprintf(name, sizeof(name), "composite-dev-%zu-comp-device-1", count);
    if (strcmp(child->name().data(), name)) {
      status = ZX_ERR_INTERNAL;
    }
  }
  ASSERT_OK(status);
  ASSERT_EQ(count, 5);
}

TEST_F(CompositeTestCase, SharedFragmentUnbinds) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDev1Name = "composite-dev-1";
  const char* kCompositeDev2Name = "composite-dev-2";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDev1Name));

  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDev2Name));

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  DeviceState composite1, composite2;
  size_t fragment_device1_indexes[std::size(device_indexes)];
  size_t fragment_device2_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDev1Name, device_indexes,
                                                 std::size(device_indexes),
                                                 fragment_device1_indexes, &composite1));
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDev2Name, device_indexes,
                                                 std::size(device_indexes),
                                                 fragment_device2_indexes, &composite2));
  coordinator_loop()->RunUntilIdle();
  {
    auto device1 = device(device_indexes[1])->device;
    fbl::RefPtr<Device> comp_device1;
    fbl::RefPtr<Device> comp_device2;
    for (auto& comp : device1->fragments()) {
      auto comp_device = comp.composite()->device();
      if (!strcmp(comp_device->name().data(), kCompositeDev1Name)) {
        comp_device1 = comp_device;
        continue;
      }
      if (!strcmp(comp_device->name().data(), kCompositeDev2Name)) {
        comp_device2 = comp_device;
        continue;
      }
    }
    ASSERT_NOT_NULL(comp_device1);
    ASSERT_NOT_NULL(comp_device2);
  }
  // Remove device 0 and its children (fragment and composite devices).
  ASSERT_NO_FATAL_FAILURE(
      coordinator().device_manager()->ScheduleRemove(device(device_indexes[0])->device));
  coordinator_loop()->RunUntilIdle();

  auto device_zero = device(device_indexes[0]);
  auto fragment1 = device(fragment_device1_indexes[0]);
  auto fragment2 = device(fragment_device2_indexes[0]);

  // Check the fragments have received their unbind requests.
  ASSERT_NO_FATAL_FAILURE(fragment1->CheckUnbindReceived());
  ASSERT_NO_FATAL_FAILURE(fragment2->CheckUnbindReceived());

  // The device and composites should not have received any requests yet.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(composite1.HasPendingMessages());
  ASSERT_FALSE(composite2.HasPendingMessages());

  ASSERT_NO_FATAL_FAILURE(fragment1->SendUnbindReply());
  ASSERT_NO_FATAL_FAILURE(fragment2->SendUnbindReply());
  coordinator_loop()->RunUntilIdle();

  // The composites should start unbinding since the fragments finished unbinding.
  ASSERT_NO_FATAL_FAILURE(composite1.CheckUnbindReceivedAndReply());
  ASSERT_NO_FATAL_FAILURE(composite2.CheckUnbindReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  // We are still waiting for the composites to be removed.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(fragment1->HasPendingMessages());
  ASSERT_FALSE(fragment2->HasPendingMessages());

  // Finish removing the composites.
  ASSERT_NO_FATAL_FAILURE(composite1.CheckRemoveReceivedAndReply());
  ASSERT_NO_FATAL_FAILURE(composite2.CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_FALSE(device_zero->HasPendingMessages());

  // Finish removing the fragments.
  ASSERT_NO_FATAL_FAILURE(fragment1->CheckRemoveReceivedAndReply());
  ASSERT_NO_FATAL_FAILURE(fragment2->CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_NO_FATAL_FAILURE(device_zero->CheckRemoveReceivedAndReply());

  // Add the device back and verify the composite gets created again
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(platform_bus()->device, "device-0a", protocol_id[0], "", &device_indexes[0]));
  {
    auto device_state = device(device_indexes[0]);
    // Wait for the fragments to get bound
    fbl::String driver = coordinator().LoadFragmentDriver()->libname;
    ASSERT_NO_FATAL_FAILURE(device_state->CheckBindDriverReceivedAndReply(driver.data()));
    coordinator_loop()->RunUntilIdle();

    // Synthesize the AddDevice request the fragment driver would send
    ASSERT_NO_FATAL_FAILURE(AddDevice(device_state->device, "composite-dev1-comp-device-0", 0,
                                      driver, &fragment_device1_indexes[0]));
  }
  {
    auto device_state = device(device_indexes[0]);
    // Wait for the fragments to get bound
    fbl::String driver = coordinator().LoadFragmentDriver()->libname;
    ASSERT_NO_FATAL_FAILURE(device_state->CheckBindDriverReceivedAndReply(driver.data()));
    coordinator_loop()->RunUntilIdle();

    // Synthesize the AddDevice request the fragment driver would send
    ASSERT_NO_FATAL_FAILURE(AddDevice(device_state->device, "composite-dev2-comp-device-0", 0,
                                      driver, &fragment_device2_indexes[0]));
  }
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(
      driver_host_server(), kCompositeDev1Name, std::size(device_indexes), &composite1));
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(
      driver_host_server(), kCompositeDev2Name, std::size(device_indexes), &composite2));
}

TEST_F(CompositeTestCase, FragmentUnbinds) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
  coordinator_loop()->RunUntilIdle();

  {
    fbl::RefPtr<Device> comp_device = GetCompositeDevice(kCompositeDevName);
    ASSERT_NOT_NULL(comp_device);
  }

  // Remove device 0 and its children (fragment and composite devices).
  ASSERT_NO_FATAL_FAILURE(
      coordinator().device_manager()->ScheduleRemove(device(device_indexes[0])->device));
  coordinator_loop()->RunUntilIdle();

  auto device_zero = device(device_indexes[0]);
  auto fragment = device(fragment_device_indexes[0]);

  // The device and composite should not have received an unbind request yet.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(composite.HasPendingMessages());

  // Check the fragment and composite are unbound.
  ASSERT_NO_FATAL_FAILURE(fragment->CheckUnbindReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(fragment->HasPendingMessages());

  ASSERT_NO_FATAL_FAILURE(composite.CheckUnbindReceivedAndReply());
  coordinator_loop()->RunUntilIdle();
  // Still waiting for the composite to be removed.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(fragment->HasPendingMessages());

  // Finish removing the composite.
  ASSERT_NO_FATAL_FAILURE(composite.CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_FALSE(device_zero->HasPendingMessages());

  // Finish removing the fragment.
  ASSERT_NO_FATAL_FAILURE(fragment->CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_NO_FATAL_FAILURE(device_zero->CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  // Add the device back and verify the composite gets created again
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(platform_bus()->device, "device-0b", protocol_id[0], "", &device_indexes[0]));
  {
    auto device_state = device(device_indexes[0]);
    // Wait for the fragments to get bound
    fbl::String driver = coordinator().LoadFragmentDriver()->libname;
    ASSERT_NO_FATAL_FAILURE(device_state->CheckBindDriverReceivedAndReply(driver.data()));
    coordinator_loop()->RunUntilIdle();

    // Synthesize the AddDevice request the fragment driver would send
    ASSERT_NO_FATAL_FAILURE(AddDevice(device_state->device, "fragment-device-0", 0, driver,
                                      &fragment_device_indexes[0]));
  }
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(
      driver_host_server(), kCompositeDevName, std::size(device_indexes), &composite));
}

TEST_F(CompositeTestCase, SuspendOrder) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));
  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));

  const uint32_t suspend_flags = DEVICE_SUSPEND_FLAG_POWEROFF;
  ASSERT_NO_FATAL_FAILURE(DoSuspend(suspend_flags));

  // Make sure none of the fragments have received their suspend requests
  ASSERT_FALSE(platform_bus()->HasPendingMessages());
  for (auto idx : device_indexes) {
    ASSERT_FALSE(device(idx)->HasPendingMessages());
  }
  for (auto idx : fragment_device_indexes) {
    ASSERT_FALSE(device(idx)->HasPendingMessages());
  }
  // The composite should have been the first to get one
  ASSERT_NO_FATAL_FAILURE(composite.CheckSuspendReceivedAndReply(suspend_flags, ZX_OK));
  coordinator_loop()->RunUntilIdle();

  // Next, all of the internal fragment devices should have them, but none of the devices
  // themselves
  ASSERT_FALSE(platform_bus()->HasPendingMessages());
  for (auto idx : device_indexes) {
    ASSERT_FALSE(device(idx)->HasPendingMessages());
  }
  for (auto idx : fragment_device_indexes) {
    ASSERT_NO_FATAL_FAILURE(device(idx)->CheckSuspendReceivedAndReply(suspend_flags, ZX_OK));
  }
  coordinator_loop()->RunUntilIdle();

  // Next, the devices should get them
  ASSERT_FALSE(platform_bus()->HasPendingMessages());
  for (auto idx : device_indexes) {
    ASSERT_NO_FATAL_FAILURE(device(idx)->CheckSuspendReceivedAndReply(suspend_flags, ZX_OK));
  }
  coordinator_loop()->RunUntilIdle();

  // Finally, the platform bus driver, which is the parent of all of the devices
  ASSERT_NO_FATAL_FAILURE(platform_bus()->CheckSuspendReceivedAndReply(suspend_flags, ZX_OK));
  coordinator_loop()->RunUntilIdle();
}

TEST_F(CompositeTestCase, ResumeOrder) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));
  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  size_t fragment_device_indexes[std::size(device_indexes)];
  DeviceState composite;
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
  fbl::RefPtr<Device> comp_device = GetCompositeDevice(kCompositeDevName);
  ASSERT_NOT_NULL(comp_device);

  // Put all the devices in suspended state
  coordinator().root_device()->set_state(Device::State::kSuspended);
  coordinator().root_device()->proxy()->set_state(Device::State::kSuspended);
  platform_bus()->device->set_state(Device::State::kSuspended);
  for (auto idx : device_indexes) {
    device(idx)->device->set_state(Device::State::kSuspended);
  }
  for (auto idx : fragment_device_indexes) {
    device(idx)->device->set_state(Device::State::kSuspended);
  }
  comp_device->set_state(Device::State::kSuspended);

  fuchsia_hardware_power_statecontrol::wire::SystemPowerState state =
      fuchsia_hardware_power_statecontrol::wire::SystemPowerState::kFullyOn;
  ASSERT_NO_FATAL_FAILURE(DoResume(state));

  // First, the sys proxy driver, which is the parent of all of the devices
  ASSERT_NO_FATAL_FAILURE(
      root_proxy()->CheckResumeReceivedAndReply(SystemPowerState::kFullyOn, ZX_OK));
  coordinator_loop()->RunUntilIdle();

  // Then platform devices
  ASSERT_NO_FATAL_FAILURE(platform_bus()->CheckResumeReceivedAndReply(state, ZX_OK));
  coordinator_loop()->RunUntilIdle();

  // Next the devices
  for (auto idx : device_indexes) {
    ASSERT_NO_FATAL_FAILURE(device(idx)->CheckResumeReceivedAndReply(state, ZX_OK));
  }
  coordinator_loop()->RunUntilIdle();

  // Then the fragments
  for (auto idx : fragment_device_indexes) {
    ASSERT_NO_FATAL_FAILURE(device(idx)->CheckResumeReceivedAndReply(state, ZX_OK));
  }
  coordinator_loop()->RunUntilIdle();

  // Then finally the composite device itself
  ASSERT_NO_FATAL_FAILURE(composite.CheckResumeReceivedAndReply(state, ZX_OK));
  coordinator_loop()->RunUntilIdle();
}

// Make sure we receive devfs notifications when composite devices appear
TEST_F(CompositeTestCase, DevfsNotifications) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  // Install a watcher on each of the parent devices.
  fs::SynchronousVfs vfs(coordinator().dispatcher());
  std::vector<fidl::ClientEnd<fuchsia_io::DirectoryWatcher>> client_ends;
  for (const auto& device : devices_) {
    zx::result server =
        fidl::CreateEndpoints<fuchsia_io::DirectoryWatcher>(&client_ends.emplace_back());
    ASSERT_OK(server.status_value());
    std::optional<Devnode>& dn = device.device->devfs.topological_node();
    ASSERT_TRUE(dn.has_value());
    ASSERT_OK(dn.value().children().WatchDir(&vfs, fio::wire::WatchMask::kAdded, 0,
                                             std::move(server.value())));
  }

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));

  // Assert that our watchers were notified of the expected devices' creation.
  for (size_t i = 0; i < client_ends.size(); ++i) {
    const auto& client_end = client_ends.at(i);
    uint8_t msg[fio::wire::kMaxFilename + 2];
    uint32_t msg_len;
    ASSERT_OK(client_end.channel().read(0, msg, nullptr, sizeof(msg), 0, &msg_len, nullptr));
    ASSERT_GE(msg_len, 1);
    ASSERT_EQ(static_cast<fio::wire::WatchEvent>(msg[0]), fio::wire::WatchEvent::kAdded);
    ASSERT_GE(msg_len, 2);

    // Each device gets a fragment as its child.
    {
      const std::string_view name{reinterpret_cast<char*>(msg + 2), msg[1]};
      ASSERT_EQ(name.find(kCompositeDevName), 0);
      ASSERT_TRUE(cpp20::starts_with(name.substr(strlen(kCompositeDevName)), "-comp-device-"));
    }

    if (i == 0) {
      // This is the primary parent; expect a notification for the composite device.
      ASSERT_OK(client_end.channel().read(0, msg, nullptr, sizeof(msg), 0, &msg_len, nullptr));
      ASSERT_GE(msg_len, 1);
      ASSERT_EQ(static_cast<fio::wire::WatchEvent>(msg[0]), fio::wire::WatchEvent::kAdded);
      ASSERT_GE(msg_len, 2);
      const std::string name{reinterpret_cast<char*>(msg + 2), msg[1]};
      EXPECT_EQ(name, kCompositeDevName);
    }
    ASSERT_STATUS(client_end.channel().read(0, msg, nullptr, sizeof(msg), 0, &msg_len, nullptr),
                  ZX_ERR_SHOULD_WAIT);
  }
}

// Make sure the path returned by GetTopologicalPath is accurate
TEST_F(CompositeTestCase, Topology) {
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));
  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));

  fbl::RefPtr<Device> comp_device = GetCompositeDevice(kCompositeDevName);
  ASSERT_NOT_NULL(comp_device);

  std::optional<Devnode>& dn = coordinator().root_devnode();
  ASSERT_TRUE(dn.has_value());
  Devnode& root = dn.value();

  constexpr std::string_view kDev = "/dev/";

  for (size_t i = 0; i < devices_.size(); ++i) {
    const auto& device = devices_[i];

    auto path = device.device->GetTopologicalPath();
    ASSERT_OK(path.status_value());

    const std::string_view parent_topological_path{path.value().c_str() + kDev.size()};
    zx::result parent = resolve_path(&root, parent_topological_path);
    ASSERT_OK(parent);
    zx::result child = resolve_path(parent.value(), kCompositeDevName);
    if (i != 0) {
      // This isn't the primary parent; the composite device isn't a child node.
      ASSERT_STATUS(child.status_value(), ZX_ERR_NOT_FOUND);
      continue;
    }
    // This is the primary parent; the composite device is a child node.
    ASSERT_OK(child.status_value());

    // Check that the Devnode we got walking the topological path is the same one
    // from our device.
    ASSERT_EQ(child.value(), &comp_device->devfs.topological_node().value());
  }
}

class CompositeMetadataTestCase : public CompositeTestCase {
 public:
  enum class AddLocation {
    // Add the composite before any fragments
    BEFORE,
    // Add the composite after some fragments
    MIDDLE,
    // Add the composite after all fragments
    AFTER,
  };
  static constexpr uint32_t kMetadataKey = 999;
  static constexpr char kMetadataStr[] = "composite-metadata";

  void AddCompositeDevice(AddLocation add = AddLocation::BEFORE);

  static void VerifyMetadata(void* data, size_t len) {
    ASSERT_EQ(strlen(kMetadataStr) + 1, len);
    ASSERT_BYTES_EQ(data, kMetadataStr, len);
  }

  void TearDown() override {
    coordinator().device_manager()->RemoveDevice(composite_device, /* forced */ false);
    CompositeTestCase::TearDown();
  }

  fbl::RefPtr<Device> composite_device;

  // Hold reference to remote channels so that they do not close
  DeviceState composite;
};

void CompositeMetadataTestCase::AddCompositeDevice(AddLocation add) {
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
      ZX_PROTOCOL_ETHERNET,
  };
  size_t device_indexes[std::size(protocol_id)];

  const device_metadata_t metadata[] = {
      {
          .type = kMetadataKey,
          .data = const_cast<char*>(kMetadataStr),
          .length = strlen(kMetadataStr) + 1,
      },
  };

  const char* kCompositeDevName = "composite-dev";
  auto do_add = [&]() {
    ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                         std::size(protocol_id), kCompositeDevName,
                                                         ZX_OK, metadata, std::size(metadata)));
  };

  if (add == AddLocation::BEFORE) {
    ASSERT_NO_FATAL_FAILURE(do_add());
  }

  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < std::size(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURE(
        AddDevice(platform_bus()->device, name, protocol_id[i], "", &device_indexes[i]));
    if (i == 0 && add == AddLocation::MIDDLE) {
      ASSERT_NO_FATAL_FAILURE(do_add());
    }
  }

  if (add == AddLocation::AFTER) {
    ASSERT_NO_FATAL_FAILURE(do_add());
  }

  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
  composite_device = GetCompositeDevice(kCompositeDevName);
  ASSERT_NOT_NULL(composite_device);
}

TEST_F(CompositeMetadataTestCase, AddAndGetMetadata) {
  char buf[32] = "";
  size_t len = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice());
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey, buf,
                                                             32, &len));
  VerifyMetadata(buf, len);
}

TEST_F(CompositeMetadataTestCase, FailGetMetadata) {
  size_t len = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice());
  ASSERT_EQ(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey + 1,
                                                             nullptr, 0, &len),
            ZX_ERR_NOT_FOUND);
}

TEST_F(CompositeMetadataTestCase, FailGetMetadataFromParent) {
  size_t len = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice());

  // Find the first bound fragment and get its metadata.
  bool found_fragment = false;
  for (auto& fragment : composite_device->composite()->get().fragments()) {
    if (!fragment.IsBound()) {
      continue;
    }
    fbl::RefPtr<Device> parent =
        composite_device->composite()->get().fragments().front().bound_device();
    ASSERT_EQ(
        platform_bus()->device->coordinator->GetMetadata(parent, kMetadataKey, nullptr, 0, &len),
        ZX_ERR_NOT_FOUND);
    found_fragment = true;
    break;
  }
  ASSERT_TRUE(found_fragment);
}

TEST_F(CompositeMetadataTestCase, DefineAfterDevices) {
  char buf[32] = "";
  size_t len = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice(AddLocation::AFTER));
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey, buf,
                                                             32, &len));
  VerifyMetadata(buf, len);
}

TEST_F(CompositeMetadataTestCase, DefineInBetweenDevices) {
  char buf[32] = "";
  size_t len = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice(AddLocation::MIDDLE));
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey, buf,
                                                             32, &len));
  VerifyMetadata(buf, len);
}

TEST_F(CompositeMetadataTestCase, GetMetadataFromChild) {
  char buf[32] = "";
  size_t len = 0;
  size_t child_index = 0;
  ASSERT_NO_FATAL_FAILURE(AddCompositeDevice());
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(composite_device, "child", ZX_PROTOCOL_AUDIO, "", &child_index));
  fbl::RefPtr<Device> child = device(child_index)->device;
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(child, kMetadataKey, buf, 32, &len));
  VerifyMetadata(buf, len);
}

// Make sure metadata exists after composite device is destroyed and re-created
// due to fragment removal and addition
TEST_F(CompositeMetadataTestCase, GetMetadataAfterCompositeReassemble) {
  char buf[32] = "";
  size_t len = 0;
  const uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
      ZX_PROTOCOL_ETHERNET,
  };
  size_t device_indexes[std::size(protocol_id)];

  const device_metadata_t metadata[] = {
      {
          .type = kMetadataKey,
          .data = const_cast<char*>(kMetadataStr),
          .length = strlen(kMetadataStr) + 1,
      },
  };

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName,
                                                       ZX_OK, metadata, std::size(metadata)));

  AddCompositeFragmentDevices(protocol_id, std::size(protocol_id), device_indexes);

  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
  composite_device = GetCompositeDevice(kCompositeDevName);
  ASSERT_NOT_NULL(composite_device);

  // Get and verify metadata
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey, buf,
                                                             32, &len));
  VerifyMetadata(buf, len);

  // Remove device 0 and its children (fragment and composite devices).
  ASSERT_NO_FATAL_FAILURE(
      coordinator().device_manager()->ScheduleRemove(device(device_indexes[0])->device));
  coordinator_loop()->RunUntilIdle();

  auto device_zero = device(device_indexes[0]);
  auto fragment = device(fragment_device_indexes[0]);

  // The device and composite should not have received an unbind request yet.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(composite.HasPendingMessages());

  // Check the fragment and composite are unbound.
  ASSERT_NO_FATAL_FAILURE(fragment->CheckUnbindReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(fragment->HasPendingMessages());

  ASSERT_NO_FATAL_FAILURE(composite.CheckUnbindReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  // Still waiting for the composite to be removed.
  ASSERT_FALSE(device_zero->HasPendingMessages());
  ASSERT_FALSE(fragment->HasPendingMessages());

  // Finish removing the composite.
  ASSERT_NO_FATAL_FAILURE(composite.CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_FALSE(device_zero->HasPendingMessages());

  // Finish removing the fragment.
  ASSERT_NO_FATAL_FAILURE(fragment->CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  ASSERT_NO_FATAL_FAILURE(device_zero->CheckRemoveReceivedAndReply());
  coordinator_loop()->RunUntilIdle();

  // Add the device back and verify the composite gets created again
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(platform_bus()->device, "device-0c", protocol_id[0], "", &device_indexes[0]));
  {
    auto device_state = device(device_indexes[0]);
    // Wait for the fragments to get bound
    fbl::String driver = coordinator().LoadFragmentDriver()->libname;
    ASSERT_NO_FATAL_FAILURE(device_state->CheckBindDriverReceivedAndReply(driver.data()));
    coordinator_loop()->RunUntilIdle();

    // Synthesize the AddDevice request the fragment driver would send
    ASSERT_NO_FATAL_FAILURE(AddDevice(device_state->device, "fragment-device-0", 0, driver,
                                      &fragment_device_indexes[0]));
  }
  ASSERT_NO_FATAL_FAILURE(CheckCreateCompositeDeviceReceived(
      driver_host_server(), kCompositeDevName, std::size(device_indexes), &composite));

  composite_device = GetCompositeDevice(kCompositeDevName);
  ASSERT_NOT_NULL(composite_device);

  // Get and verify metadata again
  ASSERT_OK(platform_bus()->device->coordinator->GetMetadata(composite_device, kMetadataKey, buf,
                                                             32, &len));
  VerifyMetadata(buf, len);
}

// Tests that a composite is not created until the fragment devices finish initializing.
TEST_F(CompositeTestCase, FragmentDeviceInit) {
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  size_t device_indexes[std::size(protocol_id)];

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURE(BindCompositeDefineComposite(platform_bus()->device, protocol_id,
                                                       std::size(protocol_id), kCompositeDevName));

  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < std::size(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURE(AddDevice(platform_bus()->device, name, protocol_id[i], "",
                                      true /* has_init */, false /* reply_to_init */,
                                      true /* always_init */, zx::vmo() /* inspect */,
                                      &device_indexes[i]));
    auto index = device_indexes[i];
    ASSERT_FALSE(device(index)->device->is_visible());
    ASSERT_NO_FATAL_FAILURE(device(index)->CheckInitReceived());
    ASSERT_EQ(Device::State::kInitializing, device(index)->device->state());
    coordinator_loop()->RunUntilIdle();
  }

  for (uint64_t index : device_indexes) {
    // Check that the fragment isn't being bound yet.
    ASSERT_FALSE(device(index)->HasPendingMessages());

    ASSERT_NO_FATAL_FAILURE(device(index)->SendInitReply());
    coordinator_loop()->RunUntilIdle();

    ASSERT_TRUE(device(index)->device->is_visible());
    ASSERT_EQ(Device::State::kActive, device(index)->device->state());
  }

  DeviceState composite;
  size_t fragment_device_indexes[std::size(device_indexes)];
  ASSERT_NO_FATAL_FAILURE(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                 std::size(device_indexes), fragment_device_indexes,
                                                 &composite));
  coordinator_loop()->RunUntilIdle();

  {
    fbl::RefPtr<Device> comp_device = GetCompositeDevice(kCompositeDevName);
    ASSERT_NOT_NULL(comp_device);
    ASSERT_EQ(Device::State::kActive, comp_device->state());
  }
}

TEST_F(CompositeTestCase, DeviceIteratorCompositeChild) {
  size_t parent_index;
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(platform_bus()->device, "parent-device", 1 /* protocol id */, "", &parent_index));

  uint32_t protocol_id = 1;
  ASSERT_NO_FATAL_FAILURE(
      BindCompositeDefineComposite(platform_bus()->device, &protocol_id, 1, "composite"));

  DeviceState composite;
  size_t fragment_device_indexes;
  ASSERT_NO_FATAL_FAILURE(
      CheckCompositeCreation("composite", &parent_index, 1, &fragment_device_indexes, &composite));
  const std::list children = device(parent_index)->device->children();
  std::list<Device*>::const_iterator it = children.begin();
  ASSERT_EQ((*it++)->name(), "composite-comp-device-0");
  ASSERT_EQ((*it++)->name(), "composite");
  ASSERT_EQ(it, children.end());
}

TEST_F(CompositeTestCase, DeviceIteratorCompositeChildNoFragment) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(endpoints.status_value());

  size_t parent_index;
  ASSERT_NO_FATAL_FAILURE(AddDevice(platform_bus()->device, "parent-device", 1 /* protocol id */,
                                    "", true, true, true, fdm::AddDeviceConfig::kMustIsolate,
                                    std::move(endpoints->client), zx::vmo(), &parent_index));

  // If a parent device has these properties, any composite devices will be
  // created without an intermediate fragment device.
  ASSERT_TRUE(device(parent_index)->device->has_outgoing_directory());
  ASSERT_TRUE(device(parent_index)->device->flags & DEV_CTX_MUST_ISOLATE);

  uint32_t protocol_id = 1;
  ASSERT_NO_FATAL_FAILURE(
      BindCompositeDefineComposite(platform_bus()->device, &protocol_id, 1, "composite"));

  DeviceState fidl_proxy;
  ASSERT_NO_FATAL_FAILURE(CheckCreateFidlProxyDeviceReceived(driver_host_server(), &fidl_proxy));

  // Make sure the composite comes up
  DeviceState composite;
  ASSERT_NO_FATAL_FAILURE(
      CheckCreateCompositeDeviceReceived(driver_host_server(), "composite", 1, &composite));

  ASSERT_FALSE(device(parent_index)->device->children().empty());
  for (auto& d : device(parent_index)->device->children()) {
    ASSERT_EQ(d->name(), "composite");
  }
}

TEST_F(CompositeTestCase, DeviceIteratorCompositeSibling) {
  size_t parent_index;
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(platform_bus()->device, "parent-device", 1 /* protocol id */, "", &parent_index));

  uint32_t protocol_id = 1;
  ASSERT_NO_FATAL_FAILURE(
      BindCompositeDefineComposite(platform_bus()->device, &protocol_id, 1, "composite"));
  DeviceState composite;
  size_t fragment_device_indexes;
  ASSERT_NO_FATAL_FAILURE(
      CheckCompositeCreation("composite", &parent_index, 1, &fragment_device_indexes, &composite));

  size_t child_index;
  ASSERT_NO_FATAL_FAILURE(
      AddDevice(device(parent_index)->device, "sibling-device", 0, "", &child_index));

  ASSERT_TRUE(device(child_index)->device->children().empty());
}

TEST_F(CompositeTestCase, DeviceIteratorOrphanFragment) {
  size_t index;
  fbl::String driver = coordinator().LoadFragmentDriver()->libname;
  ASSERT_NO_FATAL_FAILURE(AddDevice(platform_bus()->device, "fragment-device", 0, driver, &index));

  device(index)->device->DetachFromParent();

  ASSERT_TRUE(device(index)->device->children().empty());
}

TEST_F(CompositeTestCase, MultibindWithOutgoingDirectory) {
  // Make sure that a device with an outgoing directory can have multiple
  // composite devices bind to it. See fxbug.dev/105017 for details.
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(endpoints.status_value());

  size_t parent_index;
  ASSERT_NO_FATAL_FAILURE(AddDevice(platform_bus()->device, "parent-device", 1 /* protocol id */,
                                    "", true, true, true, fdm::AddDeviceConfig::kMustIsolate,
                                    std::move(endpoints->client), zx::vmo(), &parent_index));

  // If a parent device has these properties, any composite devices will be
  // created without an intermediate fragment device.
  ASSERT_TRUE(device(parent_index)->device->has_outgoing_directory());
  ASSERT_TRUE(device(parent_index)->device->flags & DEV_CTX_MUST_ISOLATE);

  uint32_t protocol_id = 1;
  ASSERT_NO_FATAL_FAILURE(
      BindCompositeDefineComposite(platform_bus()->device, &protocol_id, 1, "composite-1"));

  DeviceState fidl_proxy;
  ASSERT_NO_FATAL_FAILURE(CheckCreateFidlProxyDeviceReceived(driver_host_server(), &fidl_proxy));

  // Make sure the composite comes up.
  DeviceState composite;
  ASSERT_NO_FATAL_FAILURE(
      CheckCreateCompositeDeviceReceived(driver_host_server(), "composite-1", 1, &composite));

  uint32_t protocol_id2 = 1;
  ASSERT_NO_FATAL_FAILURE(
      BindCompositeDefineComposite(platform_bus()->device, &protocol_id2, 1, "composite-2"));

  DeviceState fidl_proxy2;
  ASSERT_NO_FATAL_FAILURE(CheckCreateFidlProxyDeviceReceived(driver_host_server(), &fidl_proxy2));

  // Make sure a second composite comes up.
  DeviceState composite2;
  ASSERT_NO_FATAL_FAILURE(
      CheckCreateCompositeDeviceReceived(driver_host_server(), "composite-2", 1, &composite2));
}
