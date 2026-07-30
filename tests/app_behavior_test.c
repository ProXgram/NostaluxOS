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
static const char* g_test_startup_argument = "";
static const char* g_test_file_name;
static uint8_t g_test_file_bytes[8192];
static size_t g_test_file_length;
static const char* g_test_existing_paths[4];
static size_t g_test_existing_path_count;
static char g_test_replaced_path[APP_FILE_PATH_MAX + 1u];
static uint8_t g_test_replaced_bytes[APP_NETWORK_RESPONSE_MAX];
static size_t g_test_replaced_length;
static char g_test_created_path[APP_FILE_PATH_MAX + 1u];
static uint8_t g_test_created_bytes[APP_NETWORK_RESPONSE_MAX];
static size_t g_test_created_length;
static bool g_test_corrupt_replaced_read;
static char g_test_last_log[128];
static char g_test_digest_log[128];
static uint64_t g_test_network_start_result = 1u;
static char g_test_network_started_url[APP_NETWORK_URL_MAX + 1u];
static struct app_network_http_request g_test_network_started_request;
static struct app_network_request_status g_test_network_status;
static uint8_t g_test_network_response[APP_NETWORK_RESPONSE_MAX];
static size_t g_test_network_response_length;
static unsigned int g_test_network_cancel_count;
static unsigned int g_test_network_close_count;

static uint64_t app_syscall2(uint64_t syscall_id,
                             uint64_t argument1,
                             uint64_t argument2) {
    if (syscall_id == APP_SYSCALL_LOG_WRITE) {
        const char* text =
            (const char*)(uintptr_t)argument1;
        size_t length = (size_t)argument2;
        if (length >= sizeof(g_test_last_log)) {
            length = sizeof(g_test_last_log) - 1u;
        }
        if (text != NULL) {
            memcpy(g_test_last_log, text, length);
        } else {
            length = 0;
        }
        g_test_last_log[length] = '\0';
        static const char digest_prefix[] =
            "Browser explicit download digest:";
        if (strncmp(
                g_test_last_log, digest_prefix,
                sizeof(digest_prefix) - 1u) == 0) {
            memcpy(g_test_digest_log, g_test_last_log, length + 1u);
        }
    } else if (syscall_id == APP_SYSCALL_TIME_GET) {
        struct app_time* time =
            (struct app_time*)(uintptr_t)argument1;
        if (time == NULL) return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
        memset(time, 0, sizeof(*time));
        time->hour = 7;
        time->minute = 8;
        time->second = 9;
    } else if (syscall_id == APP_SYSCALL_NETWORK_STATUS) {
        struct app_network_status* status =
            (struct app_network_status*)(uintptr_t)argument1;
        if (status == NULL) {
            return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
        }
        memset(status, 0, sizeof(*status));
        status->flags =
            APP_NETWORK_DEVICE_PRESENT |
            APP_NETWORK_DEVICE_READY |
            APP_NETWORK_LINK_UP |
            APP_NETWORK_CONFIGURED;
        status->ipv4_address = 0x0a00020fu;
    }
    return 0;
}

static uint64_t app_time_get(struct app_time* time) {
    return app_syscall2(
        APP_SYSCALL_TIME_GET, (uint64_t)(uintptr_t)time, sizeof(*time));
}

static uint64_t app_network_status_get(
    struct app_network_status* status) {
    return app_syscall2(
        APP_SYSCALL_NETWORK_STATUS,
        (uint64_t)(uintptr_t)status, sizeof(*status));
}

static uint64_t app_argument_get(char* buffer, size_t capacity) {
    const size_t length = strlen(g_test_startup_argument);
    if (buffer == NULL || capacity < length + 1u) {
        return (uint64_t)APP_STATUS_NO_SPACE;
    }
    memcpy(buffer, g_test_startup_argument, length + 1u);
    return length;
}

static uint64_t app_file_open(const char* path, size_t path_length,
                              uint32_t flags) {
    if (path == NULL || flags != APP_FILE_OPEN_READ) {
        return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
    }
    if (g_test_file_name != NULL &&
        path_length == strlen(g_test_file_name) &&
        memcmp(path, g_test_file_name, path_length) == 0) {
        return 1u;
    }
    if (g_test_replaced_path[0] != '\0' &&
        path_length == strlen(g_test_replaced_path) &&
        memcmp(path, g_test_replaced_path, path_length) == 0) {
        return 100u;
    }
    for (size_t index = 0;
         index < g_test_existing_path_count; index++) {
        if (path_length == strlen(g_test_existing_paths[index]) &&
            memcmp(
                path, g_test_existing_paths[index],
                path_length) == 0) {
            return 2u + index;
        }
    }
    return (uint64_t)APP_STATUS_NOT_FOUND;
}

