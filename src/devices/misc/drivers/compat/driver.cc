// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/misc/drivers/compat/driver.h"

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <fidl/fuchsia.scheduler/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/ddk/binding_priv.h>
#include <lib/driver/compat/cpp/connect.h>
#include <lib/driver/component/cpp/start_args.h>
#include <lib/driver/promise/cpp/promise.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fit/defer.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/promise.h>
#include <lib/fpromise/single_threaded_executor.h>
#include <lib/sync/cpp/completion.h>
#include <zircon/dlfcn.h>

#include "src/devices/misc/drivers/compat/loader.h"

namespace fboot = fuchsia_boot;
namespace fdf {
using namespace fuchsia_driver_framework;
}
namespace fio = fuchsia_io;
namespace fldsvc = fuchsia_ldsvc;
namespace fdm = fuchsia_device_manager;

using fpromise::bridge;
using fpromise::error;
using fpromise::join_promises;
using fpromise::ok;
using fpromise::promise;
using fpromise::result;

// This lock protects any globals, as globals could be accessed by other
// drivers and other threads within the process.
// Currently this protects the root resource and the loader service.
std::mutex kDriverGlobalsLock;
zx::resource kRootResource;

namespace {

constexpr auto kOpenFlags = fio::wire::OpenFlags::kRightReadable |
                            fio::wire::OpenFlags::kRightExecutable |
                            fio::wire::OpenFlags::kNotDirectory;
constexpr auto kVmoFlags = fio::wire::VmoFlags::kRead | fio::wire::VmoFlags::kExecute;
constexpr auto kLibDriverPath = "/pkg/driver/compat.so";

}  // namespace

