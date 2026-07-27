#include "graphics.h"
#include "terminal.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int draw_calls;

void graphics_init(void) {}

uint32_t graphics_get_width(void) {
    return 640;
}

uint32_t graphics_get_height(void) {
    return 480;
}

void graphics_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    (void)x;
    (void)y;
    (void)c;
    (void)fg;
    (void)bg;
    draw_calls++;
}

void graphics_fill_rect(int x, int y, int w, int h, uint32_t color) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    draw_calls++;
}

static void test_capture_does_not_render(void) {
    char output[16];
    bool truncated = true;
    unsigned int before = draw_calls;

    assert(terminal_capture_begin(output, sizeof(output)));
    assert(terminal_capture_active());
    terminal_writestring("hello");
    terminal_newline();
    assert(draw_calls == before);
    assert(terminal_capture_end(&truncated) == 6);
    assert(!truncated);
    assert(!terminal_capture_active());
    assert(strcmp(output, "hello\n") == 0);
}

static void test_truncation_and_nesting(void) {
    char output[4];
    char nested[4] = "old";
    bool truncated = false;

    assert(terminal_capture_begin(output, sizeof(output)));
    assert(!terminal_capture_begin(nested, sizeof(nested)));
    terminal_writestring("abcdef");
    assert(terminal_capture_end(&truncated) == 3);
    assert(truncated);
    assert(strcmp(output, "abc") == 0);
    assert(strcmp(nested, "old") == 0);
}

static void test_argument_validation(void) {
    char output[1];
    bool truncated = false;

    assert(!terminal_capture_begin(NULL, 1));
    assert(!terminal_capture_begin(output, 0));
    assert(terminal_capture_begin(output, sizeof(output)));
    terminal_write_char('x');
    assert(terminal_capture_end(&truncated) == 0);
    assert(truncated);
    assert(output[0] == '\0');
}

int main(void) {
    terminal_initialize(640, 480);
    test_capture_does_not_render();
    test_truncation_and_nesting();
    test_argument_validation();
    puts("terminal capture tests passed");
    return 0;
}