static uint64_t app_file_read(uint64_t handle, void* buffer,
                              size_t length, size_t offset) {
    if (buffer == NULL) {
        return (uint64_t)APP_STATUS_BAD_HANDLE;
    }
    if (handle == 100u) {
        if (offset >= g_test_replaced_length) return 0;
        size_t available = g_test_replaced_length - offset;
        if (length > available) length = available;
        memcpy(buffer, g_test_replaced_bytes + offset, length);
        if (g_test_corrupt_replaced_read &&
            offset == 0 && length != 0) {
            ((uint8_t*)buffer)[0] ^= 0xffu;
        }
        return length;
    }
    if (handle != 1u) {
        return (uint64_t)APP_STATUS_BAD_HANDLE;
    }
    if (offset >= g_test_file_length) return 0;
    size_t available = g_test_file_length - offset;
    if (length > available) length = available;
    memcpy(buffer, g_test_file_bytes + offset, length);
    return length;
}

static uint64_t app_file_close(uint64_t handle) {
    (void)handle;
    return 0;
}

static uint64_t app_file_replace(const char* path, size_t path_length,
                                 const void* bytes, size_t length) {
    if (path == NULL || path_length > APP_FILE_PATH_MAX ||
        bytes == NULL || length > sizeof(g_test_replaced_bytes)) {
        return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
    }
    memcpy(g_test_replaced_path, path, path_length);
    g_test_replaced_path[path_length] = '\0';
    memcpy(g_test_replaced_bytes, bytes, length);
    g_test_replaced_length = length;
    return length;
}

static uint64_t app_file_create_exclusive(
    const char* path, size_t path_length,
    const void* bytes, size_t length) {
    if (path == NULL || path_length > APP_FILE_PATH_MAX ||
        (bytes == NULL && length != 0) ||
        length > sizeof(g_test_created_bytes)) {
        return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0;
         index < g_test_existing_path_count; index++) {
        if (path_length == strlen(g_test_existing_paths[index]) &&
            memcmp(
                path, g_test_existing_paths[index],
                path_length) == 0) {
            return (uint64_t)APP_STATUS_ALREADY_EXISTS;
        }
    }
    if ((g_test_replaced_path[0] != '\0' &&
         path_length == strlen(g_test_replaced_path) &&
         memcmp(path, g_test_replaced_path, path_length) == 0) ||
        (g_test_created_path[0] != '\0' &&
         path_length == strlen(g_test_created_path) &&
         memcmp(path, g_test_created_path, path_length) == 0)) {
        return (uint64_t)APP_STATUS_ALREADY_EXISTS;
    }
    memcpy(g_test_created_path, path, path_length);
    g_test_created_path[path_length] = '\0';
    if (length != 0) {
        memcpy(g_test_created_bytes, bytes, length);
    }
    g_test_created_length = length;
    return length;
}

static uint64_t app_network_http_start(
    const struct app_network_http_request* request) {
    if (request == NULL || request->url == NULL ||
        request->url_length > APP_NETWORK_URL_MAX) {
        return (uint64_t)APP_STATUS_INVALID_ARGUMENT;
    }
    g_test_network_started_request = *request;
    memcpy(
        g_test_network_started_url, request->url, request->url_length);
    g_test_network_started_url[request->url_length] = '\0';
    return g_test_network_start_result;
}

static uint64_t app_network_request_status_get(
    uint64_t handle, struct app_network_request_status* status) {
    if (handle == 0 || status == NULL) {
        return (uint64_t)APP_STATUS_BAD_HANDLE;
    }
    *status = g_test_network_status;
    return APP_STATUS_OK;
}

static uint64_t app_network_request_read(
    uint64_t handle, void* buffer, size_t length, size_t offset) {
    if (handle == 0 || buffer == NULL) {
        return (uint64_t)APP_STATUS_BAD_HANDLE;
    }
    if (offset >= g_test_network_response_length) return 0;
    size_t available = g_test_network_response_length - offset;
    if (length > available) length = available;
    memcpy(buffer, g_test_network_response + offset, length);
    return length;
}

