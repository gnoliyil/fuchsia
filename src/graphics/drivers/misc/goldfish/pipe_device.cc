// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/misc/goldfish/pipe_device.h"

#include <fidl/fuchsia.hardware.goldfish.pipe/cpp/markers.h>
#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <inttypes.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/ddk/trace/event.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fidl/cpp/wire/internal/transport.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <zircon/assert.h>
#include <zircon/syscalls/iommu.h>
#include <zircon/threads.h>

#include <fbl/auto_lock.h>

#include "src/devices/lib/acpi/client.h"
#include "src/devices/lib/goldfish/pipe_headers/include/base.h"
#include "src/graphics/drivers/misc/goldfish/instance.h"
#include "zircon/status.h"

namespace goldfish {
namespace {

const char* kTag = "goldfish-pipe";

constexpr uint32_t PIPE_DRIVER_VERSION = 4;
constexpr uint32_t PIPE_MIN_DEVICE_VERSION = 2;
constexpr uint32_t MAX_SIGNALLED_PIPES = 64;

enum PipeV2Regs {
  PIPE_V2_REG_CMD = 0,
  PIPE_V2_REG_SIGNAL_BUFFER_HIGH = 4,
  PIPE_V2_REG_SIGNAL_BUFFER = 8,
  PIPE_V2_REG_SIGNAL_BUFFER_COUNT = 12,
  PIPE_V2_REG_OPEN_BUFFER_HIGH = 20,
  PIPE_V2_REG_OPEN_BUFFER = 24,
  PIPE_V2_REG_VERSION = 36,
  PIPE_V2_REG_GET_SIGNALLED = 48,
};

// Parameters for the PIPE_CMD_OPEN command.
struct OpenCommandBuffer {
  uint64_t pa_command_buffer;
  uint32_t rw_params_max_count;
};

// Information for a single signalled pipe.
struct SignalBuffer {
  uint32_t id;
  uint32_t flags;
};

// Device-level set of buffers shared with the host.
struct CommandBuffers {
  OpenCommandBuffer open_command_buffer;
  SignalBuffer signal_buffers[MAX_SIGNALLED_PIPES];
};

uint32_t upper_32_bits(uint64_t n) { return static_cast<uint32_t>(n >> 32); }

uint32_t lower_32_bits(uint64_t n) { return static_cast<uint32_t>(n); }

}  // namespace

// static
zx_status_t PipeDevice::Create(void* ctx, zx_device_t* parent) {
  auto acpi = acpi::Client::Create(parent);
  if (acpi.is_error()) {
    return acpi.status_value();
  }
  auto pipe_device = std::make_unique<goldfish::PipeDevice>(
      parent, std::move(acpi.value()), fdf::Dispatcher::GetCurrent()->async_dispatcher());
  zx_status_t status = pipe_device->ConnectToSysmem();
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to connect to sysmem fidl: %s", zx_status_get_string(status));
    return status;
  }

  status = pipe_device->Bind();
  if (status != ZX_OK) {
    return status;
  }

  constexpr zx_device_prop_t kControlProps[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GOOGLE},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GOLDFISH},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_GOLDFISH_PIPE_CONTROL},
  };
  constexpr const char* kControlDeviceName = "goldfish-pipe-control";
  status = pipe_device->CreateChildDevice(kControlProps, kControlDeviceName);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: create %s child device failed: %d", kTag, kControlDeviceName, status);
    return status;
  }

  constexpr zx_device_prop_t kSensorProps[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GOOGLE},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GOLDFISH},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_GOLDFISH_PIPE_SENSOR},
  };
  constexpr const char* kSensorDeviceName = "goldfish-pipe-sensor";
  status = pipe_device->CreateChildDevice(kSensorProps, kSensorDeviceName);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: create %s child device failed: %d", kTag, kSensorDeviceName, status);
    return status;
  }

  // devmgr now owns the device.
  [[maybe_unused]] auto* dev = pipe_device.release();
  return ZX_OK;
}

PipeDevice::PipeDevice(zx_device_t* parent, acpi::Client client, async_dispatcher_t* dispatcher)
    : DeviceType(parent), acpi_fidl_(std::move(client)), dispatcher_(dispatcher) {}

