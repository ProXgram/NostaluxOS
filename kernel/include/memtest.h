#ifndef MEMTEST_H
#define MEMTEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
