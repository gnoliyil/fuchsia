// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gather_memory.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include "harvester.h"
#include "sample_bundle.h"

namespace harvester {

void AddGlobalMemorySamples(SampleBundle* samples, zx_handle_t info_resource) {
  zx_info_kmem_stats_t stats;
  zx_status_t err = zx_object_get_info(info_resource, ZX_INFO_KMEM_STATS,
                                       &stats, sizeof(stats),
                                       /*actual=*/nullptr, /*avail=*/nullptr);
  if (err != ZX_OK) {
    FX_LOGS(ERROR) << "ZX_INFO_KMEM_STATS error " << zx_status_get_string(err);
    return;
  }

  FX_VLOGS(2) << "free memory total " << stats.free_bytes << ", heap "
              << stats.free_heap_bytes << ", vmo " << stats.vmo_bytes
              << ", mmu " << stats.mmu_overhead_bytes << ", ipc "
              << stats.ipc_bytes;

  const std::string DEVICE_FREE = "memory:device_free_bytes";

  const std::string KERNEL_TOTAL = "memory:kernel_total_bytes";
  const std::string KERNEL_FREE = "memory:kernel_free_bytes";
  const std::string KERNEL_OTHER = "memory:kernel_other_bytes";

  const std::string VMO = "memory:vmo_bytes";
  const std::string MMU_OVERHEAD = "memory:mmu_overhead_bytes";
  const std::string IPC = "memory:ipc_bytes";
  const std::string OTHER = "memory:device_other_bytes";

  // Memory for the entire machine.
  // Note: stats.total_bytes is recorded by InitialData().
  samples->AddIntSample(DEVICE_FREE, stats.free_bytes);
  // Memory in the kernel.
  samples->AddIntSample(KERNEL_TOTAL, stats.total_heap_bytes);
  samples->AddIntSample(KERNEL_FREE, stats.free_heap_bytes);
  samples->AddIntSample(KERNEL_OTHER, stats.wired_bytes);
  // Categorized memory.
  samples->AddIntSample(MMU_OVERHEAD, stats.mmu_overhead_bytes);
  samples->AddIntSample(VMO, stats.vmo_bytes);
  samples->AddIntSample(IPC, stats.ipc_bytes);
  samples->AddIntSample(OTHER, stats.other_bytes);
}

void GatherMemory::GatherDeviceProperties() {
  const std::string DEVICE_TOTAL = "memory:device_total_bytes";
  zx_info_kmem_stats_t stats;
  zx_status_t err = zx_object_get_info(InfoResource(), ZX_INFO_KMEM_STATS,
                                       &stats, sizeof(stats),
                                       /*actual=*/nullptr, /*avail=*/nullptr);
  if (err != ZX_OK) {
    FX_LOGS(ERROR) << ZxErrorString("ZX_INFO_KMEM_STATS", err);
    return;
  }
  SampleList list;
  list.emplace_back(DEVICE_TOTAL, stats.total_bytes);
  DockyardProxyStatus status = Dockyard().SendSampleList(list);
  if (status != DockyardProxyStatus::OK) {
    FX_LOGS(ERROR) << DockyardErrorString("SendSampleList", status)
                   << " The total memory value will be missing";
  }
}

void GatherMemory::Gather() {
  SampleBundle samples;
  AddGlobalMemorySamples(&samples, InfoResource());
  samples.Upload(DockyardPtr());
}

}  // namespace harvester
