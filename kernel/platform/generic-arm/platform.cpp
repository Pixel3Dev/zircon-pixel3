// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <debug.h>
#include <err.h>
#include <fbl/atomic.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <reg.h>
#include <trace.h>

#include <arch.h>
#include <dev/display.h>
#include <dev/hw_rng.h>
#include <dev/interrupt.h>
#include <dev/power.h>
#include <dev/psci.h>
#include <dev/uart.h>
#include <kernel/cmdline.h>
#include <kernel/dpc.h>
#include <kernel/spinlock.h>
#include <lk/init.h>
#include <object/resource_dispatcher.h>
#include <vm/kstack.h>
#include <vm/physmap.h>
#include <vm/vm.h>

#include <mexec.h>
#include <platform.h>

#include <target.h>

#include <arch/arch_ops.h>
#include <arch/arm64.h>
#include <arch/arm64/mmu.h>
#include <arch/arm64/mp.h>
#include <arch/arm64/periphmap.h>
#include <arch/mp.h>

#include <vm/bootreserve.h>
#include <vm/vm_aspace.h>

#include <lib/console.h>
#include <lib/debuglog.h>
#include <lib/memory_limit.h>
#include <lib/system-topology.h>
#if WITH_PANIC_BACKTRACE
#include <kernel/thread.h>
#endif

#include <libzbi/zbi-cpp.h>
#include <pdev/pdev.h>
#include <zircon/boot/image.h>
#include <zircon/rights.h>
#include <zircon/syscalls/smc.h>
#include <zircon/types.h>

// Defined in start.S.
extern paddr_t kernel_entry_paddr;
extern paddr_t zbi_paddr;

static void* ramdisk_base;
static size_t ramdisk_size;

static zbi_header_t* zbi_root = nullptr;

static zbi_nvram_t lastlog_nvram;

static uint cpu_cluster_count = 0;
static uint cpu_cluster_cpus[SMP_CPU_MAX_CLUSTERS] = {0};

static bool halt_on_panic = false;
static bool uart_disabled = false;

// all of the configured memory arenas from the zbi
// at the moment, only support 1 arena
static pmm_arena_info_t mem_arena = {
    /* .name */ "sdram",
    /* .flags */ 0,
    /* .priority */ 0,
    /* .base */ 0, // filled in by zbi
    /* .size */ 0, // filled in by zbi
};

// boot items to save for mexec
// TODO(voydanoff): more generic way of doing this that can be shared with PC platform
static uint8_t mexec_zbi[4096];
static size_t mexec_zbi_length = 0;

static volatile int panic_started;

// TODO(ZX-3068) This is temporary until we fully deprecate ZBI_CPU_CONFIG.
static bool use_topology = false;

static constexpr bool kProcessZbiEarly = true;

static void halt_other_cpus(void) {
    static volatile int halted = 0;

    if (atomic_swap(&halted, 1) == 0) {
        // stop the other cpus
        printf("stopping other cpus\n");
        arch_mp_send_ipi(MP_IPI_TARGET_ALL_BUT_LOCAL, 0, MP_IPI_HALT);

        // spin for a while
        // TODO: find a better way to spin at this low level
        for (volatile int i = 0; i < 100000000; i++) {
            __asm volatile("nop");
        }
    }
}

// Difference on SMT systems is that the AFF0 (cpu_id) level is implicit and not stored in the info.
static uint64_t ToSmtMpid(const zbi_topology_processor_t& processor, uint8_t cpu_id) {
    DEBUG_ASSERT(processor.architecture == ZBI_TOPOLOGY_ARCH_ARM);
    const auto& info = processor.architecture_info.arm;
    return (uint64_t)info.cluster_3_id << 32 |
           info.cluster_2_id << 16 |
           info.cluster_1_id << 8 |
           cpu_id;
}

static uint64_t ToMpid(const zbi_topology_processor_t& processor) {
    DEBUG_ASSERT(processor.architecture == ZBI_TOPOLOGY_ARCH_ARM);
    const auto& info = processor.architecture_info.arm;
    return (uint64_t)info.cluster_3_id << 32 |
           info.cluster_2_id << 16 |
           info.cluster_1_id << 8 |
           info.cpu_id;
}

void platform_panic_start(void) {
    arch_disable_ints();

    halt_other_cpus();

    if (atomic_swap(&panic_started, 1) == 0) {
        dlog_bluescreen_init();
    }
}