PipeDevice::~PipeDevice() {
  if (irq_.is_valid()) {
    irq_.destroy();
    thrd_join(irq_thread_, nullptr);
  }
}

zx_status_t PipeDevice::Bind() {
  auto bti_result = acpi_fidl_.borrow()->GetBti(0);
  if (!bti_result.ok() || bti_result->is_error()) {
    zx_status_t status = bti_result.ok() ? bti_result->error_value() : bti_result.status();
    zxlogf(ERROR, "%s: GetBti failed: %d", kTag, status);
    return status;
  }
  bti_ = std::move(bti_result->value()->bti);

  auto mmio_result = acpi_fidl_.borrow()->GetMmio(0);
  if (!mmio_result.ok() || mmio_result->is_error()) {
    zx_status_t status = mmio_result.ok() ? mmio_result->error_value() : mmio_result.status();
    zxlogf(ERROR, "%s: GetMmio failed: %d", kTag, status);
    return status;
  }

  fbl::AutoLock lock(&mmio_lock_);
  auto& mmio = mmio_result->value()->mmio;
  zx_status_t status = fdf::MmioBuffer::Create(mmio.offset, mmio.size, std::move(mmio.vmo),
                                               ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: mmiobuffer create failed: %d", kTag, status);
    return status;
  }

  // Check device version.
  mmio_->Write32(PIPE_DRIVER_VERSION, PIPE_V2_REG_VERSION);
  uint32_t version = mmio_->Read32(PIPE_V2_REG_VERSION);
  if (version < PIPE_MIN_DEVICE_VERSION) {
    zxlogf(ERROR, "%s: insufficient device version: %d", kTag, version);
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto irq = acpi_fidl_.borrow()->MapInterrupt(0);
  if (!irq.ok() || irq->is_error()) {
    zxlogf(ERROR, "%s: map_interrupt failed: %d", kTag,
           !irq.ok() ? irq.status() : irq->error_value());
    return status;
  }
  irq_.reset(irq->value()->irq.release());

  int rc = thrd_create_with_name(
      &irq_thread_, [](void* arg) { return static_cast<PipeDevice*>(arg)->IrqHandler(); }, this,
      "goldfish_pipe_irq_thread");
  if (rc != thrd_success) {
    irq_.destroy();
    return thrd_status_to_zx_status(rc);
  }

  static_assert(sizeof(CommandBuffers) <= PAGE_SIZE, "cmds size");
  status = io_buffer_.Init(bti_.get(), PAGE_SIZE, IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: io_buffer_init failed: %d", kTag, status);
    return status;
  }

  // Register the buffer addresses with the device.
  zx_paddr_t pa_signal_buffers = io_buffer_.phys() + offsetof(CommandBuffers, signal_buffers);
  mmio_->Write32(upper_32_bits(pa_signal_buffers), PIPE_V2_REG_SIGNAL_BUFFER_HIGH);
  mmio_->Write32(lower_32_bits(pa_signal_buffers), PIPE_V2_REG_SIGNAL_BUFFER);
  mmio_->Write32(MAX_SIGNALLED_PIPES, PIPE_V2_REG_SIGNAL_BUFFER_COUNT);
  zx_paddr_t pa_open_command_buffer =
      io_buffer_.phys() + offsetof(CommandBuffers, open_command_buffer);
  mmio_->Write32(upper_32_bits(pa_open_command_buffer), PIPE_V2_REG_OPEN_BUFFER_HIGH);
  mmio_->Write32(lower_32_bits(pa_open_command_buffer), PIPE_V2_REG_OPEN_BUFFER);

  status = DdkAdd("goldfish-pipe", DEVICE_ADD_NON_BINDABLE);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: create goldfish-pipe root device failed: %d", kTag, status);
    return status;
  }
  return ZX_OK;
}