namespace compat {

zx_status_t AddMetadata(Device* device,
                        fidl::VectorView<fuchsia_driver_compat::wire::Metadata> data) {
  for (auto& metadata : data) {
    size_t size;
    zx_status_t status = metadata.data.get_property(ZX_PROP_VMO_CONTENT_SIZE, &size, sizeof(size));
    if (status != ZX_OK) {
      return status;
    }
    std::vector<uint8_t> data(size);
    status = metadata.data.read(data.data(), 0, data.size());
    if (status != ZX_OK) {
      return status;
    }

    status = device->AddMetadata(metadata.type, data.data(), data.size());
    if (status != ZX_OK) {
      return status;
    }
  }
  return ZX_OK;
}

promise<void, zx_status_t> GetAndAddMetadata(
    fidl::WireClient<fuchsia_driver_compat::Device>& client, Device* device) {
  bridge<void, zx_status_t> bridge;
  client->GetMetadata().Then(
      [device, completer = std::move(bridge.completer)](
          fidl::WireUnownedResult<fuchsia_driver_compat::Device::GetMetadata>& result) mutable {
        if (!result.ok()) {
          return;
        }
        auto* response = result.Unwrap();
        if (response->is_error()) {
          completer.complete_error(response->error_value());
          return;
        }
        zx_status_t status = AddMetadata(device, response->value()->metadata);
        if (status != ZX_OK) {
          completer.complete_error(status);
          return;
        }
        completer.complete_ok();
      });
  return bridge.consumer.promise_or(error(ZX_ERR_INTERNAL));
}

DriverList global_driver_list;

zx_driver_t* DriverList::ZxDriver() { return static_cast<zx_driver_t*>(this); }

void DriverList::AddDriver(Driver* driver) {
  std::scoped_lock lock(kDriverGlobalsLock);
  drivers_.insert(driver);
}

void DriverList::RemoveDriver(Driver* driver) {
  std::scoped_lock lock(kDriverGlobalsLock);
  drivers_.erase(driver);
}

void DriverList::Log(FuchsiaLogSeverity severity, const char* tag, const char* file, int line,
                     const char* msg, va_list args) {
  std::scoped_lock lock(kDriverGlobalsLock);
  if (drivers_.empty()) {
    return;
  }
  (*drivers_.begin())->Log(severity, tag, file, line, msg, args);
}

Driver::Driver(fdf::DriverStartArgs start_args,
               fdf::UnownedSynchronizedDispatcher driver_dispatcher, device_t device,
               const zx_protocol_device_t* ops, std::string_view driver_path)
    : fdf::DriverBase("compat", std::move(start_args), std::move(driver_dispatcher)),
      executor_(dispatcher()),
      driver_path_(driver_path),
      device_(device, ops, this, std::nullopt, nullptr, dispatcher()) {
  // Give the parent device the correct node.
  device_.Bind({std::move(node()), dispatcher()});
  // Call this so the parent device is in the post-init state.
  device_.InitReply(ZX_OK);

  global_driver_list.AddDriver(this);
  ZX_ASSERT(url().has_value());
}

Driver::~Driver() {
  if (ShouldCallRelease()) {
    record_->ops->release(context_);
  }
  dlclose(library_);
  global_driver_list.RemoveDriver(this);
}

zx::result<> Driver::Start() {
  // Connect to devfs exporter.
  {
    zx::result result = context().incoming()->Connect<fuchsia_device_fs::Exporter>();
    if (result.is_error()) {
      return result.take_error();
    }
    devfs_exporter_ = fidl::WireSharedClient(std::move(result.value()), dispatcher());
  }

  // Serve the diagnostics directory.
  if (zx_status_t status = ServeDiagnosticsDir(); status != ZX_OK) {
    return zx::error(status);
  }

  auto compat_connect =
      Driver::ConnectToParentDevices()
          .and_then(fit::bind_member<&Driver::GetDeviceInfo>(this))
          .then([this](result<void, zx_status_t>& result) -> fpromise::result<void, zx_status_t> {
            if (result.is_error()) {
              FDF_LOG(WARNING, "Getting DeviceInfo failed with: %s",
                      zx_status_get_string(result.error()));
            }
            return ok();
          });

  auto root_resource =
      fpromise::make_result_promise<zx::resource, zx_status_t>(error(ZX_ERR_ALREADY_BOUND)).box();
  {
    std::scoped_lock lock(kDriverGlobalsLock);
    if (!kRootResource.is_valid()) {
      // If the root resource is invalid, try fetching it. Once we've fetched it we might find that
      // we lost the race with another process -- we'll handle that later.
      auto connect_promise =
          fdf::Connect<fboot::RootResource>(*context().incoming(), dispatcher())
              .and_then(fit::bind_member<&Driver::GetRootResource>(this))
              .or_else([this](zx_status_t& status) {
                FDF_LOG(WARNING, "Failed to get root resource: %s", zx_status_get_string(status));
                FDF_LOG(WARNING, "Assuming test environment and continuing");
                return error(status);
              })
              .box();
      root_resource.swap(connect_promise);
    }
  }

  auto loader_vmo = fdf::Open(*context().incoming(), dispatcher(), kLibDriverPath, kOpenFlags)
                        .and_then(fit::bind_member<&Driver::GetBuffer>(this));
  auto driver_vmo = fdf::Open(*context().incoming(), dispatcher(), driver_path_.c_str(), kOpenFlags)
                        .and_then(fit::bind_member<&Driver::GetBuffer>(this));
  auto start_driver =
      join_promises(std::move(root_resource), std::move(loader_vmo), std::move(driver_vmo))
          .then(fit::bind_member<&Driver::Join>(this))
          .and_then(fit::bind_member<&Driver::LoadDriver>(this))
          .and_then(std::move(compat_connect))
          .and_then(fit::bind_member<&Driver::StartDriver>(this))
          .or_else(fit::bind_member<&Driver::StopDriver>(this))
          .wrap_with(scope_);
  executor_.schedule_task(std::move(start_driver));

  return zx::ok();
}

bool Driver::IsComposite() { return !parent_clients_.empty(); }

bool Driver::IsRunningOnDispatcher() const {
  fdf::Unowned<fdf::Dispatcher> current_dispatcher = fdf::Dispatcher::GetCurrent();
  if (current_dispatcher == fdf::Unowned<fdf::Dispatcher>{}) {
    return false;
  }
  return current_dispatcher->async_dispatcher() == dispatcher();
}

zx_status_t Driver::RunOnDispatcher(fit::callback<zx_status_t()> task) {
  if (IsRunningOnDispatcher()) {
    return task();
  }

  libsync::Completion completion;
  zx_status_t task_status;
  auto discarded = fit::defer([&] {
    task_status = ZX_ERR_CANCELED;
    completion.Signal();
  });
  zx_status_t status =
      async::PostTask(dispatcher(), [&task_status, &completion, task = std::move(task),
                                     discarded = std::move(discarded)]() mutable {
        discarded.cancel();
        task_status = task();
        completion.Signal();
      });
  if (status != ZX_OK) {
    return status;
  }
  completion.Wait();
  return status;
}

void Driver::PrepareStop(fdf::PrepareStopCompleter completer) {
  zx::result client =
      this->context().incoming()->Connect<fuchsia_device_manager::SystemStateTransition>();
  if (client.is_error()) {
    FDF_SLOG(ERROR, "failed to connect to fuchsia.device.manager/SystemStateTransition",
             KV("status", client.status_value()));
    completer(client.take_error());
    return;
  }
  fidl::WireResult result = fidl::WireCall(client.value())->GetTerminationSystemState();
  if (!result.ok()) {
    FDF_SLOG(ERROR, "failed to get termination state", KV("status", client.status_value()));
    completer(zx::error(result.error().status()));
    return;
  }

  system_state_ = result->state;

  executor_.schedule_task(
      device_.HandleStopSignal(result->state)
          .then([completer = std::move(completer)](fpromise::result<void>& init) mutable {
            completer(zx::ok());
          }));
}

promise<zx::resource, zx_status_t> Driver::GetRootResource(
    const fidl::WireSharedClient<fboot::RootResource>& root_resource) {
  bridge<zx::resource, zx_status_t> bridge;
  auto callback = [completer = std::move(bridge.completer)](
                      fidl::WireUnownedResult<fboot::RootResource::Get>& result) mutable {
    if (!result.ok()) {
      completer.complete_error(result.status());
      return;
    }
    completer.complete_ok(std::move(result.value().resource));
  };
  root_resource->Get().ThenExactlyOnce(std::move(callback));
  return bridge.consumer.promise();
}

promise<Driver::FileVmo, zx_status_t> Driver::GetBuffer(
    const fidl::WireSharedClient<fio::File>& file) {
  bridge<FileVmo, zx_status_t> bridge;
  auto callback = [completer = std::move(bridge.completer)](
                      fidl::WireUnownedResult<fio::File::GetBackingMemory>& result) mutable {
    if (!result.ok()) {
      completer.complete_error(result.status());
      return;
    }
    const auto* res = result.Unwrap();
    if (res->is_error()) {
      completer.complete_error(res->error_value());
      return;
    }
    zx::vmo& vmo = res->value()->vmo;
    uint64_t size;
    if (zx_status_t status = vmo.get_prop_content_size(&size); status != ZX_OK) {
      completer.complete_error(status);
      return;
    }
    completer.complete_ok(FileVmo{
        .vmo = std::move(vmo),
        .size = size,
    });
    return;
  };
  file->GetBackingMemory(kVmoFlags).ThenExactlyOnce(std::move(callback));
  return bridge.consumer.promise().or_else([this](zx_status_t& status) {
    FDF_LOG(WARNING, "Failed to get buffer: %s", zx_status_get_string(status));
    return error(status);
  });
}

result<std::tuple<zx::vmo, zx::vmo>, zx_status_t> Driver::Join(
    result<std::tuple<result<zx::resource, zx_status_t>, result<FileVmo, zx_status_t>,
                      result<FileVmo, zx_status_t>>>& results) {
  if (results.is_error()) {
    return error(ZX_ERR_INTERNAL);
  }
  auto& [root_resource, loader_vmo, driver_vmo] = results.value();
  if (root_resource.is_ok()) {
    std::scoped_lock lock(kDriverGlobalsLock);
    if (!kRootResource.is_valid()) {
      kRootResource = root_resource.take_value();
    }
  }
  if (loader_vmo.is_error()) {
    return loader_vmo.take_error_result();
  }
  if (driver_vmo.is_error()) {
    return driver_vmo.take_error_result();
  }
  return fpromise::ok(
      std::make_tuple(std::move((loader_vmo.value().vmo)), std::move((driver_vmo.value().vmo))));
}

result<void, zx_status_t> Driver::LoadDriver(std::tuple<zx::vmo, zx::vmo>& vmos) {
  auto& [loader_vmo, driver_vmo] = vmos;
  std::string& url_str = url().value();

  // Replace loader service to load the DFv1 driver, load the driver,
  // then place the original loader service back.
  {
    // This requires a lock because the loader is a global variable.
    std::scoped_lock lock(kDriverGlobalsLock);
    auto endpoints = fidl::CreateEndpoints<fldsvc::Loader>();
    if (endpoints.is_error()) {
      return error(endpoints.status_value());
    }
    zx::channel loader_channel(dl_set_loader_service(endpoints->client.channel().release()));
    fidl::ClientEnd<fldsvc::Loader> loader_client(std::move(loader_channel));
    auto clone = fidl::CreateEndpoints<fldsvc::Loader>();
    if (clone.is_error()) {
      return error(clone.status_value());
    }
    auto result = fidl::WireCall(loader_client)->Clone(std::move(clone->server));
    if (!result.ok()) {
      FDF_LOG(ERROR, "Failed to load driver '%s', cloning loader failed with FIDL status: %s",
              url_str.data(), result.status_string());
      return error(result.status());
    }
    if (result.value().rv != ZX_OK) {
      FDF_LOG(ERROR, "Failed to load driver '%s', cloning loader failed with status: %s",
              url_str.data(), zx_status_get_string(result.value().rv));
      return error(result.value().rv);
    }

    // Start loader.
    async::Loop loader_loop(&kAsyncLoopConfigNeverAttachToThread);
    zx_status_t status = loader_loop.StartThread("loader-loop");
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to load driver '%s', could not start thread for loader loop: %s",
              url_str.data(), zx_status_get_string(status));
      return error(status);
    }
    Loader loader(loader_loop.dispatcher());
    auto bind = loader.Bind(fidl::ClientEnd<fldsvc::Loader>(std::move(loader_client)),
                            std::move(loader_vmo));
    if (bind.is_error()) {
      return error(bind.status_value());
    }
    fidl::BindServer(loader_loop.dispatcher(), std::move(endpoints->server), &loader);

