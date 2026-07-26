#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>

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
    uint32_t memory_total_kb;
    uint32_t memory_used_kb;
};

void system_cache_boot_info(const struct BootInfo* boot_info);
void system_set_total_memory(uint32_t total_kb);
const struct BootInfo* system_boot_info(void);
const struct system_profile* system_profile_info(void);

#endif /* SYSTEM_H */
