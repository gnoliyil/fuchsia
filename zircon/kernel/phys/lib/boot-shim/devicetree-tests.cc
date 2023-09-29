// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/boot-shim/devicetree-boot-shim.h>
#include <lib/boot-shim/devicetree.h>
#include <lib/devicetree/devicetree.h>
#include <lib/devicetree/matcher.h>
#include <lib/devicetree/testing/loaded-dtb.h>
#include <lib/fit/defer.h>
#include <lib/memalloc/range.h>
#include <lib/uart/all.h>
#include <lib/uart/ns8250.h>
#include <lib/zbi-format/cpu.h>
#include <lib/zbi-format/driver-config.h>
#include <lib/zbi-format/internal/deprecated-cpu.h>
#include <lib/zbi-format/memory.h>
#include <lib/zbi-format/zbi.h>
#include <lib/zbitl/item.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <type_traits>

#include <zxtest/zxtest.h>

namespace {

using devicetree::testing::LoadDtb;
using devicetree::testing::LoadedDtb;

class ArmDevicetreeTest {
 public:
  static void SetUpTestSuite() {
    auto loaded_dtb = LoadDtb("crosvm-arm.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    crosvm_arm_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("qemu-arm-gic3.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    qemu_arm_gic3_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("qemu-arm-gic2.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    qemu_arm_gic2_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    qemu_arm_gic3_ = std::nullopt;
    qemu_arm_gic2_ = std::nullopt;
    crosvm_arm_ = std::nullopt;
  }

  devicetree::Devicetree qemu_arm_gic3() { return qemu_arm_gic3_->fdt(); }

  devicetree::Devicetree qemu_arm_gic2() { return qemu_arm_gic2_->fdt(); }

  devicetree::Devicetree crosvm_arm() { return crosvm_arm_->fdt(); }

 private:
  static std::optional<LoadedDtb> crosvm_arm_;
  static std::optional<LoadedDtb> qemu_arm_gic3_;
  static std::optional<LoadedDtb> qemu_arm_gic2_;
};

std::optional<LoadedDtb> ArmDevicetreeTest::crosvm_arm_ = std::nullopt;
std::optional<LoadedDtb> ArmDevicetreeTest::qemu_arm_gic3_ = std::nullopt;
std::optional<LoadedDtb> ArmDevicetreeTest::qemu_arm_gic2_ = std::nullopt;

class RiscvDevicetreeTest {
 public:
  static void SetUpTestSuite() {
    auto loaded_dtb = LoadDtb("qemu-riscv.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    qemu_riscv_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("sifive-hifive-unmatched.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    sifive_hifive_unmatched_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("vision-five-2.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    vision_five_2_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    qemu_riscv_ = std::nullopt;
    vision_five_2_ = std::nullopt;
    sifive_hifive_unmatched_ = std::nullopt;
  }

  devicetree::Devicetree qemu_riscv() { return qemu_riscv_->fdt(); }
  devicetree::Devicetree sifive_hifive_unmatched() { return sifive_hifive_unmatched_->fdt(); }
  devicetree::Devicetree vision_five_2() { return vision_five_2_->fdt(); }

 private:
  static std::optional<LoadedDtb> qemu_riscv_;
  static std::optional<LoadedDtb> sifive_hifive_unmatched_;
  static std::optional<LoadedDtb> vision_five_2_;
};

std::optional<LoadedDtb> RiscvDevicetreeTest::qemu_riscv_ = std::nullopt;
std::optional<LoadedDtb> RiscvDevicetreeTest::sifive_hifive_unmatched_ = std::nullopt;
std::optional<LoadedDtb> RiscvDevicetreeTest::vision_five_2_ = std::nullopt;

template <typename... Base>
class TestMixin : public zxtest::Test, public Base... {
 public:
  using Mixin = TestMixin;
  static_assert(sizeof...(Base) > 0);

  static void SetUpTestSuite() { SetUp<Base...>(); }
  static void TearDownTestSuite() { TearDown<Base...>(); }

 private:
  template <typename Head, typename... Rest>
  static void SetUp() {
    Head::SetUpTestSuite();
    if constexpr (sizeof...(Rest) > 0) {
      SetUp<Rest...>();
    }
  }
  template <typename Head, typename... Rest>
  static void TearDown() {
    Head::TearDownTestSuite();
    if constexpr (sizeof...(Rest) > 1) {
      TearDown<Head, Rest...>();
    }
  }
};

class ArmDevicetreePsciItemTest : public zxtest::Test {
 public:
  static void SetUpTestSuite() {
    auto loaded_dtb = LoadDtb("psci-hvc.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    psci_hvc_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("psci-smc.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    psci_smc_dtb_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    psci_hvc_dtb_ = std::nullopt;
    psci_smc_dtb_ = std::nullopt;
  }

  devicetree::Devicetree psci_hvc() { return psci_hvc_dtb_->fdt(); }
  devicetree::Devicetree psci_smc() { return psci_smc_dtb_->fdt(); }

 private:
  static std::optional<LoadedDtb> psci_hvc_dtb_;
  static std::optional<LoadedDtb> psci_smc_dtb_;
};

std::optional<LoadedDtb> ArmDevicetreePsciItemTest::psci_hvc_dtb_ = std::nullopt;
std::optional<LoadedDtb> ArmDevicetreePsciItemTest::psci_smc_dtb_ = std::nullopt;

TEST_F(ArmDevicetreePsciItemTest, ParseSmc) {
  std::array<std::byte, 256> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = psci_smc();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreePsciItem> shim("test", fdt);

  shim.Init();
  EXPECT_TRUE(shim.AppendItems(image).is_ok());

  // Look for a gic 2 driver.
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_ARM_PSCI) {
      present = true;
      ASSERT_EQ(payload.size(), sizeof(zbi_dcfg_arm_psci_driver_t));
      const auto* dcfg = reinterpret_cast<const zbi_dcfg_arm_psci_driver_t*>(payload.data());
      EXPECT_FALSE(dcfg->use_hvc);
      break;
    }
  }
  image.ignore_error();
  ASSERT_TRUE(present, "ZBI Driver for PSCI missing.");
}

TEST_F(ArmDevicetreePsciItemTest, ParseHvc) {
  std::array<std::byte, 256> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = psci_hvc();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreePsciItem> shim("test", fdt);

  shim.Init();
  EXPECT_TRUE(shim.AppendItems(image).is_ok());

  // Look for a gic 2 driver.
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_ARM_PSCI) {
      present = true;
      ASSERT_EQ(payload.size(), sizeof(zbi_dcfg_arm_psci_driver_t));
      const auto* dcfg = reinterpret_cast<const zbi_dcfg_arm_psci_driver_t*>(payload.data());
      EXPECT_TRUE(dcfg->use_hvc);
      break;
    }
  }
  image.ignore_error();
  ASSERT_TRUE(present, "ZBI Driver for PSCI missing.");
}

using ArmDevicetreeGicItemTest = TestMixin<ArmDevicetreeTest>;

TEST_F(ArmDevicetreeGicItemTest, ParseGicV2WithMsi) {
  std::array<std::byte, 256> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_arm_gic2();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreeGicItem> shim("test", fdt);

  shim.Init();
  EXPECT_TRUE(shim.AppendItems(image).is_ok());

  // Look for a gic 2 driver.
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_ARM_GIC_V2) {
      present = true;
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_arm_gic_v2_driver_t));
      auto* dcfg = reinterpret_cast<zbi_dcfg_arm_gic_v2_driver_t*>(payload.data());
      EXPECT_EQ(dcfg->mmio_phys, 0x08000000);
      EXPECT_EQ(dcfg->msi_frame_phys, 0x08020000);
      EXPECT_EQ(dcfg->gicd_offset, 0x00000);
      EXPECT_EQ(dcfg->gicc_offset, 0x10000);
      EXPECT_EQ(dcfg->ipi_base, 0x0);
      EXPECT_TRUE(dcfg->use_msi);
      EXPECT_FALSE(dcfg->optional);
      break;
    }
  }
  image.ignore_error();
  ASSERT_TRUE(present, "ZBI Driver for GIC V2 missing.");
}

