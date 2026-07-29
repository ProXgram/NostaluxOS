#ifndef NOSTALUX_APP_UI_H
#define NOSTALUX_APP_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_abi.h"

struct app_ui {
    uint64_t window;
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    size_t byte_count;
};

static inline bool app_result_ok(uint64_t result) {
    return (int64_t)result >= 0;
}

static inline size_t app_text_length(const char* text) {
    size_t length = 0;
    if (text != NULL) {
        while (text[length] != '\0') length++;
    }
    return length;
}

static inline char app_upper(char value) {
    return value >= 'a' && value <= 'z'
        ? (char)(value - ('a' - 'A'))
        : value;
}

static inline bool app_text_contains(const char* text, const char* needle) {
    if (text == NULL || needle == NULL || needle[0] == '\0') return false;
    for (size_t start = 0; text[start] != '\0'; start++) {
        size_t offset = 0;
        while (needle[offset] != '\0' &&
               text[start + offset] != '\0' &&
               app_upper(text[start + offset]) ==
                   app_upper(needle[offset])) {
            offset++;
        }
        if (needle[offset] == '\0') return true;
    }
    return false;
}

static inline bool app_word_character(char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '_';
}

static inline bool app_text_has_word(
    const char* text, const char* word) {
    if (text == NULL || word == NULL || word[0] == '\0') return false;
    for (size_t start = 0; text[start] != '\0'; start++) {
        if (start != 0 && app_word_character(text[start - 1u])) continue;
        size_t offset = 0;
        while (word[offset] != '\0' &&
               text[start + offset] != '\0' &&
               app_upper(text[start + offset]) ==
                   app_upper(word[offset])) {
            offset++;
        }
        if (word[offset] == '\0' &&
            !app_word_character(text[start + offset])) {
            return true;
        }
    }
    return false;
}

