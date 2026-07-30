#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ui.h"

static void assistant_time_answer(char* answer, size_t capacity) {
    struct app_time time;
    uint64_t result =
        app_syscall2(APP_SYSCALL_TIME_GET,
                     (uint64_t)(uintptr_t)&time, sizeof(time));
    if (!app_result_ok(result) || capacity < 24u) {
        app_text_copy(answer, capacity, "RTC TIME IS UNAVAILABLE.");
        return;
    }
    app_text_copy(answer, capacity, "TIME 00:00:00 - REAL RTC");
    answer[5] = (char)('0' + time.hour / 10u);
    answer[6] = (char)('0' + time.hour % 10u);
    answer[8] = (char)('0' + time.minute / 10u);
    answer[9] = (char)('0' + time.minute % 10u);
    answer[11] = (char)('0' + time.second / 10u);
    answer[12] = (char)('0' + time.second % 10u);
}

static void assistant_append_octet(
    char* text, size_t capacity, uint8_t value) {
    char digits[3];
    size_t count = 0;
    if (value >= 100u) {
        digits[count++] = (char)('0' + value / 100u);
    }
    if (value >= 10u) {
        digits[count++] =
            (char)('0' + (value / 10u) % 10u);
    }
    digits[count++] = (char)('0' + value % 10u);
    for (size_t index = 0; index < count; index++) {
        size_t length = app_text_length(text);
        if (length + 1u >= capacity) return;
        text[length] = digits[index];
        text[length + 1u] = '\0';
    }
}

static void assistant_append_ipv4(
    char* text, size_t capacity, uint32_t address) {
    for (unsigned int index = 0; index < 4u; index++) {
        if (index != 0u) {
            size_t length = app_text_length(text);
            if (length + 1u >= capacity) return;
            text[length] = '.';
            text[length + 1u] = '\0';
        }
        unsigned int shift = 24u - index * 8u;
        assistant_append_octet(
            text, capacity, (uint8_t)(address >> shift));
    }
}

static void assistant_network_answer(
    char* answer, size_t capacity) {
    struct app_network_status status;
    uint64_t result = app_syscall2(
        APP_SYSCALL_NETWORK_STATUS,
        (uint64_t)(uintptr_t)&status, sizeof(status));
    if (!app_result_ok(result)) {
        app_text_copy(
            answer, capacity, "NETWORK STATUS IS UNAVAILABLE.");
        return;
    }
    if ((status.flags & APP_NETWORK_DEVICE_PRESENT) == 0u) {
        app_text_copy(
            answer, capacity,
            "NO COMPATIBLE NETWORK CARD WAS DETECTED.");
        return;
    }
    if ((status.flags & APP_NETWORK_LINK_UP) == 0u) {
        app_text_copy(
            answer, capacity,
            "THE NETWORK CARD IS REAL, BUT ITS LINK IS DOWN.");
        return;
    }
    if ((status.flags & APP_NETWORK_CONFIGURED) == 0u) {
        app_text_copy(
            answer, capacity,
            (status.flags & APP_NETWORK_DHCP_ACTIVE) != 0u
                ? "THE LINK IS UP. DHCP IS REQUESTING AN ADDRESS."
                : "THE LINK IS UP, BUT IP IS NOT CONFIGURED.");
        return;
    }

    app_text_copy(answer, capacity, "NETWORK ONLINE. IP ");
    assistant_append_ipv4(
        answer, capacity, status.ipv4_address);
    size_t length = app_text_length(answer);
    const char suffix[] = ". BROWSER LOADS HTTP.";
    for (size_t index = 0;
         suffix[index] != '\0' && length + 1u < capacity;
         index++) {
        answer[length++] = suffix[index];
    }
    answer[length] = '\0';
}

static void assistant_answer(const char* prompt,
                             char* answer, size_t capacity) {
    if (app_text_has_word(prompt, "TIME") ||
        app_text_has_word(prompt, "CLOCK")) {
        assistant_time_answer(answer, capacity);
    } else if (app_text_has_word(prompt, "FILE") ||
               app_text_has_word(prompt, "FILES") ||
               app_text_has_word(prompt, "STORAGE") ||
               app_text_has_word(prompt, "NOTEPAD")) {
        app_text_copy(
            answer, capacity,
            "FILES USE THE REAL OS FILE SERVICE. STORAGE MAY BE ATA OR VOLATILE.");
    } else if (app_text_has_word(prompt, "CPU") ||
               app_text_has_word(prompt, "MEMORY") ||
               app_text_has_word(prompt, "TASK") ||
               app_text_has_word(prompt, "TASKS")) {
        app_text_copy(
            answer, capacity,
            "SYSTEM MONITOR READS REAL KERNEL COUNTERS.");
    } else if (app_text_has_word(prompt, "INTERNET") ||
               app_text_has_word(prompt, "NETWORK") ||
               app_text_has_word(prompt, "BROWSER")) {
        assistant_network_answer(answer, capacity);
    } else if (app_text_has_word(prompt, "CALCULATOR") ||
               app_text_has_word(prompt, "IMAGE") ||
               app_text_has_word(prompt, "IMAGES") ||
               app_text_has_word(prompt, "APP") ||
               app_text_has_word(prompt, "APPS")) {
        app_text_copy(
            answer, capacity,
            "OPEN APPS FROM START. ELF APPS RUN IN RING 3.");
    } else if (app_text_has_word(prompt, "WHO") ||
               app_text_has_word(prompt, "AI")) {
        app_text_copy(
            answer, capacity,
            "I AM A SMALL OFFLINE RULE-BASED ASSISTANT.");
    } else if (app_text_has_word(prompt, "HELLO") ||
               app_text_has_word(prompt, "HI")) {
        app_text_copy(answer, capacity, "HELLO. NOSTALUX IS READY.");
    } else if (app_text_has_word(prompt, "HELP") ||
               prompt[0] == '\0') {
        app_text_copy(
            answer, capacity,
            "ASK ABOUT TIME FILES STORAGE APPS MEMORY OR NETWORK.");
    } else {
        app_text_copy(
            answer, capacity,
            "I DO NOT KNOW THAT YET. TRY HELP FOR REAL TOPICS.");
    }
}

