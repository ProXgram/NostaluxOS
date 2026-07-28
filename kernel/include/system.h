#ifndef SYSTEM_H
#define SYSTEM_H

#include <stddef.h>
#include <stdint.h>

/*
 * The current fixed physical layout keeps the loader, kernel, stacks, page
 * tables, and graphics backbuffer below 8 MiB. The heap starts immediately
 * after this region.
 */
#define SYSTEM_RESERVED_LOW_MEMORY_BYTES 0x00800000ull

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} __attribute__((packed));

struct BootInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t framebuffer;
    uint32_t e820_count;
    uint32_t e820_entry_size;
    uint64_t e820_map;
};

struct system_profile {
    const char* architecture;
    /* All physical RAM reported usable by the normalized E820 map. */
    uint64_t memory_total_kb;
    /* Portion of usable RAM addressable by the current low identity map. */
    uint64_t memory_mapped_kb;
    /* Size of the RAM region managed by the kernel heap allocator. */
    uint64_t memory_managed_kb;
    /* Usable low RAM reserved by the fixed kernel/graphics layout. */
    uint64_t memory_reserved_kb;
    /* Heap allocations plus allocator metadata, excluding free payload. */
    uint64_t memory_heap_committed_kb;
    /* Reserved low RAM plus committed heap RAM. */
    uint64_t memory_used_kb;
};

void system_cache_boot_info(const struct BootInfo* boot_info);
/*
 * Configures RAM accounting from the normalized E820 map and heap layout.
 * fixed_used_bytes must count only usable RAM reserved by the fixed layout;
 * firmware/MMIO holes are not part of either total or used RAM.
 */
void system_configure_memory(uint64_t total_usable_bytes,
                             uint64_t mapped_usable_bytes,
                             uint64_t fixed_used_bytes,
                             size_t heap_capacity_bytes);
const struct BootInfo* system_boot_info(void);
const struct system_profile* system_profile_info(void);

#endif /* SYSTEM_H */