    // Open driver.
    library_ = dlopen_vmo(driver_vmo.get(), RTLD_NOW);
    if (library_ == nullptr) {
      FDF_LOG(ERROR, "Failed to load driver '%s', could not load library: %s", url_str.data(),
              dlerror());
      return error(ZX_ERR_INTERNAL);
    }

    // Return original loader service.
    loader_channel.reset(dl_set_loader_service(clone->client.channel().release()));
  }

  // Load and verify symbols.
  auto note = static_cast<const zircon_driver_note_t*>(dlsym(library_, "__zircon_driver_note__"));
  if (note == nullptr) {
    FDF_LOG(ERROR, "Failed to load driver '%s', driver note not found", url_str.data());
    return error(ZX_ERR_BAD_STATE);
  }
  FDF_LOG(INFO, "Loaded driver '%s'", note->payload.name);
  record_ = static_cast<zx_driver_rec_t*>(dlsym(library_, "__zircon_driver_rec__"));
  if (record_ == nullptr) {
    FDF_LOG(ERROR, "Failed to load driver '%s', driver record not found", url_str.data());
    return error(ZX_ERR_BAD_STATE);
  }
  if (record_->ops == nullptr) {
    FDF_LOG(ERROR, "Failed to load driver '%s', missing driver ops", url_str.data());
    return error(ZX_ERR_BAD_STATE);
  }
  if (record_->ops->version != DRIVER_OPS_VERSION) {
    FDF_LOG(ERROR, "Failed to load driver '%s', incorrect driver version", url_str.data());
    return error(ZX_ERR_WRONG_TYPE);
  }
  if (record_->ops->bind == nullptr && record_->ops->create == nullptr) {
    FDF_LOG(ERROR, "Failed to load driver '%s', missing '%s'", url_str.data(),
            (record_->ops->bind == nullptr ? "bind" : "create"));
    return error(ZX_ERR_BAD_STATE);
  }
  if (record_->ops->bind != nullptr && record_->ops->create != nullptr) {
    FDF_LOG(ERROR, "Failed to load driver '%s', both 'bind' and 'create' are defined",
            url_str.data());
    return error(ZX_ERR_INVALID_ARGS);
  }
  record_->driver = global_driver_list.ZxDriver();

  // Create logger.
  auto inner_logger = fdf::Logger::Create(*context().incoming(), dispatcher(), note->payload.name);
  if (inner_logger.is_error()) {
    return error(inner_logger.status_value());
  }
  inner_logger_ = std::move(inner_logger.value());
  device_.set_logger(inner_logger_.get());

  return ok();
}

