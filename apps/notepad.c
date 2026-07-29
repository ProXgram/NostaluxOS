#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ui.h"

#define NOTE_CAPACITY 8192u

static bool notepad_save(const char* path,
                         const char* text, size_t length) {
    const uint64_t written = app_file_replace(
        path, app_text_length(path), text, length);
    return app_result_ok(written) && written == length;
}

static size_t notepad_view_start(
    const char* text, size_t cursor,
    size_t columns, size_t visible_lines) {
    size_t line = 0;
    size_t column = 0;
    for (size_t index = 0; index < cursor; index++) {
        if (text[index] == '\n') {
            line++;
            column = 0;
        } else {
            if (column >= columns) {
                line++;
                column = 0;
            }
            column++;
        }
    }
    /*
     * When the cursor is before more text at an exact soft-wrap boundary,
     * the renderer places it at the start of the following visual line.
     */
    if (text[cursor] != '\0' &&
        text[cursor] != '\n' &&
        column >= columns) {
        line++;
    }
    const size_t first_line =
        line + 1u > visible_lines
            ? line + 1u - visible_lines
            : 0u;
    if (first_line == 0) return 0;

    line = 0;
    column = 0;
    for (size_t index = 0; index < cursor; index++) {
        if (text[index] == '\n') {
            line++;
            column = 0;
            if (line == first_line) return index + 1u;
        } else {
            if (column >= columns) {
                line++;
                column = 0;
                if (line == first_line) return index;
            }
            column++;
        }
    }
    return cursor;
}

static size_t notepad_index_at_position(
    const char* text, size_t length, size_t start,
    size_t columns, size_t target_line, size_t target_column) {
    size_t line = 0;
    size_t column = 0;
    for (size_t index = start; index < length; index++) {
        const char value = text[index];
        if (value != '\n' && column >= columns) {
            line++;
            column = 0;
        }
        if (line > target_line) return index;
        if (line == target_line && column >= target_column) {
            return index;
        }
        if (value == '\n') {
            if (line == target_line) return index;
            line++;
            column = 0;
        } else {
            column++;
        }
    }
    return length;
}

static bool notepad_insert_character(
    char* text, size_t capacity,
    size_t* length, size_t* cursor, char value) {
    if (text == NULL || length == NULL || cursor == NULL ||
        *cursor > *length || *length + 1u >= capacity) {
        return false;
    }
    for (size_t index = *length; index > *cursor; index--) {
        text[index] = text[index - 1u];
    }
    text[*cursor] = value;
    (*cursor)++;
    (*length)++;
    text[*length] = '\0';
    return true;
}

static bool notepad_backspace(
    char* text, size_t* length, size_t* cursor) {
    if (text == NULL || length == NULL || cursor == NULL ||
        *cursor == 0 || *cursor > *length) {
        return false;
    }
    (*cursor)--;
    for (size_t index = *cursor; index < *length; index++) {
        text[index] = text[index + 1u];
    }
    (*length)--;
    text[*length] = '\0';
    return true;
}