static uint64_t app_network_request_cancel(uint64_t handle) {
    if (handle == 0) return (uint64_t)APP_STATUS_BAD_HANDLE;
    g_test_network_cancel_count++;
    return APP_STATUS_OK;
}

static uint64_t app_network_request_close(uint64_t handle) {
    if (handle == 0) return (uint64_t)APP_STATUS_BAD_HANDLE;
    g_test_network_close_count++;
    return APP_STATUS_OK;
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

#define nostalux_app_entry browser_test_entry
#include "../apps/browser.c"
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
    assistant_answer("network status", answer, sizeof(answer));
    assert(strcmp(
               answer,
               "NETWORK ONLINE. IP 10.0.2.15. BROWSER LOADS HTTP.") == 0);
}

static void test_browser_reset(void) {
    memset(&g_browser, 0, sizeof(g_browser));
    g_test_startup_argument = "";
    g_test_file_name = NULL;
    g_test_file_length = 0;
    g_test_existing_path_count = 0;
    g_test_replaced_path[0] = '\0';
    g_test_replaced_length = 0;
    g_test_created_path[0] = '\0';
    g_test_created_length = 0;
    g_test_corrupt_replaced_read = false;
    g_test_last_log[0] = '\0';
    g_test_digest_log[0] = '\0';
    g_test_network_start_result = 1u;
    memset(
        g_test_network_started_url, 0,
        sizeof(g_test_network_started_url));
    memset(
        &g_test_network_started_request, 0,
        sizeof(g_test_network_started_request));
    memset(&g_test_network_status, 0, sizeof(g_test_network_status));
    g_test_network_response_length = 0;
    g_test_network_cancel_count = 0;
    g_test_network_close_count = 0;
}

static void test_browser_text_processing(void) {
    test_browser_reset();
    browser_set_page(
        "<html><body>BEFORE "
        "<script>if (a < b) hidden();</script>"
        "<style>.x { width: 1 < 2; }</style>"
        "<p>AFTER &amp; SAFE</p></body></html>",
        "TEST");
    browser_make_display();
    assert(strstr(g_browser.display, "BEFORE") != NULL);
    assert(strstr(g_browser.display, "AFTER & SAFE") != NULL);
    assert(strstr(g_browser.display, "hidden") == NULL);
    assert(strstr(g_browser.display, "width") == NULL);

    memset(g_browser.display, 'x', BROWSER_COLUMNS);
    g_browser.display[BROWSER_COLUMNS] = '\0';
    g_browser.display_length = BROWSER_COLUMNS;
    assert(browser_visual_line_count() == 1u);

    g_browser.display[BROWSER_COLUMNS] = 'x';
    g_browser.display[BROWSER_COLUMNS + 1u] = '\0';
    g_browser.display_length = BROWSER_COLUMNS + 1u;
    assert(browser_visual_line_count() == 2u);

    memset(
        g_browser.display, 'x',
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES);
    g_browser.display[
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES] = '\0';
    g_browser.display_length =
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES;
    assert(browser_max_first_line() == 0u);
    g_browser.display[
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES] = 'x';
    g_browser.display[
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES + 1u] = '\0';
    g_browser.display_length =
        BROWSER_COLUMNS * BROWSER_VISIBLE_LINES + 1u;
    assert(browser_max_first_line() == 1u);
}

