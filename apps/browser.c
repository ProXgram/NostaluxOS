#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ui.h"

#define BROWSER_CONTENT_CAPACITY (APP_NETWORK_RESPONSE_MAX + 1u)
#define BROWSER_STATUS_CAPACITY 96u
#define BROWSER_COLUMNS 71u
#define BROWSER_VISIBLE_LINES 21u
#define BROWSER_DOWNLOAD_ATTEMPTS 100u

struct browser_state {
    char address[APP_NETWORK_URL_MAX + 1u];
    size_t address_length;
    char raw[BROWSER_CONTENT_CAPACITY];
    size_t raw_length;
    char display[BROWSER_CONTENT_CAPACITY];
    size_t display_length;
    char status[BROWSER_STATUS_CAPACITY];
    size_t first_line;
    uint64_t request_handle;
    uint64_t last_live_refresh_ms;
    bool loading;
    bool address_selected;
    bool response_available;
    bool explicit_download;
};

static struct browser_state g_browser;

static void browser_clear_bytes(void* destination, size_t length) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < length; index++) bytes[index] = 0;
}

static char browser_lower(char value) {
    return value >= 'A' && value <= 'Z'
         ? (char)(value + ('a' - 'A')) : value;
}

static bool browser_starts_with(const char* text, const char* prefix) {
    if (text == NULL || prefix == NULL) return false;
    for (size_t index = 0; prefix[index] != '\0'; index++) {
        if (browser_lower(text[index]) != browser_lower(prefix[index])) {
            return false;
        }
    }
    return true;
}

static bool browser_text_equal(const char* first, const char* second) {
    if (first == NULL || second == NULL) return false;
    size_t index = 0;
    while (first[index] != '\0' && second[index] != '\0') {
        if (browser_lower(first[index]) !=
            browser_lower(second[index])) {
            return false;
        }
        index++;
    }
    return first[index] == second[index];
}

static void browser_append(char* destination, size_t capacity,
                           const char* source) {
    if (destination == NULL || capacity == 0 || source == NULL) return;
    size_t output = app_text_length(destination);
    size_t input = 0;
    while (source[input] != '\0' && output + 1u < capacity) {
        destination[output++] = source[input++];
    }
    destination[output] = '\0';
}

static void browser_append_number(char* destination, size_t capacity,
                                  uint64_t value) {
    char reversed[24];
    size_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0 && count < sizeof(reversed));
    char number[25];
    size_t output = 0;
    while (count != 0) number[output++] = reversed[--count];
    number[output] = '\0';
    browser_append(destination, capacity, number);
}

static uint32_t browser_fnv1a32(
    const uint8_t* bytes, size_t length) {
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < length; index++) {
        value ^= bytes[index];
        value *= 16777619u;
    }
    return value;
}

static void browser_set_status(const char* text) {
    app_text_copy(
        g_browser.status, sizeof(g_browser.status), text);
}

static void browser_set_page(const char* text, const char* status) {
    app_text_copy(
        g_browser.raw, sizeof(g_browser.raw), text);
    g_browser.raw_length = app_text_length(g_browser.raw);
    app_text_copy(
        g_browser.display, sizeof(g_browser.display), text);
    g_browser.display_length = app_text_length(g_browser.display);
    g_browser.first_line = 0;
    g_browser.response_available = false;
    browser_set_status(status);
}

static bool browser_html_tag_is(const char* tag, const char* name) {
    while (*tag == ' ' || *tag == '\t' || *tag == '/') tag++;
    size_t index = 0;
    while (name[index] != '\0') {
        if (browser_lower(tag[index]) != name[index]) return false;
        index++;
    }
    const char end = tag[index];
    return end == ' ' || end == '\t' || end == '\r' ||
           end == '\n' || end == '/' || end == '>';
}

static bool browser_entity(
    const char* input, char* decoded, size_t* consumed) {
    struct entity {
        const char* encoded;
        size_t length;
        char decoded;
    };
    static const struct entity entities[] = {
        {"&amp;", 5u, '&'}, {"&lt;", 4u, '<'},
        {"&gt;", 4u, '>'}, {"&quot;", 6u, '"'},
        {"&apos;", 6u, '\''}, {"&nbsp;", 6u, ' '},
    };
    for (size_t item = 0;
         item < sizeof(entities) / sizeof(entities[0]); item++) {
        bool match = true;
        for (size_t index = 0; index < entities[item].length; index++) {
            if (input[index] == '\0' ||
                input[index] != entities[item].encoded[index]) {
                match = false;
                break;
            }
        }
        if (match) {
            *decoded = entities[item].decoded;
            *consumed = entities[item].length;
            return true;
        }
    }
    return false;
}

static bool browser_looks_html(const char* body) {
    if (body == NULL) return false;
    while (*body == ' ' || *body == '\t' ||
           *body == '\r' || *body == '\n') body++;
    return *body == '<' &&
           (app_text_contains(body, "<html") ||
            app_text_contains(body, "<!doctype") ||
            app_text_contains(body, "<body"));
}