void* platform_get_ramdisk(size_t* size) {
    if (ramdisk_base) {
        *size = ramdisk_size;
        return ramdisk_base;
    } else {
        *size = 0;
        return nullptr;
    }
}

void platform_halt_cpu(void) {
    uint32_t result = psci_cpu_off();
    // should have never returned
    panic("psci_cpu_off returned %u\n", result);
}

void platform_halt_secondary_cpus(void) {
    // Ensure the current thread is pinned to the boot CPU.
    DEBUG_ASSERT(get_current_thread()->cpu_affinity == cpu_num_to_mask(BOOT_CPU_ID));

    // "Unplug" online secondary CPUs before halting them.
    cpu_mask_t primary = cpu_num_to_mask(BOOT_CPU_ID);
    cpu_mask_t mask = mp_get_online_mask() & ~primary;
    zx_status_t result = mp_unplug_cpu_mask(mask);
    DEBUG_ASSERT(result == ZX_OK);
}

static zx_status_t platform_start_cpu(uint64_t mpid) {
    // Issue memory barrier before starting to ensure previous stores will be visible to new CPU.
    smp_mb();

    uint32_t ret = psci_cpu_on(mpid, kernel_entry_paddr);
    dprintf(INFO, "Trying to start cpu %lu returned: %d\n", mpid, (int)ret);
    if (ret != 0) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

static void topology_cpu_init(void) {
    for (auto* node : system_topology::GetSystemTopology().processors()) {
        if (node->entity_type != ZBI_TOPOLOGY_ENTITY_PROCESSOR ||
            node->entity.processor.architecture != ZBI_TOPOLOGY_ARCH_ARM) {
            panic("Invalid processor node.");
        }

        zx_status_t status;
        const auto& processor = node->entity.processor;
        for (uint8_t i = 0; i < processor.logical_id_count; i++) {
            const uint64_t mpid =
                (processor.logical_id_count > 1) ? ToSmtMpid(processor, i) : ToMpid(processor);
            arch_register_mpid(processor.logical_ids[i], mpid);

            // Skip processor 0, we are only starting secondary processors.
            if (processor.logical_ids[i] == 0) {
                continue;
            }

            status = arm64_create_secondary_stack(processor.logical_ids[i], mpid);
            DEBUG_ASSERT(status == ZX_OK);

            // start the cpu
            status = platform_start_cpu(mpid);

            if (status != ZX_OK) {
                // TODO(maniscalco): Is continuing really the right thing to do here?

                // start failed, free the stack
                status = arm64_free_secondary_stack(processor.logical_ids[i]);
                DEBUG_ASSERT(status == ZX_OK);
                continue;
            }
        }
        // the cpu booted
        //
        // bootstrap thread is now responsible for freeing its stack
    }
}

static void platform_cpu_init(void) {
    for (uint cluster = 0; cluster < cpu_cluster_count; cluster++) {
        for (uint cpu = 0; cpu < cpu_cluster_cpus[cluster]; cpu++) {
            if (cluster != 0 || cpu != 0) {
                const uint cpu_num = arch_mpid_to_cpu_num(cluster, cpu);
                const uint64_t mpid = ARM64_MPID(cluster, cpu);

                // create a stack for the cpu we're about to start
                zx_status_t status = arm64_create_secondary_stack(cpu_num, mpid);

                DEBUG_ASSERT(status == ZX_OK);

                // start the cpu
                status = platform_start_cpu(mpid);

                if (status != ZX_OK) {
                    // TODO(maniscalco): Is continuing really the right thing to do here?

                    // start failed, free the stack
                    zx_status_t status = arm64_free_secondary_stack(cpu_num);
                    DEBUG_ASSERT(status == ZX_OK);
                    continue;
                }

                // the cpu booted
                //
                // bootstrap thread is now responsible for freeing its stack
            }
        }
    }
}

static inline bool is_zbi_container(void* addr) {
    DEBUG_ASSERT(addr);

    zbi_header_t* item = (zbi_header_t*)addr;
    return item->type == ZBI_TYPE_CONTAINER;
}

static void save_mexec_zbi(zbi_header_t* item) {
    size_t length = ZBI_ALIGN(
        static_cast<uint32_t>(sizeof(zbi_header_t) + item->length));
    ASSERT(sizeof(mexec_zbi) - mexec_zbi_length >= length);

    memcpy(&mexec_zbi[mexec_zbi_length], item, length);
    mexec_zbi_length += length;
}

static void process_mem_range(const zbi_mem_range_t* mem_range) {
    switch (mem_range->type) {
    case ZBI_MEM_RANGE_RAM:
        if (mem_arena.size == 0) {
            mem_arena.base = mem_range->paddr;
            mem_arena.size = mem_range->length;
            dprintf(INFO, "mem_arena.base %#" PRIx64 " size %#" PRIx64 "\n", mem_arena.base,
                    mem_arena.size);
        } else {
            if (mem_range->paddr) {
                mem_arena.base = mem_range->paddr;
                dprintf(INFO, "overriding mem arena 0 base from FDT: %#zx\n", mem_arena.base);
            }
            // if mem_area.base is already set, then just update the size
            mem_arena.size = mem_range->length;
            dprintf(INFO, "overriding mem arena 0 size from FDT: %#zx\n", mem_arena.size);
        }
        break;
    case ZBI_MEM_RANGE_PERIPHERAL: {
        auto status = add_periph_range(mem_range->paddr, mem_range->length);
        ASSERT(status == ZX_OK);
        break;
    }
    case ZBI_MEM_RANGE_RESERVED:
        dprintf(INFO, "boot reserve mem range: phys base %#" PRIx64 " length %#" PRIx64 "\n",
                mem_range->paddr, mem_range->length);
        boot_reserve_add_range(mem_range->paddr, mem_range->length);
        break;
    default:
        panic("bad mem_range->type in process_mem_range\n");
        break;
    }
}

// Called during platform_init_early, the heap is not yet present.
static zbi_result_t process_zbi_item_early(zbi_header_t* item,
                                           void* payload, void*) {
    if (ZBI_TYPE_DRV_METADATA(item->type)) {
        save_mexec_zbi(item);
        return ZBI_RESULT_OK;
    }

    switch (item->type) {
    case ZBI_TYPE_KERNEL_DRIVER:
    case ZBI_TYPE_PLATFORM_ID:
        // we don't process these here, but we need to save them for mexec
        save_mexec_zbi(item);
        break;
    case ZBI_TYPE_CMDLINE: {
        if (item->length < 1) {
            break;
        }
        char* contents = reinterpret_cast<char*>(payload);
        contents[item->length - 1] = '\0';
        cmdline_append(contents);
        break;
    }
    case ZBI_TYPE_MEM_CONFIG: {
        zbi_mem_range_t* mem_range = reinterpret_cast<zbi_mem_range_t*>(payload);
        uint32_t count = item->length / (uint32_t)sizeof(zbi_mem_range_t);
        for (uint32_t i = 0; i < count; i++) {
            process_mem_range(mem_range++);
        }
        save_mexec_zbi(item);
        break;
    }
    case ZBI_TYPE_CPU_CONFIG: {
        zbi_cpu_config_t* cpu_config = reinterpret_cast<zbi_cpu_config_t*>(payload);
        cpu_cluster_count = cpu_config->cluster_count;
        for (uint32_t i = 0; i < cpu_cluster_count; i++) {
            cpu_cluster_cpus[i] = cpu_config->clusters[i].cpu_count;
        }
        arch_init_cpu_map(cpu_cluster_count, cpu_cluster_cpus);
        save_mexec_zbi(item);
        break;
    }
    case ZBI_TYPE_NVRAM: {
        zbi_nvram_t* nvram = reinterpret_cast<zbi_nvram_t*>(payload);
        memcpy(&lastlog_nvram, nvram, sizeof(lastlog_nvram));
        dprintf(INFO, "boot reserve nvram range: phys base %#" PRIx64 " length %#" PRIx64 "\n",
                nvram->base, nvram->length);
        boot_reserve_add_range(nvram->base, nvram->length);
        save_mexec_zbi(item);
        break;
    }
    }

    return ZBI_RESULT_OK;
}

// Called after heap is up, but before multithreading.
static zbi_result_t process_zbi_item_late(zbi_header_t* item,
                                          void* payload, void*) {
    switch (item->type) {
    case ZBI_TYPE_CPU_TOPOLOGY: {
        const int node_count = item->length / item->extra;

        zbi_topology_node_t* nodes =
            reinterpret_cast<zbi_topology_node_t*>(payload);

        auto result = system_topology::GetMutableSystemTopology()
                          .Update(nodes, node_count);
        if (result != ZX_OK) {
            printf("Failed to initialize system topology! error: %d \n",
                   result);
        } else {
            use_topology = true;
            arch_set_num_cpus(static_cast<uint>(
                system_topology::GetSystemTopology().processor_count()));

            // TODO(ZX-3068) Print the whole topology of the system.
            if (LK_DEBUGLEVEL >= INFO) {
                for (auto* proc :
                     system_topology::GetSystemTopology().processors()) {
                    auto& info = proc->entity.processor.architecture_info.arm;
                    dprintf(INFO, "System topology: CPU %u:%u:%u:%u\n",
                            info.cluster_3_id,
                            info.cluster_2_id,
                            info.cluster_1_id,
                            info.cpu_id);
                }
            }
        }

        break;
    }
    }
    return ZBI_RESULT_OK;
}

static void process_zbi(zbi_header_t* root, bool early) {
    DEBUG_ASSERT(root);
    zbi_result_t result;

    uint8_t* zbi_base = reinterpret_cast<uint8_t*>(root);
    zbi::Zbi image(zbi_base);

    // Make sure the image looks valid.
    result = image.Check(nullptr);
    if (result != ZBI_RESULT_OK) {
        // TODO(gkalsi): Print something informative here?
        return;
    }

    image.ForEach(early ? process_zbi_item_early : process_zbi_item_late, nullptr);
}

void platform_early_init(void) {
    // if the zbi_paddr variable is -1, it was not set
    // in start.S, so we are in a bad place.
    if (zbi_paddr == -1UL) {
        panic("no zbi_paddr!\n");
    }

    void* zbi_vaddr = paddr_to_physmap(zbi_paddr);

    // initialize the boot memory reservation system
    boot_reserve_init();

    if (zbi_vaddr && is_zbi_container(zbi_vaddr)) {
        zbi_header_t* header = (zbi_header_t*)zbi_vaddr;

        ramdisk_base = header;
        ramdisk_size = ROUNDUP(header->length + sizeof(*header), PAGE_SIZE);
    } else {
        panic("no bootdata!\n");
    }

    if (!ramdisk_base || !ramdisk_size) {
        panic("no ramdisk!\n");
    }

    zbi_root = reinterpret_cast<zbi_header_t*>(ramdisk_base);
    // walk the zbi structure and process all the items
    process_zbi(zbi_root, kProcessZbiEarly);

    // is the cmdline option to bypass dlog set ?
    dlog_bypass_init();

    // bring up kernel drivers after we have mapped our peripheral ranges
    pdev_init(zbi_root);

    // Serial port should be active now

    // Read cmdline after processing zbi, which may contain cmdline data.
    halt_on_panic = cmdline_get_bool("kernel.halt-on-panic", false);

    // Check if serial should be enabled
    const char* serial_mode = cmdline_get("kernel.serial");
    uart_disabled = (serial_mode != NULL && !strcmp(serial_mode, "none"));

    // add the ramdisk to the boot reserve memory list
    paddr_t ramdisk_start_phys = physmap_to_paddr(ramdisk_base);
    paddr_t ramdisk_end_phys = ramdisk_start_phys + ramdisk_size;
    dprintf(INFO, "reserving ramdisk phys range [%#" PRIx64 ", %#" PRIx64 "]\n",
            ramdisk_start_phys, ramdisk_end_phys - 1);
    boot_reserve_add_range(ramdisk_start_phys, ramdisk_size);

    // check if a memory limit was passed in via kernel.memory-limit-mb and
    // find memory ranges to use if one is found.
    zx_status_t status = memory_limit_init();
    if (status == ZX_OK) {
        // Figure out and add arenas based on the memory limit and our range of DRAM
        memory_limit_add_range(mem_arena.base, mem_arena.size, mem_arena);
        status = memory_limit_add_arenas(mem_arena);
    }

    // If no memory limit was found, or adding arenas from the range failed, then add
    // the existing global arena.
    if (status != ZX_OK) {
        // Init returns not supported if no limit exists
        if (status != ZX_ERR_NOT_SUPPORTED) {
            dprintf(INFO, "memory limit lib returned an error (%d), falling back to defaults\n",
                    status);
        }
        pmm_add_arena(&mem_arena);
    }

    // tell the boot allocator to mark ranges we've reserved as off limits
    boot_reserve_wire();
}

// Called after the heap is up but before the system is multithreaded.
void platform_init_pre_thread(uint) {
    process_zbi(zbi_root, !kProcessZbiEarly);
}

LK_INIT_HOOK(platform_init_pre_thread, platform_init_pre_thread,
             LK_INIT_LEVEL_THREADING - 1);

void platform_init(void) {
    if (use_topology) {
        topology_cpu_init();
    } else {
        platform_cpu_init();
    }
}

// after the fact create a region to reserve the peripheral map(s)
static void platform_init_postvm(uint level) {
    reserve_periph_ranges();
}

LK_INIT_HOOK(platform_postvm, platform_init_postvm, LK_INIT_LEVEL_VM);

void platform_dputs_thread(const char* str, size_t len) {
    if (uart_disabled) {
        return;
    }
    uart_puts(str, len, true, true);
}

void platform_dputs_irq(const char* str, size_t len) {
    if (uart_disabled) {
        return;
    }
    uart_puts(str, len, false, true);
}

int platform_dgetc(char* c, bool wait) {
    if (uart_disabled) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    int ret = uart_getc(wait);
    if (ret < 0)
        return ret;
    *c = static_cast<char>(ret);
    return 0;
}

void platform_pputc(char c) {
    if (uart_disabled) {
        return;
    }
    uart_pputc(c);
}

int platform_pgetc(char* c, bool wait) {
    if (uart_disabled) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    int r = uart_pgetc();
    if (r < 0) {
        return r;
    }

    *c = static_cast<char>(r);
    return 0;
}

/* stub out the hardware rng entropy generator, which doesn't exist on this platform */
size_t hw_rng_get_entropy(void* buf, size_t len, bool block) {
    return 0;
}

/* no built in framebuffer */
zx_status_t display_get_info(struct display_info* info) {
    return ZX_ERR_NOT_FOUND;
}

void platform_halt(platform_halt_action suggested_action, platform_halt_reason reason) {

    if (suggested_action == HALT_ACTION_REBOOT) {
        power_reboot(REBOOT_NORMAL);
        printf("reboot failed\n");
    } else if (suggested_action == HALT_ACTION_REBOOT_BOOTLOADER) {
        power_reboot(REBOOT_BOOTLOADER);
        printf("reboot-bootloader failed\n");
    } else if (suggested_action == HALT_ACTION_REBOOT_RECOVERY) {
        power_reboot(REBOOT_RECOVERY);
        printf("reboot-recovery failed\n");
    } else if (suggested_action == HALT_ACTION_SHUTDOWN) {
        power_shutdown();
    }

    if (reason == HALT_REASON_SW_PANIC) {
        thread_print_current_backtrace();
        dlog_bluescreen_halt();
        if (!halt_on_panic) {
            power_reboot(REBOOT_NORMAL);
            printf("reboot failed\n");
        }
#if ENABLE_PANIC_SHELL
        dprintf(ALWAYS, "CRASH: starting debug shell... (reason = %d)\n", reason);
        arch_disable_ints();
        panic_shell_start();
#endif // ENABLE_PANIC_SHELL
    }

    dprintf(ALWAYS, "HALT: spinning forever... (reason = %d)\n", reason);

    // catch all fallthrough cases
    arch_disable_ints();
    for (;;)
        ;
}

typedef struct {
    //TODO: combine with x86 nvram crashlog handling
    //TODO: ECC for more robust crashlogs
    uint64_t magic;
    uint64_t length;
    uint64_t nmagic;
    uint64_t nlength;
} log_hdr_t;

#define NVRAM_MAGIC (0x6f8962d66b28504fULL)

size_t platform_stow_crashlog(void* log, size_t len) {
    size_t max = lastlog_nvram.length - sizeof(log_hdr_t);
    void* nvram = paddr_to_physmap(lastlog_nvram.base);
    if (nvram == NULL) {
        return 0;
    }

    if (log == NULL) {
        return max;
    }
    if (len > max) {
        len = max;
    }

    log_hdr_t hdr = {
        .magic = NVRAM_MAGIC,
        .length = len,
        .nmagic = ~NVRAM_MAGIC,
        .nlength = ~len,
    };
    memcpy(nvram, &hdr, sizeof(hdr));
    memcpy(static_cast<char*>(nvram) + sizeof(hdr), log, len);
    arch_clean_cache_range((uintptr_t)nvram, sizeof(hdr) + len);
    return len;
}

size_t platform_recover_crashlog(size_t len, void* cookie,
                                 void (*func)(const void* data, size_t, size_t len, void* cookie)) {
    size_t max = lastlog_nvram.length - sizeof(log_hdr_t);
    void* nvram = paddr_to_physmap(lastlog_nvram.base);
    if (nvram == NULL) {
        return 0;
    }
    log_hdr_t hdr;
    memcpy(&hdr, nvram, sizeof(hdr));
    if ((hdr.magic != NVRAM_MAGIC) || (hdr.length > max) ||
        (hdr.nmagic != ~NVRAM_MAGIC) || (hdr.nlength != ~hdr.length)) {
        printf("nvram-crashlog: bad header: %016lx %016lx %016lx %016lx\n",
               hdr.magic, hdr.length, hdr.nmagic, hdr.nlength);
        return 0;
    }
    if (len == 0) {
        return hdr.length;
    }
    if (len > hdr.length) {
        len = hdr.length;
    }
    func(static_cast<char*>(nvram) + sizeof(hdr), 0, len, cookie);

    // invalidate header so we don't get a stale crashlog
    // on future boots
    hdr.magic = 0;
    memcpy(nvram, &hdr, sizeof(hdr));
    return hdr.length;
}

zx_status_t platform_mexec_patch_zbi(uint8_t* zbi, const size_t len) {
    size_t offset = 0;

    // copy certain boot items provided by the bootloader or boot shim
    // to the mexec zbi
    zbi::Zbi image(zbi, len);
    while (offset < mexec_zbi_length) {
        zbi_header_t* item = reinterpret_cast<zbi_header_t*>(mexec_zbi + offset);

        zbi_result_t status;
        status = image.AppendSection(item->length, item->type, item->extra,
                                     item->flags,
                                     reinterpret_cast<uint8_t*>(item + 1));

        if (status != ZBI_RESULT_OK)
            return ZX_ERR_INTERNAL;

        offset += ZBI_ALIGN(
            static_cast<uint32_t>(sizeof(zbi_header_t)) + item->length);
    }

    return ZX_OK;
}

void platform_mexec_prep(uintptr_t new_bootimage_addr, size_t new_bootimage_len) {
    DEBUG_ASSERT(!arch_ints_disabled());
    DEBUG_ASSERT(mp_get_online_mask() == cpu_num_to_mask(BOOT_CPU_ID));
}

void platform_mexec(mexec_asm_func mexec_assembly, memmov_ops_t* ops,
                    uintptr_t new_bootimage_addr, size_t new_bootimage_len,
                    uintptr_t entry64_addr) {
    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(mp_get_online_mask() == cpu_num_to_mask(BOOT_CPU_ID));

    paddr_t kernel_src_phys = (paddr_t)ops[0].src;
    paddr_t kernel_dst_phys = (paddr_t)ops[0].dst;

    // check to see if the kernel is packaged as a zbi container
    zbi_header_t* header = (zbi_header_t*)paddr_to_physmap(kernel_src_phys);
    if (header[0].type == ZBI_TYPE_CONTAINER && header[1].type == ZBI_TYPE_KERNEL_ARM64) {
        zbi_kernel_t* kernel_header = (zbi_kernel_t*)&header[2];
        // add offset from kernel header to entry point
        kernel_dst_phys += kernel_header->entry;
    }
    // else just jump to beginning of kernel image

    mexec_assembly((uintptr_t)new_bootimage_addr, 0, 0, arm64_get_boot_el(), ops,
                   (void*)kernel_dst_phys);
}

bool platform_serial_enabled(void) {
    return !uart_disabled && uart_present();
}

bool platform_early_console_enabled() {
    return false;
}

// Initialize Resource system after the heap is initialized.
static void arm_resource_dispatcher_init_hook(unsigned int rl) {
    // 64 bit address space for MMIO on ARM64
    zx_status_t status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_MMIO, 0,
                                                                 UINT64_MAX);
    if (status != ZX_OK) {
        printf("Resources: Failed to initialize MMIO allocator: %d\n", status);
    }
    // Set up IRQs based on values from the GIC
    status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_IRQ,
                                                     interrupt_get_base_vector(),
                                                     interrupt_get_max_vector());
    if (status != ZX_OK) {
        printf("Resources: Failed to initialize IRQ allocator: %d\n", status);
    }
    // Set up SMC valid service call range
    status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_SMC,
                                                     0,
                                                     ARM_SMC_SERVICE_CALL_NUM_MAX + 1);
    if (status != ZX_OK) {
        printf("Resources: Failed to initialize SMC allocator: %d\n", status);
    }
}

LK_INIT_HOOK(arm_resource_init, arm_resource_dispatcher_init_hook, LK_INIT_LEVEL_HEAP);