static inline void app_text_copy(char* destination, size_t capacity,
                                 const char* source) {
    if (destination == NULL || capacity == 0) return;
    size_t index = 0;
    if (source != NULL) {
        while (source[index] != '\0' && index + 1u < capacity) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static inline void app_log(const char* text) {
    if (text == NULL) return;
    (void)app_syscall2(APP_SYSCALL_LOG_WRITE,
                       (uint64_t)(uintptr_t)text,
                       app_text_length(text));
}

static inline void app_yield(void) {
    (void)app_syscall2(APP_SYSCALL_YIELD, 0, 0);
}

__attribute__((noreturn)) static inline void app_exit(int64_t code) {
    (void)app_syscall2(APP_SYSCALL_EXIT, (uint64_t)code, 0);
    for (;;) app_yield();
}

static const uint8_t APP_FONT_DIGITS[10][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e},
    {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e},
};

static const uint8_t APP_FONT_LETTERS[26][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e},
    {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c},
    {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a},
    {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01},
    {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06},
    {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f},
    {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t APP_FONT_LOWER[26][5] = {
    {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7f, 0x48, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7f},
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7e, 0x09, 0x01, 0x02},
    {0x0c, 0x52, 0x52, 0x52, 0x3e},
    {0x7f, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7d, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3d, 0x00},
    {0x7f, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7f, 0x40, 0x00},
    {0x7c, 0x04, 0x18, 0x04, 0x78},
    {0x7c, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7c, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7c},
    {0x7c, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3f, 0x44, 0x40, 0x20},
    {0x3c, 0x40, 0x40, 0x20, 0x7c},
    {0x1c, 0x20, 0x40, 0x20, 0x1c},
    {0x3c, 0x40, 0x30, 0x40, 0x3c},
    {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0c, 0x50, 0x50, 0x50, 0x3c},
    {0x44, 0x64, 0x54, 0x4c, 0x44},
};

static inline void app_glyph(char value, uint8_t columns[5]) {
    if (value >= '0' && value <= '9') {
        for (size_t index = 0; index < 5; index++) {
            columns[index] = APP_FONT_DIGITS[value - '0'][index];
        }
        return;
    }
    if (value >= 'a' && value <= 'z') {
        for (size_t index = 0; index < 5; index++) {
            columns[index] = APP_FONT_LOWER[value - 'a'][index];
        }
        return;
    }
    if (value >= 'A' && value <= 'Z') {
        for (size_t index = 0; index < 5; index++) {
            columns[index] = APP_FONT_LETTERS[value - 'A'][index];
        }
        return;
    }

    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t unknown[5] = {0x7f, 0x41, 0x41, 0x41, 0x7f};
    const uint8_t* glyph = unknown;
    static const uint8_t period[5] = {0, 0x60, 0x60, 0, 0};
    static const uint8_t comma[5] = {0, 0x40, 0x20, 0, 0};
    static const uint8_t colon[5] = {0, 0x36, 0x36, 0, 0};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t plus[5] = {0x08, 0x08, 0x3e, 0x08, 0x08};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t equals[5] = {0x14, 0x14, 0x14, 0x14, 0x14};
    static const uint8_t question[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t bang[5] = {0, 0, 0x5f, 0, 0};
    static const uint8_t left[5] = {0x08, 0x14, 0x22, 0x41, 0};
    static const uint8_t right[5] = {0, 0x41, 0x22, 0x14, 0x08};
    static const uint8_t asterisk[5] = {0x14, 0x08, 0x3e, 0x08, 0x14};
    static const uint8_t apostrophe[5] = {0, 0x05, 0x03, 0, 0};
    static const uint8_t quote[5] = {0x03, 0, 0x03, 0, 0};
    static const uint8_t underscore[5] = {0x40, 0x40, 0x40, 0x40, 0x40};
    static const uint8_t left_paren[5] = {0, 0x1c, 0x22, 0x41, 0};
    static const uint8_t right_paren[5] = {0, 0x41, 0x22, 0x1c, 0};
    static const uint8_t semicolon[5] = {0, 0x56, 0x36, 0, 0};
    static const uint8_t at[5] = {0x3e, 0x41, 0x5d, 0x55, 0x1e};
    static const uint8_t hash[5] = {0x14, 0x7f, 0x14, 0x7f, 0x14};
    static const uint8_t backslash[5] = {0x02, 0x04, 0x08, 0x10, 0x20};
    static const uint8_t pipe[5] = {0, 0, 0x7f, 0, 0};
    static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
    static const uint8_t ampersand[5] = {0x36, 0x49, 0x55, 0x22, 0x50};
    static const uint8_t dollar[5] = {0x24, 0x2a, 0x7f, 0x2a, 0x12};
    static const uint8_t caret[5] = {0x04, 0x02, 0x01, 0x02, 0x04};
    static const uint8_t tilde[5] = {0x08, 0x04, 0x08, 0x10, 0x08};
    static const uint8_t backtick[5] = {0, 0x01, 0x02, 0x04, 0};
    static const uint8_t left_bracket[5] = {0, 0x7f, 0x41, 0x41, 0};
    static const uint8_t right_bracket[5] = {0, 0x41, 0x41, 0x7f, 0};
    static const uint8_t left_brace[5] = {0x08, 0x36, 0x41, 0, 0};
    static const uint8_t right_brace[5] = {0, 0, 0x41, 0x36, 0x08};
    switch (value) {
        case ' ': glyph = blank; break;
        case '.': glyph = period; break;
        case ',': glyph = comma; break;
        case ':': glyph = colon; break;
        case '-': glyph = dash; break;
        case '+': glyph = plus; break;
        case '/': glyph = slash; break;
        case '=': glyph = equals; break;
        case '?': glyph = question; break;
        case '!': glyph = bang; break;
        case '<': glyph = left; break;
        case '>': glyph = right; break;
        case '*': glyph = asterisk; break;
        case '\'': glyph = apostrophe; break;
        case '"': glyph = quote; break;
        case '_': glyph = underscore; break;
        case '(': glyph = left_paren; break;
        case ')': glyph = right_paren; break;
        case ';': glyph = semicolon; break;
        case '@': glyph = at; break;
        case '#': glyph = hash; break;
        case '\\': glyph = backslash; break;
        case '|': glyph = pipe; break;
        case '%': glyph = percent; break;
        case '&': glyph = ampersand; break;
        case '$': glyph = dollar; break;
        case '^': glyph = caret; break;
        case '~': glyph = tilde; break;
        case '`': glyph = backtick; break;
        case '[': glyph = left_bracket; break;
        case ']': glyph = right_bracket; break;
        case '{': glyph = left_brace; break;
        case '}': glyph = right_brace; break;
        default: break;
    }
    for (size_t index = 0; index < 5; index++) columns[index] = glyph[index];
}

static inline void app_ui_pixel(struct app_ui* ui, int32_t x, int32_t y,
                                uint32_t color) {
    if (ui == NULL || ui->pixels == NULL || x < 0 || y < 0 ||
        (uint32_t)x >= ui->width || (uint32_t)y >= ui->height) {
        return;
    }
    ui->pixels[(size_t)y * ui->width + (size_t)x] = color;
}

static inline void app_ui_fill(struct app_ui* ui, int32_t x, int32_t y,
                               int32_t width, int32_t height,
                               uint32_t color) {
    if (ui == NULL || width <= 0 || height <= 0) return;
    for (int32_t row = 0; row < height; row++) {
        for (int32_t column = 0; column < width; column++) {
            app_ui_pixel(ui, x + column, y + row, color);
        }
    }
}

static inline void app_ui_clear(struct app_ui* ui, uint32_t color) {
    if (ui == NULL || ui->pixels == NULL) return;
    size_t count = (size_t)ui->width * ui->height;
    for (size_t index = 0; index < count; index++) {
        ui->pixels[index] = color;
    }
}

static inline void app_ui_char(struct app_ui* ui, int32_t x, int32_t y,
                               char value, uint32_t color,
                               uint32_t scale) {
    if (scale == 0) return;
    uint8_t columns[5];
    app_glyph(value, columns);
    for (uint32_t column = 0; column < 5; column++) {
        for (uint32_t row = 0; row < 7; row++) {
            if ((columns[column] & (1u << row)) == 0) continue;
            app_ui_fill(ui, x + (int32_t)(column * scale),
                        y + (int32_t)(row * scale),
                        (int32_t)scale, (int32_t)scale, color);
        }
    }
}

static inline void app_ui_text(struct app_ui* ui, int32_t x, int32_t y,
                               const char* text, uint32_t color,
                               uint32_t scale) {
    if (text == NULL || scale == 0) return;
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    for (size_t index = 0; text[index] != '\0'; index++) {
        if (text[index] == '\n') {
            cursor_x = x;
            cursor_y += (int32_t)(9u * scale);
            continue;
        }
        app_ui_char(ui, cursor_x, cursor_y, text[index], color, scale);
        cursor_x += (int32_t)(6u * scale);
    }
}

static inline bool app_ui_open(struct app_ui* ui, uint32_t width,
                               uint32_t height, const char* title) {
    if (ui == NULL || title == NULL || width == 0 || height == 0 ||
        (uint64_t)width * height >
            (uint64_t)APP_MEMORY_MAP_MAX / sizeof(uint32_t)) {
        return false;
    }
    ui->window = 0;
    ui->pixels = NULL;
    ui->width = width;
    ui->height = height;
    ui->byte_count = (size_t)width * height * sizeof(uint32_t);

    uint64_t mapped =
        app_memory_map(ui->byte_count, APP_MEMORY_READ | APP_MEMORY_WRITE);
    if (!app_result_ok(mapped) || mapped == 0) return false;
    ui->pixels = (uint32_t*)(uintptr_t)mapped;

    const struct app_window_create request = {
        .width = width,
        .height = height,
        .title = title,
        .title_length = app_text_length(title),
    };
    uint64_t window = app_window_create(&request);
    if (!app_result_ok(window) || window == 0) {
        (void)app_memory_unmap(ui->pixels, ui->byte_count);
        ui->pixels = NULL;
        return false;
    }
    ui->window = window;
    return true;
}

static inline bool app_ui_present(struct app_ui* ui) {
    if (ui == NULL || ui->window == 0 || ui->pixels == NULL) return false;
    const struct app_window_present request = {
        .window_handle = ui->window,
        .pixels = ui->pixels,
        .width = ui->width,
        .height = ui->height,
        .stride_pixels = ui->width,
    };
    return app_result_ok(app_window_present(&request));
}

static inline bool app_ui_poll(struct app_ui* ui,
                               struct app_input_event* event) {
    if (ui == NULL || event == NULL || ui->window == 0) return false;
    uint64_t result = app_input_poll(ui->window, event);
    return app_result_ok(result) && result == 1u;
}

static inline void app_ui_close(struct app_ui* ui) {
    if (ui == NULL) return;
    if (ui->window != 0) (void)app_window_close(ui->window);
    if (ui->pixels != NULL) {
        (void)app_memory_unmap(ui->pixels, ui->byte_count);
    }
    ui->window = 0;
    ui->pixels = NULL;
}

static inline size_t app_int_to_text(int64_t value, char* output,
                                     size_t capacity) {
    if (output == NULL || capacity == 0) return 0;
    char reversed[24];
    size_t count = 0;
    bool negative = value < 0;
    uint64_t magnitude =
        negative
            ? (uint64_t)(-(value + 1)) + 1u
            : (uint64_t)value;
    do {
        reversed[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0 && count < sizeof(reversed));

    size_t written = 0;
    if (negative && written + 1u < capacity) output[written++] = '-';
    while (count > 0 && written + 1u < capacity) {
        output[written++] = reversed[--count];
    }
    output[written] = '\0';
    return written;
}

#endif /* NOSTALUX_APP_UI_H */
