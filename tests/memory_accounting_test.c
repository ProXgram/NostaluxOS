#define _GNU_SOURCE

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "heap.h"
#include "memtest.h"
#include "system.h"

#define TEST_MAP_ADDRESS ((void*)(uintptr_t)0x10000)
#define TEST_MAP_BYTES   4096u
#define TEST_ENTRY_COUNT 7u
#define MIB(value)       ((uint64_t)(value) * 1024u * 1024u)
#define GIB(value)       ((uint64_t)(value) * 1024u * 1024u * 1024u)

static size_t g_heap_free_bytes;

size_t heap_free_space(void) {
    return g_heap_free_bytes;
}

size_t heap_used_space(void) {
    return 0;
}

void* kmalloc(size_t size) {
    return malloc(size);
}

void kfree(void* pointer) {
    free(pointer);
}

void syslog_write(const char* message) {
    (void)message;
}

void terminal_writestring(const char* text) {
    (void)text;
}

void terminal_write_uint(unsigned int value) {
    (void)value;
}

static void configure_test_map(struct e820_entry* entries) {
    /*
     * Keep the firmware order deliberately shuffled. The two usable ranges
     * that meet at 12 MiB must normalize into one range before the reserved
     * 16-17 MiB overlap stops heap growth.
     */
    entries[0] = (struct e820_entry){
        .base = GIB(4),
        .length = MIB(64),
        .type = 1,
        .attributes = 1,
    };
    entries[1] = (struct e820_entry){
        .base = MIB(12),
        .length = GIB(2) - MIB(12),
        .type = 1,
        .attributes = 1,
    };
    entries[2] = (struct e820_entry){
        .base = 0,
        .length = 0x9FC00,
        .type = 1,
        .attributes = 1,
    };
    /* Disabled extended-attribute entries must not count. */
    entries[3] = (struct e820_entry){
        .base = GIB(3),
        .length = MIB(64),
        .type = 1,
        .attributes = 0,
    };
    /* Reserved overlap must split the otherwise contiguous heap range. */
    entries[4] = (struct e820_entry){
        .base = MIB(16),
        .length = MIB(1),
        .type = 2,
        .attributes = 1,
    };
    entries[5] = (struct e820_entry){
        .base = 0x9FC00,
        .length = 0x60400,
        .type = 2,
        .attributes = 1,
    };
    entries[6] = (struct e820_entry){
        .base = MIB(1),
        .length = MIB(12) - MIB(1),
        .type = 1,
        .attributes = 1,
    };
}

static void assert_map_bounds_rejected(const struct BootInfo* valid_boot) {
    struct memtest_memory_info memory;
    struct BootInfo invalid_boot = *valid_boot;

    /* The table begins in low memory but its first record crosses 1 MiB. */
    invalid_boot.e820_count = 1;
    invalid_boot.e820_entry_size = sizeof(struct e820_entry);
    invalid_boot.e820_map =
        0x100000u - sizeof(struct e820_entry) + 1u;
    system_cache_boot_info(&invalid_boot);
    assert(!memtest_detect_memory(&memory));
    assert(memory.physical_usable_bytes == 0);
    assert(memory.mapped_usable_bytes == 0);
    assert(memory.reserved_low_usable_bytes == 0);
    assert(memory.heap_upper_limit == 0);

    /* A table may not start at or beyond the low-memory map boundary. */
    invalid_boot.e820_map = 0x100000u;
    system_cache_boot_info(&invalid_boot);
    assert(!memtest_detect_memory(&memory));
    assert(memtest_detect_upper_limit() == 0);
}

int main(void) {
    void* mapped = mmap(TEST_MAP_ADDRESS, TEST_MAP_BYTES,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
    assert(mapped == TEST_MAP_ADDRESS);

    struct e820_entry* entries = (struct e820_entry*)mapped;
    configure_test_map(entries);

    const struct BootInfo boot = {
        .width = 800,
        .height = 600,
        .pitch = 3200,
        .bpp = 32,
        .framebuffer = 0xFD000000,
        .e820_count = TEST_ENTRY_COUNT,
        .e820_entry_size = sizeof(struct e820_entry),
        .e820_map = (uint64_t)(uintptr_t)entries,
    };
    system_cache_boot_info(&boot);

    struct memtest_memory_info memory;
    assert(memtest_detect_memory(&memory));

    const uint64_t conventional_bytes = 0x9FC00;
    const uint64_t expected_physical =
        conventional_bytes + (GIB(2) - MIB(1)) - MIB(1) + MIB(64);
    const uint64_t expected_mapped =
        conventional_bytes + (GIB(1) - MIB(1)) - MIB(1);
    const uint64_t expected_reserved =
        conventional_bytes + (MIB(8) - MIB(1));

    assert(memory.physical_usable_bytes == expected_physical);
    assert(memory.mapped_usable_bytes == expected_mapped);
    assert(memory.reserved_low_usable_bytes == expected_reserved);
    assert(memory.heap_upper_limit == MIB(16));
    assert(memtest_detect_upper_limit() == MIB(16));

    const size_t heap_capacity = (size_t)MIB(16);
    g_heap_free_bytes = heap_capacity - 32u;
    system_configure_memory(memory.physical_usable_bytes,
                            memory.mapped_usable_bytes,
                            memory.reserved_low_usable_bytes,
                            heap_capacity);

    const struct system_profile* profile = system_profile_info();
    assert(profile->memory_total_kb == expected_physical / 1024u);
    assert(profile->memory_mapped_kb == expected_mapped / 1024u);
    assert(profile->memory_managed_kb == heap_capacity / 1024u);
    assert(profile->memory_reserved_kb == expected_reserved / 1024u);
    assert(profile->memory_heap_committed_kb == 1u);
    assert(profile->memory_used_kb ==
           expected_reserved / 1024u + 1u);

    /* Allocator headers are included because accounting uses total - free. */
    g_heap_free_bytes = heap_capacity - 4160u;
    profile = system_profile_info();
    assert(profile->memory_heap_committed_kb == 5u);
    assert(profile->memory_used_kb ==
           expected_reserved / 1024u + 5u);

    assert_map_bounds_rejected(&boot);

    assert(munmap(mapped, TEST_MAP_BYTES) == 0);
    puts("memory accounting tests passed");
    return 0;
}
