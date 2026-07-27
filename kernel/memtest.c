#include "memtest.h"

#include <stdbool.h>
#include <stdint.h>

#include "heap.h"
#include "system.h"
#include "syslog.h"
#include "terminal.h"

#define HEAP_BASE_ADDR  (8ull * 1024ull * 1024ull)
#define MAPPED_RAM_LIMIT (1024ull * 1024ull * 1024ull)
#define E820_TYPE_USABLE 1u
#define E820_MAX_ENTRIES 64u
#define E820_MAX_ENDPOINTS (E820_MAX_ENTRIES * 2u)

static uint64_t range_end(const struct e820_entry* entry) {
    if (entry->length > UINT64_MAX - entry->base) {
        return UINT64_MAX;
    }
    return entry->base + entry->length;
}

static bool entry_is_enabled(const struct e820_entry* entry,
                             uint32_t entry_size) {
    return entry->length != 0 &&
           (entry_size < 24u || (entry->attributes & 1u) != 0);
}

static uint64_t saturating_add(uint64_t left, uint64_t right) {
    if (right > UINT64_MAX - left) return UINT64_MAX;
    return left + right;
}

static uint64_t intersection_size(uint64_t start, uint64_t end,
                                  uint64_t limit_start,
                                  uint64_t limit_end) {
    if (start < limit_start) start = limit_start;
    if (end > limit_end) end = limit_end;
    return end > start ? end - start : 0;
}

bool memtest_detect_memory(struct memtest_memory_info* info) {
    if (info == NULL) return false;
    *info = (struct memtest_memory_info){0};

    const struct BootInfo* boot = system_boot_info();
    if (!boot || boot->e820_count == 0 ||
        boot->e820_count > E820_MAX_ENTRIES ||
        boot->e820_entry_size < 20 || boot->e820_entry_size > 64 ||
        boot->e820_map < 0x1000 || boot->e820_map >= 0x100000) {
        syslog_write("MemTest: BIOS E820 map is unavailable or invalid");
        return false;
    }

    const uint64_t map_bytes =
        (uint64_t)boot->e820_count * boot->e820_entry_size;
    if (map_bytes > 0x100000u - boot->e820_map) {
        syslog_write("MemTest: BIOS E820 map extends outside low memory");
        return false;
    }

    const uint8_t* map = (const uint8_t*)(uintptr_t)boot->e820_map;
    uint64_t endpoints[E820_MAX_ENDPOINTS];
    size_t endpoint_count = 0;

    /*
     * Every place where E820 coverage can change is an interval endpoint.
     * Sorting these points lets us normalize unsorted and overlapping firmware
     * entries without allocating memory before the heap exists.
     */
    for (uint32_t i = 0; i < boot->e820_count; i++) {
        const struct e820_entry* entry =
            (const struct e820_entry*)(const void*)(map +
                (size_t)i * boot->e820_entry_size);
        if (!entry_is_enabled(entry, boot->e820_entry_size)) continue;

        const uint64_t end = range_end(entry);
        if (end <= entry->base) continue;
        endpoints[endpoint_count++] = entry->base;
        endpoints[endpoint_count++] = end;
    }

    if (endpoint_count < 2) {
        syslog_write("MemTest: BIOS E820 map contains no enabled ranges");
        return false;
    }

    for (size_t i = 1; i < endpoint_count; i++) {
        const uint64_t value = endpoints[i];
        size_t position = i;
        while (position > 0 && endpoints[position - 1] > value) {
            endpoints[position] = endpoints[position - 1];
            position--;
        }
        endpoints[position] = value;
    }

    size_t unique_count = 0;
    for (size_t i = 0; i < endpoint_count; i++) {
        if (unique_count == 0 ||
            endpoints[i] != endpoints[unique_count - 1]) {
            endpoints[unique_count++] = endpoints[i];
        }
    }

    bool found_heap_base = false;
    uint64_t heap_limit = HEAP_BASE_ADDR;

    for (size_t point = 0; point + 1 < unique_count; point++) {
        const uint64_t start = endpoints[point];
        const uint64_t end = endpoints[point + 1];
        bool covered_by_usable = false;
        bool covered_by_reserved = false;

        for (uint32_t i = 0; i < boot->e820_count; i++) {
            const struct e820_entry* entry =
                (const struct e820_entry*)(const void*)(map +
                    (size_t)i * boot->e820_entry_size);
            if (!entry_is_enabled(entry, boot->e820_entry_size)) continue;

            const uint64_t entry_end = range_end(entry);
            if (entry->base >= end || entry_end <= start) continue;
            if (entry->type == E820_TYPE_USABLE) {
                covered_by_usable = true;
            } else {
                covered_by_reserved = true;
            }
        }

        /*
         * Firmware-reserved coverage wins if the map contains conflicting
         * overlap. Never count or probe an ambiguous segment as RAM.
         */
        if (!covered_by_usable || covered_by_reserved) continue;

        const uint64_t bytes = end - start;
        info->physical_usable_bytes =
            saturating_add(info->physical_usable_bytes, bytes);
        info->mapped_usable_bytes = saturating_add(
            info->mapped_usable_bytes,
            intersection_size(start, end, 0, MAPPED_RAM_LIMIT));
        info->reserved_low_usable_bytes = saturating_add(
            info->reserved_low_usable_bytes,
            intersection_size(start, end, 0, HEAP_BASE_ADDR));

        if (!found_heap_base &&
            start <= HEAP_BASE_ADDR && end > HEAP_BASE_ADDR) {
            found_heap_base = true;
            heap_limit = end;
            if (heap_limit > MAPPED_RAM_LIMIT) {
                heap_limit = MAPPED_RAM_LIMIT;
            }
        } else if (found_heap_base && start <= heap_limit &&
                   end > heap_limit && heap_limit < MAPPED_RAM_LIMIT) {
            heap_limit = end;
            if (heap_limit > MAPPED_RAM_LIMIT) {
                heap_limit = MAPPED_RAM_LIMIT;
            }
        }
    }

    if (found_heap_base) {
        info->heap_upper_limit = (size_t)heap_limit;
    }

    if (info->physical_usable_bytes == 0) {
        syslog_write("MemTest: BIOS E820 map contains no usable RAM");
        return false;
    }
    return true;
}