// We dont support GicV3 with MSI yet, not reflected in the driver configuration.
TEST_F(ArmDevicetreeGicItemTest, ParseGicV3) {
  std::array<std::byte, 256> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_arm_gic3();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreeGicItem> shim("test", fdt);

  shim.Init();
  EXPECT_TRUE(shim.AppendItems(image).is_ok());

  // Look for a gic 2 driver.
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_ARM_GIC_V3) {
      present = true;
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_arm_gic_v3_driver_t));
      auto* dcfg = reinterpret_cast<zbi_dcfg_arm_gic_v3_driver_t*>(payload.data());
      EXPECT_EQ(dcfg->mmio_phys, 0x08000000);
      EXPECT_EQ(dcfg->gicd_offset, 0x00000);
      EXPECT_EQ(dcfg->gicr_offset, 0xa0000);
      EXPECT_EQ(dcfg->gicr_stride, 0x20000);
      EXPECT_EQ(dcfg->ipi_base, 0x0);
      EXPECT_FALSE(dcfg->optional);
      break;
    }
  }
  image.ignore_error();
  ASSERT_TRUE(present, "ZBI Driver for GIC V3 missing.");
}

class ArmDevicetreeTimerItemTest : public TestMixin<ArmDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    Mixin::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("arm_timer.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    timer_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("arm_timer_no_frequency_override.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    timer_no_frequency_override_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    timer_ = std::nullopt;
    Mixin::TearDownTestSuite();
  }

  devicetree::Devicetree timer() { return timer_->fdt(); }
  devicetree::Devicetree timer_no_frequency_override() {
    return timer_no_frequency_override_->fdt();
  }

 private:
  static std::optional<LoadedDtb> timer_;
  static std::optional<LoadedDtb> timer_no_frequency_override_;
};

std::optional<LoadedDtb> ArmDevicetreeTimerItemTest::timer_ = std::nullopt;
std::optional<LoadedDtb> ArmDevicetreeTimerItemTest::timer_no_frequency_override_ = std::nullopt;

