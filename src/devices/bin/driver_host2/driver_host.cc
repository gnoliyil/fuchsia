// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_host2/driver_host.h"

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async-loop/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fit/function.h>
#include <zircon/dlfcn.h>

#include "src/devices/lib/driver2/start_args.h"
#include "src/devices/lib/log/log.h"
#include "src/lib/storage/vfs/cpp/service.h"

namespace fdf = fuchsia_driver_framework;
namespace fio = fuchsia_io;
namespace frunner = fuchsia_component_runner;

namespace {

class FileEventHandler : public fidl::WireAsyncEventHandler<fio::File> {
 public:
  explicit FileEventHandler(const std::string& binary_value) : binary_value_(binary_value) {}

  void Unbound(fidl::UnbindInfo info) override {
    if (!info.ok()) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not open library: %s, %s",
           binary_value_.c_str(), info.status_string(), info.error_message());
    }
  }

 private:
  const std::string& binary_value_;
};

}  // namespace

zx::status<std::unique_ptr<Driver>> Driver::Load(std::string url, std::string binary, zx::vmo vmo) {
  void* library = dlopen_vmo(vmo.get(), RTLD_NOW);
  if (library == nullptr) {
    LOGF(ERROR, "Failed to start driver, could not load library: %s", dlerror());
    return zx::error(ZX_ERR_INTERNAL);
  }
  auto record = static_cast<DriverRecordV1*>(dlsym(library, "__fuchsia_driver_record__"));
  if (record == nullptr) {
    LOGF(ERROR, "Failed to start driver, driver record not found");
    return zx::error(ZX_ERR_NOT_FOUND);
  }
  if (record->version != 1) {
    LOGF(ERROR, "Failed to start driver, unknown driver record version: %lu", record->version);
    return zx::error(ZX_ERR_WRONG_TYPE);
  }
  return zx::ok(std::make_unique<Driver>(std::move(url), std::move(binary), library, record));
}

Driver::Driver(std::string url, std::string binary, void* library, DriverRecordV1* record)
    : url_(std::move(url)), binary_(std::move(binary)), library_(library), record_(record) {}

Driver::~Driver() {
  zx_status_t status = record_->stop(opaque_);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to stop driver: %s", zx_status_get_string(status));
  }
  dlclose(library_);

  if (binding_.has_value()) {
    binding_->Unbind();
  }
}

void Driver::set_binding(fidl::ServerBindingRef<fuchsia_driver_framework::Driver> binding) {
  binding_.emplace(std::move(binding));
}

zx::status<> Driver::Start(fidl::OutgoingMessage& start_args, async_dispatcher_t* dispatcher) {
  auto converted = fidl::OutgoingToIncomingMessage(start_args);
  if (converted.status() != ZX_OK) {
    return zx::error(converted.status());
  }
  // After calling |record_->start|, we assume it has taken ownership of
  // the handles from |start_args|, and can therefore relinquish ownership.
  fidl_incoming_msg_t c_msg = std::move(converted.incoming_message()).ReleaseToEncodedCMessage();
  zx_status_t status = record_->start(&c_msg, dispatcher, &opaque_);
  return zx::make_status(status);
}

DriverHost::DriverHost(inspect::Inspector* inspector, async::Loop* loop) : loop_(loop) {
  inspector->GetRoot().CreateLazyNode(
      "drivers", [this] { return Inspect(); }, inspector);
}

fit::promise<inspect::Inspector> DriverHost::Inspect() {
  inspect::Inspector inspector;
  auto& root = inspector.GetRoot();
  size_t i = 0;
  for (auto& driver : drivers_) {
    auto child = root.CreateChild("driver-" + std::to_string(++i));
    child.CreateString("url", driver.url(), &inspector);
    child.CreateString("binary", driver.binary(), &inspector);
    inspector.emplace(std::move(child));
  }
  return fit::make_ok_promise(std::move(inspector));
}

zx::status<> DriverHost::PublishDriverHost(const fbl::RefPtr<fs::PseudoDir>& svc_dir) {
  const auto service = [this](zx::channel request) {
    fidl::BindServer(loop_->dispatcher(), std::move(request), this);
    return ZX_OK;
  };
  zx_status_t status = svc_dir->AddEntry(fidl::DiscoverableProtocolName<fdf::DriverHost>,
                                         fbl::MakeRefCounted<fs::Service>(service));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to add directory entry '%s': %s",
         fidl::DiscoverableProtocolName<fdf::DriverHost>, zx_status_get_string(status));
  }
  return zx::make_status(status);
}

