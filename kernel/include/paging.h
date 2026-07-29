#ifndef PAGING_H
#define PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "system.h"

/*
 * Keep application virtual memory in PML4 slots 2 and 3.  The kernel's
 * identity map and framebuffer live in slot 0, so an application mapping can
 * never replace a virtual address that kernel code relies on while running
 * under the application's CR3.
 */
#define PAGING_USER_BASE  0x0000010000000000ull
#define PAGING_USER_LIMIT 0x0000020000000000ull
#define PAGING_PAGE_SIZE  0x1000ull

struct paging_address_space;

void paging_init(const struct BootInfo* boot_info);

/*
 * A NULL address space passed to paging_activate() selects the kernel address
 * space.  Each non-NULL address space owns its page tables and user frames;
 * kernel mappings are shared and remain supervisor-only.
 */
struct paging_address_space* paging_address_space_create(void);
void paging_address_space_destroy(struct paging_address_space* space);
void paging_activate(struct paging_address_space* space);
uint64_t paging_address_space_cr3(const struct paging_address_space* space);
uint64_t paging_kernel_cr3(void);
bool paging_nx_available(void);

/*
 * Installs/removes a supervisor-only, non-present 4 KiB guard page. The page
 * must be page-aligned and either belong to the low kernel image mapping or
 * the fixed managed-heap range. These calls are idempotent.
 */
bool paging_kernel_guard_page(void* page);
bool paging_kernel_unguard_page(void* page);

/*
 * Maps fresh zeroed pages. address and size must be page-aligned and wholly
 * inside [PAGING_USER_BASE, PAGING_USER_LIMIT). Existing mappings are never
 * replaced.
 */
bool paging_user_map_anonymous(struct paging_address_space* space,
                               uint64_t address,
                               size_t size,
                               bool writable,
                               bool executable);
/*
 * Removes an exact range of process-owned anonymous leaf mappings and
 * releases their frames. The full range is validated before any PTE changes;
 * shared kernel mappings and process page-table pages are never freed.
 */
bool paging_user_unmap_anonymous(struct paging_address_space* space,
                                 uint64_t address,
                                 size_t size);

/*
 * Checked user copies walk the supplied page tables rather than dereferencing
 * an untrusted virtual address in the kernel. copy_to_user additionally
 * requires writable leaf mappings. A zero-length copy succeeds.
 */
bool paging_copy_to_user(const struct paging_address_space* space,
                         uint64_t user_destination,
                         const void* kernel_source,
                         size_t count);
/*
 * Loader-only variant: requires present user mappings but may initialize a
 * final read-only code page. Call only before the address space is scheduled.
 */
bool paging_initialize_user_memory(
    const struct paging_address_space* space,
    uint64_t user_destination,
    const void* kernel_source,
    size_t count);
bool paging_copy_from_user(const struct paging_address_space* space,
                           void* kernel_destination,
                           uint64_t user_source,
                           size_t count);
bool paging_user_range_mapped(const struct paging_address_space* space,
                              uint64_t address,
                              size_t count,
                              bool require_writable);

#endif /* PAGING_H */