result<void, zx_status_t> Driver::StartDriver() {
  std::string& url_str = url().value();
  if (record_->ops->init != nullptr) {
    // If provided, run init.
    zx_status_t status = record_->ops->init(&context_);
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to load driver '%s', 'init' failed: %s", url_str.data(),
              zx_status_get_string(status));
      return error(status);
    }
  }
  if (record_->ops->bind != nullptr) {
    // If provided, run bind and return.
    zx_status_t status = record_->ops->bind(context_, device_.ZxDevice());
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to load driver '%s', 'bind' failed: %s", url_str.data(),
              zx_status_get_string(status));
      return error(status);
    }
  } else {
    // Else, run create and return.
    auto client_end = context().incoming()->Connect<fboot::Items>();
    if (client_end.is_error()) {
      return error(client_end.status_value());
    }
    zx_status_t status = record_->ops->create(context_, device_.ZxDevice(), "proxy",
                                              client_end->channel().release());
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to load driver '%s', 'create' failed: %s", url_str.data(),
              zx_status_get_string(status));
      return error(status);
    }
  }
  if (!device_.HasChildren()) {
    FDF_LOG(ERROR, "Driver '%s' did not add a child device", url_str.data());
    return error(ZX_ERR_BAD_STATE);
  }
  return ok();
}