TEST_F(ArmDevicetreeTimerItemTest, ParseTimer) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = timer();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreeTimerItem> shim("test", fdt);

  shim.Init();

  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_ARM_GENERIC_TIMER) {
      ASSERT_GE(payload.size_bytes(), sizeof(zbi_dcfg_arm_generic_timer_driver_t));
      auto* timer_item = reinterpret_cast<zbi_dcfg_arm_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(timer_item->freq_override, 1234);
      EXPECT_EQ(timer_item->irq_phys, 27);
      EXPECT_EQ(timer_item->irq_sphys, 26);
      EXPECT_EQ(timer_item->irq_virt, 28);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(ArmDevicetreeTimerItemTest, ParseTimerNoFrequencyOverride) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = timer_no_frequency_override();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreeTimerItem> shim("test", fdt);

  shim.Init();

  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_ARM_GENERIC_TIMER) {
      ASSERT_GE(payload.size_bytes(), sizeof(zbi_dcfg_arm_generic_timer_driver_t));
      auto* timer_item = reinterpret_cast<zbi_dcfg_arm_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(timer_item->freq_override, 0);
      EXPECT_EQ(timer_item->irq_phys, 27);
      EXPECT_EQ(timer_item->irq_sphys, 26);
      EXPECT_EQ(timer_item->irq_virt, 28);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(ArmDevicetreeTimerItemTest, ParseQemu) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_arm_gic3();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevicetreeTimerItem> shim("test", fdt);

  shim.Init();

  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_ARM_GENERIC_TIMER) {
      ASSERT_GE(payload.size_bytes(), sizeof(zbi_dcfg_arm_generic_timer_driver_t));
      auto* timer_item = reinterpret_cast<zbi_dcfg_arm_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(timer_item->freq_override, 0);
      EXPECT_EQ(timer_item->irq_phys, 30);
      EXPECT_EQ(timer_item->irq_sphys, 29);
      EXPECT_EQ(timer_item->irq_virt, 27);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

class ArmCpuTopologyItemTest : public TestMixin<ArmDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    Mixin::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("cpus_arm.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    cpus_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("cpus_arm_no_cpu_map.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    cpus_no_cpu_map_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("cpus_arm_single_cell.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    cpus_single_cell_dtb_ = std::move(loaded_dtb).value();
  }
  static void TearDownTestSuite() {
    cpus_dtb_ = std::nullopt;
    cpus_no_cpu_map_dtb_ = std::nullopt;
    cpus_single_cell_dtb_ = std::nullopt;
    Mixin::TearDownTestSuite();
  }

  devicetree::Devicetree cpus() { return cpus_dtb_->fdt(); }
  devicetree::Devicetree cpus_single_cell() { return cpus_single_cell_dtb_->fdt(); }
  devicetree::Devicetree cpus_no_cpu_map() { return cpus_no_cpu_map_dtb_->fdt(); }

 private:
  static std::optional<LoadedDtb> cpus_dtb_;
  static std::optional<LoadedDtb> cpus_single_cell_dtb_;
  static std::optional<LoadedDtb> cpus_no_cpu_map_dtb_;
};

std::optional<LoadedDtb> ArmCpuTopologyItemTest::cpus_dtb_ = std::nullopt;
std::optional<LoadedDtb> ArmCpuTopologyItemTest::cpus_single_cell_dtb_ = std::nullopt;
std::optional<LoadedDtb> ArmCpuTopologyItemTest::cpus_no_cpu_map_dtb_ = std::nullopt;

TEST_F(ArmCpuTopologyItemTest, ParseCpus) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = cpus();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 7u);

      // socket0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_SOCKET);

      // cluster0
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[1].entity.cluster.performance_class, 0x7F);

      // cpu@11100000101
      EXPECT_EQ(nodes[2].parent_index, 1);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_3_id, 7);

      // cpu@11100000100
      EXPECT_EQ(nodes[3].parent_index, 1);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_3_id, 7);

      // cluster1
      EXPECT_EQ(nodes[4].parent_index, 0);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[4].entity.cluster.performance_class, 0xFF);

      // cpu@1
      EXPECT_EQ(nodes[5].parent_index, 4);
      EXPECT_EQ(nodes[5].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[5].entity.processor.flags, 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@0
      EXPECT_EQ(nodes[6].parent_index, 4);
      EXPECT_EQ(nodes[6].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[6].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_3_id, 0);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(ArmCpuTopologyItemTest, ParseCpusSingleCell) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = cpus_single_cell();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 7u);

      // socket0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_SOCKET);

      // cluster0
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[1].entity.cluster.performance_class, 0x7F);

      // cpu@101
      EXPECT_EQ(nodes[2].parent_index, 1);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@100
      EXPECT_EQ(nodes[3].parent_index, 1);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cluster1
      EXPECT_EQ(nodes[4].parent_index, 0);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[4].entity.cluster.performance_class, 0xFF);

      // cpu@1
      EXPECT_EQ(nodes[5].parent_index, 4);
      EXPECT_EQ(nodes[5].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[5].entity.processor.flags, 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@0
      EXPECT_EQ(nodes[6].parent_index, 4);
      EXPECT_EQ(nodes[6].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[6].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.arm64.cluster_3_id, 0);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(ArmCpuTopologyItemTest, ParseCpusNoCpuMap) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = cpus_no_cpu_map();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 4u);

      // cpu@0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[0].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[1].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@100
      EXPECT_EQ(nodes[2].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@101
      EXPECT_EQ(nodes[3].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_1_id, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_3_id, 0);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(ArmCpuTopologyItemTest, ParseQemu) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_arm_gic3();
  boot_shim::DevicetreeBootShim<boot_shim::ArmDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 4u);

      // cpu@0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[0].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cpu_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[1].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cpu_id, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@2
      EXPECT_EQ(nodes[2].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cpu_id, 2);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.arm64.cluster_3_id, 0);

      // cpu@3
      EXPECT_EQ(nodes[3].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cpu_id, 3);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_1_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_2_id, 0);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.arm64.cluster_3_id, 0);
    }
  }
  ASSERT_TRUE(present);
}

class ChosenNodeMatcherTest : public TestMixin<ArmDevicetreeTest, RiscvDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    Mixin::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("chosen.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    chosen_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("chosen_unknown_intc.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    chosen_unknown_intc_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("chosen_with_reg_offset.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    chosen_with_reg_offset_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("chosen_with_translation.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    chosen_with_translation_dtb_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    chosen_dtb_ = std::nullopt;
    chosen_unknown_intc_dtb_ = std::nullopt;
    chosen_with_reg_offset_dtb_ = std::nullopt;
    chosen_with_translation_dtb_ = std::nullopt;
    Mixin::TearDownTestSuite();
  }

  devicetree::Devicetree chosen() { return chosen_dtb_->fdt(); }
  devicetree::Devicetree chosen_with_reg_offset() { return chosen_with_reg_offset_dtb_->fdt(); }
  devicetree::Devicetree chosen_unknown_intc() { return chosen_unknown_intc_dtb_->fdt(); }
  devicetree::Devicetree chosen_with_translation() { return chosen_with_translation_dtb_->fdt(); }

 private:
  static std::optional<LoadedDtb> chosen_dtb_;
  static std::optional<LoadedDtb> chosen_with_reg_offset_dtb_;
  static std::optional<LoadedDtb> chosen_unknown_intc_dtb_;
  static std::optional<LoadedDtb> chosen_with_translation_dtb_;
};

std::optional<LoadedDtb> ChosenNodeMatcherTest::chosen_dtb_ = std::nullopt;
std::optional<LoadedDtb> ChosenNodeMatcherTest::chosen_with_reg_offset_dtb_ = std::nullopt;
std::optional<LoadedDtb> ChosenNodeMatcherTest::chosen_unknown_intc_dtb_ = std::nullopt;
std::optional<LoadedDtb> ChosenNodeMatcherTest::chosen_with_translation_dtb_ = std::nullopt;

struct ExpectedChosen {
  uintptr_t ramdisk_start;
  uintptr_t ramdisk_end;
  std::string_view cmdline;
  std::string_view uart_config_name;
  zbi_dcfg_simple_t uart_config;
  std::string_view uart_absolute_path;
};

using AllUartDrivers =
    std::variant<uart::null::Driver, uart::ns8250::LegacyDw8250Driver, uart::pl011::Driver,
                 uart::ns8250::Mmio8Driver, uart::ns8250::Mmio32Driver>;

