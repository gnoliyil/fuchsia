// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/block/drivers/nvme/nvme.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/device-protocol/pci.h>
#include <lib/fit/defer.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/sync/completion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <hwreg/bitfields.h>

#include "src/devices/block/drivers/nvme/commands/features.h"
#include "src/devices/block/drivers/nvme/commands/identify.h"
#include "src/devices/block/drivers/nvme/commands/queue.h"
#include "src/devices/block/drivers/nvme/namespace.h"
#include "src/devices/block/drivers/nvme/nvme_bind.h"
#include "src/devices/block/drivers/nvme/registers.h"

namespace nvme {

// For safety - so that the driver doesn't draw too many resources.
constexpr size_t kMaxNamespacesToBind = 4;

// c.f. NVMe Base Specification 2.0, section 3.1.3.8 "AQA - Admin Queue Attributes"
constexpr size_t kAdminQueueMaxEntries = 4096;

int Nvme::IrqLoop() {
  while (true) {
    zx_status_t status = zx_interrupt_wait(irqh_, nullptr);
    if (status != ZX_OK) {
      zxlogf(ERROR, "irq wait failed: %s", zx_status_get_string(status));
      break;
    }

    Completion* admin_completion;
    if (admin_queue_->CheckForNewCompletion(&admin_completion) != ZX_ERR_SHOULD_WAIT) {
      admin_result_ = *admin_completion;
      sync_completion_signal(&admin_signal_);
      admin_queue_->RingCompletionDb();
    }

    // TODO(fxbug.dev/102133): Consider using interrupt vector (dedicated interrupt per
    // namespace/queue).
    for (Namespace* ns : namespaces_) {
      ns->SignalIo();
    }
  }
  return 0;
}

zx_status_t Nvme::DoAdminCommandSync(Submission& submission,
                                     std::optional<zx::unowned_vmo> admin_data) {
  zx_status_t status;
  fbl::AutoLock lock(&admin_lock_);
  sync_completion_reset(&admin_signal_);

  uint64_t data_size = 0;
  if (admin_data.has_value()) {
    status = admin_data.value()->get_size(&data_size);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to get size of vmo: %s", zx_status_get_string(status));
      return status;
    }
  }
  status = admin_queue_->Submit(submission, admin_data, 0, data_size, nullptr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to submit admin command: %s", zx_status_get_string(status));
    return status;
  }

  status = sync_completion_wait(&admin_signal_, ZX_SEC(1));
  if (status != ZX_OK) {
    zxlogf(ERROR, "Timed out waiting for admin command: %s", zx_status_get_string(status));
    return status;
  }