static void browser_make_display(void) {
    if (!browser_looks_html(g_browser.raw)) {
        size_t output = 0;
        for (size_t input = 0;
             input < g_browser.raw_length &&
             output + 1u < sizeof(g_browser.display);
             input++) {
            const unsigned char value =
                (unsigned char)g_browser.raw[input];
            if (value == '\r') continue;
            g_browser.display[output++] =
                value == '\n' || value == '\t' ||
                (value >= 32u && value < 127u)
                    ? (char)value : '.';
        }
        g_browser.display[output] = '\0';
        g_browser.display_length = output;
        return;
    }

    size_t read = 0;
    size_t write = 0;
    bool pending_space = false;
    enum browser_suppressed_element {
        BROWSER_SUPPRESS_NONE = 0,
        BROWSER_SUPPRESS_SCRIPT,
        BROWSER_SUPPRESS_STYLE,
    } suppressed = BROWSER_SUPPRESS_NONE;
    while (read < g_browser.raw_length &&
           write + 1u < sizeof(g_browser.display)) {
        if (suppressed != BROWSER_SUPPRESS_NONE) {
            if (g_browser.raw[read] == '<') {
                const char* tag = &g_browser.raw[read + 1u];
                const bool closes_suppressed_element =
                    g_browser.raw[read + 1u] == '/' &&
                    ((suppressed == BROWSER_SUPPRESS_SCRIPT &&
                      browser_html_tag_is(tag, "script")) ||
                     (suppressed == BROWSER_SUPPRESS_STYLE &&
                      browser_html_tag_is(tag, "style")));
                if (closes_suppressed_element) {
                    while (read < g_browser.raw_length &&
                           g_browser.raw[read] != '>') {
                        read++;
                    }
                    if (read < g_browser.raw_length) read++;
                    suppressed = BROWSER_SUPPRESS_NONE;
                    pending_space = false;
                    continue;
                }
            }
            read++;
            continue;
        }
        if (g_browser.raw[read] == '<') {
            const char* tag = &g_browser.raw[read + 1u];
            const bool closing =
                g_browser.raw[read + 1u] == '/';
            const bool script = browser_html_tag_is(tag, "script");
            const bool style = browser_html_tag_is(tag, "style");
            const bool line_break =
                browser_html_tag_is(tag, "br") ||
                browser_html_tag_is(tag, "p") ||
                browser_html_tag_is(tag, "div") ||
                browser_html_tag_is(tag, "li") ||
                browser_html_tag_is(tag, "h1") ||
                browser_html_tag_is(tag, "h2") ||
                browser_html_tag_is(tag, "h3") ||
                browser_html_tag_is(tag, "tr");
            while (read < g_browser.raw_length &&
                   g_browser.raw[read] != '>') read++;
            if (read < g_browser.raw_length) read++;
            if (script && !closing) {
                suppressed = BROWSER_SUPPRESS_SCRIPT;
            } else if (style && !closing) {
                suppressed = BROWSER_SUPPRESS_STYLE;
            }
            if (suppressed == BROWSER_SUPPRESS_NONE &&
                line_break && write > 0 &&
                g_browser.display[write - 1u] != '\n') {
                g_browser.display[write++] = '\n';
            }
            pending_space = false;
            continue;
        }

        char value = g_browser.raw[read];
        size_t consumed = 1;
        if (value == '&') {
            (void)browser_entity(
                &g_browser.raw[read], &value, &consumed);
        }
        read += consumed;
        if (value == '\r' || value == '\t' || value == ' ') {
            pending_space = write > 0 &&
                            g_browser.display[write - 1u] != '\n';
            continue;
        }
        if (value == '\n') {
            if (write > 0 &&
                g_browser.display[write - 1u] != '\n') {
                g_browser.display[write++] = '\n';
            }
            pending_space = false;
            continue;
        }
        if (pending_space &&
            write + 1u < sizeof(g_browser.display)) {
            g_browser.display[write++] = ' ';
        }
        pending_space = false;
        g_browser.display[write++] = value;
    }
    while (write > 0 &&
           (g_browser.display[write - 1u] == ' ' ||
            g_browser.display[write - 1u] == '\n')) {
        write--;
    }
    g_browser.display[write] = '\0';
    g_browser.display_length = write;
}

static size_t browser_visual_line_count(void) {
    size_t lines = 1;
    size_t column = 0;
    for (size_t index = 0; index < g_browser.display_length; index++) {
        if (g_browser.display[index] == '\n') {
            lines++;
            column = 0;
        } else {
            if (column >= BROWSER_COLUMNS) {
                lines++;
                column = 0;
            }
            column++;
        }
    }
    return lines;
}