size_t memtest_detect_upper_limit(void) {
    struct memtest_memory_info info;
    if (!memtest_detect_memory(&info)) return 0;
    return info.heap_upper_limit;
}

bool memtest_region(uintptr_t start, size_t size) {
    volatile uint8_t* ptr = (volatile uint8_t*)start;
    bool ok = true;

    for (size_t i = 0; i < size; i++) {
        uint8_t original = ptr[i];
        
        /* Simple invert test */
        ptr[i] = 0xAA;
        if (ptr[i] != 0xAA) {
            ok = false;
            ptr[i] = original; /* Try to restore */
            break;
        }

        ptr[i] = 0x55;
        if (ptr[i] != 0x55) {
            ok = false;
            ptr[i] = original;
            break;
        }

        ptr[i] = original;
    }

    return ok;
}

void memtest_run_diagnostic(void) {
    terminal_writestring("Starting non-destructive RAM diagnostics...\n");

    size_t upper_limit = memtest_detect_upper_limit();
    
    terminal_writestring("Contiguous mapped heap limit: ");
    terminal_write_uint((unsigned int)(upper_limit / 1024 / 1024));
    terminal_writestring(" MB\n");

    /*
     * Test only a private heap allocation. The physical memory layout comes
     * from E820, so this diagnostic never writes to unknown physical ranges.
     */
    const size_t scratch_size = 64 * 1024;
    void* scratch = kmalloc(scratch_size);
    if (scratch == NULL) {
        terminal_writestring("Unable to reserve diagnostic scratch memory.\n");
        syslog_write("MemTest: scratch allocation failed");
        return;
    }

    terminal_writestring("Testing a reserved 64 KB scratch region...\n");
    bool ok = memtest_region((uintptr_t)scratch, scratch_size);
    kfree(scratch);

    if (ok) {
        terminal_writestring("Memory integrity sample: OK.\n");
        syslog_write("MemTest: passed successfully");
    } else {
        terminal_writestring("Memory errors detected in scratch region!\n");
        syslog_write("MemTest: errors detected");
    }
}