static void test_browser_startup_and_navigation(void) {
    test_browser_reset();
    char maximum_argument[APP_STARTUP_ARGUMENT_MAX + 1u];
    memset(maximum_argument, 'a', APP_STARTUP_ARGUMENT_MAX);
    maximum_argument[APP_STARTUP_ARGUMENT_MAX] = '\0';
    g_test_startup_argument = maximum_argument;
    browser_set_startup_address();
    assert(!g_browser.explicit_download);
    assert(g_browser.address_length == APP_STARTUP_ARGUMENT_MAX);
    assert(memcmp(
               g_browser.address, maximum_argument,
               APP_STARTUP_ARGUMENT_MAX + 1u) == 0);

    char overlong_argument[APP_STARTUP_ARGUMENT_MAX + 2u];
    memset(overlong_argument, 'b', APP_STARTUP_ARGUMENT_MAX + 1u);
    overlong_argument[APP_STARTUP_ARGUMENT_MAX + 1u] = '\0';
    g_test_startup_argument = overlong_argument;
    browser_set_startup_address();
    assert(!g_browser.explicit_download);
    assert(strcmp(g_browser.address, "about:home") == 0);

    g_test_startup_argument =
        "--download   http://example.com/archive";
    browser_set_startup_address();
    assert(g_browser.explicit_download);
    assert(strcmp(
               g_browser.address,
               "http://example.com/archive") == 0);

    g_test_startup_argument = "--download";
    browser_set_startup_address();
    assert(!g_browser.explicit_download);
    assert(strcmp(g_browser.address, "--download") == 0);

    app_text_copy(
        g_browser.address, sizeof(g_browser.address),
        "https://example.com/");
    g_browser.address_length = strlen(g_browser.address);
    browser_navigate();
    assert(!g_browser.loading);
    assert(strcmp(g_browser.status, "HTTPS SAFELY REJECTED") == 0);
    assert(strstr(g_browser.display, "will not downgrade") != NULL);

    app_text_copy(
        g_browser.address, sizeof(g_browser.address),
        "http://example.com/path");
    g_browser.address_length = strlen(g_browser.address);
    browser_navigate();
    assert(g_browser.loading);
    assert(g_browser.request_handle == 1u);
    assert(strcmp(
               g_test_network_started_url,
               "http://example.com/path") == 0);
    assert(g_test_network_started_request.response_capacity ==
           APP_NETWORK_RESPONSE_MAX);
    assert(g_test_network_started_request.flags ==
           APP_NETWORK_HTTP_FOLLOW_REDIRECTS);
    assert(g_test_network_started_request.timeout_milliseconds == 10000u);
}

static void test_browser_request_lifecycle(void) {
    test_browser_reset();
    g_browser.loading = true;
    g_browser.request_handle = 7u;
    memcpy(g_test_network_response, "hello", 5u);
    g_test_network_response_length = 5u;
    g_test_network_status.state = APP_NETWORK_REQUEST_COMPLETE;
    g_test_network_status.http_status = 200u;
    g_test_network_status.flags = APP_NETWORK_STATUS_TOTAL_KNOWN;
    g_test_network_status.received_bytes = 5u;
    g_test_network_status.total_bytes = 5u;
    browser_poll_request();
    assert(!g_browser.loading);
    assert(g_browser.request_handle == 0);
    assert(g_browser.response_available);
    assert(g_browser.raw_length == 5u);
    assert(memcmp(g_browser.raw, "hello", 5u) == 0);
    assert(strcmp(g_browser.status, "HTTP 200 - 5 BYTES") == 0);
    assert(g_test_network_close_count == 1u);

    browser_save_download();
    assert(strcmp(g_test_created_path, "download.txt") == 0);
    assert(g_test_created_length == 5u);
    assert(memcmp(g_test_created_bytes, "hello", 5u) == 0);
    assert(g_test_replaced_path[0] == '\0');
    assert(strcmp(
               g_test_last_log,
               "Browser download saved as download.txt") == 0);

    test_browser_reset();
    g_browser.loading = true;
    g_browser.request_handle = 8u;
    memcpy(g_test_network_response, "four", 4u);
    g_test_network_response_length = 4u;
    g_test_network_status.state = APP_NETWORK_REQUEST_COMPLETE;
    g_test_network_status.flags = APP_NETWORK_STATUS_TOTAL_KNOWN;
    g_test_network_status.total_bytes = 5u;
    browser_poll_request();
    assert(!g_browser.response_available);
    assert(strcmp(g_browser.status, "RESPONSE READ INCOMPLETE") == 0);
    assert(g_test_network_close_count == 1u);

    test_browser_reset();
    g_browser.loading = true;
    g_browser.request_handle = 9u;
    browser_cancel_request();
    assert(!g_browser.loading);
    assert(g_test_network_cancel_count == 1u);
    assert(g_test_network_close_count == 1u);
    assert(strcmp(g_browser.status, "REQUEST CANCELED") == 0);
}