static size_t browser_max_first_line(void) {
    const size_t lines = browser_visual_line_count();
    return lines > BROWSER_VISIBLE_LINES
         ? lines - BROWSER_VISIBLE_LINES : 0u;
}

static void browser_draw_page(struct app_ui* ui) {
    size_t line = 0;
    size_t column = 0;
    int32_t x = 13;
    int32_t y = 83;
    for (size_t index = 0;
         index < g_browser.display_length &&
         line < g_browser.first_line + BROWSER_VISIBLE_LINES;
         index++) {
        const char value = g_browser.display[index];
        if (value == '\n') {
            line++;
            column = 0;
            x = 13;
            if (line >= g_browser.first_line) {
                y = 83 + (int32_t)(
                    (line - g_browser.first_line) * 10u);
            }
            continue;
        }
        if (column >= BROWSER_COLUMNS) {
            line++;
            column = 0;
            x = 13;
            if (line >= g_browser.first_line) {
                y = 83 + (int32_t)(
                    (line - g_browser.first_line) * 10u);
            }
        }
        if (line >= g_browser.first_line &&
            line < g_browser.first_line + BROWSER_VISIBLE_LINES) {
            app_ui_char(ui, x, y, value, 0xff17202au, 1);
        }
        x += 6;
        column++;
    }
}

static void browser_render(struct app_ui* ui) {
    app_ui_clear(ui, 0xffdce2e8u);
    app_ui_fill(ui, 0, 0, (int32_t)ui->width, 34, 0xff263e5au);
    app_ui_text(ui, 10, 8, "NOSTALUX BROWSER",
                0xffffffffu, 2);

    app_ui_fill(ui, 8, 42, 343, 27, 0xffffffffu);
    const size_t visible_start =
        g_browser.address_length > 53u
            ? g_browser.address_length - 53u : 0u;
    app_ui_text(ui, 14, 51,
                &g_browser.address[visible_start],
                0xff17202au, 1);
    if (!g_browser.loading) {
        const int32_t cursor =
            14 + (int32_t)(
                (g_browser.address_length - visible_start) * 6u);
        app_ui_fill(ui, cursor, 59, 5, 2, 0xff1769a6u);
    }

    app_ui_fill(ui, 357, 42, 43, 27,
                g_browser.loading ? 0xff8793a0u : 0xff1769a6u);
    app_ui_text(ui, 368, 51, "GO", 0xffffffffu, 1);
    app_ui_fill(ui, 405, 42, 47, 27,
                g_browser.loading ? 0xffb13b3bu : 0xff4d6074u);
    app_ui_text(ui, 410, 51,
                g_browser.loading ? "STOP" : "SAVE",
                0xffffffffu, 1);

    app_ui_fill(ui, 8, 76, 444, 218, 0xfff8fafcu);
    browser_draw_page(ui);
    app_ui_fill(ui, 0, 302, (int32_t)ui->width, 28, 0xffc4cdd6u);
    app_ui_text(ui, 9, 312, g_browser.status,
                0xff263746u, 1);
    (void)app_ui_present(ui);
}

static void browser_close_request(void) {
    if (g_browser.request_handle != 0) {
        (void)app_network_request_close(
            g_browser.request_handle);
    }
    g_browser.request_handle = 0;
    g_browser.loading = false;
}

static void browser_cancel_request(void) {
    if (g_browser.request_handle == 0) return;
    (void)app_network_request_cancel(
        g_browser.request_handle);
    browser_close_request();
    browser_set_status("REQUEST CANCELED");
    app_log("Browser HTTP request canceled");
}

static const char* browser_error_text(uint32_t error) {
    switch (error) {
        case APP_NETWORK_ERROR_NOT_READY:
            return "NETWORK DEVICE IS NOT READY";
        case APP_NETWORK_ERROR_LINK_DOWN:
            return "NETWORK LINK IS DOWN";
        case APP_NETWORK_ERROR_BUSY:
            return "NETWORK SERVICE IS BUSY";
        case APP_NETWORK_ERROR_INVALID_URL:
            return "THE URL IS INVALID";
        case APP_NETWORK_ERROR_TIMEOUT:
            return "THE REQUEST TIMED OUT";
        case APP_NETWORK_ERROR_DNS:
            return "THE HOSTNAME DID NOT RESOLVE";
        case APP_NETWORK_ERROR_CONNECTION:
            return "THE TCP CONNECTION FAILED";
        case APP_NETWORK_ERROR_PROTOCOL:
            return "THE SERVER RESPONSE WAS INVALID";
        case APP_NETWORK_ERROR_RESPONSE_TOO_LARGE:
            return "THE RESPONSE IS TOO LARGE";
        case APP_NETWORK_ERROR_UNSUPPORTED:
            return "THE PROTOCOL IS NOT SUPPORTED";
        case APP_NETWORK_ERROR_IO:
            return "THE NETWORK DEVICE REPORTED AN ERROR";
        case APP_NETWORK_ERROR_CANCELED:
            return "THE REQUEST WAS CANCELED";
        default:
            return "THE REQUEST FAILED";
    }
}

