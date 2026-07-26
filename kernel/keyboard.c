#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "keyboard.h"
#include "kstring.h"
#include "terminal.h"
#include "interrupts.h"

/* --- Constants & Macros --- */

#define SCANCODE_BUFFER_SIZE   256
#define SCANCODE_BUFFER_MASK   (SCANCODE_BUFFER_SIZE - 1)

#define SCANCODE_RELEASE_MASK  0x80
#define SCANCODE_EXTENDED      0xE0
#define SCANCODE_EXTENDED_MASK 0xE000

#define KM(n, s) { .normal = n, .shifted = s }

/* --- Data Structures --- */

struct keymap_entry {
    char normal;
    char shifted;
};

/* --- Global State --- */

static volatile uint8_t g_kb_buffer[SCANCODE_BUFFER_SIZE];
static volatile size_t g_kb_head = 0;
static volatile size_t g_kb_tail = 0;

static bool g_shift_l = false;
static bool g_shift_r = false;
static bool g_caps_lock = false;

/* History Buffer */
static char g_history[KEYBOARD_HISTORY_LIMIT][KEYBOARD_MAX_LINE];
static size_t g_history_count = 0;
static size_t g_history_view_idx = 0;

/* --- Keymap --- */

static const struct keymap_entry KEYMAP_SET1[128] = {
    [0x01] = KM(27, 27),
    [0x02] = KM('1', '!'), [0x03] = KM('2', '@'), [0x04] = KM('3', '#'),
    [0x05] = KM('4', '$'), [0x06] = KM('5', '%'), [0x07] = KM('6', '^'),
    [0x08] = KM('7', '&'), [0x09] = KM('8', '*'), [0x0A] = KM('9', '('),
    [0x0B] = KM('0', ')'), [0x0C] = KM('-', '_'), [0x0D] = KM('=', '+'),
    [0x0E] = KM('\b', '\b'),
    [0x0F] = KM('\t', '\t'),
    [0x10] = KM('q', 'Q'), [0x11] = KM('w', 'W'), [0x12] = KM('e', 'E'),
    [0x13] = KM('r', 'R'), [0x14] = KM('t', 'T'), [0x15] = KM('y', 'Y'),
    [0x16] = KM('u', 'U'), [0x17] = KM('i', 'I'), [0x18] = KM('o', 'O'),
    [0x19] = KM('p', 'P'), [0x1A] = KM('[', '{'), [0x1B] = KM(']', '}'),
    [0x1C] = KM('\n', '\n'),
    [0x1E] = KM('a', 'A'), [0x1F] = KM('s', 'S'), [0x20] = KM('d', 'D'),
    [0x21] = KM('f', 'F'), [0x22] = KM('g', 'G'), [0x23] = KM('h', 'H'),
    [0x24] = KM('j', 'J'), [0x25] = KM('k', 'K'), [0x26] = KM('l', 'L'),
    [0x27] = KM(';', ':'), [0x28] = KM('\'', '"'), [0x29] = KM('`', '~'),
    [0x2B] = KM('\\', '|'),
    [0x2C] = KM('z', 'Z'), [0x2D] = KM('x', 'X'), [0x2E] = KM('c', 'C'),
    [0x2F] = KM('v', 'V'), [0x30] = KM('b', 'B'), [0x31] = KM('n', 'N'),
    [0x32] = KM('m', 'M'), [0x33] = KM(',', '<'), [0x34] = KM('.', '>'),
    [0x35] = KM('/', '?'), [0x39] = KM(' ', ' '),
    // Basic Arrow Key Mapping
    [0x48] = KM(0, 0), 
    [0x4B] = KM(0, 0), 
    [0x50] = KM(0, 0), 
    [0x4D] = KM(0, 0), 
};

/* --- Driver Core --- */

void keyboard_init(void) {
    g_kb_head = 0;
    g_kb_tail = 0;
    interrupts_enable_irq(1); // Unmask Keyboard IRQ
}

/* Called from ISR */
void keyboard_push_byte(uint8_t byte) {
    size_t next = (g_kb_head + 1) & SCANCODE_BUFFER_MASK;
    if (next != g_kb_tail) {
        g_kb_buffer[g_kb_head] = byte;
        g_kb_head = next;
    }
}

static uint8_t keyboard_pop_byte(void) {
    while (g_kb_head == g_kb_tail) {
        __asm__ volatile("hlt");
    }
    uint8_t byte = g_kb_buffer[g_kb_tail];
    g_kb_tail = (g_kb_tail + 1) & SCANCODE_BUFFER_MASK;
    return byte;
}