static void test_browser_safe_download_modes(void) {
    test_browser_reset();
    g_browser.loading = true;
    g_browser.request_handle = 10u;
    g_browser.response_available = true;
    memcpy(g_browser.raw, "do not save", 11u);
    g_browser.raw_length = 11u;
    assert(browser_handle_window_open_failure() == 1);
    assert(!g_browser.loading);
    assert(g_test_network_cancel_count == 1u);
    assert(g_test_network_close_count == 1u);
    assert(g_test_replaced_path[0] == '\0');
    assert(strcmp(
               g_test_last_log,
               "Browser window unavailable; no download file was saved") ==
           0);

    test_browser_reset();
    g_browser.response_available = true;
    memcpy(g_browser.raw, "payload", 7u);
    g_browser.raw_length = 7u;
    g_test_existing_paths[0] = "download.txt";
    g_test_existing_paths[1] = "download-1.txt";
    g_test_existing_path_count = 2u;
    assert(browser_save_download());
    assert(strcmp(g_test_created_path, "download-2.txt") == 0);
    assert(g_test_created_length == 7u);
    assert(memcmp(g_test_created_bytes, "payload", 7u) == 0);
    assert(g_test_replaced_path[0] == '\0');
    assert(strcmp(
               g_test_last_log,
               "Browser download saved as download-2.txt") == 0);
    assert(strcmp(
               g_browser.status,
               "SAVED EXACT RESPONSE BODY AS download-2.txt") == 0);

    test_browser_reset();
    g_browser.explicit_download = true;
    g_browser.loading = true;
    g_browser.request_handle = 11u;
    memcpy(g_test_network_response, "payload", 7u);
    g_test_network_response_length = 7u;
    g_test_network_status.state = APP_NETWORK_REQUEST_COMPLETE;
    g_test_network_status.http_status = 200u;
    g_test_network_status.flags = APP_NETWORK_STATUS_TOTAL_KNOWN;
    g_test_network_status.received_bytes = 7u;
    g_test_network_status.total_bytes = 7u;
    g_test_existing_paths[0] = "download.txt";
    g_test_existing_path_count = 1u;
    assert(browser_run_explicit_download());
    assert(strcmp(g_test_replaced_path, "download.txt") == 0);
    assert(g_test_created_path[0] == '\0');
    assert(g_test_replaced_length == 7u);
    assert(memcmp(g_test_replaced_bytes, "payload", 7u) == 0);
    assert(g_browser.request_handle == 11u);
    assert(g_test_network_close_count == 0u);
    assert(strcmp(
               g_test_digest_log,
               "Browser explicit download digest: bytes=7 "
               "fnv1a=856651685") == 0);
    assert(strcmp(
               g_test_last_log,
               "Browser explicit download verified: download.txt") == 0);
    assert(strcmp(
               g_browser.status,
               "VERIFIED EXACT RESPONSE BODY AS download.txt") == 0);

    test_browser_reset();
    g_browser.explicit_download = true;
    g_browser.loading = true;
    g_browser.request_handle = 12u;
    memcpy(g_test_network_response, "payload", 7u);
    g_test_network_response_length = 7u;
    g_test_network_status.state = APP_NETWORK_REQUEST_COMPLETE;
    g_test_network_status.http_status = 200u;
    g_test_network_status.flags = APP_NETWORK_STATUS_TOTAL_KNOWN;
    g_test_network_status.received_bytes = 7u;
    g_test_network_status.total_bytes = 7u;
    g_test_corrupt_replaced_read = true;
    assert(!browser_run_explicit_download());
    assert(strcmp(
               g_test_last_log,
               "Browser explicit download failed: verification error") == 0);
    assert(strcmp(
               g_browser.status,
               "EXPLICIT DOWNLOAD VERIFY FAILED") == 0);

    test_browser_reset();
    g_browser.explicit_download = true;
    g_browser.response_available = true;
    assert(!browser_run_explicit_download());
    assert(g_test_replaced_path[0] == '\0');
    assert(strcmp(
               g_test_last_log,
               "Browser explicit download failed: request did not start") ==
           0);
}

static void test_browser_large_file_reporting(void) {
    test_browser_reset();
    g_test_file_name = "large.txt";
    g_test_file_length = sizeof(g_test_file_bytes);
    memset(g_test_file_bytes, 'z', g_test_file_length);
    app_text_copy(
        g_browser.address, sizeof(g_browser.address),
        "file:large.txt");
    g_browser.address_length = strlen(g_browser.address);
    browser_navigate();
    assert(g_browser.raw_length == APP_NETWORK_RESPONSE_MAX);
    assert(strcmp(
               g_browser.status,
               "LOCAL FILE - TRUNCATED TO 8191 BYTES") == 0);
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
    test_browser_text_processing();
    test_browser_startup_and_navigation();
    test_browser_request_lifecycle();
    test_browser_safe_download_modes();
    test_browser_large_file_reporting();
    test_shared_ui_helpers();
    puts("app behavior tests passed");
    return 0;
}