static const char* browser_phase_text(uint32_t state) {
    switch (state) {
        case APP_NETWORK_REQUEST_CONFIGURING: return "DHCP";
        case APP_NETWORK_REQUEST_RESOLVING: return "DNS";
        case APP_NETWORK_REQUEST_CONNECTING: return "CONNECTING";
        case APP_NETWORK_REQUEST_SENDING: return "SENDING";
        case APP_NETWORK_REQUEST_RECEIVING: return "RECEIVING";
        case APP_NETWORK_REQUEST_REDIRECTING: return "REDIRECTING";
        default: return "WAITING";
    }
}

static void browser_poll_request(void) {
    if (!g_browser.loading || g_browser.request_handle == 0) return;
    struct app_network_request_status request;
    const uint64_t result = app_network_request_status_get(
        g_browser.request_handle, &request);
    if (!app_result_ok(result)) {
        browser_set_page(
            "The network request status could not be read.",
            "NETWORK STATUS FAILED");
        browser_close_request();
        app_log("Browser HTTP request failed");
        return;
    }

    if (request.state == APP_NETWORK_REQUEST_COMPLETE) {
        if ((request.flags & APP_NETWORK_STATUS_TOTAL_KNOWN) != 0u &&
            request.total_bytes > APP_NETWORK_RESPONSE_MAX) {
            browser_set_page(
                "The completed response exceeded the requested size.",
                "RESPONSE SIZE INVALID");
            browser_close_request();
            app_log("Browser HTTP request failed");
            return;
        }
        size_t offset = 0;
        while (offset + 1u < sizeof(g_browser.raw)) {
            size_t capacity =
                sizeof(g_browser.raw) - 1u - offset;
            if (capacity > APP_NETWORK_TRANSFER_MAX) {
                capacity = APP_NETWORK_TRANSFER_MAX;
            }
            uint64_t read = app_network_request_read(
                g_browser.request_handle,
                g_browser.raw + offset, capacity, offset);
            if (!app_result_ok(read)) {
                browser_set_page(
                    "The completed response could not be copied.",
                    "RESPONSE READ FAILED");
                browser_close_request();
                app_log("Browser HTTP request failed");
                return;
            }
            if (read == 0) break;
            offset += (size_t)read;
            if ((size_t)read < capacity) break;
        }
        if ((request.flags & APP_NETWORK_STATUS_TOTAL_KNOWN) != 0u &&
            request.total_bytes != offset) {
            browser_set_page(
                "The completed response could not be copied in full.",
                "RESPONSE READ INCOMPLETE");
            browser_close_request();
            app_log("Browser HTTP request failed");
            return;
        }
        g_browser.raw[offset] = '\0';
        g_browser.raw_length = offset;
        g_browser.response_available = true;
        browser_make_display();
        g_browser.first_line = 0;

        char status[BROWSER_STATUS_CAPACITY] = "HTTP ";
        browser_append_number(
            status, sizeof(status), request.http_status);
        browser_append(status, sizeof(status), " - ");
        browser_append_number(status, sizeof(status), offset);
        browser_append(status, sizeof(status), " BYTES");
        if (request.redirect_count != 0) {
            browser_append(status, sizeof(status), " - ");
            browser_append_number(
                status, sizeof(status), request.redirect_count);
            browser_append(status, sizeof(status), " REDIRECTS");
        }
        browser_set_status(status);
        if (g_browser.explicit_download) {
            /*
             * The headless regression intentionally leaves this completed
             * handle owned by the process. app_exit() must reclaim it through
             * app_network_services_release_process(), and the next Browser
             * request proves that teardown path really ran.
             */
            g_browser.loading = false;
        } else {
            browser_close_request();
        }
        app_log("Browser HTTP request completed");
        return;
    }
    if (request.state == APP_NETWORK_REQUEST_FAILED ||
        request.state == APP_NETWORK_REQUEST_CANCELED) {
        const char* error = browser_error_text(request.error);
        browser_set_page(error, error);
        browser_close_request();
        app_log("Browser HTTP request failed");
        return;
    }

    char status[BROWSER_STATUS_CAPACITY];
    app_text_copy(
        status, sizeof(status), browser_phase_text(request.state));
    browser_append(status, sizeof(status), " - ");
    browser_append_number(
        status, sizeof(status), request.received_bytes);
    browser_append(status, sizeof(status), " BYTES");
    browser_set_status(status);
}

