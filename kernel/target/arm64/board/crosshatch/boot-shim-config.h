// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define HAS_DEVICE_TREE 1

// secondary CPUs currently won't start.
static const zbi_cpu_config_t cpu_config = {
    .cluster_count = 1,
    .clusters = {
        {
            .cpu_count = 1,
        },
    },
};

static const zbi_mem_range_t mem_config[] = {
    {
        .type = ZBI_MEM_RANGE_RAM,
	.paddr = 0x80000000,
        .length = 0xfe0a0000, // 4GB
    },
    {
        .type = ZBI_MEM_RANGE_PERIPHERAL,
        .paddr = 0x00000000,
        .length = 0x80000000,
    },
    {
        // hyp_region: hyp_region@85700000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x85700000,
        .length = 0x600000,
    },
    {
        // xbl_region: xbl_region@85e00000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x85e00000,
        .length = 0x100000,
    },
    {
        // removed_region: removed_region@85fc0000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x85fc0000,
        .length = 0x2f40000,
    },
    {
        // qseecom_mem: qseecom_region@0x8ab00000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x8ab00000,
        .length = 0x1400000,
    },
    // omitted: pil/fw regions (there's no support for loading firmware yet)
    {
        // cont_splash_memory: cont_splash_region@9d400000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x9d400000,
        .length = 0x02400000,
    },
    {
        // easel_mem: easel_mem@9f800000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0x9f800000,
        .length = 0x2000000,
    },
    {
        // debug_info: debug_info@0
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0xa1800000,
        .length = 0x00010000,
    },
    {
        // ramoops_mem: ramoops_region@a1810000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0xa1810000,
        .length = 0x200000,
    },
    {
        // ramoops_meta_mem: ramoops_meta_region@a1c10000
        .type = ZBI_MEM_RANGE_RESERVED,
        .paddr = 0xa1c10000,
        .length = 0x1000,
    },
};

static const zbi_nvram_t nvram_config = {
    // pstore alternate
    .base = 0xa1a10000,
    .length = 0x200000,
};

static const dcfg_arm_gicv3_driver_t gicv3_driver = {
    .mmio_phys = 0x17a00000,
    .gicd_offset = 0x00000,
    .gicr_offset = 0x60000,
    .gicr_stride = 0x20000,
    .ipi_base = 9,
};

static const dcfg_arm_psci_driver_t psci_driver = {
    .use_hvc = false,
};

static const dcfg_arm_generic_timer_driver_t timer_driver = {
    .irq_phys = 16 + 2, // GIC_PPI 2
    .irq_virt = 16 + 3, // GIC_PPI 3
};

static const zbi_platform_id_t platform_id = {
    .vid = PDEV_VID_GOOGLE,
    .pid = PDEV_PID_CROSSHATCH,
    .board_name = "crosshatch",
};

static void append_board_boot_item(zbi_header_t* bootdata) {
    // add CPU configuration
    append_boot_item(bootdata, ZBI_TYPE_CPU_CONFIG, 0, &cpu_config,
                    sizeof(zbi_cpu_config_t) +
                    sizeof(zbi_cpu_cluster_t) * cpu_config.cluster_count);

    // add memory configuration
    append_boot_item(bootdata, ZBI_TYPE_MEM_CONFIG, 0, &mem_config,
                    sizeof(zbi_mem_range_t) * countof(mem_config));

    // append nvram config. Needed since otherwise Zircon defaults to 0x0?
    append_boot_item(bootdata, ZBI_TYPE_NVRAM, 0, &nvram_config,
                    sizeof(zbi_nvram_t));

    // add kernel drivers
    append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, KDRV_ARM_GIC_V3, &gicv3_driver,
                    sizeof(gicv3_driver));
    append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, KDRV_ARM_PSCI, &psci_driver,
                    sizeof(psci_driver));
    append_boot_item(bootdata, ZBI_TYPE_KERNEL_DRIVER, KDRV_ARM_GENERIC_TIMER, &timer_driver,
                    sizeof(timer_driver));

    // add platform ID
    append_boot_item(bootdata, ZBI_TYPE_PLATFORM_ID, 0, &platform_id, sizeof(platform_id));
}
