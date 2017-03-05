// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <reg.h>
#include <err.h>
#include <debug.h>
#include <trace.h>

#include <dev/uart.h>
#include <arch.h>
#include <lk/init.h>
#include <kernel/vm.h>
#include <kernel/spinlock.h>
#include <dev/display.h>
#include <dev/hw_rng.h>
#include <dev/psci.h>

#include <platform.h>
#include <arch/arm64/platform.h>

#include <target.h>

#include <arch/mp.h>
#include <arch/arm64/mp.h>
#include <arch/arm64.h>
#include <arch/arm64/mmu.h>

#include <lib/console.h>
#if WITH_LIB_DEBUGLOG
#include <lib/debuglog.h>
#endif
#if WITH_PANIC_BACKTRACE
#include <kernel/thread.h>
#endif

#include <magenta/bootdata.h>
#include <mdi/mdi.h>
#include <mdi/mdi-defs.h>
#include <pdev/pdev.h>

static void* ramdisk_base;
static size_t ramdisk_size;

static mdi_node_ref_t  cpu_map = {0};

/* initial memory mappings. parsed by start.S */
struct mmu_initial_mapping mmu_initial_mappings[] = {
 /* 1GB of sdram space */
 {
     .phys = MEMBASE,
     .virt = KERNEL_BASE,
     .size = MEMORY_APERTURE_SIZE,
     .flags = 0,
     .name = "memory"
 },

 /* peripherals */
 {
     .phys = PERIPH_BASE_PHYS,
     .virt = PERIPH_BASE_VIRT,
     .size = PERIPH_SIZE,
     .flags = MMU_INITIAL_MAPPING_FLAG_DEVICE,
     .name = "msm peripherals"
 },
 /* null entry to terminate the list */
 {}
};

#define DEBUG_UART 1

extern void arm_reset(void);

static pmm_arena_info_t arena = {
    .name = "sdram",
    .base = MEMBASE,
    .size = MEMSIZE,
    .flags = PMM_ARENA_FLAG_KMAP,
};

static volatile int panic_started;

static void halt_other_cpus(void)
{
#if WITH_SMP
    static volatile int halted = 0;

    if (atomic_swap(&halted, 1) == 0) {
        // stop the other cpus
        printf("stopping other cpus\n");
        arch_mp_send_ipi(MP_CPU_ALL_BUT_LOCAL, MP_IPI_HALT);

        // spin for a while
        // TODO: find a better way to spin at this low level
        for (volatile int i = 0; i < 100000000; i++) {
            __asm volatile ("nop");
        }
    }
#endif
}

void platform_panic_start(void)
{
    arch_disable_ints();

    halt_other_cpus();

    if (atomic_swap(&panic_started, 1) == 0) {
#if WITH_LIB_DEBUGLOG
        dlog_bluescreen_init();
#endif
    }
}

void platform_init_mmu_mappings(void)
{
}

void* platform_get_ramdisk(size_t *size) {
    if (ramdisk_base) {
        *size = ramdisk_size;
        return ramdisk_base;
    } else {
        *size = 0;
        return NULL;
    }
}

#if WITH_SMP
static void platform_cpu_init(void) {
    uint cluster_cpus[SMP_CPU_MAX_CLUSTERS];
    mdi_node_ref_t  clusters;

    if (mdi_find_node(&cpu_map, MDI_CPU_MAP_CLUSTERS, &clusters) != NO_ERROR) {
        printf("mdi_find_node couldn't find clusters\n");
        return;
    }

    mdi_node_ref_t  cluster;
    uint32_t cluster_count = 0;

    mdi_each_child(&clusters, &cluster) {
        mdi_node_ref_t node;
        uint8_t cpu_count;

        if (mdi_find_node(&cluster, MDI_CPU_MAP_CLUSTERS_CPU_COUNT, &node) != NO_ERROR) {
            printf("mdi_find_node couldn't find cluster cpu-count\n");
            return;
        }
        if (mdi_node_uint8(&node, &cpu_count) != NO_ERROR) {
            printf("could not read cluster id\n");
            return;
        }

        if (cluster_count >= SMP_CPU_MAX_CLUSTERS) {
            printf("platform_cpu_init: MDI contains more than SMP_CPU_MAX_CLUSTERS clusters\n");
            return;
        }
        cluster_cpus[cluster_count++] = cpu_count;
    }
    arch_init_cpu_map(cluster_count, cluster_cpus);

    for (uint cluster = 0; cluster < cluster_count; cluster++) {
        for (uint cpu = 0; cpu < cluster_cpus[cluster]; cpu++) {
            if (cluster != 0 || cpu != 0) {
                arm64_set_secondary_sp(cluster, cpu,
                        pmm_alloc_kpages(ARCH_DEFAULT_STACK_SIZE / PAGE_SIZE, NULL, NULL));
                psci_cpu_on(cluster, cpu, MEMBASE + KERNEL_LOAD_OFFSET);
            }
        }
    }
}
#endif

