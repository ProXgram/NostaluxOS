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

struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

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
    
    // Mask all interrupts initially except:
    // Bit 0: Timer (IRQ0)
    // Bit 1: Keyboard (IRQ1)
    // Bit 2: Cascade (IRQ2) - REQUIRED for Slave PIC (Mouse) to work
    // 1111 1000 = 0xF8
    outb(PIC1_DATA, 0xF8); 
    outb(PIC2_DATA, 0xFF); // Mask all slave interrupts (Mouse unmasked later)
    
    syslog_write("PIC remapped (0x20/0x28) with Cascade enabled.");
}

void interrupts_enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) { port = PIC1_DATA; } else { port = PIC2_DATA; irq -= 8; }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
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
static void exception_panic(uint8_t vector, uint64_t error_code, bool has_error_code, const struct interrupt_frame* frame) {
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

#define DECLARE_NOERR_HANDLER(num) \
    __attribute__((interrupt)) static void handler_##num(struct interrupt_frame* frame) { \
        exception_panic((uint8_t)(num), 0, false, frame); \
    }
#define DECLARE_ERR_HANDLER(num) \
    __attribute__((interrupt)) static void handler_##num(struct interrupt_frame* frame, uint64_t error_code) { \
        exception_panic((uint8_t)(num), error_code, true, frame); \
    }

DECLARE_NOERR_HANDLER(0); DECLARE_NOERR_HANDLER(1);
__attribute__((interrupt)) static void handler_2(struct interrupt_frame* frame) { (void)frame; }
DECLARE_NOERR_HANDLER(3); DECLARE_NOERR_HANDLER(4); DECLARE_NOERR_HANDLER(5); DECLARE_NOERR_HANDLER(6); DECLARE_NOERR_HANDLER(7);
DECLARE_ERR_HANDLER(8); DECLARE_NOERR_HANDLER(9); DECLARE_ERR_HANDLER(10); DECLARE_ERR_HANDLER(11); DECLARE_ERR_HANDLER(12);
DECLARE_ERR_HANDLER(13); DECLARE_ERR_HANDLER(14); DECLARE_NOERR_HANDLER(15); DECLARE_NOERR_HANDLER(16); DECLARE_ERR_HANDLER(17);
DECLARE_NOERR_HANDLER(18); DECLARE_NOERR_HANDLER(19); DECLARE_NOERR_HANDLER(20); DECLARE_ERR_HANDLER(21); DECLARE_NOERR_HANDLER(22);
DECLARE_NOERR_HANDLER(23); DECLARE_NOERR_HANDLER(24); DECLARE_NOERR_HANDLER(25); DECLARE_NOERR_HANDLER(26); DECLARE_NOERR_HANDLER(27);
DECLARE_NOERR_HANDLER(28); DECLARE_ERR_HANDLER(29); DECLARE_ERR_HANDLER(30); DECLARE_NOERR_HANDLER(31);

__attribute__((interrupt)) static void handler_irq_master(struct interrupt_frame* frame) {
    (void)frame;
    timer_interrupt_entry();
    outb(PIC1_COMMAND, PIC_EOI);
}
__attribute__((interrupt)) static void handler_irq_slave(struct interrupt_frame* frame) {
    (void)frame;
    timer_interrupt_entry();
    outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}
__attribute__((interrupt)) static void handler_irq_keyboard(struct interrupt_frame* frame) {
    (void)frame;
    timer_interrupt_entry();
    uint8_t scancode = inb(0x60);
    outb(PIC1_COMMAND, PIC_EOI);
    keyboard_push_byte(scancode);
}
__attribute__((interrupt)) static void handler_irq_timer(struct interrupt_frame* frame) {
    (void)frame;
    timer_interrupt_entry();
    timer_handler();
    outb(PIC1_COMMAND, PIC_EOI);
}
__attribute__((interrupt)) static void handler_irq_mouse(struct interrupt_frame* frame) {
    (void)frame;
    timer_interrupt_entry();
    mouse_handle_interrupt();
    outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
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

    for (uint8_t vector = 0x20; vector < 0x28; vector++) { idt_set_gate(vector, handler_irq_master); }
    for (uint8_t vector = 0x28; vector < 0x30; vector++) { idt_set_gate(vector, handler_irq_slave); }

    idt_set_gate(0x20, handler_irq_timer);
    idt_set_gate(0x21, handler_irq_keyboard);
    idt_set_gate(0x2C, handler_irq_mouse);
    
    const struct idt_descriptor descriptor = { .limit = (uint16_t)(sizeof(g_idt) - 1), .base = (uint64_t)g_idt };
    
    if (g_idt[8].selector != 0x08 || g_idt[8].ist != 1) { halt_on_invalid("Critical: IDT vector 8 misconfigured."); }

    __asm__ volatile("lidt %0" : : "m"(descriptor));
    // INT 0x80 intentionally remains not-present. Kernel page tables are
    // supervisor-only and no safe user-pointer boundary exists yet.
    syslog_write("Interrupts initialized (user syscall gate disabled)");
}