static bool keyboard_try_pop_byte(uint8_t* out) {
    if (g_kb_head == g_kb_tail) return false;
    *out = g_kb_buffer[g_kb_tail];
    g_kb_tail = (g_kb_tail + 1) & SCANCODE_BUFFER_MASK;
    return true;
}

/* --- Translation Logic --- */

static uint16_t keyboard_read_scancode(void) {
    uint16_t prefix = 0;
    for (;;) {
        uint8_t val = keyboard_pop_byte();
        if (val == SCANCODE_EXTENDED) {
            prefix = SCANCODE_EXTENDED_MASK;
            continue;
        }
        return prefix | val;
    }
}

static bool keyboard_poll_scancode(uint16_t* out_code) {
    uint8_t val;
    static uint16_t prefix = 0;
    
    if (!keyboard_try_pop_byte(&val)) return false;

    if (val == SCANCODE_EXTENDED) {
        prefix = SCANCODE_EXTENDED_MASK;
        return false;
    }

    *out_code = prefix | val;
    prefix = 0;
    return true;
}

static void update_modifiers(uint8_t scancode, bool released) {
    switch (scancode) {
        case 0x2A: g_shift_l = !released; break;
        case 0x36: g_shift_r = !released; break;
        case 0x3A: if (!released) g_caps_lock = !g_caps_lock; break;
    }
}

static char translate_scancode(uint8_t scancode) {
    if (scancode >= sizeof(KEYMAP_SET1) / sizeof(KEYMAP_SET1[0])) return 0;

    struct keymap_entry entry = KEYMAP_SET1[scancode];
    if (entry.normal == 0) return 0;

    bool shift = g_shift_l || g_shift_r;
    bool is_alpha = (entry.normal >= 'a' && entry.normal <= 'z');
    
    if (is_alpha) {
        if (g_caps_lock) shift = !shift;
    }

    return shift ? entry.shifted : entry.normal;
}

/* --- History System --- */
static size_t history_start_idx(void) {
    return (g_history_count > KEYBOARD_HISTORY_LIMIT) ? 
           (g_history_count - KEYBOARD_HISTORY_LIMIT) : 0;
}
static const char* history_get_abs(size_t abs_idx) {
    if (abs_idx >= g_history_count || abs_idx < history_start_idx()) return NULL;
    return g_history[abs_idx % KEYBOARD_HISTORY_LIMIT];
}
void keyboard_history_record(const char* line) {
    if (kstrlen(line) == 0) return;
    if (g_history_count > 0) {
        size_t last = (g_history_count - 1) % KEYBOARD_HISTORY_LIMIT;
        if (kstrcmp(g_history[last], line) == 0) {
            g_history_view_idx = g_history_count;
            return;
        }
    }
    size_t slot = g_history_count % KEYBOARD_HISTORY_LIMIT;
    size_t len = 0;
    while (line[len] && len < KEYBOARD_MAX_LINE - 1) {
        g_history[slot][len] = line[len];
        len++;
    }
    g_history[slot][len] = '\0';
    g_history_count++;
    g_history_view_idx = g_history_count;
}
size_t keyboard_history_length(void) {
    size_t start = history_start_idx();
    return g_history_count - start;
}
const char* keyboard_history_entry(size_t relative_idx) {
    return history_get_abs(history_start_idx() + relative_idx);
}
void keyboard_history_reset_iteration(void) {
    g_history_view_idx = g_history_count;
}
const char* keyboard_history_step(int dir) {
    size_t start = history_start_idx();
    if (dir < 0) { 
        if (g_history_view_idx > start) g_history_view_idx--;
        return history_get_abs(g_history_view_idx);
    } 
    if (dir > 0) { 
        if (g_history_view_idx < g_history_count) g_history_view_idx++;
        if (g_history_view_idx == g_history_count) return ""; 
        return history_get_abs(g_history_view_idx);
    }
    return NULL;
}