template <typename ChosenItemType>
void CheckChosenMatcher(const ChosenItemType& matcher, const ExpectedChosen& expected) {
  std::vector<std::unique_ptr<devicetree::Node>> nodes_in_path;
  size_t current = 0;
  while (current < expected.uart_absolute_path.length()) {
    size_t next = current;
    if (next = expected.uart_absolute_path.substr(current).find('/');
        next == std::string_view::npos) {
      next = expected.uart_absolute_path.length() - current;
    }
    nodes_in_path.push_back(
        std::make_unique<devicetree::Node>(expected.uart_absolute_path.substr(current, next)));
    current += next + 1;
  }

  devicetree::NodePath expected_uart_path;
  auto cleanup = fit::defer([&]() {
    while (!expected_uart_path.is_empty()) {
      expected_uart_path.pop_back();
    }
  });
  for (auto& node : nodes_in_path) {
    expected_uart_path.push_back(node.get());
  }

  // Cmdline Check.
  EXPECT_EQ(matcher.cmdline(), expected.cmdline);

  // Ramdisk captured correctly.
  auto ramdisk = matcher.zbi();
  uintptr_t ramdisk_start = reinterpret_cast<uintptr_t>(ramdisk.data());
  EXPECT_EQ(ramdisk_start, expected.ramdisk_start);
  EXPECT_EQ(ramdisk.size(), expected.ramdisk_end - expected.ramdisk_start);

  ASSERT_TRUE(matcher.stdout_path());
  EXPECT_EQ(*matcher.stdout_path(), expected_uart_path);

  // Uart.
  matcher.uart().Visit([&](const auto& driver) {
    using config_t = std::decay_t<decltype(driver.uart().config())>;
    if constexpr (std::is_same_v<config_t, zbi_dcfg_simple_t>) {
      auto& uart = driver.uart();
      EXPECT_EQ(uart.config_name(), expected.uart_config_name, "Actual name %s\n",
                uart.config_name().data());
      EXPECT_EQ(uart.config().mmio_phys, expected.uart_config.mmio_phys);
      // The bootstrap phase does not decode interrupt.
      EXPECT_EQ(uart.config().irq, expected.uart_config.irq);
    } else {
      FAIL("Unexpected driver: %s", fbl::TypeInfo<decltype(driver)>::Name());
    }
  });
}

TEST_F(ChosenNodeMatcherTest, ParseChosen) {
  auto fdt = chosen();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x48000000,
                         .ramdisk_end = 0x58000000,
                         .cmdline = "-foo=bar -bar=baz",
                         .uart_config_name = uart::pl011::Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x9000000,
                                 .irq = 33,
                             },
                         .uart_absolute_path = "/some-interrupt-controller/pl011uart@9000000",
                     });
}

TEST_F(ChosenNodeMatcherTest, ParseChosenWithRegOffset) {
  auto fdt = chosen_with_reg_offset();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x48000000,
                         .ramdisk_end = 0x58000000,
                         .cmdline = "-foo=bar -bar=baz",
                         .uart_config_name = uart::pl011::Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x9000123,
                                 .irq = 33,
                             },
                         .uart_absolute_path = "/some-interrupt-controller/pl011uart@9000000",
                     });
}

TEST_F(ChosenNodeMatcherTest, ParseChosenWithTranslation) {
  auto fdt = chosen_with_translation();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x48000000,
                         .ramdisk_end = 0x58000000,
                         .cmdline = "-foo=bar -bar=baz",
                         .uart_config_name = uart::pl011::Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x9030000,
                                 .irq = 33,
                             },
                         .uart_absolute_path = "/some-interrupt-controller/pl011uart@9000000",
                     });
}

TEST_F(ChosenNodeMatcherTest, ParseChosenWithUnknownInterruptController) {
  auto fdt = chosen_unknown_intc();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));
  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x48000000,
                         .ramdisk_end = 0x58000000,
                         .cmdline = "-foo=bar -bar=baz",
                         .uart_config_name = uart::pl011::Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x9000000,
                                 .irq = 0,
                             },
                         .uart_absolute_path = "/some-interrupt-controller/pl011uart@9000000",
                     });
}

TEST_F(ChosenNodeMatcherTest, CrosVmArm) {
  constexpr std::string_view kCmdline =
      "panic=-1 kernel.experimental.serial_migration=true console.shell=true zircon.autorun.boot=/boot/bin/devicetree-extract";

  auto fdt = crosvm_arm();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x81000000,
                         .ramdisk_end = 0x82bd4e28,
                         .cmdline = kCmdline,
                         .uart_config_name = uart::ns8250::Mmio8Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x3F8,
                                 .irq = 32,
                             },
                         .uart_absolute_path = "/U6_16550A@3f8",
                     });
}

TEST_F(ChosenNodeMatcherTest, ArmQemu) {
  constexpr std::string_view kQemuCmdline =
      "TERM=xterm-256color kernel.entropy-mixin=cd93b8955fc588b1bcde0d691a694b926d53faeca61c386635739b24df717363 kernel.halt-on-panic=true ";
  constexpr uint32_t kQemuRamdiskStart = 0x48000000;
  constexpr uint32_t kQemuRamdiskEnd = 0x499e8458;

  auto fdt = qemu_arm_gic3();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher, {
                                         .ramdisk_start = kQemuRamdiskStart,
                                         .ramdisk_end = kQemuRamdiskEnd,
                                         .cmdline = kQemuCmdline,
                                         .uart_config_name = uart::pl011::Driver::config_name(),
                                         .uart_config =
                                             {
                                                 .mmio_phys = uart::pl011::kQemuConfig.mmio_phys,
                                                 .irq = 33,
                                             },
                                         .uart_absolute_path = "/pl011@9000000",
                                     });
}

TEST_F(ChosenNodeMatcherTest, RiscvQemu) {
  constexpr std::string_view kQemuCmdline =
      "BOOT_IMAGE=/vmlinuz-5.19.0-1012-generic root=/dev/mapper/ubuntu--vg-ubuntu--lv ro";
  constexpr uint32_t kQemuRamdiskStart = 0xD646A000;
  constexpr uint32_t kQemuRamdiskEnd = 0xDAFEFDB6;

  auto fdt = qemu_riscv();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = kQemuRamdiskStart,
                         .ramdisk_end = kQemuRamdiskEnd,
                         .cmdline = kQemuCmdline,
                         .uart_config_name = uart::ns8250::Mmio8Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x10000000,
                                 .irq = 10,
                             },
                         .uart_absolute_path = "/soc/serial@10000000",
                     });
}