  if (admin_result_.status_code_type() == StatusCodeType::kGeneric &&
      admin_result_.status_code() == 0) {
    zxlogf(TRACE, "Completed admin command OK.");
  } else {
    zxlogf(ERROR, "Completed admin command ERROR: status type=%01x, status=%02x",
           admin_result_.status_code_type(), admin_result_.status_code());
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

void Nvme::DdkRelease() {
  zxlogf(DEBUG, "Releasing driver.");
  if (mmio_->get_vmo() != ZX_HANDLE_INVALID) {
    pci_set_bus_mastering(&pci_, false);
    zx_handle_close(bti_.get());
    // TODO: risks a handle use-after-close, will be resolved by IRQ api
    // changes coming soon
    zx_handle_close(irqh_);
  }
  if (irq_thread_started_) {
    int unused;
    thrd_join(irq_thread_, &unused);
  }

  delete this;
}

static zx_status_t WaitForReset(bool desired_ready_state, fdf::MmioBuffer* mmio) {
  constexpr int kResetWaitMs = 5000;
  int ms_remaining = kResetWaitMs;
  while (ControllerStatusReg::Get().ReadFrom(mmio).ready() != desired_ready_state) {
    if (ms_remaining-- == 0) {
      zxlogf(ERROR, "Timed out waiting for controller ready state %u: ", desired_ready_state);
      return ZX_ERR_TIMED_OUT;
    }
    zx_nanosleep(zx_deadline_after(ZX_MSEC(1)));
  }
  zxlogf(DEBUG, "Controller reached ready state %u (took %u ms).", desired_ready_state,
         kResetWaitMs - ms_remaining);
  return ZX_OK;
}

static zx_status_t CheckMinMaxSize(const std::string& name, size_t our_size, size_t min_size,
                                   size_t max_size) {
  if (our_size < min_size) {
    zxlogf(ERROR, "%s size is too small (ours: %zu, min: %zu).", name.c_str(), our_size, min_size);
    return ZX_ERR_NOT_SUPPORTED;
  }
  if (our_size > max_size) {
    zxlogf(ERROR, "%s size is too large (ours: %zu, max: %zu).", name.c_str(), our_size, max_size);
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

void Nvme::DdkInit(ddk::InitTxn txn) {
  // The drive initialization has numerous error conditions. Wrap the initialization here to ensure
  // we always call txn.Reply() in any outcome.
  zx_status_t status = Init();
  if (status != ZX_OK) {
    zxlogf(ERROR, "Driver initialization failed: %s", zx_status_get_string(status));
  }
  txn.Reply(status);
}

zx_status_t Nvme::Init() {
  caps_ = CapabilityReg::Get().ReadFrom(mmio_.get());
  version_ = VersionReg::Get().ReadFrom(mmio_.get());

  zxlogf(INFO, "Version %d.%d.%d", version_.major(), version_.minor(), version_.tertiary());
  zxlogf(DEBUG, "Memory page size: (MPSMIN) %u bytes, (MPSMAX) %u bytes",
         caps_.memory_page_size_min_bytes(), caps_.memory_page_size_max_bytes());
  zxlogf(DEBUG, "Doorbell stride (DSTRD): %u bytes", caps_.doorbell_stride_bytes());
  zxlogf(DEBUG, "Timeout (TO): %u ms", caps_.timeout_ms());
  zxlogf(DEBUG, "Boot partition support (BPS): %c", caps_.boot_partition_support() ? 'Y' : 'N');
  zxlogf(DEBUG, "Supports NVM command set (CSS:NVM): %c",
         caps_.nvm_command_set_support() ? 'Y' : 'N');
  zxlogf(DEBUG, "NVM subsystem reset supported (NSSRS): %c",
         caps_.nvm_subsystem_reset_supported() ? 'Y' : 'N');
  zxlogf(DEBUG, "Weighted round robin supported (AMS:WRR): %c",
         caps_.weighted_round_robin_arbitration_supported() ? 'Y' : 'N');
  zxlogf(DEBUG, "Vendor specific arbitration supported (AMS:VS): %c",
         caps_.vendor_specific_arbitration_supported() ? 'Y' : 'N');
  zxlogf(DEBUG, "Contiguous queues required (CQR): %c",
         caps_.contiguous_queues_required() ? 'Y' : 'N');
  zxlogf(DEBUG, "Maximum queue entries supported (MQES): %u", caps_.max_queue_entries());

  const size_t kPageSize = zx_system_get_page_size();
  zx_status_t status = CheckMinMaxSize("System page", kPageSize, caps_.memory_page_size_min_bytes(),
                                       caps_.memory_page_size_max_bytes());
  if (status != ZX_OK) {
    return status;
  }

  if (ControllerStatusReg::Get().ReadFrom(&*mmio_).ready()) {
    zxlogf(DEBUG, "Controller is already enabled. Resetting it.");
    ControllerConfigReg::Get().ReadFrom(&*mmio_).set_enabled(0).WriteTo(&*mmio_);
    status = WaitForReset(/*desired_ready_state=*/false, &*mmio_);
    if (status != ZX_OK) {
      return status;
    }
  }

  // Set up admin submission and completion queues.
  auto admin_queue = QueuePair::Create(bti_.borrow(), 0, kAdminQueueMaxEntries, caps_, *mmio_,
                                       /*prealloc_prp=*/false);
  if (admin_queue.is_error()) {
    zxlogf(ERROR, "Failed to set up admin queue: %s", admin_queue.status_string());
    return admin_queue.status_value();
  }
  admin_queue_ = std::move(*admin_queue);

  // Configure the admin queue.
  AdminQueueAttributesReg::Get()
      .ReadFrom(&*mmio_)
      .set_completion_queue_size(admin_queue_->completion().entry_count() - 1)
      .set_submission_queue_size(admin_queue_->submission().entry_count() - 1)
      .WriteTo(&*mmio_);

  AdminQueueAddressReg::CompletionQueue()
      .ReadFrom(&*mmio_)
      .set_addr(admin_queue_->completion().GetDeviceAddress())
      .WriteTo(&*mmio_);
  AdminQueueAddressReg::SubmissionQueue()
      .ReadFrom(&*mmio_)
      .set_addr(admin_queue_->submission().GetDeviceAddress())
      .WriteTo(&*mmio_);

  zxlogf(DEBUG, "Enabling controller.");
  ControllerConfigReg::Get()
      .ReadFrom(&*mmio_)
      .set_controller_ready_independent_of_media(0)
      // Queue entry sizes are powers of two.
      .set_io_completion_queue_entry_size(__builtin_ctzl(sizeof(Completion)))
      .set_io_submission_queue_entry_size(__builtin_ctzl(sizeof(Submission)))
      .set_arbitration_mechanism(ControllerConfigReg::ArbitrationMechanism::kRoundRobin)
      // We know that page size is always at least 4096 (required by spec), and we check
      // that zx_system_get_page_size is supported by the controller above.
      .set_memory_page_size(__builtin_ctzl(kPageSize) - 12)
      .set_io_command_set(ControllerConfigReg::CommandSet::kNvm)
      .set_enabled(1)
      .WriteTo(&*mmio_);

  status = WaitForReset(/*desired_ready_state=*/true, &*mmio_);
  if (status != ZX_OK) {
    return status;
  }

  // Timeout may have changed, so double check it.
  caps_.ReadFrom(&*mmio_);

  // Set up IO submission and completion queues.
  auto io_queue = QueuePair::Create(bti_.borrow(), 1, caps_.max_queue_entries(), caps_, *mmio_,
                                    /*prealloc_prp=*/true);
  if (io_queue.is_error()) {
    zxlogf(ERROR, "Failed to set up io queue: %s", io_queue.status_string());
    return io_queue.status_value();
  }
  io_queue_ = std::move(*io_queue);
  zxlogf(DEBUG, "Using IO submission queue size of %lu, IO completion queue size of %lu.",
         io_queue_->submission().entry_count(), io_queue_->completion().entry_count());

  // Spin up IRQ thread so we can start issuing admin commands to the device.
  int thrd_status = thrd_create_with_name(&irq_thread_, IrqThread, this, "nvme-irq-thread");
  if (thrd_status) {
    zxlogf(ERROR, " Failed to create IRQ thread: %d", thrd_status);
    return ZX_ERR_INTERNAL;
  }
  irq_thread_started_ = true;

  zx::vmo admin_data;
  status = zx::vmo::create(kPageSize, 0, &admin_data);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to create vmo: %s", zx_status_get_string(status));
    return status;
  }

  fzl::VmoMapper mapper;
  status = mapper.Map(admin_data);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to map vmo: %s", zx_status_get_string(status));
    return status;
  }

  IdentifySubmission identify_controller;
  identify_controller.set_structure(IdentifySubmission::IdentifyCns::kIdentifyController);
  status = DoAdminCommandSync(identify_controller, admin_data.borrow());
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to identify controller: %s", zx_status_get_string(status));
    return status;
  }

  auto identify = static_cast<IdentifyController*>(mapper.start());
  zxlogf(INFO, "Model number:  '%s'",
         std::string(identify->model_number, sizeof(identify->model_number)).c_str());
  zxlogf(INFO, "Serial number: '%s'",
         std::string(identify->serial_number, sizeof(identify->serial_number)).c_str());
  zxlogf(INFO, "Firmware rev.: '%s'",
         std::string(identify->firmware_rev, sizeof(identify->firmware_rev)).c_str());

  status = CheckMinMaxSize("Submission queue entry", sizeof(Submission),
                           identify->minimum_sq_entry_size(), identify->maximum_sq_entry_size());
  if (status != ZX_OK) {
    return status;
  }
  status = CheckMinMaxSize("Completion queue entry", sizeof(Completion),
                           identify->minimum_cq_entry_size(), identify->maximum_cq_entry_size());
  if (status != ZX_OK) {
    return status;
  }

  zxlogf(DEBUG, "Maximum outstanding commands: %u", identify->max_cmd);
  zxlogf(DEBUG, "Number of namespaces: %u", identify->num_namespaces);
  if (identify->max_allowed_namespaces != 0) {
    zxlogf(DEBUG, "Maximum number of allowed namespaces: %u", identify->max_allowed_namespaces);
  }
  zxlogf(DEBUG, "SGL support: %c (0x%08x)", (identify->sgl_support & 3) ? 'Y' : 'N',
         identify->sgl_support);
  if (identify->max_data_transfer == 0) {
    max_data_transfer_bytes_ = 0;
  } else {
    max_data_transfer_bytes_ =
        caps_.memory_page_size_min_bytes() * (1 << identify->max_data_transfer);
    zxlogf(DEBUG, "Maximum data transfer size: %u bytes", max_data_transfer_bytes_);
  }

  zxlogf(DEBUG, "sanitize caps: %u", identify->sanicap & 3);
  zxlogf(DEBUG, "abort command limit (ACL): %u", identify->acl + 1);
  zxlogf(DEBUG, "asynch event req limit (AERL): %u", identify->aerl + 1);
  zxlogf(DEBUG, "firmware: slots: %u reset: %c slot1ro: %c", (identify->frmw >> 1) & 3,
         (identify->frmw & (1 << 4)) ? 'N' : 'Y', (identify->frmw & 1) ? 'Y' : 'N');
  zxlogf(DEBUG, "host buffer: min/preferred: %u/%u pages", identify->hmmin, identify->hmpre);
  zxlogf(DEBUG, "capacity: total/unalloc: %zu/%zu", identify->tnvmcap[0], identify->unvmcap[0]);

  if (identify->vwc & 1) {
    volatile_write_cache_ = true;
  }
  atomic_write_unit_normal_ = identify->atomic_write_unit_normal + 1;
  atomic_write_unit_power_fail_ = identify->atomic_write_unit_power_fail + 1;
  zxlogf(DEBUG, "volatile write cache (VWC): %s", volatile_write_cache_ ? "Y" : "N");
  zxlogf(DEBUG, "atomic write unit (AWUN)/(AWUPF): %u/%u blks", atomic_write_unit_normal_,
         atomic_write_unit_power_fail_);

#define LOG_NVME_FEATURE(name)           \
  if (identify->name()) {                \
    zxlogf(DEBUG, "feature: %s", #name); \
  }
  LOG_NVME_FEATURE(doorbell_buffer_config);
  LOG_NVME_FEATURE(virtualization_management);
  LOG_NVME_FEATURE(nvme_mi_send_recv);
  LOG_NVME_FEATURE(directive_send_recv);
  LOG_NVME_FEATURE(device_self_test);
  LOG_NVME_FEATURE(namespace_management);
  LOG_NVME_FEATURE(firmware_download_commit);
  LOG_NVME_FEATURE(format_nvm);
  LOG_NVME_FEATURE(security_send_recv);
  LOG_NVME_FEATURE(timestamp);
  LOG_NVME_FEATURE(reservations);
  LOG_NVME_FEATURE(save_select_nonzero);
  LOG_NVME_FEATURE(write_uncorrectable);
  LOG_NVME_FEATURE(compare);
#undef LOG_NVME_FEATURE

  // Set feature (number of queues) to 1 IO submission queue and 1 IO completion queue.
  SetIoQueueCountSubmission set_queue_count;
  set_queue_count.set_num_submission_queues(1).set_num_completion_queues(1);
  status = DoAdminCommandSync(set_queue_count);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to set feature (number of queues): %s", zx_status_get_string(status));
    return status;
  }
  auto result = static_cast<SetIoQueueCountCompletion*>(&admin_result_);
  if (result->num_submission_queues() < 1) {
    zxlogf(ERROR, "Unexpected IO submission queue count: %u", result->num_submission_queues());
    return ZX_ERR_IO;
  }
  if (result->num_completion_queues() < 1) {
    zxlogf(ERROR, "Unexpected IO completion queue count: %u", result->num_completion_queues());
    return ZX_ERR_IO;
  }

  // Create IO completion queue.
  CreateIoCompletionQueueSubmission create_iocq;
  create_iocq.set_queue_id(io_queue_->completion().id())
      .set_queue_size(io_queue_->completion().entry_count() - 1)
      .set_contiguous(true)
      .set_interrupt_en(true)
      .set_interrupt_vector(0);
  create_iocq.data_pointer[0] = io_queue_->completion().GetDeviceAddress();
  status = DoAdminCommandSync(create_iocq);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to create IO completion queue: %s", zx_status_get_string(status));
    return status;
  }

