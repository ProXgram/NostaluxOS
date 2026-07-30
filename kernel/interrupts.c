#include "interrupts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "syslog.h"
#include "io.h"
#include "keyboard.h"
#include "timer.h"
#include "graphics.h"
#include "mouse.h"
#include "scheduler.h"
#include "user_return.h"
#include "irq_registry.h"

extern void isr_syscall(void);

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_descriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20
#define PIC_READ_ISR 0x0B
#define INTERRUPT_CLD() __asm__ volatile("cld" ::: "cc")

static struct irq_registry g_irq_registry;
static bool g_interrupts_ready;

static uint64_t interrupt_save_and_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    if ((flags & (1ull << 9)) != 0u) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void normalize_user_interrupt_return(
    struct user_return_frame* frame) {
    /*
     * Same-CPL kernel frames stop after RFLAGS, so test CS before the shared
     * normalizer is allowed to inspect the saved RSP and SS.
     */
    if (user_return_frame_is_user(frame)) {
        (void)user_return_frame_normalize(frame);
    }
}

static void halt_on_invalid(const char* message) {
    syslog_write(message);
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static void pic_remap_and_mask(void) {
    outb(PIC1_COMMAND, 0x11); io_wait();
    outb(PIC2_COMMAND, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    
    /*
     * Keep every device IRQ masked until its driver has initialized its
     * software state. Leave only the cascade line open so a slave driver can
     * unmask its own IRQ later. In particular, this prevents BIOS-rate IRQ0
     * ticks and keyboard bytes from arriving before timer_init()/keyboard_init().
     *
     * 1111 1011 = 0xFB (IRQ2 cascade only)
     */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF); // Mask all slave interrupts (Mouse unmasked later)
    
    syslog_write("PIC remapped; device IRQs masked pending driver init");
}

void interrupts_enable_irq(uint8_t irq) {
    if (irq >= IRQ_REGISTRY_LINE_COUNT) return;
    const uint64_t flags = interrupt_save_and_disable();
    if (irq < 8u) {
        outb(PIC1_DATA,
             (uint8_t)(inb(PIC1_DATA) & ~(uint8_t)(1u << irq)));
    } else {
        const uint8_t slave_irq = (uint8_t)(irq - 8u);
        outb(PIC2_DATA,
             (uint8_t)(inb(PIC2_DATA) &
                       ~(uint8_t)(1u << slave_irq)));
        /* A slave line is unreachable while the master's cascade is masked. */
        outb(PIC1_DATA,
             (uint8_t)(inb(PIC1_DATA) & ~(uint8_t)(1u << 2)));
    }
    interrupt_restore(flags);
}

void interrupts_disable_irq(uint8_t irq) {
    if (irq >= IRQ_REGISTRY_LINE_COUNT || irq == 2u) return;
    const uint64_t flags = interrupt_save_and_disable();
    if (irq < 8u) {
        outb(PIC1_DATA,
             (uint8_t)(inb(PIC1_DATA) | (uint8_t)(1u << irq)));
    } else {
        const uint8_t slave_irq = (uint8_t)(irq - 8u);
        outb(PIC2_DATA,
             (uint8_t)(inb(PIC2_DATA) |
                       (uint8_t)(1u << slave_irq)));
    }
    interrupt_restore(flags);
}

bool interrupts_register_irq(uint8_t irq,
                             irq_registry_handler_t handler,
                             void* context) {
    if (!g_interrupts_ready || irq >= IRQ_REGISTRY_LINE_COUNT ||
        irq == 2u || handler == NULL) {
        return false;
    }
    const uint64_t flags = interrupt_save_and_disable();
    const bool registered =
        irq_registry_register(&g_irq_registry, irq, handler, context);
    interrupt_restore(flags);
    return registered;
}

bool interrupts_unregister_irq(uint8_t irq,
                               irq_registry_handler_t handler,
                               void* context) {
    if (!g_interrupts_ready || irq >= IRQ_REGISTRY_LINE_COUNT ||
        irq == 2u || handler == NULL) {
        return false;
    }
    const uint64_t flags = interrupt_save_and_disable();
    const bool removed =
        irq_registry_unregister(&g_irq_registry, irq, handler, context);
    interrupt_restore(flags);
    return removed;
}

static const char* const EXCEPTION_NAMES[] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow", 
    "Bound Range", "Invalid Opcode", "Device NA", "Double Fault", 
    "Coprocessor", "Invalid TSS", "Segment NP", "Stack Fault", 
    "GP Fault", "Page Fault", "Reserved", "x87 FPU", "Alignment", 
    "Machine Check", "SIMD FPU", "Virtualization", "Control Prot"
};

