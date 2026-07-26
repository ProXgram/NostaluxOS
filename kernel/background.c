#include "background.h"
#include <stddef.h>
#include "os_info.h"
#include "syslog.h"
#include "terminal.h"
#include "kstring.h"
#include "graphics.h"
#include "system.h"

// Reserve top 12 rows for the graphical banner
#define HEADER_ROWS 12
// Windows 2000 "Setup" Blue
#define BG_COLOR_HEX 0xFF0000AA 

void background_render(void) {
    uint8_t original_fg = 0;
    uint8_t original_bg = 0;
    terminal_getcolors(&original_fg, &original_bg);

    terminal_begin_batch();
    // Set terminal colors to White on Blue for the header text feel
    terminal_setcolors(0x0F, 0x01); 
    
    // Clear the screen cleanly before setting the margin
    terminal_clear(); 

    // Reserve the header area so terminal output flows underneath the banner
    // We reserve rows 0 to 10. Row 11 is the border line, which we allow.
    terminal_set_margin_top(HEADER_ROWS - 1);
    
    // Draw simple white lines for a clean, professional look
    const char* border = "________________________________________________________________________________";
    terminal_write_at(HEADER_ROWS - 1, 0, border, 0x0F, 0x01);
    
    // Move cursor below the header area
    for(int i = 0; i < HEADER_ROWS; i++) {
        terminal_newline();
    }

    terminal_setcolors(original_fg, original_bg);
    terminal_end_batch();

    syslog_write("UI: background refreshed");
}

void background_animate(void) {
    // Usable pixel area: y=8 to y=(HEADER_ROWS-1)*8.
    
    static int tick = 0;
    static int x_pos = 10;
    static int direction = 1;
    
    int width = graphics_get_width();
    
    // Calculate header area
    int y_start = 0; // Start from very top
    int y_end = (HEADER_ROWS - 1) * 8;
    int height = y_end - y_start;
    
    if (height <= 0) return;

    // 1. Clear the header area to Blue
    graphics_fill_rect(0, y_start, width, height, BG_COLOR_HEX);
    
    // 2. Animate "NOSTALUX"
    const char* text = "NOSTALUX OS";
    // Windows 2000 logo style: White text
    uint32_t text_color = 0xFFFFFFFF; 
    
    int scale = 4; // Slightly smaller, cleaner scale
    int text_w = kstrlen(text) * 8 * scale;
    int text_h = 8 * scale;
    
    // Center Y position
    int y_pos = y_start + (height - text_h) / 2;
    
    // Slow down animation: only update position every 4th tick
    if (tick % 4 == 0) {
        int max_x = width - text_w;
        if (max_x < 0) max_x = 0;
        
        if (x_pos >= max_x) {
            x_pos = max_x;
            direction = -1;
        }
        if (x_pos <= 0) {
            x_pos = 0;
            direction = 1;
        }
        x_pos += direction;
    }
    
    // Draw the Logo
    graphics_draw_string_scaled(x_pos, y_pos, text, text_color, BG_COLOR_HEX, scale);
    
    // 3. Draw subtitle (Static, like the "Professional" text in Win2k)
    const char* sub = "Built on 64-bit Architecture";
    int sub_scale = 1;
    int sub_w = kstrlen(sub) * 8 * sub_scale;
    // Draw right below the main logo area, but above the line
    graphics_draw_string_scaled((width - sub_w)/2, y_end - 14, sub, 0xFFCCCCCC, BG_COLOR_HEX, sub_scale);
    
    tick++;
}
