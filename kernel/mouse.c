#include "mouse.h"
#include "io.h"
#include "interrupts.h"
#include "graphics.h"
#include "syslog.h"

#define MOUSE_PORT_DATA    0x60
#define MOUSE_PORT_STATUS  0x64
#define MOUSE_PORT_CMD     0x64
#define MOUSE_ACK          0xFA
#define MOUSE_RESEND       0xFE
#define MOUSE_MAX_RETRIES  3

static uint8_t g_mouse_cycle = 0;
static uint8_t g_mouse_byte[3];
static int     g_mouse_x = 0;
static int     g_mouse_y = 0;
static bool    g_left_btn = false;
static bool    g_right_btn = false;
static int     g_sensitivity = 1;

static int mouse_decode_delta(uint8_t flags, uint8_t movement,
                              uint8_t sign_mask, uint8_t overflow_mask) {
    bool negative = (flags & sign_mask) != 0;

    // The movement byte is the low eight bits of a signed nine-bit value.
    // Saturate an overflow packet instead of allowing it to wrap direction.
    if (flags & overflow_mask) return negative ? -255 : 255;

    int delta = (int)movement;
    if (negative) delta -= 256;
    return delta;
}

static bool mouse_wait(bool wait_for_read) {
    uint32_t timeout = 100000;
    if (!wait_for_read) {
        while (timeout--) {
            if ((inb(MOUSE_PORT_STATUS) & 0x02) == 0) return true;
        }
    } else {
        while (timeout--) {
            if ((inb(MOUSE_PORT_STATUS) & 0x01) != 0) return true;
        }
    }
    return false;
}

static bool mouse_write(uint8_t value) {
    if (!mouse_wait(false)) return false;
    outb(MOUSE_PORT_CMD, 0xD4);
    if (!mouse_wait(false)) return false;
    outb(MOUSE_PORT_DATA, value);
    return true;
}

static bool mouse_read(uint8_t* value) {
    if (!value || !mouse_wait(true)) return false;
    *value = inb(MOUSE_PORT_DATA);
    return true;
}

static bool mouse_send_command(uint8_t command) {
    for (int attempt = 0; attempt < MOUSE_MAX_RETRIES; attempt++) {
        uint8_t response;
        if (!mouse_write(command) || !mouse_read(&response)) return false;
        if (response == MOUSE_ACK) return true;
        if (response != MOUSE_RESEND) return false;
    }
    return false;
}

static void mouse_restore_interrupts(uint64_t saved_rflags) {
    if ((saved_rflags & (1ULL << 9)) != 0) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void mouse_init(void) {
    uint8_t status;
    uint64_t saved_rflags;
    const char* failure = NULL;

    // Disable interrupts during setup, but preserve the caller's IF state.
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_rflags) :: "memory");

    // 1. Enable Mouse Port (Command 0xA8)
    if (!mouse_wait(false)) {
        failure = "Mouse: controller write timeout";
        goto fail;
    }
    outb(MOUSE_PORT_CMD, 0xA8);

    // 2. Enable Interrupts (Read/Modify/Write Config Byte)
    if (!mouse_wait(false)) {
        failure = "Mouse: config command timeout";
        goto fail;
    }
    outb(MOUSE_PORT_CMD, 0x20); // Read Controller Config Byte
    if (!mouse_read(&status)) {
        failure = "Mouse: config read timeout";
        goto fail;
    }
    
    // Set Bit 1 (Enable Mouse IRQ12)
    // Clear Bit 5 (Disable Mouse Port) -> Must be 0 to enable
    status |= 0x02;
    status &= (uint8_t)~0x20u;
    
    if (!mouse_wait(false)) {
        failure = "Mouse: config write timeout";
        goto fail;
    }
    outb(MOUSE_PORT_CMD, 0x60); // Write Controller Config Byte
    if (!mouse_wait(false)) {
        failure = "Mouse: config data timeout";
        goto fail;
    }
    outb(MOUSE_PORT_DATA, status);

    // 3. Set Defaults
    if (!mouse_send_command(0xF6)) {
        failure = "Mouse: set-defaults command failed";
        goto fail;
    }

    // 4. Enable Data Reporting
    if (!mouse_send_command(0xF4)) {
        failure = "Mouse: enable-reporting command failed";
        goto fail;
    }

    // Initialize position to center
    int w = graphics_get_width();
    int h = graphics_get_height();
    g_mouse_x = (w > 0) ? w / 2 : 400;
    g_mouse_y = (h > 0) ? h / 2 : 300;
    g_sensitivity = 1;
    g_mouse_cycle = 0;
    g_left_btn = false;
    g_right_btn = false;

    // Unmask IRQ 12 (Slave PIC line 4)
    interrupts_enable_irq(12);
    
    mouse_restore_interrupts(saved_rflags);
    syslog_write("Mouse: PS/2 initialized");
    return;

fail:
    g_mouse_cycle = 0;
    mouse_restore_interrupts(saved_rflags);
    syslog_write(failure ? failure : "Mouse: PS/2 initialization failed");
}

void mouse_handle_interrupt(void) {
    uint8_t status = inb(MOUSE_PORT_STATUS);
    
    // Leave non-auxiliary bytes for the keyboard IRQ handler.
    if ((status & 0x21) != 0x21) return;

    // Read the data
    uint8_t b = inb(MOUSE_PORT_DATA);

    switch(g_mouse_cycle) {
        case 0:
            // Packet Byte 1
            // Bit 3 should be 1. If not, we are out of sync.
            // However, some scroll mice use different packets.
            // We try to enforce synchronization.
            if ((b & 0x08) == 0x08) { 
                g_mouse_byte[0] = b;
                g_mouse_cycle++;
            } else {
                // Desync detected, reset cycle
                g_mouse_cycle = 0;
            }
            break;
        case 1:
            // Packet Byte 2: X Movement
            g_mouse_byte[1] = b;
            g_mouse_cycle++;
            break;
        case 2:
            // Packet Byte 3: Y Movement
            g_mouse_byte[2] = b;
            g_mouse_cycle = 0;

            // Process Packet
            int dx = mouse_decode_delta(g_mouse_byte[0], g_mouse_byte[1],
                                        0x10, 0x40);
            int dy = mouse_decode_delta(g_mouse_byte[0], g_mouse_byte[2],
                                        0x20, 0x80);

            dx *= g_sensitivity;
            dy *= g_sensitivity;
            
            // Update Position
            // PS/2 Y is bottom-to-top, screen is top-to-bottom -> subtract dy
            g_mouse_x += dx;
            g_mouse_y -= dy; 

            // Clamp to screen dimensions
            int w = graphics_get_width();
            int h = graphics_get_height();
            
            if (g_mouse_x < 0) g_mouse_x = 0;
            if (g_mouse_x >= w) g_mouse_x = w - 1;
            
            if (g_mouse_y < 0) g_mouse_y = 0;
            if (g_mouse_y >= h) g_mouse_y = h - 1;

            // Buttons
            g_left_btn = (g_mouse_byte[0] & 0x01) != 0;
            g_right_btn = (g_mouse_byte[0] & 0x02) != 0;
            break;
    }
}

MouseState mouse_get_state(void) {
    MouseState s;
    s.x = g_mouse_x;
    s.y = g_mouse_y;
    s.left_button = g_left_btn;
    s.right_button = g_right_btn;
    return s;
}

void mouse_set_sensitivity(int sense) {
    if (sense < 1) sense = 1;
    g_sensitivity = sense;
}

int mouse_get_sensitivity(void) {
    return g_sensitivity;
}
