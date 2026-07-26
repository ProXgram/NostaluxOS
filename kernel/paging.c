#include "paging.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "syslog.h"
#include "system.h"

#define PAGE_PRESENT (1ull << 0)
#define PAGE_RW      (1ull << 1)
#define PAGE_PS      (1ull << 7) 

#define PAGE_SIZE       0x1000ull
#define HUGE_PAGE_SIZE  0x200000ull
#define PDPT_SPAN        (1ull << 30)
#define PML4_SPAN        (1ull << 39)
#define FRAMEBUFFER_PD_TABLES 2u

extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __bss_end[];

static uint64_t g_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_kernel_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_framebuffer_pd[FRAMEBUFFER_PD_TABLES][512] __attribute__((aligned(PAGE_SIZE)));

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

static bool map_framebuffer(const struct BootInfo* boot_info, uint64_t flags) {
    if (boot_info == NULL || boot_info->framebuffer == 0) {
        return true;
    }

    if (boot_info->pitch == 0 || boot_info->height == 0) {
        syslog_write("Paging: rejected framebuffer with empty dimensions");
        return false;
    }

    const uint64_t framebuffer_start = boot_info->framebuffer;
    const uint64_t framebuffer_size =
        (uint64_t)boot_info->pitch * (uint64_t)boot_info->height;

    // PML4[0] covers the first 512 GiB. Validate the complete inclusive
    // framebuffer range before installing any of its page-table entries.
    if (framebuffer_size == 0 || framebuffer_start >= PML4_SPAN ||
        framebuffer_size - 1u > (PML4_SPAN - 1u) - framebuffer_start) {
        syslog_write("Paging: rejected framebuffer outside the identity-map range");
        return false;
    }

    const uint64_t framebuffer_end = framebuffer_start + framebuffer_size - 1u;
    const size_t first_pdpt = (size_t)(framebuffer_start / PDPT_SPAN);
    const size_t last_pdpt = (size_t)(framebuffer_end / PDPT_SPAN);

    // PDPT[0] already points at the kernel's identity-map directory. Two
    // additional directories are enough even when a normal framebuffer
    // straddles a 1 GiB boundary; reject larger untrusted ranges atomically.
    size_t first_dynamic_pdpt = 1;
    size_t dynamic_pd_count = 0;
    if (last_pdpt >= 1) {
        first_dynamic_pdpt = first_pdpt == 0 ? 1 : first_pdpt;
        dynamic_pd_count = last_pdpt - first_dynamic_pdpt + 1;
    }
    if (dynamic_pd_count > FRAMEBUFFER_PD_TABLES) {
        syslog_write("Paging: rejected oversized framebuffer mapping");
        return false;
    }

    for (size_t slot = 0; slot < dynamic_pd_count; slot++) {
        for (size_t entry = 0; entry < 512; entry++) {
            g_framebuffer_pd[slot][entry] = 0;
        }
        const size_t pdpt_index = first_dynamic_pdpt + slot;
        g_pdpt[pdpt_index] = (uint64_t)g_framebuffer_pd[slot] | flags;
    }

    const uint64_t first_page = align_down(framebuffer_start, HUGE_PAGE_SIZE);
    for (uint64_t page_phys = first_page;
         page_phys <= framebuffer_end;
         page_phys += HUGE_PAGE_SIZE) {
        const size_t pdpt_index = (size_t)(page_phys / PDPT_SPAN);
        if (pdpt_index == 0) {
            // The low 1 GiB is already fully identity-mapped by g_pd.
            continue;
        }

        const size_t slot = pdpt_index - first_dynamic_pdpt;
        const size_t pd_index = (size_t)((page_phys >> 21) & 0x1FFu);
        g_framebuffer_pd[slot][pd_index] = page_phys | flags | PAGE_PS;
    }

    return true;
}

static bool initialize_identity_map(const struct BootInfo* boot_info) {
    // Kernel identity mappings are supervisor-only. User tasks stay disabled
    // until the kernel can create explicit, isolated user mappings.
    uint64_t flags = PAGE_PRESENT | PAGE_RW;

    g_pml4[0] = (uint64_t)g_pdpt | flags;
    g_pdpt[0] = (uint64_t)g_pd | flags;

    // 0-1GB
    g_pd[0] = (uint64_t)g_kernel_pt | flags;
    for (size_t i = 1; i < 512; i++) {
        uint64_t base = (uint64_t)i * HUGE_PAGE_SIZE;
        g_pd[i] = base | flags | PAGE_PS;
    }

    // 0-2MB (4KB pages)
    for (size_t i = 0; i < 512; i++) {
        uint64_t base = (uint64_t)i * PAGE_SIZE;
        g_kernel_pt[i] = base | flags;
    }

    return map_framebuffer(boot_info, flags);
}

static void load_new_tables(void) {
    uint64_t pml4_phys = (uint64_t)g_pml4;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

void paging_init(const struct BootInfo* boot_info) {
    if (!initialize_identity_map(boot_info)) {
        syslog_write("Paging: framebuffer mapping failed; system halted");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    load_new_tables();
    syslog_write("Paging: Initialized (Supervisor Identity Map)");
}