  // Create IO submission queue.
  CreateIoSubmissionQueueSubmission create_iosq;
  create_iosq.set_queue_id(io_queue_->submission().id())
      .set_queue_size(io_queue_->submission().entry_count() - 1)
      .set_completion_queue_id(io_queue_->completion().id())
      .set_contiguous(true);
  create_iosq.data_pointer[0] = io_queue_->submission().GetDeviceAddress();
  status = DoAdminCommandSync(create_iosq);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to create IO submission queue: %s", zx_status_get_string(status));
    return status;
  }

  // Identify active namespaces.
  IdentifySubmission identify_ns_list;
  identify_ns_list.set_structure(IdentifySubmission::IdentifyCns::kActiveNamespaceList);
  status = DoAdminCommandSync(identify_ns_list, admin_data.borrow());
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to identify active namespace list: %s", zx_status_get_string(status));
    return status;
  }

  // Bind active namespaces.
  auto ns_list = static_cast<IdentifyActiveNamespaces*>(mapper.start());
  for (size_t i = 0; i < std::size(ns_list->nsid) && ns_list->nsid[i] != 0; i++) {
    if (namespaces_.size() >= kMaxNamespacesToBind) {
      zxlogf(WARNING, "Skipping additional namespaces after adding %zu.", namespaces_.size());
      break;
    }
    auto result = Namespace::Bind(this, ns_list->nsid[i]);
    if (result.is_error()) {
      zxlogf(ERROR, "Failed to add namespace %u: %s", ns_list->nsid[i], result.status_string());
      return result.status_value();
    }
    namespaces_.push_back(result.value());
  }

  return ZX_OK;
}

