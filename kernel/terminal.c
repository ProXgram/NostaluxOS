#include "terminal.h"
#include <stdbool.h>
#include "graphics.h"
#include "system.h"

#define FONT_W 8
#define FONT_H 8

// Standard VGA Colors converted to 32-bit ARGB
static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, // 0: Black
    0xFF0000AA, // 1: Blue
    0xFF00AA00, // 2: Green
    0xFF00AAAA, // 3: Cyan
    0xFFAA0000, // 4: Red
    0xFFAA00AA, // 5: Magenta
    0xFFAA5500, // 6: Brown
    0xFFAAAAAA, // 7: Light Grey
    0xFF555555, // 8: Dark Grey
    0xFF5555FF, // 9: Light Blue
    0xFF55FF55, // 10: Light Green
    0xFF55FFFF, // 11: Light Cyan
    0xFFFF5555, // 12: Light Red
    0xFFFF55FF, // 13: Light Magenta
    0xFFFFFF55, // 14: Yellow
    0xFFFFFFFF  // 15: White
};

#define HISTORY_LINES 200

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color_fg;
static uint8_t terminal_color_bg;
static size_t terminal_cols;
static size_t terminal_rows;
static size_t terminal_batch_depth;
static size_t terminal_margin_top = 0; // Number of rows to skip drawing at the top
static bool terminal_wrap_pending = false;

static uint16_t g_history[HISTORY_LINES * 200];
static size_t g_scroll_offset = 0;

static inline uint16_t make_entry(char c, uint8_t fg, uint8_t bg) {
    uint8_t color = fg | (bg << 4);
    return (uint16_t)c | ((uint16_t)color << 8);
}

void terminal_initialize(uint32_t width, uint32_t height) {
    (void)width;  // Mark unused
    (void)height; // Mark unused

    graphics_init();
    
    // Calculate columns/rows based on font size
    terminal_cols = graphics_get_width() / FONT_W;
    terminal_rows = graphics_get_height() / FONT_H;
    
    // Sanity check bounds for static buffer
    if (terminal_cols > 200) terminal_cols = 200;
    
    terminal_row = 0;
    terminal_column = 0;
    terminal_color_fg = 15; // White
    terminal_color_bg = 1;  // Blue
    g_scroll_offset = 0;
    terminal_batch_depth = 0;
    terminal_margin_top = 0;
    terminal_wrap_pending = false;

    terminal_clear();
}

void terminal_set_margin_top(size_t rows) {
    terminal_margin_top = rows;
}

static void terminal_refresh_screen(void) {
    if (terminal_batch_depth > 0) return;

    size_t view_h = terminal_rows;
    size_t start_row = 0;
    
    if (terminal_row >= view_h) {
        start_row = terminal_row - (view_h - 1);
    }
    if (g_scroll_offset > start_row) g_scroll_offset = start_row;
    start_row -= g_scroll_offset;

    for (size_t y = 0; y < view_h; y++) {
        // FIX: Skip drawing if this row is inside the reserved top margin.
        // This prevents the terminal from overwriting the graphical banner.
        if (y < terminal_margin_top) continue;

        for (size_t x = 0; x < terminal_cols; x++) {
            size_t hist_idx = (start_row + y) * terminal_cols + x;
            uint16_t entry = g_history[hist_idx];
            char c = (char)(entry & 0xFF);
            uint8_t color_byte = (uint8_t)(entry >> 8);
            uint8_t fg_idx = color_byte & 0x0F;
            uint8_t bg_idx = (color_byte >> 4) & 0x0F;

            uint32_t fg = VGA_PALETTE[fg_idx];
            uint32_t bg = VGA_PALETTE[bg_idx];

            graphics_draw_char(x * FONT_W, y * FONT_H, c, fg, bg);
        }
    }
    
    // Draw cursor
    if (g_scroll_offset == 0) {
        size_t cur_y = (terminal_row >= view_h) ? (view_h - 1) : terminal_row;
        // Only draw cursor if it's outside margin
        if (cur_y >= terminal_margin_top) {
            graphics_fill_rect(terminal_column * FONT_W, cur_y * FONT_H + (FONT_H-2), FONT_W, 2, VGA_PALETTE[terminal_color_fg]);
        }
    }
}

void terminal_begin_batch(void) {
    terminal_batch_depth++;
}

void terminal_end_batch(void) {
    if (terminal_batch_depth > 0) terminal_batch_depth--;
    if (terminal_batch_depth == 0) terminal_refresh_screen();
}

void terminal_clear(void) {
    for (size_t i = 0; i < HISTORY_LINES * terminal_cols; i++) {
        g_history[i] = make_entry(' ', terminal_color_fg, terminal_color_bg);
    }
    terminal_row = 0;
    terminal_column = 0;
    g_scroll_offset = 0;
    terminal_wrap_pending = false;
    terminal_refresh_screen();
}

void terminal_setcolors(uint8_t fg, uint8_t bg) {
    terminal_color_fg = fg;
    terminal_color_bg = bg;
}

