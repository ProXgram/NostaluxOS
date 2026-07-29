#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_abi.h"

/*
 * The application sources are included below so their pure parsing, intent,
 * viewport, arithmetic, and glyph helpers can be exercised without booting
 * the kernel. These stubs satisfy UI/syscall references that the pure tests do
 * not use.
 */
static uint64_t app_syscall2(uint64_t syscall_id,
                             uint64_t argument1,
                             uint64_t argument2) {
    (void)argument2;
    if (syscall_id == APP_SYSCALL_TIME_GET) {
        struct app_time* time =
            (struct app_time*)(uintptr_t)argument1;
        if (time == NULL) return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
        memset(time, 0, sizeof(*time));
        time->hour = 7;
        time->minute = 8;
        time->second = 9;
    }
    return 0;
}

static uint64_t app_argument_get(char* buffer, size_t capacity) {
    if (buffer != NULL && capacity != 0) buffer[0] = '\0';
    return 0;
}

static uint64_t app_file_open(const char* path, size_t path_length,
                              uint32_t flags) {
    (void)path;
    (void)path_length;
    (void)flags;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_file_read(uint64_t handle, void* buffer,
                              size_t length, size_t offset) {
    (void)handle;
    (void)buffer;
    (void)length;
    (void)offset;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_file_close(uint64_t handle) {
    (void)handle;
    return 0;
}

static uint64_t app_file_replace(const char* path, size_t path_length,
                                 const void* bytes, size_t length) {
    (void)path;
    (void)path_length;
    (void)bytes;
    (void)length;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_memory_map(size_t byte_count,
                               uint32_t protection_flags) {
    (void)byte_count;
    (void)protection_flags;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_memory_unmap(void* address, size_t byte_count) {
    (void)address;
    (void)byte_count;
    return 0;
}

static uint64_t app_window_create(
    const struct app_window_create* request) {
    (void)request;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_window_present(
    const struct app_window_present* request) {
    (void)request;
    return (uint64_t)APP_STATUS_UNSUPPORTED;
}

static uint64_t app_window_close(uint64_t handle) {
    (void)handle;
    return 0;
}

static uint64_t app_input_poll(
    uint64_t handle, struct app_input_event* event) {
    (void)handle;
    (void)event;
    return 0;
}

#define nostalux_app_entry calculator_test_entry
#include "../apps/calculator.c"
#undef nostalux_app_entry

#define nostalux_app_entry notepad_test_entry
#include "../apps/notepad.c"
#undef nostalux_app_entry

#define nostalux_app_entry image_viewer_test_entry
#include "../apps/image_viewer.c"
#undef nostalux_app_entry

#define nostalux_app_entry ai_assistant_test_entry
#include "../apps/ai_assistant.c"
#undef nostalux_app_entry

static void test_write_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void test_write_u32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void test_calculator_arithmetic(void) {
    int64_t result = 0;
    assert(calculator_apply(7, 8, '+', &result) && result == 15);
    assert(calculator_apply(7, 8, '-', &result) && result == -1);
    assert(calculator_apply(-7, 8, '*', &result) && result == -56);
    assert(calculator_apply(9, 2, '/', &result) && result == 4);
    assert(!calculator_apply(1, 0, '/', &result));
    assert(!calculator_apply(INT64_MIN, -1, '/', &result));
    assert(!calculator_apply(INT64_MAX, 1, '+', &result));
    assert(!calculator_apply(INT64_MIN, 1, '-', &result));
    assert(!calculator_apply(INT64_MAX, 2, '*', &result));
}

static void test_calculator_input_state(void) {
    struct calculator_state state = {0};

    assert(calculator_handle_key(&state, '2'));
    assert(calculator_handle_key(&state, '+'));
    assert(calculator_handle_key(&state, '3'));
    assert(calculator_handle_key(&state, '='));
    assert(state.value == 5 && !state.error && !state.entering);

    /* A digit entered after a result must begin a new expression. */
    assert(calculator_handle_key(&state, '4'));
    assert(state.value == 4 && state.entering);

    assert(calculator_handle_key(&state, 'C'));
    assert(calculator_handle_key(&state, '1'));
    assert(calculator_handle_key(&state, '-'));
    assert(calculator_handle_key(&state, '2'));
    assert(calculator_handle_key(&state, '='));
    assert(state.value == -1 && !state.error && !state.entering);
    assert(calculator_handle_key(&state, '3'));
    assert(state.value == 3 && state.entering);

    assert(calculator_handle_key(&state, 'C'));
    assert(calculator_handle_key(&state, '8'));
    assert(calculator_handle_key(&state, '/'));
    assert(calculator_handle_key(&state, '0'));
    assert(calculator_handle_key(&state, '='));
    assert(state.error);
    assert(calculator_handle_key(&state, 'C'));
    assert(state.value == 0 && state.pending == 0 && !state.error);
}

static void test_notepad_viewport(void) {
    static const char newlines[] = "a\nb\nc\n";
    assert(notepad_view_start(
               newlines, sizeof(newlines) - 1u, 54u, 2u) == 4u);

    static const char wrapped[] = "abcdefghij";
    assert(notepad_view_start(
               wrapped, sizeof(wrapped) - 1u, 4u, 2u) == 4u);
    assert(notepad_view_start(
               "abcdefghi", 8u, 4u, 2u) == 4u);
    assert(notepad_view_start(
               wrapped, sizeof(wrapped) - 1u, 20u, 2u) == 0u);

    assert(notepad_index_at_position(
               "abc\ndef", 7u, 0u, 54u, 0u, 2u) == 2u);
    assert(notepad_index_at_position(
               "abc\ndef", 7u, 0u, 54u, 1u, 1u) == 5u);

    char editable[8] = "abcd";
    size_t length = 4;
    size_t cursor = 2;
    assert(notepad_insert_character(
        editable, sizeof(editable), &length, &cursor, 'X'));
    assert(strcmp(editable, "abXcd") == 0);
    assert(length == 5u && cursor == 3u);
    assert(notepad_backspace(editable, &length, &cursor));
    assert(strcmp(editable, "abcd") == 0);
    assert(length == 4u && cursor == 2u);

    cursor = length;
    assert(notepad_insert_character(
        editable, sizeof(editable), &length, &cursor, 'e'));
    assert(notepad_insert_character(
        editable, sizeof(editable), &length, &cursor, 'f'));
    assert(notepad_insert_character(
        editable, sizeof(editable), &length, &cursor, 'g'));
    assert(!notepad_insert_character(
        editable, sizeof(editable), &length, &cursor, 'h'));
}

static void test_image_parser(void) {
    enum { WIDTH = 2500, FILE_SIZE = 54 + WIDTH * 3 };
    static uint8_t bytes[FILE_SIZE];
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 'B';
    bytes[1] = 'M';
    test_write_u32(bytes + 2, sizeof(bytes));
    test_write_u32(bytes + 10, 54u);
    test_write_u32(bytes + 14, 40u);
    test_write_u32(bytes + 18, WIDTH);
    test_write_u32(bytes + 22, 1u);
    test_write_u16(bytes + 26, 1u);
    test_write_u16(bytes + 28, 24u);

    struct app_bmp image;
    assert(image_parse_bmp(bytes, sizeof(bytes), &image));
    assert(image.width == WIDTH);
    assert(image.height == 1u);
    assert(!image.top_down);

    test_write_u32(bytes + 10, 20u);
    assert(!image_parse_bmp(bytes, sizeof(bytes), &image));
    test_write_u32(bytes + 10, 54u);

    test_write_u32(bytes + 2, 60u);
    assert(!image_parse_bmp(bytes, sizeof(bytes), &image));
    test_write_u32(bytes + 2, sizeof(bytes));

    test_write_u32(bytes + 22, UINT32_MAX);
    assert(image_parse_bmp(bytes, sizeof(bytes), &image));
    assert(image.top_down && image.height == 1u);
}

static void test_assistant_intents(void) {
    char answer[192];

    assistant_answer("what time is it", answer, sizeof(answer));
    assert(strcmp(answer, "TIME 07:08:09 - REAL RTC") == 0);

    assistant_answer("paint", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "I DO NOT KNOW THAT YET. TRY HELP FOR REAL TOPICS.") == 0);

    assistant_answer("this sentence stays unknown", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "I DO NOT KNOW THAT YET. TRY HELP FOR REAL TOPICS.") == 0);

    assistant_answer("sometimes", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "I DO NOT KNOW THAT YET. TRY HELP FOR REAL TOPICS.") == 0);

    assistant_answer("profile", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "I DO NOT KNOW THAT YET. TRY HELP FOR REAL TOPICS.") == 0);

    assistant_answer("who are you", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "I AM A SMALL OFFLINE RULE-BASED ASSISTANT.") == 0);

    assistant_answer("storage", answer, sizeof(answer));
    assert(strstr(answer, "MAY BE ATA OR VOLATILE") != NULL);
    assistant_answer("show files", answer, sizeof(answer));
    assert(strstr(answer, "REAL OS FILE SERVICE") != NULL);
}

static void test_shared_ui_helpers(void) {
    uint8_t glyph[5];
    app_glyph('*', glyph);
    assert(glyph[0] != 0 || glyph[1] != 0 || glyph[2] != 0 ||
           glyph[3] != 0 || glyph[4] != 0);
    app_glyph('q', glyph);
    assert(glyph[0] != 0 || glyph[1] != 0 || glyph[2] != 0 ||
           glyph[3] != 0 || glyph[4] != 0);
    app_glyph('\x01', glyph);
    assert(glyph[0] != 0 || glyph[1] != 0 || glyph[2] != 0 ||
           glyph[3] != 0 || glyph[4] != 0);

    char number[32];
    assert(app_int_to_text(INT64_MIN, number, sizeof(number)) == 20u);
    assert(strcmp(number, "-9223372036854775808") == 0);
    assert(app_text_has_word("paint ai tools", "AI"));
    assert(!app_text_has_word("paint", "AI"));
    assert(!app_text_has_word("this", "HI"));
}

int main(void) {
    test_calculator_arithmetic();
    test_calculator_input_state();
    test_notepad_viewport();
    test_image_parser();
    test_assistant_intents();
    test_shared_ui_helpers();
    puts("app behavior tests passed");
    return 0;
}