static size_t panic_line = 0;
static void panic_draw_bg(void) {
    // Critical: Disable double buffering to ensure panic is seen on screen
    graphics_disable_double_buffer();
    
    if (graphics_get_width() > 0) graphics_fill_rect(0, 0, graphics_get_width(), graphics_get_height(), 0xFF0000AA);
    panic_line = 0;
}
static void panic_write_line(const char* text) {
    if (graphics_get_width() == 0) return;
    int x = 10; int y = 10 + (panic_line * 10);
    for (int i = 0; text[i] != '\0'; i++) graphics_draw_char(x + (i * 8), y, text[i], 0xFFFFFFFF, 0xFF0000AA);
    panic_line++;
}
static void panic_write_hex_line(const char* label, uint64_t value) {
    char buffer[80]; size_t index = 0;
    while (label[index] != '\0' && index < sizeof(buffer) - 1) { buffer[index] = label[index]; index++; }
    if (index < sizeof(buffer) - 1) buffer[index++] = '0';
    if (index < sizeof(buffer) - 1) buffer[index++] = 'x';
    for (int shift = 60; shift >= 0 && index < sizeof(buffer) - 1; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        buffer[index++] = (char)(nibble < 10 ? ('0' + nibble) : ('A' + (nibble - 10)));
    }
    buffer[index] = '\0';
    panic_write_line(buffer);
}
static void panic_write_vector_line(uint8_t vector) { panic_write_hex_line("Exception Vector: ", vector); }
static void exception_panic(uint8_t vector, uint64_t error_code, bool has_error_code, const struct user_return_frame* frame) {
    __asm__ volatile("cli");
    panic_draw_bg();
    panic_write_line("!!! SYSTEM PANIC (GUI MODE) !!!");
    panic_write_vector_line(vector);
    if (vector < sizeof(EXCEPTION_NAMES)/sizeof(char*)) panic_write_line(EXCEPTION_NAMES[vector]);
    if (has_error_code) panic_write_hex_line("Error code: ", error_code);
    if (frame != NULL) {
        panic_write_hex_line("RIP: ", frame->rip);
        panic_write_hex_line("CS: ", frame->cs);
        panic_write_hex_line("RFLAGS: ", frame->rflags);

        // RSP and SS are appended by the CPU only when privilege changes or
        // when the IDT selects another stack through IST. All ordinary kernel
        // exceptions are same-CPL frames, so reading those fields would report
        // unrelated stack contents as fabricated diagnostics.
        const bool stack_was_saved = (frame->cs & 3u) != 0 || vector == 8;
        if (stack_was_saved) {
            panic_write_hex_line("RSP: ", frame->rsp);
            panic_write_hex_line("SS: ", frame->ss);
        } else {
            panic_write_line("RSP/SS: not saved for same-CPL exception");
        }
    }
    panic_write_line("System halted.");
    for (;;) __asm__ volatile("hlt");
}

static bool exception_came_from_user(
    const struct user_return_frame* frame) {
    return frame != NULL &&
           (frame->cs & 3u) == 3u &&
           scheduler_current_is_user();
}

static _Noreturn void contain_user_exception(
    uint8_t vector,
    uint64_t error_code,
    bool has_error_code,
    const struct user_return_frame* frame) {
    uint64_t fault_address = 0;
    if (vector == 14) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
    }
    scheduler_fault_current_user(
        vector, has_error_code, error_code,
        frame != NULL ? frame->rip : 0,
        frame != NULL ? frame->rsp : 0,
        fault_address);
}

#define DECLARE_NOERR_HANDLER(num) \
    __attribute__((interrupt)) static void handler_##num(struct user_return_frame* frame) { \
        INTERRUPT_CLD(); \
        if ((num) != 8 && (num) != 18 && exception_came_from_user(frame)) { \
            contain_user_exception((uint8_t)(num), 0, false, frame); \
        } \
        exception_panic((uint8_t)(num), 0, false, frame); \
    }
#define DECLARE_ERR_HANDLER(num) \
    __attribute__((interrupt)) static void handler_##num(struct user_return_frame* frame, uint64_t error_code) { \
        INTERRUPT_CLD(); \
        if ((num) != 8 && (num) != 18 && exception_came_from_user(frame)) { \
            contain_user_exception((uint8_t)(num), error_code, true, frame); \
        } \
        exception_panic((uint8_t)(num), error_code, true, frame); \
    }