zx_status_t Nvme::AddDevice(zx_device_t* dev) {
  auto cleanup = fit::defer([&] { DdkRelease(); });

  zx_status_t status = device_get_fragment_protocol(dev, "pci", ZX_PROTOCOL_PCI, &pci_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to find PCI fragment: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }

  mmio_buffer_t mmio_buffer;
  status = pci_map_bar_buffer(&pci_, 0u, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_buffer);
  if (status != ZX_OK) {
    zxlogf(ERROR, "cannot map registers: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }
  mmio_ = std::make_unique<fdf::MmioBuffer>(mmio_buffer);

  status = pci_configure_interrupt_mode(&pci_, 1, nullptr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not configure irqs: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }

  status = pci_map_interrupt(&pci_, 0, &irqh_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not map irq: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }

  status = pci_set_bus_mastering(&pci_, true);
  if (status != ZX_OK) {
    zxlogf(ERROR, "cannot enable bus mastering: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_handle_t bti_handle;
  status = pci_get_bti(&pci_, 0, &bti_handle);
  if (status != ZX_OK) {
    zxlogf(ERROR, "cannot obtain bti handle: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }
  bti_ = zx::bti(bti_handle);

  status = DdkAdd(ddk::DeviceAddArgs("nvme"));
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed DdkAdd: %s", zx_status_get_string(status));
    return ZX_ERR_NOT_SUPPORTED;
  }

  cleanup.cancel();
  return ZX_OK;
}

zx_status_t Nvme::Bind(void* ctx, zx_device_t* dev) {
  auto driver = std::make_unique<nvme::Nvme>(dev);
  if (zx_status_t status = driver->AddDevice(dev); status != ZX_OK) {
    return status;
  }

  // The DriverFramework now owns driver.
  [[maybe_unused]] auto placeholder = driver.release();
  return ZX_OK;
}

static zx_driver_ops_t driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = Nvme::Bind,
};

}  // namespace nvme

ZIRCON_DRIVER(nvme, nvme::driver_ops, "zircon", "0.1");
