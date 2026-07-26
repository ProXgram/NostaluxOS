#include "heap.h"
#include "syslog.h"
#include <stdbool.h>

// Header for each block
// We use padding to ensure the structure size is a multiple of 16.
// This guarantees that if the block starts at 16-aligned address, 
// the data payload following it is also 16-aligned.
struct heap_block {
    size_t size;        // 8 bytes
    bool is_free;       // 1 byte
    struct heap_block* next; // 8 bytes
    // Pad to 32 bytes total size (16-byte aligned)
    // 8 + 1 + 8 = 17 used. Need 15 bytes padding.
    uint8_t padding[15];
} __attribute__((aligned(16)));

static struct heap_block* g_head = NULL;
static size_t g_heap_total_size = 0;
static size_t g_heap_used_size = 0;
static uintptr_t g_heap_start = 0;
static uintptr_t g_heap_end = 0;

void heap_init(void* start_addr, size_t size_bytes) {
    g_head = NULL;
    g_heap_total_size = 0;
    g_heap_used_size = 0;
    g_heap_start = 0;
    g_heap_end = 0;

    // 1. Align the start address to 16 bytes
    uintptr_t addr = (uintptr_t)start_addr;
    size_t misalignment = addr % 16;
    if (misalignment != 0) {
        size_t adjustment = 16 - misalignment;
        if (size_bytes <= adjustment) return; // Too small
        start_addr = (void*)(addr + adjustment);
        size_bytes -= adjustment;
    }

    if (size_bytes < sizeof(struct heap_block)) {
        syslog_write("Heap: Too small to initialize");
        return;
    }

    g_head = (struct heap_block*)start_addr;
    g_head->size = size_bytes - sizeof(struct heap_block);
    g_head->is_free = true;
    g_head->next = NULL;
    g_heap_total_size = size_bytes;
    g_heap_start = (uintptr_t)start_addr;
    g_heap_end = g_heap_start + size_bytes;
    
    syslog_write("Heap: Initialized (16-byte aligned)");
}

void* kmalloc(size_t size) {
    if (size == 0 || g_head == NULL) return NULL;

    // Align requested size to 16 bytes
    if (size > SIZE_MAX - 15u) {
        syslog_write("Heap: allocation size overflow");
        return NULL;
    }
    size_t aligned_size = (size + 15u) & ~(size_t)15u;
    
    struct heap_block* curr = g_head;
    while (curr) {
        if (curr->is_free && curr->size >= aligned_size) {
            // Found a fit. Check if we can split
            if (curr->size >= aligned_size + sizeof(struct heap_block) + 16) {
                struct heap_block* new_block = (struct heap_block*)((uint8_t*)curr + sizeof(struct heap_block) + aligned_size);
                
                new_block->size = curr->size - aligned_size - sizeof(struct heap_block);
                new_block->is_free = true;
                new_block->next = curr->next;
                
                curr->size = aligned_size;
                curr->next = new_block;
            }
            curr->is_free = false;
            g_heap_used_size += curr->size;
            
            // Return pointer to data payload
            return (void*)((uint8_t*)curr + sizeof(struct heap_block));
        }
        curr = curr->next;
    }
    
    syslog_write("Heap: Out of memory");
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;

    uintptr_t address = (uintptr_t)ptr;
    if (address < g_heap_start + sizeof(struct heap_block) || address >= g_heap_end ||
        (address & 0xFu) != 0) {
        syslog_write("Heap: rejected invalid free");
        return;
    }

    // Accept only exact payload pointers that belong to the allocator list.
    struct heap_block* block = NULL;
    for (struct heap_block* curr = g_head; curr != NULL; curr = curr->next) {
        if ((void*)((uint8_t*)curr + sizeof(struct heap_block)) == ptr) {
            block = curr;
            break;
        }
    }
    if (block == NULL || block->is_free) {
        syslog_write("Heap: rejected invalid or duplicate free");
        return;
    }

    if (block->size <= g_heap_used_size) {
        g_heap_used_size -= block->size;
    } else {
        // Keep public accounting conservative if allocator metadata was
        // corrupted despite the exact-pointer validation above.
        g_heap_used_size = 0;
        syslog_write("Heap: allocation accounting recovered");
    }
    block->is_free = true;

    // Merge with next if free
    if (block->next && block->next->is_free) {
        block->size += sizeof(struct heap_block) + block->next->size;
        block->next = block->next->next;
    }
    
    // Global merge pass (simple defragmentation)
    struct heap_block* curr = g_head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct heap_block) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

size_t heap_free_space(void) {
    size_t total = 0;
    struct heap_block* curr = g_head;
    while (curr) {
        if (curr->is_free) total += curr->size;
        curr = curr->next;
    }
    return total;
}

size_t heap_used_space(void) {
    return g_heap_used_size;
}