DECLARE_NOERR_HANDLER(0); DECLARE_NOERR_HANDLER(1);
__attribute__((interrupt)) static void handler_2(struct user_return_frame* frame) {
    INTERRUPT_CLD();
    normalize_user_interrupt_return(frame);
}
DECLARE_NOERR_HANDLER(3); DECLARE_NOERR_HANDLER(4); DECLARE_NOERR_HANDLER(5); DECLARE_NOERR_HANDLER(6); DECLARE_NOERR_HANDLER(7);
DECLARE_ERR_HANDLER(8); DECLARE_NOERR_HANDLER(9); DECLARE_ERR_HANDLER(10); DECLARE_ERR_HANDLER(11); DECLARE_ERR_HANDLER(12);
DECLARE_ERR_HANDLER(13); DECLARE_ERR_HANDLER(14); DECLARE_NOERR_HANDLER(15); DECLARE_NOERR_HANDLER(16); DECLARE_ERR_HANDLER(17);
DECLARE_NOERR_HANDLER(18); DECLARE_NOERR_HANDLER(19); DECLARE_NOERR_HANDLER(20); DECLARE_ERR_HANDLER(21); DECLARE_NOERR_HANDLER(22);
DECLARE_NOERR_HANDLER(23); DECLARE_NOERR_HANDLER(24); DECLARE_NOERR_HANDLER(25); DECLARE_NOERR_HANDLER(26); DECLARE_NOERR_HANDLER(27);
DECLARE_NOERR_HANDLER(28); DECLARE_ERR_HANDLER(29); DECLARE_ERR_HANDLER(30); DECLARE_NOERR_HANDLER(31);

static uint16_t pic_read_in_service(void) {
    outb(PIC1_COMMAND, PIC_READ_ISR);
    outb(PIC2_COMMAND, PIC_READ_ISR);
    return (uint16_t)inb(PIC1_COMMAND) |
           ((uint16_t)inb(PIC2_COMMAND) << 8);
}

static bool pic_irq_is_spurious(uint8_t irq) {
    if (irq != 7u && irq != 15u) return false;
    return (pic_read_in_service() & (uint16_t)(1u << irq)) == 0u;
}

static void pic_acknowledge_irq(uint8_t irq) {
    if (irq >= 8u) outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

static void handle_registered_irq(struct user_return_frame* frame,
                                  uint8_t irq) {
    INTERRUPT_CLD();
    normalize_user_interrupt_return(frame);
    timer_interrupt_entry();

    if (pic_irq_is_spurious(irq)) {
        /*
         * A spurious IRQ7 has no in-service bit and needs no EOI. A spurious
         * IRQ15 still entered through the master's cascade, so acknowledge
         * only that master IRQ2 in-service state.
         */
        if (irq == 15u) outb(PIC1_COMMAND, PIC_EOI);
        return;
    }

    (void)irq_registry_dispatch(&g_irq_registry, irq);
    pic_acknowledge_irq(irq);
}

#define DECLARE_IRQ_HANDLER(num) \
    __attribute__((interrupt)) static void handler_irq_##num( \
        struct user_return_frame* frame) { \
        handle_registered_irq(frame, (uint8_t)(num)); \
    }

DECLARE_IRQ_HANDLER(0);  DECLARE_IRQ_HANDLER(1);
DECLARE_IRQ_HANDLER(2);  DECLARE_IRQ_HANDLER(3);
DECLARE_IRQ_HANDLER(4);  DECLARE_IRQ_HANDLER(5);
DECLARE_IRQ_HANDLER(6);  DECLARE_IRQ_HANDLER(7);
DECLARE_IRQ_HANDLER(8);  DECLARE_IRQ_HANDLER(9);
DECLARE_IRQ_HANDLER(10); DECLARE_IRQ_HANDLER(11);
DECLARE_IRQ_HANDLER(12); DECLARE_IRQ_HANDLER(13);
DECLARE_IRQ_HANDLER(14); DECLARE_IRQ_HANDLER(15);

__attribute__((interrupt)) static void handler_irq_keyboard(struct user_return_frame* frame) {
    INTERRUPT_CLD();
    normalize_user_interrupt_return(frame);
    timer_interrupt_entry();
    uint8_t scancode = inb(0x60);
    (void)irq_registry_dispatch(&g_irq_registry, 1);
    pic_acknowledge_irq(1);
    keyboard_push_byte(scancode);
}
__attribute__((interrupt)) static void handler_irq_timer(struct user_return_frame* frame) {
    INTERRUPT_CLD();
    normalize_user_interrupt_return(frame);
    timer_interrupt_entry();
    timer_handler();
    /*
     * A suspended timer handler must never retain the PIC in-service bit.
     * Acknowledge IRQ0 before a user quantum is allowed to switch tasks.
     */
    (void)irq_registry_dispatch(&g_irq_registry, 0);
    pic_acknowledge_irq(0);
    scheduler_timer_tick();
}
__attribute__((interrupt)) static void handler_irq_mouse(struct user_return_frame* frame) {
    INTERRUPT_CLD();
    normalize_user_interrupt_return(frame);
    timer_interrupt_entry();
    mouse_handle_interrupt();
    (void)irq_registry_dispatch(&g_irq_registry, 12);
    pic_acknowledge_irq(12);
}