result<> Driver::StopDriver(const zx_status_t& status) {
  FDF_LOG(ERROR, "Failed to start driver '%s': %s", url().value().data(),
          zx_status_get_string(status));
  device_.Unbind();
  return ok();
}

fpromise::promise<void, zx_status_t> Driver::ConnectToParentDevices() {
  bridge<void, zx_status_t> bridge;
  compat::ConnectToParentDevices(
      dispatcher(), context().incoming().get(),
      [this, completer = std::move(bridge.completer)](
          zx::result<std::vector<compat::ParentDevice>> devices) mutable {
        if (devices.is_error()) {
          completer.complete_error(devices.error_value());
          return;
        }
        std::vector<std::string> parents_names;
        for (auto& device : devices.value()) {
          if (device.name == "default") {
            parent_client_ = fidl::WireClient<fuchsia_driver_compat::Device>(
                std::move(device.client), dispatcher());
            continue;
          }

          // TODO(fxbug.dev/100985): When services stop adding extra instances
          // separated by ',' then remove this check.
          if (device.name.find(',') != std::string::npos) {
            continue;
          }

          parents_names.push_back(device.name);
          parent_clients_[device.name] = fidl::WireClient<fuchsia_driver_compat::Device>(
              std::move(device.client), dispatcher());
        }
        device_.set_fragments(std::move(parents_names));
        completer.complete_ok();
      });
  return bridge.consumer.promise_or(error(ZX_ERR_INTERNAL)).wrap_with(scope_);
}