TEST_F(ChosenNodeMatcherTest, VisionFive2) {
  constexpr std::string_view kCmdline =
      "root=/dev/mmcblk1p4 rw console=tty0 console=ttyS0,115200 earlycon rootwait stmmaceth=chain_mode:1 selinux=0";

  auto fdt = vision_five_2();
  boot_shim::DevicetreeChosenNodeMatcher<AllUartDrivers> chosen_matcher("test", stdout);

  ASSERT_TRUE(devicetree::Match(fdt, chosen_matcher));

  CheckChosenMatcher(chosen_matcher,
                     {
                         .ramdisk_start = 0x48100000,
                         .ramdisk_end = 0x48fb3df5,
                         .cmdline = kCmdline,
                         .uart_config_name = uart::ns8250::LegacyDw8250Driver::config_name(),
                         .uart_config =
                             {
                                 .mmio_phys = 0x10000000,
                                 .irq = 32,
                             },
                         .uart_absolute_path = "/soc/serial@10000000",
                     });
}

class MemoryItemTest : public zxtest::Test {
 public:
  static void SetUpTestSuite() {
    auto loaded_dtb = LoadDtb("memory.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    memory_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("reserved_memory.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    reserved_memory_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("memory_reservations.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    memreserve_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("memory_complex.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    complex_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    memory_ = std::nullopt;
    reserved_memory_ = std::nullopt;
    memreserve_ = std::nullopt;
    complex_ = std::nullopt;
  }

  devicetree::Devicetree memory() { return memory_->fdt(); }
  devicetree::Devicetree reserved_memory() { return reserved_memory_->fdt(); }
  devicetree::Devicetree memreserve() { return memreserve_->fdt(); }
  devicetree::Devicetree complex() { return complex_->fdt(); }

 private:
  static std::optional<LoadedDtb> memory_;
  static std::optional<LoadedDtb> reserved_memory_;
  static std::optional<LoadedDtb> memreserve_;
  static std::optional<LoadedDtb> complex_;
};

std::optional<LoadedDtb> MemoryItemTest::memory_ = std::nullopt;
std::optional<LoadedDtb> MemoryItemTest::reserved_memory_ = std::nullopt;
std::optional<LoadedDtb> MemoryItemTest::memreserve_ = std::nullopt;
std::optional<LoadedDtb> MemoryItemTest::complex_ = std::nullopt;

TEST_F(MemoryItemTest, ParseMemreserves) {
  std::vector<memalloc::Range> storage(5);

  auto fdt = memreserve();
  boot_shim::DevicetreeMemoryMatcher memory_matcher("test", stdout, storage);

  ASSERT_TRUE(memory_matcher.AppendAdditionalRanges(fdt));
  ASSERT_TRUE(devicetree::Match(fdt, memory_matcher));

  auto ranges = memory_matcher.memory_ranges();
  ASSERT_EQ(ranges.size(), 5);

  // Account for the devicetree in use.
  EXPECT_EQ(ranges[0].addr, reinterpret_cast<uintptr_t>(fdt.fdt().data()));
  EXPECT_EQ(ranges[0].size, fdt.size_bytes());
  EXPECT_EQ(ranges[0].type, memalloc::Type::kDevicetreeBlob);

  // Each memreserve in order.
  EXPECT_EQ(ranges[1].addr, 0x12340000);
  EXPECT_EQ(ranges[1].size, 0x2000);
  EXPECT_EQ(ranges[1].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[2].addr, 0x56780000);
  EXPECT_EQ(ranges[2].size, 0x3000);
  EXPECT_EQ(ranges[2].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[3].addr, 0x7fffffff12340000);
  EXPECT_EQ(ranges[3].size, 0x400000000);
  EXPECT_EQ(ranges[3].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[4].addr, 0x00ffffff56780000);
  EXPECT_EQ(ranges[4].size, 0x500000000);
  EXPECT_EQ(ranges[4].type, memalloc::Type::kReserved);
}

TEST_F(MemoryItemTest, ParseMemoryNodes) {
  std::vector<memalloc::Range> storage(5);

  auto fdt = memory();
  boot_shim::DevicetreeMemoryMatcher memory_matcher("test", stdout, storage);

  ASSERT_TRUE(memory_matcher.AppendAdditionalRanges(fdt));
  ASSERT_TRUE(devicetree::Match(fdt, memory_matcher));
  auto ranges = memory_matcher.memory_ranges();
  ASSERT_EQ(ranges.size(), 5);

  // Account for the devicetree in use.
  EXPECT_EQ(ranges[0].addr, reinterpret_cast<uintptr_t>(fdt.fdt().data()));
  EXPECT_EQ(ranges[0].size, fdt.size_bytes());
  EXPECT_EQ(ranges[0].type, memalloc::Type::kDevicetreeBlob);

  // Each memory nodes in order.
  EXPECT_EQ(ranges[1].addr, 0x40000000);
  EXPECT_EQ(ranges[1].size, 0x10000000);
  EXPECT_EQ(ranges[1].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[2].addr, 0x50000000);
  EXPECT_EQ(ranges[2].size, 0x20000000);
  EXPECT_EQ(ranges[2].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[3].addr, 0x60000000);
  EXPECT_EQ(ranges[3].size, 0x30000000);
  EXPECT_EQ(ranges[3].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[4].addr, 0x70000000);
  EXPECT_EQ(ranges[4].size, 0x40000000);
  EXPECT_EQ(ranges[4].type, memalloc::Type::kFreeRam);
}

TEST_F(MemoryItemTest, ParseReservedMemoryNodes) {
  std::vector<memalloc::Range> storage(3);

  auto fdt = reserved_memory();
  boot_shim::DevicetreeMemoryMatcher memory_matcher("test", stdout, storage);

  ASSERT_TRUE(memory_matcher.AppendAdditionalRanges(fdt));
  ASSERT_TRUE(devicetree::Match(fdt, memory_matcher));
  auto ranges = memory_matcher.memory_ranges();
  ASSERT_EQ(ranges.size(), 3);

  // Account for the devicetree in use.
  EXPECT_EQ(ranges[0].addr, reinterpret_cast<uintptr_t>(fdt.fdt().data()));
  EXPECT_EQ(ranges[0].size, fdt.size_bytes());
  EXPECT_EQ(ranges[0].type, memalloc::Type::kDevicetreeBlob);

  // Each reserved memory nodes in order.
  EXPECT_EQ(ranges[1].addr, 0x78000000);
  EXPECT_EQ(ranges[1].size, 0x800000);
  EXPECT_EQ(ranges[1].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[2].addr, 0x76000000);
  EXPECT_EQ(ranges[2].size, 0x400000);
  EXPECT_EQ(ranges[2].type, memalloc::Type::kReserved);
}

TEST_F(MemoryItemTest, ParseAll) {
  std::vector<memalloc::Range> storage(11);

  auto fdt = complex();
  boot_shim::DevicetreeMemoryMatcher memory_matcher("test", stdout, storage);

  ASSERT_TRUE(memory_matcher.AppendAdditionalRanges(fdt));
  ASSERT_TRUE(devicetree::Match(fdt, memory_matcher));
  auto ranges = memory_matcher.memory_ranges();
  ASSERT_EQ(ranges.size(), 11);

  // Account for the devicetree in use.
  EXPECT_EQ(ranges[0].addr, reinterpret_cast<uintptr_t>(fdt.fdt().data()));
  EXPECT_EQ(ranges[0].size, fdt.size_bytes());
  EXPECT_EQ(ranges[0].type, memalloc::Type::kDevicetreeBlob);

  // Each memreserve in order.
  EXPECT_EQ(ranges[1].addr, 0x12340000);
  EXPECT_EQ(ranges[1].size, 0x2000);
  EXPECT_EQ(ranges[1].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[2].addr, 0x56780000);
  EXPECT_EQ(ranges[2].size, 0x3000);
  EXPECT_EQ(ranges[2].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[3].addr, 0x7fffffff12340000);
  EXPECT_EQ(ranges[3].size, 0x400000000);
  EXPECT_EQ(ranges[3].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[4].addr, 0x00ffffff56780000);
  EXPECT_EQ(ranges[4].size, 0x500000000);
  EXPECT_EQ(ranges[4].type, memalloc::Type::kReserved);

  // Each memory nodes in order.
  EXPECT_EQ(ranges[5].addr, 0x40000000);
  EXPECT_EQ(ranges[5].size, 0x10000000);
  EXPECT_EQ(ranges[5].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[6].addr, 0x50000000);
  EXPECT_EQ(ranges[6].size, 0x20000000);
  EXPECT_EQ(ranges[6].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[7].addr, 0x60000000);
  EXPECT_EQ(ranges[7].size, 0x30000000);
  EXPECT_EQ(ranges[7].type, memalloc::Type::kFreeRam);

  EXPECT_EQ(ranges[8].addr, 0x70000000);
  EXPECT_EQ(ranges[8].size, 0x40000000);
  EXPECT_EQ(ranges[8].type, memalloc::Type::kFreeRam);

  // Each reserved memory nodes in order.
  EXPECT_EQ(ranges[9].addr, 0x78000000);
  EXPECT_EQ(ranges[9].size, 0x800000);
  EXPECT_EQ(ranges[9].type, memalloc::Type::kReserved);

  EXPECT_EQ(ranges[10].addr, 0x76000000);
  EXPECT_EQ(ranges[10].size, 0x400000);
  EXPECT_EQ(ranges[10].type, memalloc::Type::kReserved);
}

class RiscvDevicetreeTimerItemTest : public TestMixin<RiscvDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    Mixin::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("cpus_riscv.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    cpus_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    cpus_ = std::nullopt;
    Mixin::TearDownTestSuite();
  }

  devicetree::Devicetree cpus() { return cpus_->fdt(); }

 private:
  static std::optional<LoadedDtb> cpus_;
};

std::optional<LoadedDtb> RiscvDevicetreeTimerItemTest::cpus_ = std::nullopt;

TEST_F(RiscvDevicetreeTimerItemTest, ParseTimerFromCpus) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = cpus();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreeTimerItem> shim("test", fdt);
  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());

  bool present = false;
  auto clear_err = fit::defer([&]() { image.ignore_error(); });
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_RISCV_GENERIC_TIMER) {
      EXPECT_GE(payload.size(), sizeof(zbi_dcfg_riscv_generic_timer_driver_t));
      auto* dcfg = reinterpret_cast<zbi_dcfg_riscv_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(dcfg->freq_hz, 0x989680);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevicetreeTimerItemTest, Qemu) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_riscv();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreeTimerItem> shim("test", fdt);
  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());

  bool present = false;
  auto clear_err = fit::defer([&]() { image.ignore_error(); });
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_RISCV_GENERIC_TIMER) {
      EXPECT_GE(payload.size(), sizeof(zbi_dcfg_riscv_generic_timer_driver_t));
      auto* dcfg = reinterpret_cast<zbi_dcfg_riscv_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(dcfg->freq_hz, 0x989680);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevicetreeTimerItemTest, VisionFive2) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = vision_five_2();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreeTimerItem> shim("test", fdt);
  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());

  bool present = false;
  auto clear_err = fit::defer([&]() { image.ignore_error(); });
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER &&
        header->extra == ZBI_KERNEL_DRIVER_RISCV_GENERIC_TIMER) {
      EXPECT_GE(payload.size(), sizeof(zbi_dcfg_riscv_generic_timer_driver_t));
      auto* dcfg = reinterpret_cast<zbi_dcfg_riscv_generic_timer_driver_t*>(payload.data());
      EXPECT_EQ(dcfg->freq_hz, 0x3D0900);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

class RiscvDevictreeCpuTopologyItemTest : public TestMixin<RiscvDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    TestMixin<RiscvDevicetreeTest>::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("cpus_riscv.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    riscv_cpus_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("cpus_riscv_nested_clusters.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    riscv_cpus_nested_clusters_dtb_ = std::move(loaded_dtb).value();

    loaded_dtb = LoadDtb("cpus_no_cpu_map_riscv.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    riscv_cpus_no_cpu_map_dtb_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    riscv_cpus_dtb_ = std::nullopt;
    riscv_cpus_no_cpu_map_dtb_ = std::nullopt;
    TestMixin<RiscvDevicetreeTest>::TearDownTestSuite();
  }

  devicetree::Devicetree riscv_cpus() { return riscv_cpus_dtb_->fdt(); }
  devicetree::Devicetree riscv_cpus_nested_clusters() {
    return riscv_cpus_nested_clusters_dtb_->fdt();
  }
  devicetree::Devicetree riscv_cpus_no_cpu_map() { return riscv_cpus_no_cpu_map_dtb_->fdt(); }

 private:
  static std::optional<LoadedDtb> riscv_cpus_dtb_;
  static std::optional<LoadedDtb> riscv_cpus_nested_clusters_dtb_;
  static std::optional<LoadedDtb> riscv_cpus_no_cpu_map_dtb_;
};

std::optional<LoadedDtb> RiscvDevictreeCpuTopologyItemTest::riscv_cpus_dtb_ = std::nullopt;
std::optional<LoadedDtb> RiscvDevictreeCpuTopologyItemTest::riscv_cpus_nested_clusters_dtb_ =
    std::nullopt;
std::optional<LoadedDtb> RiscvDevictreeCpuTopologyItemTest::riscv_cpus_no_cpu_map_dtb_ =
    std::nullopt;

TEST_F(RiscvDevictreeCpuTopologyItemTest, ParseCpuNodes) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = riscv_cpus();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(3);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 7u);

      // socket0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_SOCKET);

      // cluster0
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[1].entity.cluster.performance_class, 0xFF);

      // cpu@0
      EXPECT_EQ(nodes[2].parent_index, 1);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.riscv64.hart_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[3].parent_index, 1);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cluster1
      EXPECT_EQ(nodes[4].parent_index, 0);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[4].entity.cluster.performance_class, 0x7F);

      // cpu@2
      EXPECT_EQ(nodes[5].parent_index, 4);
      EXPECT_EQ(nodes[5].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[5].entity.processor.flags, 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@3
      EXPECT_EQ(nodes[6].parent_index, 4);
      EXPECT_EQ(nodes[6].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[6].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[6].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[6].entity.processor.architecture_info.riscv64.hart_id, 3);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevictreeCpuTopologyItemTest, ParseCpuNodesNestedClusters) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = riscv_cpus_nested_clusters();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(3);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      // This is tied to the visit order of the cpu nodes.
      // 4 cpus, parents, cpu#4 (id = 3) is the one with hart id = 3.
      ASSERT_EQ(nodes.size(), 11u);

      // socket0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_SOCKET);

      // cluster0
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[1].entity.cluster.performance_class, 0xFF);

      // cluster00
      EXPECT_EQ(nodes[2].parent_index, 1);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[2].entity.cluster.performance_class, 0xFF);

      // cluster000
      EXPECT_EQ(nodes[3].parent_index, 2);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[3].entity.cluster.performance_class, 0xFF);

      // cpu@0
      // core 0 - thread 0
      EXPECT_EQ(nodes[4].parent_index, 3);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[4].entity.processor.flags, 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.riscv64.hart_id, 0);

      // cpu@1
      // core 1 - thread 0
      EXPECT_EQ(nodes[5].parent_index, 3);
      EXPECT_EQ(nodes[5].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[5].entity.processor.flags, 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cluster1
      EXPECT_EQ(nodes[6].parent_index, 0);
      EXPECT_EQ(nodes[6].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[6].entity.cluster.performance_class, 0x7F);

      // cluster10
      EXPECT_EQ(nodes[7].parent_index, 6);
      EXPECT_EQ(nodes[7].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[7].entity.cluster.performance_class, 0x7F);

      // cluster100
      EXPECT_EQ(nodes[8].parent_index, 7);
      EXPECT_EQ(nodes[8].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[8].entity.cluster.performance_class, 0x7F);

      // cpu@2
      // core 2 - thread 0
      EXPECT_EQ(nodes[9].parent_index, 8);
      EXPECT_EQ(nodes[9].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[9].entity.processor.flags, 0);
      EXPECT_EQ(nodes[9].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[9].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[9].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[9].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[9].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[9].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[9].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@3
      // core 3 - thread 0
      EXPECT_EQ(nodes[10].parent_index, 8);
      EXPECT_EQ(nodes[10].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[10].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[10].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[10].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[10].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[10].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[10].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[10].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[10].entity.processor.architecture_info.riscv64.hart_id, 3);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevictreeCpuTopologyItemTest, ParseCpuNodesNoCpuMap) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = riscv_cpus_no_cpu_map();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(3);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      ASSERT_EQ(nodes.size(), 4u);

      // cpu@0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[0].entity.processor.flags, 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.riscv64.hart_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[1].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cpu@2
      EXPECT_EQ(nodes[2].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@3
      EXPECT_EQ(nodes[3].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.riscv64.hart_id, 3);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevictreeCpuTopologyItemTest, Qemu) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_riscv();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(3);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      ASSERT_EQ(nodes.size(), 5u);

      // cluster0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[0].entity.cluster.performance_class, 1u);

      // cpu@0
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.riscv64.hart_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[2].parent_index, 0);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cpu@2
      EXPECT_EQ(nodes[3].parent_index, 0);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@3
      EXPECT_EQ(nodes[4].parent_index, 0);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[4].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.riscv64.hart_id, 3);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevictreeCpuTopologyItemTest, VisionFive2) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = vision_five_2();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(3);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      ASSERT_EQ(nodes.size(), 5u);

      // cpu@0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[0].entity.processor.flags, 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[0].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[0].entity.processor.architecture_info.riscv64.hart_id, 0);

      // cpu@1
      EXPECT_EQ(nodes[1].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cpu@2
      EXPECT_EQ(nodes[2].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@3
      EXPECT_EQ(nodes[3].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.riscv64.hart_id, 3);

      // cpu@4
      EXPECT_EQ(nodes[4].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[4].entity.processor.flags, 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[0], 4);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.riscv64.hart_id, 4);
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevictreeCpuTopologyItemTest, HifiveSifiveUnmatched) {
  std::array<std::byte, 1024> image_buffer;
  std::vector<void*> allocs;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = sifive_hifive_unmatched();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevictreeCpuTopologyItem> shim("test", fdt);
  shim.set_allocator([&allocs](size_t size, size_t alignment) -> void* {
    // Custom aligned_alloc since OS X doesnt support it in some versions.
    void* alloc = malloc(size + alignment);
    allocs.push_back(alloc);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(alloc) + alignment) &
                                   ~(alignment - 1));
  });
  shim.Get<boot_shim::RiscvDevictreeCpuTopologyItem>().set_boot_hart_id(2);

  auto release_memory = fit::defer([&]() {
    for (auto* alloc : allocs) {
      free(alloc);
    }
  });

  shim.Init();
  auto clear_errors = fit::defer([&]() { image.ignore_error(); });
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_CPU_TOPOLOGY) {
      present = true;
      cpp20::span<zbi_topology_node_t> nodes(reinterpret_cast<zbi_topology_node_t*>(payload.data()),
                                             payload.size() / sizeof(zbi_topology_node_t));

      ASSERT_EQ(nodes.size(), 6u);

      // cluster0
      EXPECT_EQ(nodes[0].parent_index, ZBI_TOPOLOGY_NO_PARENT);
      EXPECT_EQ(nodes[0].entity.discriminant, ZBI_TOPOLOGY_ENTITY_CLUSTER);
      EXPECT_EQ(nodes[0].entity.cluster.performance_class, 1);

      // cpu@3
      EXPECT_EQ(nodes[1].parent_index, 0);
      EXPECT_EQ(nodes[1].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[1].entity.processor.flags, 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[0], 3);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[1].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[1].entity.processor.architecture_info.riscv64.hart_id, 3);

      // cpu@1
      EXPECT_EQ(nodes[2].parent_index, 0);
      EXPECT_EQ(nodes[2].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[2].entity.processor.flags, 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[0], 1);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[2].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[2].entity.processor.architecture_info.riscv64.hart_id, 1);

      // cpu@4
      EXPECT_EQ(nodes[3].parent_index, 0);
      EXPECT_EQ(nodes[3].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[3].entity.processor.flags, 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[0], 2);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[3].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[3].entity.processor.architecture_info.riscv64.hart_id, 4);

      // cpu@2
      EXPECT_EQ(nodes[4].parent_index, 0);
      EXPECT_EQ(nodes[4].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[4].entity.processor.flags, ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[0], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[4].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[4].entity.processor.architecture_info.riscv64.hart_id, 2);

      // cpu@0
      EXPECT_EQ(nodes[5].parent_index, 0);
      EXPECT_EQ(nodes[5].entity.discriminant, ZBI_TOPOLOGY_ENTITY_PROCESSOR);
      EXPECT_EQ(nodes[5].entity.processor.flags, 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[0], 4);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[1], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[2], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_ids[3], 0);
      EXPECT_EQ(nodes[5].entity.processor.logical_id_count, 1);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.discriminant,
                ZBI_TOPOLOGY_ARCHITECTURE_INFO_RISCV64);
      EXPECT_EQ(nodes[5].entity.processor.architecture_info.riscv64.hart_id, 0);
    }
  }
  ASSERT_TRUE(present);
}

