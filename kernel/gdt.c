#include "gdt.h"

#include <stdint.h>
#include <stddef.h>
#include "syslog.h"

struct gdt_entry64 {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_descriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

// Layout: Null, KCode, KData, UData, UCode, TSS
struct gdt_layout {
    struct gdt_entry64 null;        // 0x00
    struct gdt_entry64 k_code;      // 0x08
    struct gdt_entry64 k_data;      // 0x10
    struct gdt_entry64 u_data;      // 0x18 (User Data)
    struct gdt_entry64 u_code;      // 0x20 (User Code)
    struct tss_descriptor tss;      // 0x28 (System Segment)
} __attribute__((packed));

_Static_assert(offsetof(struct gdt_layout, null) == 0x00, "Null selector must be at 0x00");
_Static_assert(offsetof(struct gdt_layout, k_code) == 0x08, "KCode selector must be 0x08");
_Static_assert(offsetof(struct gdt_layout, k_data) == 0x10, "KData selector must be 0x10");
_Static_assert(offsetof(struct gdt_layout, u_data) == 0x18, "UData selector must be 0x18");
_Static_assert(offsetof(struct gdt_layout, u_code) == 0x20, "UCode selector must be 0x20");
_Static_assert(offsetof(struct gdt_layout, tss) == 0x28, "TSS selector must be 0x28");

enum {
    KERNEL_STACK_SIZE = 16384,
    DOUBLE_FAULT_STACK_SIZE = 4096,
};

uint8_t g_kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
uint8_t* const g_kernel_stack_top = g_kernel_stack + KERNEL_STACK_SIZE;
static uint8_t g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

static struct tss g_tss __attribute__((aligned(16))) = {
    .rsp = {[0] = (uint64_t)(g_kernel_stack + sizeof(g_kernel_stack))},
    .ist = {[0] = (uint64_t)(g_double_fault_stack + sizeof(g_double_fault_stack))},
    .io_map_base = sizeof(struct tss),
};

static struct gdt_layout g_gdt __attribute__((aligned(16)));

void gdt_set_kernel_stack(uint64_t stack_top) {
    g_tss.rsp[0] = stack_top;
}

static void gdt_set_entry(struct gdt_entry64* entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    entry->limit_low = (uint16_t)(limit & 0xFFFF);
    entry->base_low = (uint16_t)(base & 0xFFFF);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFF);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    entry->base_high = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss_entry(struct tss_descriptor* entry, uint64_t base, uint32_t limit) {
    entry->limit_low = (uint16_t)(limit & 0xFFFF);
    entry->base_low = (uint16_t)(base & 0xFFFF);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFF);
    entry->access = 0x89; // Present, Executable, Accessed
    entry->granularity = (uint8_t)((limit >> 16) & 0x0F);
    entry->base_high = (uint8_t)((base >> 24) & 0xFF);
    entry->base_upper = (uint32_t)(base >> 32);
    entry->reserved = 0;
}

static void gdt_load_descriptor(const struct gdt_descriptor* descriptor) {
    __asm__ volatile(
        "lgdt (%0)\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : "r"(descriptor) : "rax", "memory");
}

static void tss_load(uint16_t selector) {
    __asm__ volatile("ltr %0" : : "r"(selector));
}

void gdt_init(void) {
    g_gdt = (struct gdt_layout){0};
    
    // GDT Entries
    // Access: Present(1) | DPL(0 or 3) | S(1) | Ex(1/0) | DC | RW | Ac
    // 0x9A = P=1, DPL=0, S=1, Type=Code(1010) -> Kernel Code
    // 0x92 = P=1, DPL=0, S=1, Type=Data(0010) -> Kernel Data
    // 0xF2 = P=1, DPL=3, S=1, Type=Data(0010) -> User Data
    // 0xFA = P=1, DPL=3, S=1, Type=Code(1010) -> User Code

    gdt_set_entry(&g_gdt.null, 0, 0, 0, 0);
    gdt_set_entry(&g_gdt.k_code, 0, 0, 0x9A, 0x20); // Ring 0 Code (L=1)
    gdt_set_entry(&g_gdt.k_data, 0, 0, 0x92, 0x00); // Ring 0 Data
    gdt_set_entry(&g_gdt.u_data, 0, 0, 0xF2, 0x00); // Ring 3 Data
    gdt_set_entry(&g_gdt.u_code, 0, 0, 0xFA, 0x20); // Ring 3 Code (L=1)
    
    gdt_set_tss_entry(&g_gdt.tss, (uint64_t)&g_tss, (uint32_t)sizeof(g_tss) - 1);

    const struct gdt_descriptor descriptor = {
        .limit = (uint16_t)(sizeof(g_gdt) - 1),
        .base = (uint64_t)&g_gdt,
    };

    gdt_load_descriptor(&descriptor);
    // Load TSS (Index 5 -> 0x28)
    tss_load(0x28);
    
    syslog_write("GDT: Loaded with User Segments");
}