static void browser_about_home(void) {
    browser_set_page(
        "Nostalux Browser\n\n"
        "This is an isolated ring-3 application. A malformed page can close "
        "this app, but it cannot take down the kernel.\n\n"
        "Supported now:\n"
        "  http:// cleartext pages\n"
        "  file:filename local files\n"
        "  about:system live network status\n"
        "  file:system.log live kernel log\n\n"
        "HTTP requests are asynchronous. STOP cancels an active request and "
        "SAVE writes the exact downloaded bytes to the next unused "
        "download name.\n\n"
        "HTTPS remains unavailable until Nostalux has a reviewed TLS service, "
        "certificate validation, trusted time, and secure entropy.",
        "READY - TYPE AN ADDRESS AND PRESS ENTER");
}

static void browser_about_system(void) {
    struct app_network_status network;
    if (!app_result_ok(app_network_status_get(&network))) {
        browser_set_page(
            "System status is unavailable.",
            "STATUS FAILED");
        return;
    }
    char page[BROWSER_CONTENT_CAPACITY] =
        "Nostalux Browser system page\n\nIsolation: ring 3\nNetwork device: ";
    browser_append(
        page, sizeof(page),
        (network.flags & APP_NETWORK_DEVICE_PRESENT) != 0
            ? "RTL8139 present\n" : "not detected\n");
    browser_append(page, sizeof(page), "Link: ");
    browser_append(
        page, sizeof(page),
        (network.flags & APP_NETWORK_LINK_UP) != 0
            ? "up\n" : "down\n");
    browser_append(page, sizeof(page), "IPv4: ");
    if ((network.flags & APP_NETWORK_CONFIGURED) == 0) {
        browser_append(
            page, sizeof(page),
            (network.flags & APP_NETWORK_DHCP_ACTIVE) != 0
                ? "DHCP configuring\n" : "not configured\n");
    } else {
        for (unsigned int octet = 0; octet < 4u; octet++) {
            if (octet != 0) browser_append(page, sizeof(page), ".");
            const unsigned int shift = 24u - octet * 8u;
            browser_append_number(
                page, sizeof(page),
                (network.ipv4_address >> shift) & 0xffu);
        }
        browser_append(page, sizeof(page), "\n");
    }
    browser_append(page, sizeof(page), "Packets received: ");
    browser_append_number(
        page, sizeof(page), network.received_packets);
    browser_append(page, sizeof(page), "\nPackets sent: ");
    browser_append_number(
        page, sizeof(page), network.transmitted_packets);
    browser_append(page, sizeof(page), "\nPackets dropped/errors: ");
    browser_append_number(
        page, sizeof(page), network.dropped_packets);
    browser_append(
        page, sizeof(page),
        "\n\nUse System Monitor for CPU and memory counters.");
    browser_set_page(
        page, "LIVE NETWORK COUNTERS - REFRESHES EACH SECOND");
}