class RiscvDevicetreePlicItemTest : public TestMixin<RiscvDevicetreeTest> {
 public:
  static void SetUpTestSuite() {
    Mixin::SetUpTestSuite();
    auto loaded_dtb = LoadDtb("plic_riscv.dtb");
    ASSERT_TRUE(loaded_dtb.is_ok(), "%s", loaded_dtb.error_value().c_str());
    plic_dtb_ = std::move(loaded_dtb).value();
  }

  static void TearDownTestSuite() {
    plic_dtb_ = std::nullopt;
    Mixin::TearDownTestSuite();
  }

  devicetree::Devicetree plic() { return plic_dtb_->fdt(); }

 private:
  static std::optional<LoadedDtb> plic_dtb_;
};

std::optional<LoadedDtb> RiscvDevicetreePlicItemTest::plic_dtb_ = std::nullopt;

TEST_F(RiscvDevicetreePlicItemTest, ParsePlic) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = plic();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreePlicItem> shim("test", fdt);

  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  auto clear_err = fit::defer([&]() { image.ignore_error(); });

  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_RISCV_PLIC) {
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_riscv_plic_driver_t));
      auto* plic_dcfg = reinterpret_cast<zbi_dcfg_riscv_plic_driver_t*>(payload.data());
      EXPECT_EQ(plic_dcfg->mmio_phys, 0xc000000);
      EXPECT_EQ(plic_dcfg->num_irqs, 0x60);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevicetreePlicItemTest, Qemu) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = qemu_riscv();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreePlicItem> shim("test", fdt);

  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  auto clear_err = fit::defer([&]() { image.ignore_error(); });

  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_RISCV_PLIC) {
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_riscv_plic_driver_t));
      auto* plic_dcfg = reinterpret_cast<zbi_dcfg_riscv_plic_driver_t*>(payload.data());
      EXPECT_EQ(plic_dcfg->mmio_phys, 0xc000000);
      EXPECT_EQ(plic_dcfg->num_irqs, 0x60);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevicetreePlicItemTest, VisionFive2) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = vision_five_2();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreePlicItem> shim("test", fdt);

  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  auto clear_err = fit::defer([&]() { image.ignore_error(); });

  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_RISCV_PLIC) {
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_riscv_plic_driver_t));
      auto* plic_dcfg = reinterpret_cast<zbi_dcfg_riscv_plic_driver_t*>(payload.data());
      EXPECT_EQ(plic_dcfg->mmio_phys, 0xc000000);
      EXPECT_EQ(plic_dcfg->num_irqs, 0x88);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

