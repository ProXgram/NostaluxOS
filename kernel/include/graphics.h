#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

void graphics_init(void);
void graphics_put_pixel(int x, int y, uint32_t color);
void graphics_fill_rect(int x, int y, int w, int h, uint32_t color);
void graphics_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

// New scaled drawing functions for the banner
void graphics_draw_char_scaled(int x, int y, char c, uint32_t fg, uint32_t bg, int scale);
void graphics_draw_string_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);

// Advanced Drawing
void graphics_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);

// Double Buffering
void graphics_enable_double_buffer(void);
void graphics_disable_double_buffer(void);
void graphics_swap_buffer(void);

uint32_t graphics_get_width(void);
uint32_t graphics_get_height(void);

#endif
