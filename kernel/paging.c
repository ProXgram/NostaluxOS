#include "paging.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "heap.h"
#include "syslog.h"
#include "system.h"

#define PAGE_PRESENT (1ull << 0)
#define PAGE_RW      (1ull << 1)
#define PAGE_USER    (1ull << 2)
#define PAGE_PS      (1ull << 7) 
#define PAGE_OWNED   (1ull << 9)
#define PAGE_NX      (1ull << 63)
#define PAGE_ADDRESS_MASK 0x000FFFFFFFFFF000ull

#define PAGE_SIZE       0x1000ull
#define HUGE_PAGE_SIZE  0x200000ull
#define PDPT_SPAN        (1ull << 30)
#define PML4_SPAN        (1ull << 39)
#define FRAMEBUFFER_PD_TABLES 2u

extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __bss_end[];

static uint64_t g_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_kernel_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t g_framebuffer_pd[FRAMEBUFFER_PD_TABLES][512] __attribute__((aligned(PAGE_SIZE)));
static bool g_paging_ready = false;
static bool g_nx_enabled = false;

struct paging_allocation {
    void* raw;
    uint64_t page;
    struct paging_allocation* next;
};

struct paging_address_space {
    uint64_t* pml4;
    struct paging_allocation* allocations;
};

static void clear_bytes(void* destination, size_t count) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void copy_bytes(void* destination, const void* source, size_t count) {
    uint8_t* out = (uint8_t*)destination;
    const uint8_t* in = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        out[index] = in[index];
    }
}

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

static bool map_framebuffer(const struct BootInfo* boot_info, uint64_t flags) {
    if (boot_info == NULL || boot_info->framebuffer == 0) {
        return true;
    }

    if (boot_info->pitch == 0 || boot_info->height == 0) {
        syslog_write("Paging: rejected framebuffer with empty dimensions");
        return false;
    }

    const uint64_t framebuffer_start = boot_info->framebuffer;
    const uint64_t framebuffer_size =
        (uint64_t)boot_info->pitch * (uint64_t)boot_info->height;

    // PML4[0] covers the first 512 GiB. Validate the complete inclusive
    // framebuffer range before installing any of its page-table entries.
    if (framebuffer_size == 0 || framebuffer_start >= PML4_SPAN ||
        framebuffer_size - 1u > (PML4_SPAN - 1u) - framebuffer_start) {
        syslog_write("Paging: rejected framebuffer outside the identity-map range");
        return false;
    }

    const uint64_t framebuffer_end = framebuffer_start + framebuffer_size - 1u;
    const size_t first_pdpt = (size_t)(framebuffer_start / PDPT_SPAN);
    const size_t last_pdpt = (size_t)(framebuffer_end / PDPT_SPAN);

    // PDPT[0] already points at the kernel's identity-map directory. Two
    // additional directories are enough even when a normal framebuffer
    // straddles a 1 GiB boundary; reject larger untrusted ranges atomically.
    size_t first_dynamic_pdpt = 1;
    size_t dynamic_pd_count = 0;
    if (last_pdpt >= 1) {
        first_dynamic_pdpt = first_pdpt == 0 ? 1 : first_pdpt;
        dynamic_pd_count = last_pdpt - first_dynamic_pdpt + 1;
    }
    if (dynamic_pd_count > FRAMEBUFFER_PD_TABLES) {
        syslog_write("Paging: rejected oversized framebuffer mapping");
        return false;
    }

    for (size_t slot = 0; slot < dynamic_pd_count; slot++) {
        for (size_t entry = 0; entry < 512; entry++) {
            g_framebuffer_pd[slot][entry] = 0;
        }
        const size_t pdpt_index = first_dynamic_pdpt + slot;
        g_pdpt[pdpt_index] = (uint64_t)g_framebuffer_pd[slot] | flags;
    }

    const uint64_t first_page = align_down(framebuffer_start, HUGE_PAGE_SIZE);
    for (uint64_t page_phys = first_page;
         page_phys <= framebuffer_end;
         page_phys += HUGE_PAGE_SIZE) {
        const size_t pdpt_index = (size_t)(page_phys / PDPT_SPAN);
        if (pdpt_index == 0) {
            // The low 1 GiB is already fully identity-mapped by g_pd.
            continue;
        }

        const size_t slot = pdpt_index - first_dynamic_pdpt;
        const size_t pd_index = (size_t)((page_phys >> 21) & 0x1FFu);
        g_framebuffer_pd[slot][pd_index] = page_phys | flags | PAGE_PS;
    }

    return true;
}

