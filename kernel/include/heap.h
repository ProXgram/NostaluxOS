#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

void heap_init(void* start_addr, size_t size_bytes);
void* kmalloc(size_t size);
void kfree(void* ptr);

// Helper to check heap health
size_t heap_free_space(void);
size_t heap_used_space(void);

#endif