static void browser_load_file(const char* path) {
    const size_t path_length = app_text_length(path);
    if (path_length == 0 || path_length > APP_FILE_PATH_MAX) {
        browser_set_page(
            "The file name is invalid.", "INVALID FILE NAME");
        return;
    }
    uint64_t handle = app_file_open(
        path, path_length, APP_FILE_OPEN_READ);
    if (!app_result_ok(handle)) {
        browser_set_page(
            "The requested file could not be opened.",
            "FILE OPEN FAILED");
        return;
    }
    size_t length = 0;
    bool ok = true;
    bool truncated = false;
    while (length + 1u < sizeof(g_browser.raw)) {
        size_t capacity =
            sizeof(g_browser.raw) - 1u - length;
        if (capacity > APP_FILE_TRANSFER_MAX) {
            capacity = APP_FILE_TRANSFER_MAX;
        }
        uint64_t read = app_file_read(
            handle, g_browser.raw + length,
            capacity, length);
        if (!app_result_ok(read)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        length += (size_t)read;
        if ((size_t)read < capacity) break;
    }
    if (ok && length + 1u == sizeof(g_browser.raw)) {
        uint8_t extra;
        const uint64_t read =
            app_file_read(handle, &extra, sizeof(extra), length);
        if (!app_result_ok(read) || read > sizeof(extra)) {
            ok = false;
        } else {
            truncated = read != 0;
        }
    }
    if (!app_result_ok(app_file_close(handle))) ok = false;
    if (!ok) {
        browser_set_page(
            "The requested file could not be read.",
            "FILE READ FAILED");
        return;
    }
    g_browser.raw[length] = '\0';
    g_browser.raw_length = length;
    g_browser.response_available = false;
    browser_make_display();
    g_browser.first_line = 0;
    const bool live_log =
        browser_text_equal(g_browser.address, "file:system.log");
    if (truncated) {
        browser_set_status(
            live_log
                ? "LIVE KERNEL LOG - TRUNCATED TO 8191 BYTES"
                : "LOCAL FILE - TRUNCATED TO 8191 BYTES");
    } else {
        browser_set_status(
            live_log
                ? "LIVE KERNEL LOG - REFRESHES EACH SECOND"
                : "LOCAL FILE");
    }
}

static void browser_start_http(void) {
    const struct app_network_http_request request = {
        .url = g_browser.address,
        .url_length = g_browser.address_length,
        .response_capacity = APP_NETWORK_RESPONSE_MAX,
        .flags = APP_NETWORK_HTTP_FOLLOW_REDIRECTS,
        .timeout_milliseconds = 10000u,
    };
    uint64_t handle = app_network_http_start(&request);
    if (!app_result_ok(handle) || handle == 0) {
        browser_set_page(
            "The HTTP request could not be started. Another app may be using "
            "the single network connection.",
            "NETWORK SERVICE BUSY OR UNAVAILABLE");
        return;
    }
    g_browser.request_handle = handle;
    g_browser.loading = true;
    g_browser.response_available = false;
    g_browser.raw_length = 0;
    g_browser.raw[0] = '\0';
    app_text_copy(
        g_browser.display, sizeof(g_browser.display),
        "Starting asynchronous HTTP request...");
    g_browser.display_length =
        app_text_length(g_browser.display);
    g_browser.first_line = 0;
    browser_set_status("STARTING");
    app_log("Browser HTTP request started");
}

static void browser_navigate(void) {
    if (g_browser.loading) return;
    g_browser.address_selected = false;
    g_browser.last_live_refresh_ms = 0;
    if (g_browser.address_length == 0 ||
        browser_text_equal(g_browser.address, "about:home")) {
        browser_about_home();
    } else if (browser_text_equal(
                   g_browser.address, "about:system")) {
        browser_about_system();
    } else if (browser_text_equal(
                   g_browser.address, "about:files")) {
        browser_set_page(
            "Open Files from the desktop to browse the flat filesystem. "
            "Selecting a text file launches this isolated Browser with a "
            "file: address.",
            "FILES ARE MANAGED BY THE FILES APP");
    } else if (browser_starts_with(
                   g_browser.address, "file:")) {
        browser_load_file(g_browser.address + 5u);
    } else if (browser_starts_with(
                   g_browser.address, "https://")) {
        browser_set_page(
            "HTTPS is not implemented yet.\n\n"
            "Nostalux will not downgrade or pretend that cleartext transport "
            "is secure. TLS requires certificate-chain and hostname "
            "validation, trusted time, and cryptographically secure entropy.",
            "HTTPS SAFELY REJECTED");
    } else if (browser_starts_with(
                   g_browser.address, "http://")) {
        browser_start_http();
    } else {
        browser_set_page(
            "Unsupported address. Use http://, file:, about:home, or "
            "about:system.",
            "UNSUPPORTED ADDRESS");
    }
}

static bool browser_refresh_live_page(void) {
    if (g_browser.loading ||
        (!browser_text_equal(g_browser.address, "about:system") &&
         !browser_text_equal(g_browser.address, "file:system.log"))) {
        return false;
    }

    struct app_time time;
    if (!app_result_ok(app_time_get(&time))) return false;
    if (g_browser.last_live_refresh_ms != 0 &&
        time.monotonic_milliseconds - g_browser.last_live_refresh_ms <
            1000u) {
        return false;
    }

    const size_t saved_first_line = g_browser.first_line;
    if (browser_text_equal(g_browser.address, "about:system")) {
        browser_about_system();
    } else {
        browser_load_file(g_browser.address + 5u);
    }
    const size_t maximum = browser_max_first_line();
    g_browser.first_line =
        saved_first_line < maximum ? saved_first_line : maximum;
    g_browser.last_live_refresh_ms = time.monotonic_milliseconds;
    return true;
}

enum browser_download_create_result {
    BROWSER_DOWNLOAD_CREATED = 0,
    BROWSER_DOWNLOAD_NAMES_EXHAUSTED,
    BROWSER_DOWNLOAD_CREATE_FAILED,
};

static enum browser_download_create_result
browser_create_unused_download(
    char path[APP_FILE_PATH_MAX + 1u]) {
    for (uint64_t attempt = 0;
         attempt < BROWSER_DOWNLOAD_ATTEMPTS; attempt++) {
        char candidate[APP_FILE_PATH_MAX + 1u] = "download";
        if (attempt != 0) {
            browser_append(candidate, sizeof(candidate), "-");
            browser_append_number(
                candidate, sizeof(candidate), attempt);
        }
        browser_append(candidate, sizeof(candidate), ".txt");

        const size_t length = app_text_length(candidate);
        const uint64_t created = app_file_create_exclusive(
            candidate, length, g_browser.raw, g_browser.raw_length);
        if (app_result_ok(created) &&
            created == g_browser.raw_length) {
            app_text_copy(
                path, APP_FILE_PATH_MAX + 1u, candidate);
            return BROWSER_DOWNLOAD_CREATED;
        }
        if ((int64_t)created == APP_STATUS_ALREADY_EXISTS) {
            continue;
        }
        path[0] = '\0';
        return BROWSER_DOWNLOAD_CREATE_FAILED;
    }
    path[0] = '\0';
    return BROWSER_DOWNLOAD_NAMES_EXHAUSTED;
}

static bool browser_replace_download_file(const char* path) {
    const size_t path_length = app_text_length(path);
    const uint64_t written = app_file_replace(
        path, path_length,
        g_browser.raw, g_browser.raw_length);
    return app_result_ok(written) &&
           written == g_browser.raw_length;
}

static bool browser_verify_download_file(const char* path) {
    const size_t path_length = app_text_length(path);
    const uint64_t handle = app_file_open(
        path, path_length, APP_FILE_OPEN_READ);
    if (!app_result_ok(handle) || handle == 0) return false;

    uint8_t bytes[256];
    size_t offset = 0;
    bool verified = true;
    while (offset < g_browser.raw_length) {
        size_t requested = g_browser.raw_length - offset;
        if (requested > sizeof(bytes)) requested = sizeof(bytes);
        const uint64_t read = app_file_read(
            handle, bytes, requested, offset);
        if (!app_result_ok(read) || read == 0 ||
            read > requested) {
            verified = false;
            break;
        }
        for (size_t index = 0; index < (size_t)read; index++) {
            if (bytes[index] !=
                (uint8_t)g_browser.raw[offset + index]) {
                verified = false;
                break;
            }
        }
        if (!verified) break;
        offset += (size_t)read;
    }
    if (verified) {
        uint8_t extra;
        const uint64_t read = app_file_read(
            handle, &extra, sizeof(extra), offset);
        verified = app_result_ok(read) && read == 0;
    }
    if (!app_result_ok(app_file_close(handle))) verified = false;
    return verified;
}

static bool browser_save_download(void) {
    if (!g_browser.response_available) {
        browser_set_status("NO COMPLETED HTTP RESPONSE TO SAVE");
        app_log(
            "Browser download save skipped: no completed response");
        return false;
    }

    char path[APP_FILE_PATH_MAX + 1u];
    const enum browser_download_create_result created =
        browser_create_unused_download(path);
    if (created == BROWSER_DOWNLOAD_NAMES_EXHAUSTED) {
        browser_set_status("DOWNLOAD SAVE FAILED - NO UNUSED FILE NAME");
        app_log(
            "Browser download save failed: no unused download name");
        return false;
    }
    if (created != BROWSER_DOWNLOAD_CREATED) {
        browser_set_status("DOWNLOAD SAVE FAILED");
        app_log("Browser download save failed: file write error");
        return false;
    }

    char status[BROWSER_STATUS_CAPACITY] =
        "SAVED EXACT RESPONSE BODY AS ";
    browser_append(status, sizeof(status), path);
    browser_set_status(status);

    char log[BROWSER_STATUS_CAPACITY] =
        "Browser download saved as ";
    browser_append(log, sizeof(log), path);
    app_log(log);
    return true;
}

static void browser_set_startup_address(void) {
    char argument[APP_STARTUP_ARGUMENT_MAX + 1u];
    const uint64_t length =
        app_argument_get(argument, sizeof(argument));
    g_browser.explicit_download = false;
    if (!app_result_ok(length) ||
        length > APP_STARTUP_ARGUMENT_MAX) {
        app_text_copy(
            g_browser.address, sizeof(g_browser.address),
            "about:home");
    } else if (length == 0) {
        app_text_copy(
            g_browser.address, sizeof(g_browser.address),
            "about:home");
    } else {
        const char download_option[] = "--download";
        const size_t option_length =
            sizeof(download_option) - 1u;
        const char* address = argument;
        if (browser_starts_with(argument, download_option) &&
            (argument[option_length] == ' ' ||
             argument[option_length] == '\t')) {
            address += option_length;
            while (*address == ' ' || *address == '\t') address++;
            if (*address != '\0') {
                g_browser.explicit_download = true;
            } else {
                address = "about:home";
            }
        }
        app_text_copy(
            g_browser.address, sizeof(g_browser.address),
            address);
    }
    g_browser.address_length =
        app_text_length(g_browser.address);
}

static void browser_handle_key(char key) {
    if (key == 27) return;
    if (g_browser.loading) return;
    if (key == '\n' || key == '\r') {
        browser_navigate();
        return;
    }
    if (key == '\b') {
        if (g_browser.address_selected) {
            g_browser.address_length = 0;
            g_browser.address[0] = '\0';
            g_browser.address_selected = false;
        } else if (g_browser.address_length != 0) {
            g_browser.address[--g_browser.address_length] = '\0';
        }
        return;
    }
    if (key >= 32 && key <= 126) {
        if (g_browser.address_selected) {
            g_browser.address_length = 0;
            g_browser.address[0] = '\0';
            g_browser.address_selected = false;
        }
        if (g_browser.address_length + 1u <
            sizeof(g_browser.address)) {
            g_browser.address[g_browser.address_length++] = key;
            g_browser.address[g_browser.address_length] = '\0';
        }
    }
}

static void browser_handle_pointer(
    const struct app_input_event* event) {
    if (event->button != APP_POINTER_BUTTON_LEFT ||
        (event->flags != 0 &&
         (event->flags & APP_INPUT_FLAG_PRESSED) == 0)) {
        return;
    }
    if (event->y >= 42 && event->y < 69) {
        if (event->x >= 8 && event->x < 351 &&
            !g_browser.loading) {
            g_browser.address_selected = true;
        } else if (event->x >= 357 && event->x < 400 &&
                   !g_browser.loading) {
            browser_navigate();
        } else if (event->x >= 405 && event->x < 452) {
            if (g_browser.loading) {
                browser_cancel_request();
            } else {
                (void)browser_save_download();
            }
        }
        return;
    }
    if (event->y >= 76 && event->y < 185) {
        if (g_browser.first_line != 0) {
            g_browser.first_line--;
        }
    } else if (event->y >= 185 && event->y < 294) {
        const size_t maximum = browser_max_first_line();
        if (g_browser.first_line < maximum) {
            g_browser.first_line++;
        }
    }
}

static bool browser_run_explicit_download(void) {
    if (!g_browser.explicit_download) return false;
    app_log("Browser explicit download mode started");
    if (!g_browser.loading) {
        app_log(
            "Browser explicit download failed: request did not start");
        return false;
    }

    while (g_browser.loading) {
        browser_poll_request();
        if (g_browser.loading) app_yield();
    }

    static const char path[] = "download.txt";
    bool saved = false;
    if (g_browser.response_available) {
        if (!browser_replace_download_file(path)) {
            browser_set_status("EXPLICIT DOWNLOAD WRITE FAILED");
            app_log(
                "Browser explicit download failed: file write error");
        } else if (!browser_verify_download_file(path)) {
            browser_set_status("EXPLICIT DOWNLOAD VERIFY FAILED");
            app_log(
                "Browser explicit download failed: verification error");
        } else {
            browser_set_status(
                "VERIFIED EXACT RESPONSE BODY AS download.txt");
            char digest[BROWSER_STATUS_CAPACITY] =
                "Browser explicit download digest: bytes=";
            browser_append_number(
                digest, sizeof(digest), g_browser.raw_length);
            browser_append(digest, sizeof(digest), " fnv1a=");
            browser_append_number(
                digest, sizeof(digest),
                browser_fnv1a32(
                    (const uint8_t*)g_browser.raw,
                    g_browser.raw_length));
            app_log(digest);
            app_log(
                "Browser explicit download verified: download.txt");
            saved = true;
        }
    } else {
        app_log(
            "Browser explicit download failed: response unavailable");
    }
    if (!saved) browser_close_request();
    return saved;
}

static int browser_handle_window_open_failure(void) {
    if (g_browser.loading) browser_cancel_request();
    browser_close_request();
    app_log(
        "Browser window unavailable; no download file was saved");
    return 1;
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    browser_clear_bytes(&g_browser, sizeof(g_browser));
    browser_set_startup_address();
    app_log("Browser app started");
    browser_about_home();
    if (!browser_text_equal(
            g_browser.address, "about:home")) {
        browser_navigate();
    }

    if (g_browser.explicit_download) {
        app_exit(browser_run_explicit_download() ? 0 : 1);
    }

    struct app_ui ui;
    if (!app_ui_open(&ui, 460, 330, "Browser (isolated)")) {
        app_exit(browser_handle_window_open_failure());
    }
    browser_render(&ui);

    bool close_requested = false;
    for (;;) {
        const bool was_loading = g_browser.loading;
        const size_t old_received =
            g_browser.raw_length;
        browser_poll_request();
        const bool live_refreshed = browser_refresh_live_page();

        struct app_input_event event;
        bool changed = was_loading != g_browser.loading ||
                       old_received != g_browser.raw_length ||
                       live_refreshed;
        while (app_ui_poll(&ui, &event)) {
            if (event.type == APP_INPUT_WINDOW_CLOSE) {
                close_requested = true;
                break;
            }
            if (event.type == APP_INPUT_KEY &&
                (event.flags == 0 ||
                 (event.flags & APP_INPUT_FLAG_PRESSED) != 0)) {
                const char key = (char)event.key;
                if (key == 27) {
                    close_requested = true;
                    break;
                }
                browser_handle_key(key);
                changed = true;
            } else if (event.type == APP_INPUT_POINTER_BUTTON) {
                browser_handle_pointer(&event);
                changed = true;
            }
        }
        if (close_requested) break;
        if (changed || g_browser.loading) browser_render(&ui);
        app_yield();
    }

    if (g_browser.loading) browser_cancel_request();
    browser_close_request();
    app_ui_close(&ui);
    app_exit(0);
}