static bool initialize_identity_map(const struct BootInfo* boot_info) {
    // Kernel identity mappings are supervisor-only. User tasks stay disabled
    // until the kernel can create explicit, isolated user mappings.
    uint64_t flags = PAGE_PRESENT | PAGE_RW;

    g_pml4[0] = (uint64_t)g_pdpt | flags;
    g_pdpt[0] = (uint64_t)g_pd | flags;

    // 0-1GB
    g_pd[0] = (uint64_t)g_kernel_pt | flags;
    for (size_t i = 1; i < 512; i++) {
        uint64_t base = (uint64_t)i * HUGE_PAGE_SIZE;
        g_pd[i] = base | flags | PAGE_PS;
    }

    // 0-2MB (4KB pages)
    for (size_t i = 0; i < 512; i++) {
        uint64_t base = (uint64_t)i * PAGE_SIZE;
        g_kernel_pt[i] = base | flags;
    }

    return map_framebuffer(boot_info, flags);
}

static void load_new_tables(void) {
    uint64_t pml4_phys = (uint64_t)g_pml4;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static void detect_and_enable_nx(void) {
    uint32_t maximum_extended = 0;
    uint32_t ignored_b = 0;
    uint32_t ignored_c = 0;
    uint32_t ignored_d = 0;
    __asm__ volatile(
        "cpuid"
        : "=a"(maximum_extended), "=b"(ignored_b),
          "=c"(ignored_c), "=d"(ignored_d)
        : "a"(0x80000000u), "c"(0u));
    (void)ignored_b;
    (void)ignored_c;
    (void)ignored_d;
    if (maximum_extended < 0x80000001u) return;

    uint32_t feature_d = 0;
    __asm__ volatile(
        "cpuid"
        : "=a"(ignored_b), "=b"(ignored_c),
          "=c"(ignored_d), "=d"(feature_d)
        : "a"(0x80000001u), "c"(0u));
    (void)ignored_b;
    (void)ignored_c;
    (void)ignored_d;
    if ((feature_d & (1u << 20)) == 0) return;

    uint32_t efer_low;
    uint32_t efer_high;
    __asm__ volatile(
        "rdmsr"
        : "=a"(efer_low), "=d"(efer_high)
        : "c"(0xC0000080u));
    efer_low |= (1u << 11);
    __asm__ volatile(
        "wrmsr"
        :
        : "a"(efer_low), "d"(efer_high), "c"(0xC0000080u)
        : "memory");
    g_nx_enabled = true;
}

void paging_init(const struct BootInfo* boot_info) {
    if (!initialize_identity_map(boot_info)) {
        syslog_write("Paging: framebuffer mapping failed; system halted");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    load_new_tables();
    detect_and_enable_nx();
    g_paging_ready = true;
    syslog_write("Paging: Initialized (Supervisor Identity Map)");
}

static uint64_t* allocate_owned_page(struct paging_address_space* space) {
    if (space == NULL) return NULL;

    void* raw = kmalloc((size_t)(PAGE_SIZE * 2u));
    if (raw == NULL) return NULL;

    struct paging_allocation* allocation =
        (struct paging_allocation*)kmalloc(sizeof(*allocation));
    if (allocation == NULL) {
        kfree(raw);
        return NULL;
    }

    const uintptr_t unaligned = (uintptr_t)raw;
    const uintptr_t aligned =
        (unaligned + PAGE_SIZE - 1u) & ~(uintptr_t)(PAGE_SIZE - 1u);
    clear_bytes((void*)aligned, (size_t)PAGE_SIZE);

    allocation->raw = raw;
    allocation->page = (uint64_t)aligned;
    allocation->next = space->allocations;
    space->allocations = allocation;
    return (uint64_t*)aligned;
}

static bool user_range_valid(uint64_t address, size_t count) {
    if (count == 0) return true;
    return address >= PAGING_USER_BASE &&
           address < PAGING_USER_LIMIT &&
           (uint64_t)count <= PAGING_USER_LIMIT - address;
}

struct paging_address_space* paging_address_space_create(void) {
    if (!g_paging_ready) return NULL;

    struct paging_address_space* space =
        (struct paging_address_space*)kmalloc(sizeof(*space));
    if (space == NULL) return NULL;
    space->pml4 = NULL;
    space->allocations = NULL;

    uint64_t* pml4 = allocate_owned_page(space);
    if (pml4 == NULL) {
        kfree(space);
        return NULL;
    }
    copy_bytes(pml4, g_pml4, (size_t)PAGE_SIZE);

    /*
     * Slots 2 and 3 are reserved exclusively for applications. Refuse to
     * create a process address space if a future kernel mapping starts using
     * them, rather than silently replacing a kernel mapping.
     */
    if ((pml4[2] & PAGE_PRESENT) != 0 ||
        (pml4[3] & PAGE_PRESENT) != 0) {
        paging_address_space_destroy(space);
        syslog_write("Paging: reserved user PML4 slots are occupied");
        return NULL;
    }

    space->pml4 = pml4;
    return space;
}

static void load_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint64_t paging_kernel_cr3(void) {
    return (uint64_t)g_pml4;
}

bool paging_nx_available(void) {
    return g_nx_enabled;
}

uint64_t paging_address_space_cr3(const struct paging_address_space* space) {
    return space == NULL || space->pml4 == NULL
        ? paging_kernel_cr3()
        : (uint64_t)space->pml4;
}

void paging_activate(struct paging_address_space* space) {
    load_cr3(paging_address_space_cr3(space));
}

void paging_address_space_destroy(struct paging_address_space* space) {
    if (space == NULL) return;

    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    current_cr3 &= PAGE_ADDRESS_MASK;
    if (space->pml4 != NULL &&
        current_cr3 == ((uint64_t)space->pml4 & PAGE_ADDRESS_MASK)) {
        load_cr3(paging_kernel_cr3());
    }

    struct paging_allocation* allocation = space->allocations;
    while (allocation != NULL) {
        struct paging_allocation* next = allocation->next;
        kfree(allocation->raw);
        kfree(allocation);
        allocation = next;
    }
    kfree(space);
}

static uint64_t* ensure_owned_table(struct paging_address_space* space,
                                    uint64_t* entry) {
    if (space == NULL || entry == NULL) return NULL;

    if ((*entry & PAGE_PRESENT) != 0) {
        if ((*entry & PAGE_PS) != 0 ||
            (*entry & PAGE_OWNED) == 0) {
            return NULL;
        }
        *entry |= PAGE_RW | PAGE_USER;
        return (uint64_t*)(uintptr_t)(*entry & PAGE_ADDRESS_MASK);
    }

    uint64_t* table = allocate_owned_page(space);
    if (table == NULL) return NULL;
    *entry = ((uint64_t)table & PAGE_ADDRESS_MASK) |
             PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_OWNED;
    return table;
}

static bool map_fresh_user_page(struct paging_address_space* space,
                                uint64_t address,
                                bool writable,
                                bool executable) {
    if (space == NULL || space->pml4 == NULL ||
        (address & (PAGE_SIZE - 1u)) != 0 ||
        !user_range_valid(address, (size_t)PAGE_SIZE)) {
        return false;
    }

    const size_t pml4_index = (size_t)((address >> 39) & 0x1ffu);
    const size_t pdpt_index = (size_t)((address >> 30) & 0x1ffu);
    const size_t pd_index = (size_t)((address >> 21) & 0x1ffu);
    const size_t pt_index = (size_t)((address >> 12) & 0x1ffu);

    uint64_t* pdpt =
        ensure_owned_table(space, &space->pml4[pml4_index]);
    if (pdpt == NULL) return false;
    uint64_t* pd = ensure_owned_table(space, &pdpt[pdpt_index]);
    if (pd == NULL) return false;
    uint64_t* pt = ensure_owned_table(space, &pd[pd_index]);
    if (pt == NULL || (pt[pt_index] & PAGE_PRESENT) != 0) {
        return false;
    }

    uint64_t* frame = allocate_owned_page(space);
    if (frame == NULL) return false;

    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (writable) flags |= PAGE_RW;
    if (!executable && g_nx_enabled) flags |= PAGE_NX;
    pt[pt_index] = ((uint64_t)frame & PAGE_ADDRESS_MASK) | flags;
    return true;
}

bool paging_user_map_anonymous(struct paging_address_space* space,
                               uint64_t address,
                               size_t size,
                               bool writable,
                               bool executable) {
    if (space == NULL || size == 0 ||
        (address & (PAGE_SIZE - 1u)) != 0 ||
        (size & (size_t)(PAGE_SIZE - 1u)) != 0 ||
        !user_range_valid(address, size)) {
        return false;
    }

    for (size_t offset = 0; offset < size; offset += (size_t)PAGE_SIZE) {
        if (!map_fresh_user_page(space, address + (uint64_t)offset,
                                 writable, executable)) {
            return false;
        }
    }
    return true;
}

static bool translate_user_page(const struct paging_address_space* space,
                                uint64_t address,
                                bool require_writable,
                                uint64_t* out_physical) {
    if (space == NULL || space->pml4 == NULL ||
        out_physical == NULL ||
        !user_range_valid(address, 1)) {
        return false;
    }

    const uint64_t pml4e = space->pml4[(address >> 39) & 0x1ffu];
    if ((pml4e & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER) ||
        (pml4e & PAGE_PS) != 0) {
        return false;
    }
    const uint64_t* pdpt =
        (const uint64_t*)(uintptr_t)(pml4e & PAGE_ADDRESS_MASK);
    const uint64_t pdpte = pdpt[(address >> 30) & 0x1ffu];
    if ((pdpte & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER) ||
        (pdpte & PAGE_PS) != 0) {
        return false;
    }
    const uint64_t* pd =
        (const uint64_t*)(uintptr_t)(pdpte & PAGE_ADDRESS_MASK);
    const uint64_t pde = pd[(address >> 21) & 0x1ffu];
    if ((pde & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER) ||
        (pde & PAGE_PS) != 0) {
        return false;
    }
    const uint64_t* pt =
        (const uint64_t*)(uintptr_t)(pde & PAGE_ADDRESS_MASK);
    const uint64_t pte = pt[(address >> 12) & 0x1ffu];
    if ((pte & (PAGE_PRESENT | PAGE_USER)) !=
        (PAGE_PRESENT | PAGE_USER) ||
        (require_writable && (pte & PAGE_RW) == 0)) {
        return false;
    }

    *out_physical = (pte & PAGE_ADDRESS_MASK) |
                    (address & (PAGE_SIZE - 1u));
    return true;
}

bool paging_user_range_mapped(const struct paging_address_space* space,
                              uint64_t address,
                              size_t count,
                              bool require_writable) {
    if (count == 0) return true;
    if (!user_range_valid(address, count)) return false;

    size_t checked = 0;
    while (checked < count) {
        uint64_t physical;
        if (!translate_user_page(space, address + (uint64_t)checked,
                                 require_writable, &physical)) {
            return false;
        }
        (void)physical;
        size_t chunk =
            (size_t)(PAGE_SIZE -
                     ((address + (uint64_t)checked) & (PAGE_SIZE - 1u)));
        if (chunk > count - checked) chunk = count - checked;
        checked += chunk;
    }
    return true;
}

static bool copy_to_user_internal(
    const struct paging_address_space* space,
    uint64_t user_destination,
    const void* kernel_source,
    size_t count,
    bool require_writable) {
    if (count == 0) return true;
    if (kernel_source == NULL ||
        !paging_user_range_mapped(space, user_destination, count,
                                  require_writable)) {
        return false;
    }

    const uint8_t* source = (const uint8_t*)kernel_source;
    size_t copied = 0;
    while (copied < count) {
        uint64_t physical;
        const uint64_t user_address =
            user_destination + (uint64_t)copied;
        if (!translate_user_page(space, user_address,
                                 require_writable, &physical)) {
            return false;
        }
        size_t chunk =
            (size_t)(PAGE_SIZE - (user_address & (PAGE_SIZE - 1u)));
        if (chunk > count - copied) chunk = count - copied;
        copy_bytes((void*)(uintptr_t)physical, source + copied, chunk);
        copied += chunk;
    }
    return true;
}

bool paging_copy_to_user(const struct paging_address_space* space,
                         uint64_t user_destination,
                         const void* kernel_source,
                         size_t count) {
    return copy_to_user_internal(space, user_destination, kernel_source,
                                 count, true);
}

bool paging_initialize_user_memory(
    const struct paging_address_space* space,
    uint64_t user_destination,
    const void* kernel_source,
    size_t count) {
    return copy_to_user_internal(space, user_destination, kernel_source,
                                 count, false);
}

bool paging_copy_from_user(const struct paging_address_space* space,
                           void* kernel_destination,
                           uint64_t user_source,
                           size_t count) {
    if (count == 0) return true;
    if (kernel_destination == NULL ||
        !paging_user_range_mapped(space, user_source, count, false)) {
        return false;
    }

    uint8_t* destination = (uint8_t*)kernel_destination;
    size_t copied = 0;
    while (copied < count) {
        uint64_t physical;
        const uint64_t user_address = user_source + (uint64_t)copied;
        if (!translate_user_page(space, user_address, false, &physical)) {
            return false;
        }
        size_t chunk =
            (size_t)(PAGE_SIZE - (user_address & (PAGE_SIZE - 1u)));
        if (chunk > count - copied) chunk = count - copied;
        copy_bytes(destination + copied, (const void*)(uintptr_t)physical,
                   chunk);
        copied += chunk;
    }
    return true;
}