static void assistant_wrapped_text(struct app_ui* ui, int32_t x, int32_t y,
                                   const char* text, uint32_t color,
                                   size_t columns) {
    size_t column = 0;
    for (size_t index = 0; text[index] != '\0'; index++) {
        if (text[index] == '\n' || column >= columns) {
            x -= (int32_t)(column * 6u);
            y += 11;
            column = 0;
            if (text[index] == '\n') continue;
        }
        app_ui_char(ui, x, y, text[index], color, 1);
        x += 6;
        column++;
    }
}

static void assistant_render(struct app_ui* ui, const char* prompt,
                             const char* answer) {
    app_ui_clear(ui, 0xff101827u);
    app_ui_fill(ui, 0, 0, (int32_t)ui->width, 40, 0xff283b61u);
    app_ui_text(ui, 13, 8, "AI ASSISTANT", 0xffffffffu, 2);
    app_ui_text(ui, 13, 27, "OFFLINE RULE-BASED ELF APP",
                0xff8bd5c5u, 1);

    app_ui_text(ui, 14, 56, "YOU", 0xfff6c85fu, 1);
    app_ui_fill(ui, 14, 70, (int32_t)ui->width - 28, 42, 0xfff5f7fau);
    assistant_wrapped_text(ui, 22, 80, prompt, 0xff18212bu, 64u);
    int32_t cursor_x =
        22 + (int32_t)((app_text_length(prompt) % 64u) * 6u);
    int32_t cursor_y =
        89 + (int32_t)((app_text_length(prompt) / 64u) * 11u);
    app_ui_fill(ui, cursor_x, cursor_y, 5, 2, 0xff2067a0u);

    app_ui_text(ui, 14, 130, "ASSISTANT", 0xff8bd5c5u, 1);
    app_ui_fill(ui, 14, 144, (int32_t)ui->width - 28, 72, 0xff1d2a41u);
    assistant_wrapped_text(ui, 22, 155, answer, 0xffe8edf4u, 64u);
    app_ui_text(ui, 14, (int32_t)ui->height - 25,
                "ENTER ASKS - ESC CLOSES", 0xff9fb3c8u, 1);
    (void)app_ui_present(ui);
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    struct app_ui ui;
    if (!app_ui_open(&ui, 460, 260, "AI Assistant (isolated)")) {
        app_log("AI Assistant: open it from the graphical desktop.");
        app_exit(1);
    }

    char prompt[128] = {0};
    char answer[160];
    size_t prompt_length = 0;
    assistant_answer("", answer, sizeof(answer));
    assistant_render(&ui, prompt, answer);

    for (;;) {
        struct app_input_event event;
        if (!app_ui_poll(&ui, &event)) {
            app_yield();
            continue;
        }
        if (event.type == APP_INPUT_WINDOW_CLOSE) break;
        if (event.type != APP_INPUT_KEY ||
            (event.flags != 0 &&
             (event.flags & APP_INPUT_FLAG_PRESSED) == 0)) {
            continue;
        }
        char key = (char)event.key;
        if (key == 27) break;
        if (key == '\b') {
            if (prompt_length > 0) {
                prompt[--prompt_length] = '\0';
                assistant_render(&ui, prompt, answer);
            }
        } else if (key == '\n' || key == '\r') {
            assistant_answer(prompt, answer, sizeof(answer));
            prompt_length = 0;
            prompt[0] = '\0';
            assistant_render(&ui, prompt, answer);
        } else if (key >= 32 && key <= 126 &&
                   prompt_length + 1u < sizeof(prompt)) {
            prompt[prompt_length++] = key;
            prompt[prompt_length] = '\0';
            assistant_render(&ui, prompt, answer);
        }
    }

    app_ui_close(&ui);
    app_exit(0);
}