/* --- Line Editing Utilities --- */
static void edit_clear(size_t cursor, size_t length) {
    if (length == 0) return;
    terminal_begin_batch();
    if (cursor < length) terminal_move_cursor_right(length - cursor);
    for (size_t i = 0; i < length; i++) terminal_write_char('\b');
    terminal_end_batch();
}
static void edit_replace(const char* src, char* buf, size_t max, size_t* len, size_t* cur) {
    terminal_begin_batch();
    edit_clear(*cur, *len);
    size_t i = 0;
    while (src[i] && i < max - 1) {
        buf[i] = src[i];
        terminal_write_char(src[i]);
        i++;
    }
    buf[i] = '\0';
    *len = i;
    *cur = i;
    terminal_end_batch();
}
static void edit_insert(char c, char* buf, size_t max, size_t* len, size_t* cur) {
    if (*len + 1 >= max) return;
    terminal_begin_batch();
    for (size_t i = *len; i > *cur; i--) buf[i] = buf[i - 1];
    buf[*cur] = c;
    (*len)++;
    (*cur)++;
    buf[*len] = '\0';
    terminal_write(&buf[*cur - 1], *len - (*cur - 1));
    size_t tail_len = *len - *cur;
    if (tail_len > 0) terminal_move_cursor_left(tail_len);
    terminal_end_batch();
}
static void edit_delete(char* buf, size_t* len, size_t* cur, bool backspace) {
    if (backspace) {
        if (*cur == 0) return;
        (*cur)--;
    } else {
        if (*cur >= *len) return;
    }
    terminal_begin_batch();
    for (size_t i = *cur; i < *len; i++) buf[i] = buf[i + 1];
    (*len)--;
    buf[*len] = '\0';
    if (backspace) terminal_move_cursor_left(1);
    size_t tail_len = *len - *cur;
    if (tail_len > 0) terminal_write(&buf[*cur], tail_len);
    terminal_write_char(' '); 
    terminal_move_cursor_left(tail_len + 1);
    terminal_end_batch();
}

/* --- Public API --- */

char keyboard_get_char(void) {
    for (;;) {
        uint16_t raw = keyboard_read_scancode();
        bool released = (raw & SCANCODE_RELEASE_MASK);
        bool extended = (raw & SCANCODE_EXTENDED_MASK);
        uint8_t scan = raw & 0x7F;

        update_modifiers(scan, released);
        if (released || extended) continue;

        char c = translate_scancode(scan);
        if (c) return c;
    }
}

char keyboard_poll_char(void) {
    uint16_t raw;
    if (!keyboard_poll_scancode(&raw)) return 0;

    bool released = (raw & SCANCODE_RELEASE_MASK);
    bool extended = (raw & SCANCODE_EXTENDED_MASK);
    uint8_t scan = raw & 0x7F;

    update_modifiers(scan, released);
    if (released || extended) return 0;

    return translate_scancode(scan);
}

void keyboard_read_line(char* buffer, size_t size) {
    keyboard_read_line_ex(buffer, size, NULL);
}

void keyboard_read_line_ex(char* buffer, size_t size, keyboard_idle_callback_t on_idle) {
    if (size == 0) return;
    
    size_t len = 0;
    size_t cur = 0;
    buffer[0] = '\0';
    keyboard_history_reset_iteration();

    for (;;) {
        // Polling loop
        uint16_t raw = 0;
        while (!keyboard_poll_scancode(&raw)) {
            if (on_idle) on_idle();
            
            // Wait for interrupt (Timer IRQ 100Hz or Keyboard IRQ)
            // This prevents the CPU from spinning at 100% and running animations too fast
            __asm__ volatile("hlt"); 
        }

        bool released = (raw & SCANCODE_RELEASE_MASK);
        bool extended = (raw & SCANCODE_EXTENDED_MASK);
        uint8_t scan = raw & 0x7F;

        update_modifiers(scan, released);
        if (released) continue;

        if (extended) {
            const char* hist_str;
            switch (scan) {
                case 0x4B: // Left
                    if (cur > 0) { cur--; terminal_move_cursor_left(1); }
                    break;
                case 0x4D: // Right
                    if (cur < len) { terminal_move_cursor_right(1); cur++; }
                    break;
                case 0x47: // Home
                    if (cur > 0) { terminal_move_cursor_left(cur); cur = 0; }
                    break;
                case 0x4F: // End
                    if (cur < len) { terminal_move_cursor_right(len - cur); cur = len; }
                    break;
                case 0x53: // Delete
                    edit_delete(buffer, &len, &cur, false);
                    break;
                case 0x48: // Up
                    if ((hist_str = keyboard_history_step(-1))) 
                        edit_replace(hist_str, buffer, size, &len, &cur);
                    break;
                case 0x50: // Down
                    if ((hist_str = keyboard_history_step(1))) 
                        edit_replace(hist_str, buffer, size, &len, &cur);
                    break;
                case 0x49: // Page Up
                    terminal_scroll_up();
                    break;
                case 0x51: // Page Down
                    terminal_scroll_down();
                    break;
            }
            continue;
        }

        if (scan == 0x0E) { // Backspace
            edit_delete(buffer, &len, &cur, true);
            continue;
        }

        if (scan == 0x1C) { // Enter
            if (cur < len) {
                terminal_move_cursor_right(len - cur);
                cur = len;
            }
            terminal_newline();
            return;
        }

        char c = translate_scancode(scan);
        if (c) edit_insert(c, buffer, size, &len, &cur);
    }
}
