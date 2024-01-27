// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "arch/x86/system_topology.h"

#include <lib/acpi_lite/testing/test_data.h>
#include <lib/arch/testing/x86/fake-cpuid.h>
#include <lib/system-topology.h>
#include <lib/unittest/unittest.h>

using system_topology::Graph;

namespace {

// Uses test data from a HP z840, dual socket Xeon E2690v4.
bool test_cpus_z840() {
  BEGIN_TEST;

  arch::testing::FakeCpuidIo io(arch::testing::X86Microprocessor::kIntelXeonE5_2690_V4);
  fbl::Vector<zbi_topology_node_t> flat_topology;
  auto status = x86::GenerateFlatTopology(io, acpi_lite::testing::Z840AcpiParser(), &flat_topology);
  ASSERT_EQ(ZX_OK, status);

  int numa_count = 0;
  int die_count = 0;
  int cache_count = 0;
  int core_count = 0;
  int thread_count = 0;
  int last_numa = -1;
  int last_die = -1;
  int last_cache = -1;
  for (int i = 0; i < (int)flat_topology.size(); i++) {
    const auto& node = flat_topology[i];
    switch (node.entity.discriminant) {
      case ZBI_TOPOLOGY_ENTITY_NUMA_REGION:
        last_numa = i;
        numa_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_DIE:
        EXPECT_EQ(last_numa, node.parent_index);
        last_die = i;
        die_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_CACHE:
        EXPECT_EQ(last_die, node.parent_index);
        last_cache = i;
        cache_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_PROCESSOR:
        EXPECT_EQ(last_cache, node.parent_index);
        core_count++;
        thread_count += node.entity.processor.logical_id_count;
        break;
    }
  }

  // We expect two numa regions, two dies, two caches, and 28 cores.
  EXPECT_EQ(2u + 2u + 2u + 28u, flat_topology.size());
  EXPECT_EQ(2, numa_count);
  EXPECT_EQ(2, die_count);
  EXPECT_EQ(2, cache_count);
  EXPECT_EQ(28, core_count);
  EXPECT_EQ(56, thread_count);

  // Ensure the format can be parsed and validated by the system topology library.
  Graph graph;
  EXPECT_EQ(ZX_OK, Graph::Initialize(&graph, flat_topology.data(), flat_topology.size()));

  END_TEST;
}

// Uses test data from a Threadripper 2970wx system with x399 chipset.
bool test_cpus_2970wx_x399() {
  BEGIN_TEST;

  arch::testing::FakeCpuidIo io(arch::testing::X86Microprocessor::kAmdRyzenThreadripper2970wx);
  fbl::Vector<zbi_topology_node_t> flat_topology;
  auto status =
      x86::GenerateFlatTopology(io, acpi_lite::testing::Sys2970wxAcpiParser(), &flat_topology);
  ASSERT_EQ(ZX_OK, status);

  int numa_count = 0;
  int die_count = 0;
  int core_count = 0;
  int cache_count = 0;
  int thread_count = 0;
  int last_numa = -1;
  int last_die = -1;
  int last_cache = -1;
  for (int i = 0; i < (int)flat_topology.size(); i++) {
    const auto& node = flat_topology[i];
    switch (node.entity.discriminant) {
      case ZBI_TOPOLOGY_ENTITY_NUMA_REGION:
        last_numa = i;
        numa_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_DIE:
        EXPECT_EQ(last_numa, node.parent_index);
        last_die = i;
        die_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_CACHE:
        EXPECT_EQ(last_die, node.parent_index);
        last_cache = i;
        cache_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_PROCESSOR:
        EXPECT_EQ(last_cache, node.parent_index);
        core_count++;
        thread_count += node.entity.processor.logical_id_count;
        break;
    }
  }

  EXPECT_EQ(4, numa_count);
  EXPECT_EQ(4, die_count);
  EXPECT_EQ(8, cache_count);
  EXPECT_EQ(24, core_count);
  EXPECT_EQ(48, thread_count);

  // Ensure the format can be parsed and validated by the system topology library.
  Graph graph;
  EXPECT_EQ(ZX_OK, Graph::Initialize(&graph, flat_topology.data(), flat_topology.size()));

  END_TEST;
}

// Uses bad data to trigger the fallback topology, (everything flat).
bool test_cpus_fallback() {
  BEGIN_TEST;

  // With an 'empty' CPUID data set (representing a pathological case) we would
  // expect enumeration to fall back to maximally flat description of one
  // thread to one core to one package, multiplied by the number of processors
  // enumerated by ACPI.
  arch::testing::FakeCpuidIo io;
  fbl::Vector<zbi_topology_node_t> flat_topology;
  auto status =
      x86::GenerateFlatTopology(io, acpi_lite::testing::PixelbookEveAcpiParser(), &flat_topology);
  ASSERT_EQ(ZX_OK, status);

  int numa_count = 0;
  int die_count = 0;
  int core_count = 0;
  int cache_count = 0;
  int thread_count = 0;
  for (int i = 0; i < (int)flat_topology.size(); i++) {
    const auto& node = flat_topology[i];
    switch (node.entity.discriminant) {
      case ZBI_TOPOLOGY_ENTITY_NUMA_REGION:
        numa_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_DIE:
        die_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_CACHE:
        cache_count++;
        break;
      case ZBI_TOPOLOGY_ENTITY_PROCESSOR:
        core_count++;
        thread_count += node.entity.processor.logical_id_count;
        break;
    }
  }

  EXPECT_EQ(0, numa_count);
  EXPECT_EQ(4, die_count);
  EXPECT_EQ(0, cache_count);
  EXPECT_EQ(4, core_count);
  EXPECT_EQ(4, thread_count);

  // Ensure the format can be parsed and validated by the system topology library.
  Graph graph;
  EXPECT_EQ(ZX_OK, Graph::Initialize(&graph, flat_topology.data(), flat_topology.size()));

  END_TEST;
}

}  // namespace

UNITTEST_START_TESTCASE(x86_topology_tests)
UNITTEST("Enumerate cpus using data from HP z840.", test_cpus_z840)
UNITTEST("Enumerate cpus using data from ThreadRipper 2970wx/X399.", test_cpus_2970wx_x399)
UNITTEST("Enumerate cpus using data triggering fallback.", test_cpus_fallback)
UNITTEST_END_TESTCASE(x86_topology_tests, "x86_topology",
                      "Test determining x86 topology through CPUID/APIC.")