promise<void, zx_status_t> Driver::GetDeviceInfo() {
  if (!parent_client_) {
    return fpromise::make_result_promise<void, zx_status_t>(error(ZX_ERR_PEER_CLOSED));
  }

  std::vector<promise<void, zx_status_t>> promises;

  // Get our topological path from our default parent.
  bridge<void, zx_status_t> topo_bridge;
  parent_client_->GetTopologicalPath().Then(
      [this, completer = std::move(topo_bridge.completer)](
          fidl::WireUnownedResult<fuchsia_driver_compat::Device::GetTopologicalPath>&
              result) mutable {
        if (!result.ok()) {
          FDF_LOG(ERROR, "Failed to get topo path %s", zx_status_get_string(result.status()));
          return;
        }
        auto* response = result.Unwrap();
        auto topological_path = std::string(response->path.data(), response->path.size());
        // If we are a composite then we have to add the name of our composite device
        // to our primary parent. The composite device's name is the node_name handed
        // to us.
        if (IsComposite()) {
          topological_path.append("/");
          topological_path.append(node_name().value());
        }
        device_.set_topological_path(std::move(topological_path));
        completer.complete_ok();
      });

  promises.push_back(topo_bridge.consumer.promise_or(error(ZX_ERR_INTERNAL)));

  // Get our metadata from our fragments if we are a composite,
  // or our primary parent.
  if (IsComposite()) {
    for (auto& client : parent_clients_) {
      promises.push_back(GetAndAddMetadata(client.second, &device_));
    }
  } else {
    promises.push_back(GetAndAddMetadata(parent_client_, &device_));
  }

  // Collect all our promises and return the first error we see.
  return join_promise_vector(std::move(promises))
      .then([](fpromise::result<std::vector<fpromise::result<void, zx_status_t>>>& results) {
        if (results.is_error()) {
          return fpromise::make_result_promise(error(ZX_ERR_INTERNAL));
        }
        for (auto& result : results.value()) {
          if (result.is_error()) {
            return fpromise::make_result_promise(error(result.error()));
          }
        }
        return fpromise::make_result_promise<void, zx_status_t>(ok());
      });
}

void* Driver::Context() const { return context_; }

void Driver::Log(FuchsiaLogSeverity severity, const char* tag, const char* file, int line,
                 const char* msg, va_list args) {
  if (inner_logger_.get() != nullptr) {
    inner_logger_->logvf(severity, tag, file, line, msg, args);
  }
}

zx::result<zx::vmo> Driver::LoadFirmware(Device* device, const char* filename, size_t* size) {
  std::string full_filename = "/pkg/lib/firmware/";
  full_filename.append(filename);
  fpromise::result connect_result = fpromise::run_single_threaded(
      fdf::Open(*context().incoming(), dispatcher(), full_filename.c_str(), kOpenFlags));
  if (connect_result.is_error()) {
    return zx::error(connect_result.take_error());
  }

  fidl::WireResult get_backing_memory_result =
      connect_result.take_value().sync()->GetBackingMemory(fio::wire::VmoFlags::kRead);
  if (!get_backing_memory_result.ok()) {
    if (get_backing_memory_result.is_peer_closed()) {
      return zx::error(ZX_ERR_NOT_FOUND);
    }
    return zx::error(get_backing_memory_result.status());
  }
  const auto* res = get_backing_memory_result.Unwrap();
  if (res->is_error()) {
    return zx::error(res->error_value());
  }
  zx::vmo& vmo = res->value()->vmo;
  if (zx_status_t status = vmo.get_prop_content_size(size); status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(vmo));
}

