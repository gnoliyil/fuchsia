// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_TARGET_ARM64_BOARD_VISALIA_BOOT_SHIM_CONFIG_H_
#define ZIRCON_KERNEL_TARGET_ARM64_BOARD_VISALIA_BOOT_SHIM_CONFIG_H_

#define HAS_DEVICE_TREE 0

static const zbi_mem_range_t mem_config[] = {
    {
        .paddr = 0x02000000,
        .length = 0x20000000,  // 512M
        .type = ZBI_MEM_TYPE_RAM,
    },
    {
        .paddr = 0xf0000000,
        .length = 0x10000000,
        .type = ZBI_MEM_TYPE_PERIPHERAL,
    },
};

static const zbi_dcfg_simple_t uart_driver = {
    .mmio_phys = 0xf7e80c00,
    .irq = 88,
};

static const zbi_dcfg_arm_gic_v2_driver_t gic_v2_driver = {
    .mmio_phys = 0xf7900000,
    .gicd_offset = 0x1000,
    .gicc_offset = 0x2000,
    .ipi_base = 0,
};

static const zbi_dcfg_arm_psci_driver_t psci_driver = {
    .use_hvc = false,
};

static const zbi_dcfg_arm_generic_timer_driver_t timer_driver = {
    .irq_phys = 30, .irq_virt = 27,
    //.freq_override = 8333333,
};

static const zbi_platform_id_t platform_id = {
    PDEV_VID_GOOGLE,
    PDEV_PID_VISALIA,
    "visalia",
};

static void add_cpu_topology(zbi_header_t* zbi) {
#define TOPOLOGY_CPU_COUNT 4
  zbi_topology_node_t nodes[TOPOLOGY_CPU_COUNT];

  for (uint8_t index = 0; index < TOPOLOGY_CPU_COUNT; index++) {
    nodes[index] = (zbi_topology_node_t){
        .entity =
            {
                .discriminant = ZBI_TOPOLOGY_ENTITY_PROCESSOR,
                .processor =
                    {

                        .architecture_info =
                            {
                                .discriminant = ZBI_TOPOLOGY_ARCHITECTURE_INFO_ARM64,
                                .arm64 =
                                    {
                                        .cpu_id = index,
                                        .gic_id = index,
                                    },
                            },
                        .flags = index == 0 ? ZBI_TOPOLOGY_PROCESSOR_FLAGS_PRIMARY
                                            : (zbi_topology_processor_flags_t)0,
                        .logical_ids = {index},
                        .logical_id_count = 1,
                    },
            },
        .parent_index = ZBI_TOPOLOGY_NO_PARENT,
    };
  }

  append_boot_item(zbi, ZBI_TYPE_CPU_TOPOLOGY, sizeof(zbi_topology_node_t), &nodes,
                   sizeof(zbi_topology_node_t) * TOPOLOGY_CPU_COUNT);
}

static void append_board_boot_item(zbi_header_t* bootdata) {
  add_cpu_topology(bootdata);

  // add kernel drivers
  append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_DW8250_UART, &uart_driver,
                   sizeof(uart_driver));
  append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_GIC_V2, &gic_v2_driver,
                   sizeof(gic_v2_driver));
  append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_PSCI, &psci_driver,
                   sizeof(psci_driver));
  append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_GENERIC_TIMER,
                   &timer_driver, sizeof(timer_driver));

  // append_boot_item doesn't support zero-length payloads, so we have to call zbi_create_entry
  // directly.
  uint8_t* new_section = NULL;
  zbi_result_t result = zbi_create_entry(bootdata, SIZE_MAX, ZBI_TYPE_KERNEL_DRIVER,
                                         ZBI_KERNEL_DRIVER_AS370_POWER, 0, 0, (void**)&new_section);
  if (result != ZBI_RESULT_OK) {
    fail("zbi_create_entry failed\n");
  }

  // add platform ID
  append_boot_item(bootdata, ZBI_TYPE_PLATFORM_ID, 0, &platform_id, sizeof(platform_id));
}

#endif  // ZIRCON_KERNEL_TARGET_ARM64_BOARD_VISALIA_BOOT_SHIM_CONFIG_H_
