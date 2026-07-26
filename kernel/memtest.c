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

size_t memtest_detect_upper_limit(void) {
    const struct BootInfo* boot = system_boot_info();
    if (!boot || boot->e820_count == 0 ||
        boot->e820_count > E820_MAX_ENTRIES ||
        boot->e820_entry_size < 20 || boot->e820_entry_size > 64 ||
        boot->e820_map < 0x1000 || boot->e820_map >= 0x100000) {
        syslog_write("MemTest: BIOS E820 map is unavailable or invalid");
        return 0;
    }

    const uint8_t* map = (const uint8_t*)(uintptr_t)boot->e820_map;
    uint64_t limit = HEAP_BASE_ADDR;
    bool found_heap_base = false;

    /*
     * Find the usable entry containing the heap base, then extend only through
     * usable entries that overlap or directly adjoin it. Reserved gaps are
     * never probed or treated as RAM.
     */
    for (uint32_t pass = 0; pass < boot->e820_count; pass++) {
        bool extended = false;
        for (uint32_t i = 0; i < boot->e820_count; i++) {
            const struct e820_entry* entry =
                (const struct e820_entry*)(const void*)(map +
                    (size_t)i * boot->e820_entry_size);
            if (entry->type != E820_TYPE_USABLE || entry->length == 0) continue;
            if (boot->e820_entry_size >= 24 && (entry->attributes & 1u) == 0) continue;

            uint64_t end = entry->base + entry->length;
            if (end < entry->base) end = UINT64_MAX;
            if (!found_heap_base) {
                if (entry->base <= HEAP_BASE_ADDR && end > HEAP_BASE_ADDR) {
                    found_heap_base = true;
                    limit = end;
                    extended = true;
                }
            } else if (entry->base <= limit && end > limit) {
                limit = end;
                extended = true;
            }
        }
        if (!extended) break;
    }

    if (!found_heap_base) return 0;
    if (limit > MAPPED_RAM_LIMIT) limit = MAPPED_RAM_LIMIT;
    return (size_t)limit;
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
    
    terminal_writestring("Detected RAM Limit: ");
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