void DriverHost::Start(StartRequestView request, StartCompleter::Sync& completer) {
  if (!request->start_args.has_url()) {
    LOGF(ERROR, "Failed to start driver, missing 'url' argument");
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  std::string url(request->start_args.url().get());
  auto pkg = request->start_args.has_ns() ? start_args::NsValue(request->start_args.ns(), "/pkg")
                                          : zx::error(ZX_ERR_INVALID_ARGS);
  if (pkg.is_error()) {
    LOGF(ERROR, "Failed to start driver, missing '/pkg' directory: %s",
         zx_status_get_string(pkg.error_value()));
    completer.Close(pkg.error_value());
    return;
  }
  zx::status<std::string> binary =
      request->start_args.has_program()
          ? start_args::ProgramValue(request->start_args.program(), "binary")
          : zx::error(ZX_ERR_INVALID_ARGS);
  if (binary.is_error()) {
    LOGF(ERROR, "Failed to start driver, missing 'binary' argument: %s",
         zx_status_get_string(binary.error_value()));
    completer.Close(binary.error_value());
    return;
  }
  // Open the driver's binary within the driver's package.
  auto endpoints = fidl::CreateEndpoints<fio::File>();
  if (endpoints.is_error()) {
    completer.Close(endpoints.status_value());
    return;
  }
  zx_status_t status = fdio_open_at(pkg->handle(), binary->data(),
                                    fio::wire::kOpenRightReadable | fio::wire::kOpenRightExecutable,
                                    endpoints->server.TakeChannel().release());
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to start driver '/pkg/%s', could not open library: %s", binary->data(),
         zx_status_get_string(status));
    completer.Close(status);
    return;
  }
  // We encode start_args outside of callback in order to access stack-allocated
  // data before it is destroyed.
  auto message =
      std::make_unique<fdf::wire::DriverStartArgs::OwnedEncodedMessage>(&request->start_args);
  if (!message->ok()) {
    LOGF(ERROR, "Failed to start driver '/pkg/%s', could not encode start args: %s", binary->data(),
         message->error_message());
    completer.Close(message->status());
    return;
  }

  // Once we receive the VMO from the call to GetBuffer, we can load the driver
  // into this driver host. We move the storage and encoded for start_args into
  // this callback to extend its lifetime.
  fidl::Client<fio::File> file(std::move(endpoints->client), loop_->dispatcher(),
                               std::make_shared<FileEventHandler>(binary.value()));
  auto callback = [this, request = std::move(request->driver), completer = completer.ToAsync(),
                   url = std::move(url), binary = std::move(binary.value()),
                   message = std::move(message),
                   _ = file.Clone()](fidl::WireResponse<fio::File::GetBuffer>* response) mutable {
    if (response->s != ZX_OK) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not get library VMO: %s", binary.data(),
           zx_status_get_string(response->s));
      completer.Close(response->s);
      return;
    }
    zx_status_t status =
        response->buffer->vmo.set_property(ZX_PROP_NAME, binary.data(), binary.size());
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not name library VMO: %s", binary.data(),
           zx_status_get_string(status));
      completer.Close(status);
      return;
    }
    auto driver = Driver::Load(std::move(url), binary, std::move(response->buffer->vmo));
    if (driver.is_error()) {
      completer.Close(driver.error_value());
      return;
    }
    auto driver_ptr = driver.value().get();
    auto bind = fidl::BindServer<Driver>(loop_->dispatcher(), std::move(request), driver_ptr,
                                         [this](Driver* driver, auto, auto) {
                                           drivers_.erase(*driver);
                                           // If this is the last driver, shutdown the driver host.
                                           if (drivers_.is_empty()) {
                                             loop_->Quit();
                                           }
                                         });
    driver->set_binding(std::move(bind));
    drivers_.push_back(std::move(driver.value()));

    auto start = driver_ptr->Start(message->GetOutgoingMessage(), loop_->dispatcher());
    if (start.is_error()) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s': %s", binary.data(), start.status_string());
      completer.Close(start.error_value());
      return;
    }
    LOGF(INFO, "Started '%s'", binary.data());
  };
  file->GetBuffer(fio::wire::kVmoFlagRead | fio::wire::kVmoFlagExec | fio::wire::kVmoFlagPrivate,
                  std::move(callback));
}