zx_status_t Driver::AddDevice(Device* parent, device_add_args_t* args, zx_device_t** out) {
  return RunOnDispatcher([&] {
    zx_device_t* child;
    zx_status_t status = parent->Add(args, &child);
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to add device %s: %s", args->name, zx_status_get_string(status));
      return status;
    }
    if (out) {
      *out = child;
    }
    return ZX_OK;
  });
}

zx::result<> Driver::SetProfileByRole(zx::unowned_thread thread, std::string_view role) {
  auto profile_client = context().incoming()->Connect<fuchsia_scheduler::ProfileProvider>();
  if (profile_client.is_error()) {
    return profile_client.take_error();
  }

  zx::thread duplicate_thread;
  zx_status_t status =
      thread->duplicate(ZX_RIGHT_TRANSFER | ZX_RIGHT_MANAGE_THREAD, &duplicate_thread);
  if (status != ZX_OK) {
    return zx::error(status);
  }

  fidl::WireResult result =
      fidl::WireCall(*profile_client)
          ->SetProfileByRole(std::move(duplicate_thread), fidl::StringView::FromExternal(role));
  if (!result.ok()) {
    return zx::error(result.status());
  }
  if (result->status != ZX_OK) {
    return zx::error(result->status);
  }
  return zx::ok();
}

zx::result<std::string> Driver::GetVariable(const char* name) {
  auto boot_args = context().incoming()->Connect<fuchsia_boot::Arguments>();
  if (boot_args.is_error()) {
    return boot_args.take_error();
  }

  auto result = fidl::WireCall(*boot_args)->GetString(fidl::StringView::FromExternal(name));
  if (!result.ok() || result->value.is_null() || result->value.empty()) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }
  return zx::ok(std::string(result->value.data(), result->value.size()));
}

zx::result<std::unique_ptr<fdf::DriverBase>> DriverFactory::CreateDriver(
    fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher) {
  auto compat_device =
      fdf::GetSymbol<const device_t*>(start_args.symbols(), kDeviceSymbol, &kDefaultDevice);
  const zx_protocol_device_t* ops =
      fdf::GetSymbol<const zx_protocol_device_t*>(start_args.symbols(), kOps);

  // Open the compat driver's binary within the package.
  auto compat = fdf::ProgramValue(start_args.program(), "compat");
  if (compat.is_error()) {
    return compat.take_error();
  }

  auto driver = std::make_unique<Driver>(std::move(start_args), std::move(driver_dispatcher),
                                         *compat_device, ops, "/pkg/" + *compat);

  auto result = driver->Start();
  if (result.is_error()) {
    return result.take_error();
  }
  return zx::ok(std::move(driver));
}

zx_status_t Driver::ServeDiagnosticsDir() {
  diagnostics_vfs_ = std::make_unique<fs::SynchronousVfs>(dispatcher());

  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }
  auto& [client, server] = endpoints.value();
  zx_status_t status = diagnostics_vfs_->ServeDirectory(diagnostics_dir_, std::move(server));
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to serve diagnostics dir: %s", zx_status_get_string(status));
    return status;
  }
  zx::result result = outgoing().AddDirectory(std::move(client), "diagnostics");
  if (result.is_error()) {
    FDF_LOG(ERROR, "Failed to add diagnostics directory: %s", result.status_string());
    return result.status_value();
  }
  return ZX_OK;
}

}  // namespace compat

using record = fdf::Lifecycle<compat::Driver, compat::DriverFactory>;
FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(record);