zx_status_t PipeDevice::CreateChildDevice(cpp20::span<const zx_device_prop_t> props,
                                          const char* dev_name) {
  auto child_device = std::make_unique<PipeChildDevice>(this, dispatcher_);
  zx_status_t status = child_device->Bind(props, dev_name);
  if (status == ZX_OK) {
    // devmgr now owns device.
    [[maybe_unused]] auto* dev = child_device.release();
  }
  return status;
}

void PipeDevice::DdkRelease() { delete this; }

zx_status_t PipeDevice::Create(int32_t* out_id, zx::vmo* out_vmo) {
  TRACE_DURATION("gfx", "PipeDevice::Create");

  static_assert(sizeof(PipeCmdBuffer) <= PAGE_SIZE, "cmd size");
  zx::vmo vmo;
  zx_status_t status = zx::vmo::create(PAGE_SIZE, 0, &vmo);
  if (status != ZX_OK) {
    return status;
  }

  zx_paddr_t paddr;
  zx::pmt pmt;
  status = bti_.pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE, vmo, 0, PAGE_SIZE, &paddr, 1, &pmt);
  if (status != ZX_OK) {
    return status;
  }

  fbl::AutoLock lock(&pipes_lock_);
  int32_t id = next_pipe_id_++;
  ZX_DEBUG_ASSERT(pipes_.count(id) == 0);
  pipes_[id] = std::make_unique<Pipe>(paddr, std::move(pmt), zx::event());

  *out_vmo = std::move(vmo);
  *out_id = id;
  return ZX_OK;
}

zx_status_t PipeDevice::SetEvent(int32_t id, zx::event pipe_event) {
  TRACE_DURATION("gfx", "PipeDevice::SetEvent");

  fbl::AutoLock lock(&pipes_lock_);

  ZX_DEBUG_ASSERT(pipes_.count(id) == 1);
  ZX_DEBUG_ASSERT(pipe_event.is_valid());

  zx_signals_t kSignals = fuchsia_hardware_goldfish::wire::kSignalReadable |
                          fuchsia_hardware_goldfish::wire::kSignalWritable;

  zx_signals_t observed = 0u;
  // If old pipe event exists, transfer observed signal to new pipe event.
  if (pipes_[id]->pipe_event.is_valid()) {
    zx_status_t status =
        pipes_[id]->pipe_event.wait_one(kSignals, zx::time::infinite_past(), &observed);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: failed to transfer observed signals: %d", kTag, status);
      return status;
    }
  }

  pipes_[id]->pipe_event = std::move(pipe_event);
  zx_status_t status = pipes_[id]->pipe_event.signal(kSignals, observed & kSignals);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to signal event: %d", kTag, status);
    return status;
  }
  return ZX_OK;
}

void PipeDevice::Destroy(int32_t id) {
  TRACE_DURATION("gfx", "PipeDevice::Destroy");

  fbl::AutoLock lock(&pipes_lock_);
  ZX_DEBUG_ASSERT(pipes_.count(id) == 1);
  pipes_.erase(id);
}

void PipeDevice::Open(int32_t id) {
  TRACE_DURATION("gfx", "PipeDevice::Open");

  zx_paddr_t paddr;
  {
    fbl::AutoLock lock(&pipes_lock_);
    ZX_DEBUG_ASSERT(pipes_.count(id) == 1);
    paddr = pipes_[id]->paddr;
  }

  fbl::AutoLock lock(&mmio_lock_);
  CommandBuffers* buffers = static_cast<CommandBuffers*>(io_buffer_.virt());
  buffers->open_command_buffer.pa_command_buffer = paddr;
  buffers->open_command_buffer.rw_params_max_count = MAX_BUFFERS_PER_COMMAND;
  mmio_->Write32(id, PIPE_V2_REG_CMD);
}

void PipeDevice::Exec(int32_t id) {
  TRACE_DURATION("gfx", "PipeDevice::Exec", "id", id);

  fbl::AutoLock lock(&mmio_lock_);
  mmio_->Write32(id, PIPE_V2_REG_CMD);
}

zx_status_t PipeDevice::GetBti(zx::bti* out_bti) {
  TRACE_DURATION("gfx", "PipeDevice::GetBti");

  return bti_.duplicate(ZX_RIGHT_SAME_RIGHTS, out_bti);
}