void terminal_set_theme(uint8_t fg, uint8_t bg) {
    terminal_color_fg = fg;
    terminal_color_bg = bg;
    for (size_t i = 0; i < HISTORY_LINES * terminal_cols; i++) {
        char c = (char)(g_history[i] & 0xFF);
        g_history[i] = make_entry(c, fg, bg);
    }
    terminal_refresh_screen();
}

void terminal_getcolors(uint8_t* fg, uint8_t* bg) {
    if (fg) *fg = terminal_color_fg;
    if (bg) *bg = terminal_color_bg;
}

static void scroll_buffer_if_needed(void) {
    if (terminal_row >= HISTORY_LINES) {
        // Shift up
        for (size_t i = 0; i < (HISTORY_LINES - 1) * terminal_cols; i++) {
            g_history[i] = g_history[i + terminal_cols];
        }
        // Clear new line
        size_t last = (HISTORY_LINES - 1) * terminal_cols;
        for (size_t i = 0; i < terminal_cols; i++) {
            g_history[last + i] = make_entry(' ', terminal_color_fg, terminal_color_bg);
        }
        terminal_row = HISTORY_LINES - 1;
    }
}

void terminal_write_char(char c) {
    if (g_scroll_offset != 0) {
        g_scroll_offset = 0;
        terminal_refresh_screen();
    }

    if (c == '\n') {
        terminal_column = 0;
        /*
         * Filling the final column advances the logical cursor to the next
         * row immediately. A following newline completes that pending wrap
         * rather than skipping an additional row.
         */
        if (terminal_wrap_pending) {
            terminal_wrap_pending = false;
        } else {
            terminal_row++;
            scroll_buffer_if_needed();
        }
        terminal_refresh_screen();
        return;
    }
    
    if (c == '\b') {
        terminal_wrap_pending = false;
        if (terminal_column > 0) terminal_column--;
        else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = terminal_cols - 1;
        }
        size_t idx = terminal_row * terminal_cols + terminal_column;
        g_history[idx] = make_entry(' ', terminal_color_fg, terminal_color_bg);
        terminal_refresh_screen();
        return;
    }
    
    terminal_wrap_pending = false;

    size_t idx = terminal_row * terminal_cols + terminal_column;
    g_history[idx] = make_entry(c, terminal_color_fg, terminal_color_bg);
    if (terminal_column + 1 >= terminal_cols) {
        terminal_column = 0;
        terminal_row++;
        scroll_buffer_if_needed();
        terminal_wrap_pending = true;
    } else {
        terminal_column++;
    }
    
    terminal_refresh_screen();
}

void terminal_write(const char* data, size_t length) {
    terminal_begin_batch();
    for (size_t i = 0; i < length; i++) terminal_write_char(data[i]);
    terminal_end_batch();
}

void terminal_writestring(const char* data) {
    terminal_begin_batch();
    for (size_t i = 0; data[i] != '\0'; i++) terminal_write_char(data[i]);
    terminal_end_batch();
}

void terminal_write_uint(unsigned int value) {
    char buf[16];
    int i = 0;
    if (value == 0) { buf[i++] = '0'; }
    else {
        while(value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
    }
    terminal_begin_batch();
    while(i > 0) terminal_write_char(buf[--i]);
    terminal_end_batch();
}

void terminal_newline(void) {
    terminal_write_char('\n');
}

void terminal_move_cursor_left(size_t count) {
    if (g_scroll_offset != 0) g_scroll_offset = 0;
    terminal_wrap_pending = false;

    while (count--) {
        if (terminal_column > 0) {
            terminal_column--;
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = terminal_cols - 1;
        } else {
            break;
        }
    }
    terminal_refresh_screen();
}

void terminal_move_cursor_right(size_t count) {
    if (g_scroll_offset != 0) g_scroll_offset = 0;
    terminal_wrap_pending = false;

    while (count--) {
        if (terminal_column + 1 >= terminal_cols) {
            terminal_column = 0;
            terminal_row++;
            scroll_buffer_if_needed();
            terminal_wrap_pending = true;
        } else {
            terminal_column++;
            terminal_wrap_pending = false;
        }
    }
    terminal_refresh_screen();
}

void terminal_scroll_up(void) {
    size_t max_scroll = (terminal_row < terminal_rows) ? 0 : (terminal_row - (terminal_rows - 1));
    if (g_scroll_offset < max_scroll) {
        g_scroll_offset++;
        terminal_refresh_screen();
    }
}

void terminal_scroll_down(void) {
    if (g_scroll_offset > 0) {
        g_scroll_offset--;
        terminal_refresh_screen();
    }
}

void terminal_write_at(size_t row, size_t col, const char* text, uint8_t fg, uint8_t bg) {
    if (row >= HISTORY_LINES) return;
    
    size_t idx = row * terminal_cols + col;
    while (*text && col < terminal_cols) {
        g_history[idx] = make_entry(*text, fg, bg);
        text++;
        col++;
        idx++;
    }
    // Only refresh if we aren't batching
    if (terminal_batch_depth == 0) {
        terminal_refresh_screen();
    }
}
