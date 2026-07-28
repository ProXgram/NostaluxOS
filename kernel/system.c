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
};

static uint64_t g_total_usable_bytes = 0;
static uint64_t g_mapped_usable_bytes = 0;
static uint64_t g_fixed_used_bytes = 0;
static size_t g_heap_capacity_bytes = 0;

static uint64_t bytes_to_kib_floor(uint64_t bytes) {
    return bytes / 1024u;
}

static uint64_t bytes_to_kib_ceil(uint64_t bytes) {
    uint64_t kib = bytes / 1024u;
    if (bytes % 1024u != 0) {
        if (kib != UINT64_MAX) {
            kib++;
        }
    }
    return kib;
}

static void refresh_memory_usage(void) {
    /*
     * The heap allocator reports free payload bytes. Subtracting that from the
     * full heap region counts live allocations and allocator block metadata,
     * including the initial header, without pretending the untouched part of
     * the heap is used.
     */
    uint64_t heap_committed_bytes = 0;
    if (g_heap_capacity_bytes != 0) {
        const size_t heap_free_bytes = heap_free_space();
        if (heap_free_bytes <= g_heap_capacity_bytes) {
            heap_committed_bytes =
                (uint64_t)(g_heap_capacity_bytes - heap_free_bytes);
        }
    }

    uint64_t used_bytes = g_fixed_used_bytes;
    if (heap_committed_bytes > UINT64_MAX - used_bytes) {
        used_bytes = UINT64_MAX;
    } else {
        used_bytes += heap_committed_bytes;
    }

    if (used_bytes > g_total_usable_bytes) {
        used_bytes = g_total_usable_bytes;
    }

    g_profile.memory_total_kb = bytes_to_kib_floor(g_total_usable_bytes);
    g_profile.memory_mapped_kb = bytes_to_kib_floor(g_mapped_usable_bytes);
    g_profile.memory_managed_kb =
        bytes_to_kib_floor((uint64_t)g_heap_capacity_bytes);
    g_profile.memory_reserved_kb =
        bytes_to_kib_ceil(g_fixed_used_bytes);
    g_profile.memory_heap_committed_kb =
        bytes_to_kib_ceil(heap_committed_bytes);
    g_profile.memory_used_kb = bytes_to_kib_ceil(used_bytes);
    if (g_profile.memory_used_kb > g_profile.memory_total_kb) {
        g_profile.memory_used_kb = g_profile.memory_total_kb;
    }
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

void system_configure_memory(uint64_t total_usable_bytes,
                             uint64_t mapped_usable_bytes,
                             uint64_t fixed_used_bytes,
                             size_t heap_capacity_bytes) {
    g_total_usable_bytes = total_usable_bytes;
    g_mapped_usable_bytes = mapped_usable_bytes;
    if (g_mapped_usable_bytes > g_total_usable_bytes) {
        g_mapped_usable_bytes = g_total_usable_bytes;
    }
    g_fixed_used_bytes = fixed_used_bytes;
    if (g_fixed_used_bytes > g_total_usable_bytes) {
        g_fixed_used_bytes = g_total_usable_bytes;
    }
    g_heap_capacity_bytes = heap_capacity_bytes;
    refresh_memory_usage();
}

const struct BootInfo* system_boot_info(void) {
    return &g_boot_info;
}

const struct system_profile* system_profile_info(void) {
    // Heap use changes after boot, so refresh the snapshot at query time.
    refresh_memory_usage();
    return &g_profile;
}