zx_status_t PipeDevice::ConnectSysmem(zx::channel connection) {
  TRACE_DURATION("gfx", "PipeDevice::ConnectSysmem");

  auto result =
      sysmem_->ConnectServer(fidl::ServerEnd<fuchsia_sysmem::Allocator>(std::move(connection)));
  return result.status();
}

zx_status_t PipeDevice::RegisterSysmemHeap(uint64_t heap, zx::channel connection) {
  TRACE_DURATION("gfx", "PipeDevice::RegisterSysmemHeap");

  auto result =
      sysmem_->RegisterHeap(heap, fidl::ClientEnd<fuchsia_sysmem2::Heap>(std::move(connection)));
  return result.status();
}

int PipeDevice::IrqHandler() {
  while (true) {
    zx_status_t status = irq_.wait(nullptr);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: irq.wait() got %d", kTag, status);
      break;
    }

    uint32_t count;
    {
      fbl::AutoLock lock(&mmio_lock_);
      count = mmio_->Read32(PIPE_V2_REG_GET_SIGNALLED);
    }
    if (count > MAX_SIGNALLED_PIPES) {
      count = MAX_SIGNALLED_PIPES;
    }
    if (count) {
      TRACE_DURATION("gfx", "PipeDevice::IrqHandler::Signal", "count", count);

      fbl::AutoLock lock(&pipes_lock_);

      auto buffers = static_cast<CommandBuffers*>(io_buffer_.virt());
      for (uint32_t i = 0; i < count; ++i) {
        auto it = pipes_.find(buffers->signal_buffers[i].id);
        if (it != pipes_.end()) {
          it->second->SignalEvent(buffers->signal_buffers[i].flags);
        }
      }
    }
  }

  return 0;
}

zx_status_t PipeDevice::ConnectToSysmem() {
  zx::result client =
      DdkConnectFragmentFidlProtocol<fuchsia_hardware_sysmem::Service::Sysmem>("sysmem-fidl");
  if (client.is_error()) {
    return client.status_value();
  }
  sysmem_ = fidl::WireSyncClient(std::move(*client));
  return ZX_OK;
}

PipeDevice::Pipe::Pipe(zx_paddr_t paddr, zx::pmt pmt, zx::event pipe_event)
    : paddr(paddr), pmt(std::move(pmt)), pipe_event(std::move(pipe_event)) {}

PipeDevice::Pipe::~Pipe() {
  ZX_DEBUG_ASSERT(pmt.is_valid());
  pmt.unpin();
}

void PipeDevice::Pipe::SignalEvent(uint32_t flags) const {
  if (!pipe_event.is_valid()) {
    return;
  }

  zx_signals_t state_set = 0;
  if (flags & static_cast<int32_t>(fuchsia_hardware_goldfish_pipe::PipeWakeFlag::kClosed)) {
    state_set |= fuchsia_hardware_goldfish::wire::kSignalHangup;
  }
  if (flags & static_cast<int32_t>(fuchsia_hardware_goldfish_pipe::PipeWakeFlag::kRead)) {
    state_set |= fuchsia_hardware_goldfish::wire::kSignalReadable;
  }
  if (flags & static_cast<int32_t>(fuchsia_hardware_goldfish_pipe::PipeWakeFlag::kWrite)) {
    state_set |= fuchsia_hardware_goldfish::wire::kSignalWritable;
  }

  zx_status_t status = pipe_event.signal(/*clear_mask=*/0u, state_set);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_signal_object failed: %d", kTag, status);
  }
}

PipeChildDevice::PipeChildDevice(PipeDevice* parent, async_dispatcher_t* dispatcher)
    : PipeChildDeviceType(parent->zxdev()),
      parent_(parent),
      dispatcher_(dispatcher),
      outgoing_(dispatcher) {
  ZX_DEBUG_ASSERT(parent_);
}