static void platform_mdi_init(void) {
    mdi_node_ref_t  root;

    // Look for MDI data in ramdisk bootdata
    size_t offset = 0;
    while (offset < ramdisk_size) {
        bootdata_t* header = (ramdisk_base + offset);

        if (header->magic != BOOTDATA_MAGIC) {
            panic("bad magic in bootdata header\n");
        }
        if (header->type == BOOTDATA_TYPE_MDI) {
            break;
        }
        offset += BOOTDATA_ALIGN(sizeof(*header) + header->insize);
    }
    if (offset >= ramdisk_size) {
        panic("No MDI found in ramdisk\n");
    }

    if (mdi_init(ramdisk_base + offset, ramdisk_size - offset, &root) != NO_ERROR) {
        panic("mdi_init failed\n");
    }

    // search top level nodes for CPU info and kernel drivers
    mdi_node_ref_t  child;
    mdi_each_child(&root, &child) {
        mdi_id_t id = mdi_id(&child);

        if (id == MDI_CPU_MAP) {
            cpu_map = child;
        } else if (id == MDI_KERNEL_DRIVERS) {
            pdev_init(&child);
        }
    }
}

void platform_early_init(void)
{
    read_device_tree(&ramdisk_base, &ramdisk_size, NULL);

    if (!ramdisk_base || !ramdisk_size) {
        panic("no ramdisk!\n");
    }

    platform_mdi_init();

    /* add the main memory arena */
    pmm_add_arena(&arena);

#ifdef BOOTLOADER_RESERVE_START
    /* Allocate memory regions reserved by bootloaders for other functions */
    struct list_node list = LIST_INITIAL_VALUE(list);
    pmm_alloc_range(BOOTLOADER_RESERVE_START, BOOTLOADER_RESERVE_SIZE / PAGE_SIZE, &list);
#endif

    platform_preserve_ramdisk();
}

void platform_init(void)
{
#if WITH_SMP
    platform_cpu_init();
#endif
}

void target_init(void)
{

}

void platform_dputs(const char* str, size_t len)
{
    while (len-- > 0) {
        char c = *str++;
        if (c == '\n') {
            uart_putc(DEBUG_UART, '\r');
        }
        uart_putc(DEBUG_UART, c);
    }
}

int platform_dgetc(char *c, bool wait)
{
    int ret = uart_getc(DEBUG_UART, wait);
    if (ret == -1)
        return -1;
    *c = ret;
    return 0;
}

void platform_pputc(char c)
{
    uart_pputc(DEBUG_UART, c);
}

int platform_pgetc(char *c, bool wait)
{
     int r = uart_pgetc(DEBUG_UART);
     if (r == -1) {
         return -1;
     }

     *c = r;
     return 0;
}

/* stub out the hardware rng entropy generator, which doesn't exist on this platform */
size_t hw_rng_get_entropy(void* buf, size_t len, bool block) {
    return 0;
}

/* no built in framebuffer */
status_t display_get_info(struct display_info *info) {
    return ERR_NOT_FOUND;
}

void platform_halt(platform_halt_action suggested_action, platform_halt_reason reason)
{
    if (suggested_action == HALT_ACTION_REBOOT) {
        psci_system_reset();
#ifdef MSM8998_PSHOLD_PHYS
        // Deassert PSHold
        *REG32(paddr_to_kvaddr(MSM8998_PSHOLD_PHYS)) = 0;
#endif
    } else if (suggested_action == HALT_ACTION_SHUTDOWN) {
        // XXX shutdown seem to not work through psci
        // implement shutdown via pmic
#if 0
        psci_system_off();
#endif
        printf("shutdown is unsupported\n");
    }

#if WITH_LIB_DEBUGLOG
#if WITH_PANIC_BACKTRACE
    thread_print_backtrace(get_current_thread(), __GET_FRAME(0));
#endif
    dlog_bluescreen_halt();
#endif

#if ENABLE_PANIC_SHELL
    if (reason == HALT_REASON_SW_PANIC) {
        dprintf(ALWAYS, "CRASH: starting debug shell... (reason = %u)\n", reason);
        arch_disable_ints();
        panic_shell_start();
    }
#endif  // ENABLE_PANIC_SHELL

    dprintf(ALWAYS, "HALT: spinning forever... (reason = %u)\n", reason);

    // catch all fallthrough cases
    arch_disable_ints();
    for (;;);
}