static void notepad_render(struct app_ui* ui, const char* text,
                           size_t length, size_t cursor,
                           const char* path, bool dirty,
                           bool save_failed, bool full) {
    app_ui_clear(ui, 0xffd9dde3u);
    app_ui_fill(ui, 0, 0, (int32_t)ui->width, 32, 0xff18324bu);
    app_ui_text(ui, 12, 7, "NOTEPAD -", 0xffffffffu, 1);
    app_ui_text(ui, 72, 7, path, 0xffffffffu, 1);
    app_ui_fill(ui, 10, 43, (int32_t)ui->width - 20,
                (int32_t)ui->height - 77, 0xfffffff8u);

    const size_t columns = 54u;
    const size_t visible_lines = 19u;
    size_t start = notepad_view_start(
        text, cursor, columns, visible_lines);
    int32_t x = 17;
    int32_t y = 52;
    size_t column = 0;
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    bool cursor_visible = false;
    for (size_t index = start;
         index <= length && y + 8 < (int32_t)ui->height - 34;
         index++) {
        if (index < length &&
            text[index] != '\n' && column >= columns) {
            x = 17;
            y += 10;
            column = 0;
        }
        if (index == cursor) {
            cursor_x = x;
            cursor_y = y;
            cursor_visible = true;
        }
        if (index == length) break;

        char value = text[index];
        if (value == '\n') {
            x = 17;
            y += 10;
            column = 0;
            continue;
        }
        app_ui_char(ui, x, y, value, 0xff18212bu, 1);
        x += 6;
        column++;
    }
    if (cursor_visible) {
        app_ui_fill(
            ui, cursor_x, cursor_y + 7, 5, 2, 0xff2067a0u);
    }

    const char* status =
        save_failed ? "SAVE FAILED - CLOSE TO RETRY"
                    : full ? "FILE FULL - BACKSPACE TO EDIT"
                    : dirty ? "UNSAVED - CLOSE TO SAVE"
                            : "CLICK TEXT TO MOVE CURSOR";
    app_ui_text(ui, 12, (int32_t)ui->height - 23,
                status, save_failed ? 0xffc62828u : 0xff294861u, 1);
    (void)app_ui_present(ui);
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    char path[APP_STARTUP_ARGUMENT_MAX + 1u];
    uint64_t argument_length =
        app_argument_get(path, sizeof(path));
    if (!app_result_ok(argument_length) ||
        argument_length > APP_STARTUP_ARGUMENT_MAX) {
        app_log("Notepad: invalid startup filename.");
        app_exit(1);
    }
    if (argument_length == 0) {
        app_text_copy(path, sizeof(path), "notes.txt");
    }
    char text[NOTE_CAPACITY];
    size_t length = 0;
    bool save_failed = false;

    uint64_t handle = app_file_open(
        path, app_text_length(path),
        APP_FILE_OPEN_READ | APP_FILE_OPEN_WRITE | APP_FILE_OPEN_CREATE);
    if (!app_result_ok(handle)) {
        app_log("Notepad: unable to open its file.");
        app_exit(1);
    }
    bool load_ok = true;
    while (length + 1u < sizeof(text)) {
        size_t request = sizeof(text) - 1u - length;
        if (request > APP_FILE_TRANSFER_MAX) {
            request = APP_FILE_TRANSFER_MAX;
        }
        uint64_t read =
            app_file_read(handle, text + length, request, length);
        if (!app_result_ok(read)) {
            load_ok = false;
            break;
        }
        if (read == 0) break;
        length += (size_t)read;
        if (read < request) break;
    }
    if (!app_result_ok(app_file_close(handle))) {
        load_ok = false;
    }
    if (!load_ok) {
        app_log("Notepad: unable to read its file.");
        app_exit(1);
    }
    text[length] = '\0';
    size_t cursor = length;

    struct app_ui ui;
    if (!app_ui_open(&ui, 420, 280, "Notepad (isolated)")) {
        app_log("Notepad: open it from the graphical desktop.");
        app_exit(1);
    }

    bool dirty = false;
    bool full = false;
    notepad_render(
        &ui, text, length, cursor, path,
        dirty, save_failed, full);
    for (;;) {
        struct app_input_event event;
        if (!app_ui_poll(&ui, &event)) {
            app_yield();
            continue;
        }
        if (event.type == APP_INPUT_WINDOW_CLOSE) {
            if (!dirty || notepad_save(path, text, length)) {
                dirty = false;
                save_failed = false;
                break;
            }
            save_failed = true;
            app_log("Notepad: file save failed; close to retry.");
            notepad_render(
                &ui, text, length, cursor, path,
                dirty, save_failed, full);
            continue;
        }
        if (event.type == APP_INPUT_POINTER_BUTTON &&
            event.button == APP_POINTER_BUTTON_LEFT &&
            (event.flags == 0 ||
             (event.flags & APP_INPUT_FLAG_PRESSED) != 0) &&
            event.x >= 10 &&
            event.x < (int32_t)ui.width - 10 &&
            event.y >= 43 &&
            event.y < (int32_t)ui.height - 34) {
            const size_t columns = 54u;
            const size_t start = notepad_view_start(
                text, cursor, columns, 19u);
            const size_t target_line =
                event.y <= 52
                    ? 0u
                    : (size_t)(event.y - 52) / 10u;
            const size_t target_column =
                event.x <= 17
                    ? 0u
                    : (size_t)(event.x - 17 + 3) / 6u;
            cursor = notepad_index_at_position(
                text, length, start, columns,
                target_line, target_column);
            notepad_render(
                &ui, text, length, cursor, path,
                dirty, save_failed, full);
            continue;
        }
        if (event.type != APP_INPUT_KEY ||
            (event.flags != 0 &&
             (event.flags & APP_INPUT_FLAG_PRESSED) == 0)) {
            continue;
        }

        char key = (char)event.key;
        bool changed = false;
        if (key == '\b') {
            if (notepad_backspace(
                    text, &length, &cursor)) {
                dirty = true;
                full = false;
                changed = true;
            }
        } else if (key == '\n' || key == '\r' ||
                   (key >= 32 && key <= 126)) {
            const char value =
                key == '\n' || key == '\r' ? '\n' : key;
            if (notepad_insert_character(
                    text, sizeof(text),
                    &length, &cursor, value)) {
                dirty = true;
                full = false;
                changed = true;
            } else {
                full = true;
                notepad_render(
                    &ui, text, length, cursor, path,
                    dirty, save_failed, full);
            }
        }
        if (changed) {
            save_failed = false;
            notepad_render(
                &ui, text, length, cursor, path,
                dirty, save_failed, full);
        }
    }

    app_ui_close(&ui);
    app_exit(0);
}
