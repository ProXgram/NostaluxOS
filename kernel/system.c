#include "system.h"

#include <stddef.h>

#include "heap.h"
#include "syslog.h"

_Static_assert(offsetof(struct BootInfo, framebuffer) == 16,
               "BootInfo framebuffer offset must match stage2");
_Static_assert(offsetof(struct BootInfo, e820_count) == 24,
               "BootInfo E820 count offset must match stage2");
_Static_assert(offsetof(struct BootInfo, e820_map) == 32,
               "BootInfo E820 map offset must match stage2");
_Static_assert(sizeof(struct BootInfo) == 40,
               "BootInfo size must match stage2");

static struct BootInfo g_boot_info = {
    .width = 80,
    .height = 25,
    .pitch = 80 * 2,
    .bpp = 16,
    .framebuffer = 0xB8000,
};

static struct system_profile g_profile = {
    .architecture = "x86_64",
    .memory_total_kb = 64 * 1024,
    .memory_used_kb = 512,
};

static uint32_t bytes_to_kib(uint64_t bytes) {
    uint64_t kib = bytes / 1024u;
    if (bytes % 1024u != 0) {
        kib++;
    }
    if (kib > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)kib;
}

static void refresh_memory_usage(void) {
    uint64_t framebuffer_bytes = (uint64_t)g_boot_info.width * (uint64_t)g_boot_info.height * 2u;
    if (g_boot_info.pitch != 0 && g_boot_info.height != 0) {
        framebuffer_bytes = (uint64_t)g_boot_info.pitch * (uint64_t)g_boot_info.height;
    }

    const uint64_t heap_bytes = (uint64_t)heap_used_space();
    uint64_t used_bytes = framebuffer_bytes;
    if (heap_bytes > UINT64_MAX - used_bytes) {
        used_bytes = UINT64_MAX;
    } else {
        used_bytes += heap_bytes;
    }

    uint32_t kib = bytes_to_kib(used_bytes);
    if (kib < 64) {
        kib = 64;
    }
    g_profile.memory_used_kb = kib;
}

void system_cache_boot_info(const struct BootInfo* boot_info) {
    if (boot_info != NULL) {
        g_boot_info = *boot_info;
    }

    if (g_boot_info.width == 0) {
        g_boot_info.width = 80;
    }
    if (g_boot_info.height == 0) {
        g_boot_info.height = 25;
    }
    if (g_boot_info.pitch == 0) {
        g_boot_info.pitch = g_boot_info.width * 2;
    }
    if (g_boot_info.bpp == 0) {
        g_boot_info.bpp = 16;
    }

    refresh_memory_usage();
    syslog_write("System: hardware descriptors cached");
}

void system_set_total_memory(uint32_t total_kb) {
    g_profile.memory_total_kb = total_kb;
}

const struct BootInfo* system_boot_info(void) {
    return &g_boot_info;
}

const struct system_profile* system_profile_info(void) {
    // Heap use changes after boot, so refresh the snapshot at query time.
    refresh_memory_usage();
    return &g_profile;
}