TEST_F(RiscvDevicetreePlicItemTest, HifiveSifiveUnmatched) {
  std::array<std::byte, 512> image_buffer;
  zbitl::Image<cpp20::span<std::byte>> image(image_buffer);
  ASSERT_TRUE(image.clear().is_ok());

  auto fdt = sifive_hifive_unmatched();
  boot_shim::DevicetreeBootShim<boot_shim::RiscvDevicetreePlicItem> shim("test", fdt);

  shim.Init();
  ASSERT_TRUE(shim.AppendItems(image).is_ok());
  auto clear_err = fit::defer([&]() { image.ignore_error(); });

  bool present = false;
  for (auto [header, payload] : image) {
    if (header->type == ZBI_TYPE_KERNEL_DRIVER && header->extra == ZBI_KERNEL_DRIVER_RISCV_PLIC) {
      ASSERT_GE(payload.size(), sizeof(zbi_dcfg_riscv_plic_driver_t));
      auto* plic_dcfg = reinterpret_cast<zbi_dcfg_riscv_plic_driver_t*>(payload.data());
      EXPECT_EQ(plic_dcfg->mmio_phys, 0xc000000);
      EXPECT_EQ(plic_dcfg->num_irqs, 0x45);
      present = true;
    }
  }
  ASSERT_TRUE(present);
}

}  // namespace