zx_status_t PipeChildDevice::Bind(cpp20::span<const zx_device_prop_t> props, const char* dev_name) {
  zx::result result = outgoing_.AddService<fuchsia_hardware_goldfish_pipe::Service>(
      fuchsia_hardware_goldfish_pipe::Service::InstanceHandler({
          .device = pipe_bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure),
      }));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to add service the outgoing directory");
    return result.status_value();
  }

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }

  result = outgoing_.Serve(std::move(endpoints->server));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to service the outgoing directory");
    return result.status_value();
  }

  std::array offers = {
      fuchsia_hardware_goldfish_pipe::Service::Name,
  };

  zx_status_t status = DdkAdd(ddk::DeviceAddArgs(dev_name)
                                  .set_props(props)
                                  .set_fidl_service_offers(offers)
                                  .set_outgoing_dir(endpoints->client.TakeChannel())
                                  .set_proto_id(ZX_PROTOCOL_GOLDFISH_PIPE));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: create %s device failed: %d", kTag, dev_name, status);
    return status;
  }
  return ZX_OK;
}

void PipeChildDevice::OpenSession(OpenSessionRequestView request,
                                  OpenSessionCompleter::Sync& completer) {
  std::unique_ptr instance = std::make_unique<Instance>(parent_, dispatcher_);

  bindings_.AddBinding(fdf::Dispatcher::GetCurrent()->async_dispatcher(),
                       std::move(request->session), instance.get(),
                       [instance = std::move(instance)](Instance*, fidl::UnbindInfo info) {});
}

void PipeChildDevice::DdkUnbind(ddk::UnbindTxn txn) {
  ZX_ASSERT(!unbind_txn_.has_value());
  ddk::UnbindTxn& unbind_txn = unbind_txn_.emplace(std::move(txn));
  auto cleanup = fit::bind_member(&unbind_txn, &ddk::UnbindTxn::Reply);
  bindings_.set_empty_set_handler(cleanup);
  if (!bindings_.CloseAll(ZX_OK)) {
    // Binding set was already empty.
    cleanup();
  }
}

void PipeChildDevice::DdkRelease() { delete this; }

void PipeChildDevice::Create(CreateCompleter::Sync& completer) {
  int32_t id;
  zx::vmo vmo;
  zx_status_t status = parent_->Create(&id, &vmo);
  if (status == ZX_OK) {
    completer.ReplySuccess(id, std::move(vmo));
  } else {
    completer.Close(status);
  }
}

void PipeChildDevice::SetEvent(SetEventRequestView request, SetEventCompleter::Sync& completer) {
  zx_status_t status = parent_->SetEvent(request->id, std::move(request->pipe_event));
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.Close(status);
  }
}

void PipeChildDevice::Destroy(DestroyRequestView request, DestroyCompleter::Sync& completer) {
  parent_->Destroy(request->id);
  completer.Reply();
}

void PipeChildDevice::Open(OpenRequestView request, OpenCompleter::Sync& completer) {
  parent_->Open(request->id);
  completer.Reply();
}

void PipeChildDevice::Exec(ExecRequestView request, ExecCompleter::Sync& completer) {
  parent_->Exec(request->id);
  completer.Reply();
}

void PipeChildDevice::GetBti(GetBtiCompleter::Sync& completer) {
  zx::bti bti;
  zx_status_t status = parent_->GetBti(&bti);
  if (status == ZX_OK) {
    completer.ReplySuccess(std::move(bti));
  } else {
    completer.Close(status);
  }
}

void PipeChildDevice::ConnectSysmem(ConnectSysmemRequestView request,
                                    ConnectSysmemCompleter::Sync& completer) {
  zx_status_t status = parent_->ConnectSysmem(std::move(request->connection));
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.Close(status);
  }
}

void PipeChildDevice::RegisterSysmemHeap(RegisterSysmemHeapRequestView request,
                                         RegisterSysmemHeapCompleter::Sync& completer) {
  zx_status_t status = parent_->RegisterSysmemHeap(request->heap, std::move(request->connection));
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.Close(status);
  }
}

}  // namespace goldfish

static constexpr zx_driver_ops_t goldfish_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = goldfish::PipeDevice::Create;
  return ops;
}();

ZIRCON_DRIVER(goldfish, goldfish_driver_ops, "zircon", "0.1");
