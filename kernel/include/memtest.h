#ifndef MEMTEST_H
#define MEMTEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct memtest_memory_info {
    /*
     * Sum of all usable E820 ranges. This describes physical RAM even when a
     * range is above the kernel's current low-memory identity map.
     */
    uint64_t physical_usable_bytes;

    /* Usable RAM that is currently addressable through the low 1 GiB map. */
    uint64_t mapped_usable_bytes;

    /*
     * Usable E820 bytes below the 8 MiB heap base. The current fixed layout
     * reserves these bytes for the loader, kernel, stacks, and graphics.
     */
    uint64_t reserved_low_usable_bytes;

    /*
     * Upper end of the contiguous usable range containing the heap base,
     * capped to the current identity map.
     */
    size_t heap_upper_limit;
};

/*
 * Normalizes the BIOS E820 map, with reserved ranges taking precedence over
 * overlapping usable ranges, and fills a truthful physical-memory summary.
 */
bool memtest_detect_memory(struct memtest_memory_info* info);

/*
 * Returns the upper end of the contiguous BIOS E820 usable-RAM range that
 * contains the kernel heap base at 8 MiB, capped to the 1 GiB identity map.
 * No unknown physical address is read or modified.
 */
size_t memtest_detect_upper_limit(void);

/*
 * Runs a read/write pattern test on a specific range of memory.
 * Returns true if the test passes, false if verification failed.
 */
bool memtest_region(uintptr_t start, size_t size);

/*
 * Shell command handler to run a verbose memory diagnostic.
 */
void memtest_run_diagnostic(void);

#endif /* MEMTEST_H */