static void idt_set_gate(uint8_t vector, void* handler) {
    uint64_t address = (uint64_t)handler;
    g_idt[vector].offset_low = (uint16_t)(address & 0xFFFF);
    g_idt[vector].selector = 0x08;
    g_idt[vector].ist = 0;
    g_idt[vector].type_attr = 0x8E;
    g_idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((address >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero = 0;
}

static void idt_set_gate_with_ist(uint8_t vector, void* handler, uint8_t ist) {
    uint64_t address = (uint64_t)handler;
    g_idt[vector].offset_low = (uint16_t)(address & 0xFFFF);
    g_idt[vector].selector = 0x08;
    g_idt[vector].ist = ist;
    g_idt[vector].type_attr = 0x8E;
    g_idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((address >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero = 0;
}

void interrupts_init(void) {
    g_interrupts_ready = false;
    irq_registry_init(&g_irq_registry);
    pic_remap_and_mask();

    idt_set_gate(0, handler_0); idt_set_gate(1, handler_1); idt_set_gate(2, handler_2); idt_set_gate(3, handler_3);
    idt_set_gate(4, handler_4); idt_set_gate(5, handler_5); idt_set_gate(6, handler_6); idt_set_gate(7, handler_7);
    idt_set_gate_with_ist(8, handler_8, 1);
    idt_set_gate(9, handler_9); idt_set_gate(10, handler_10); idt_set_gate(11, handler_11); idt_set_gate(12, handler_12);
    idt_set_gate(13, handler_13); idt_set_gate(14, handler_14); idt_set_gate(15, handler_15); idt_set_gate(16, handler_16);
    idt_set_gate(17, handler_17); idt_set_gate(18, handler_18); idt_set_gate(19, handler_19); idt_set_gate(20, handler_20);
    idt_set_gate(21, handler_21); idt_set_gate(22, handler_22); idt_set_gate(23, handler_23); idt_set_gate(24, handler_24);
    idt_set_gate(25, handler_25); idt_set_gate(26, handler_26); idt_set_gate(27, handler_27); idt_set_gate(28, handler_28);
    idt_set_gate(29, handler_29); idt_set_gate(30, handler_30); idt_set_gate(31, handler_31);

    idt_set_gate(0x20, handler_irq_0);
    idt_set_gate(0x21, handler_irq_1);
    idt_set_gate(0x22, handler_irq_2);
    idt_set_gate(0x23, handler_irq_3);
    idt_set_gate(0x24, handler_irq_4);
    idt_set_gate(0x25, handler_irq_5);
    idt_set_gate(0x26, handler_irq_6);
    idt_set_gate(0x27, handler_irq_7);
    idt_set_gate(0x28, handler_irq_8);
    idt_set_gate(0x29, handler_irq_9);
    idt_set_gate(0x2A, handler_irq_10);
    idt_set_gate(0x2B, handler_irq_11);
    idt_set_gate(0x2C, handler_irq_12);
    idt_set_gate(0x2D, handler_irq_13);
    idt_set_gate(0x2E, handler_irq_14);
    idt_set_gate(0x2F, handler_irq_15);

    idt_set_gate(0x20, handler_irq_timer);
    idt_set_gate(0x21, handler_irq_keyboard);
    idt_set_gate(0x2C, handler_irq_mouse);
    idt_set_gate(0x80, isr_syscall);
    g_idt[0x80].type_attr = 0xEE; /* Present DPL3 interrupt gate. */
    
    const struct idt_descriptor descriptor = { .limit = (uint16_t)(sizeof(g_idt) - 1), .base = (uint64_t)g_idt };
    
    if (g_idt[8].selector != 0x08 || g_idt[8].ist != 1) { halt_on_invalid("Critical: IDT vector 8 misconfigured."); }
    if (g_idt[0x80].selector != 0x08 ||
        g_idt[0x80].type_attr != 0xEE) {
        halt_on_invalid("Critical: user syscall gate misconfigured.");
    }

    __asm__ volatile("lidt %0" : : "m"(descriptor));
    g_interrupts_ready = true;
    syslog_write("Interrupts initialized (checked Apps INT 0x80 enabled)");
}
