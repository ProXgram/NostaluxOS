#include <stdbool.h>
#include <stdint.h>

#include "app_ui.h"

static bool calculator_apply(int64_t left, int64_t right, char operation,
                             int64_t* result) {
    if (result == NULL) return false;
    switch (operation) {
        case '+':
            return !__builtin_add_overflow(left, right, result);
        case '-':
            return !__builtin_sub_overflow(left, right, result);
        case '*':
            return !__builtin_mul_overflow(left, right, result);
        case '/':
            if (right == 0 ||
                (left == INT64_MIN && right == -1)) {
                return false;
            }
            *result = left / right;
            return true;
        default:
            *result = right;
            return true;
    }
}

struct calculator_state {
    int64_t value;
    int64_t left;
    char pending;
    bool entering;
    bool error;
};

static bool calculator_handle_key(
    struct calculator_state* state, char key) {
    if (state == NULL) return false;

    if (key == 'c' || key == 'C' || key == 27) {
        *state = (struct calculator_state){0};
        return true;
    }
    if (!state->error && key >= '0' && key <= '9') {
        int64_t next;
        if (!state->entering) state->value = 0;
        if (__builtin_mul_overflow(
                state->value, (int64_t)10, &next) ||
            __builtin_add_overflow(
                next, (int64_t)(key - '0'), &state->value)) {
            state->error = true;
        }
        state->entering = true;
        return true;
    }
    if (!state->error &&
        (key == '+' || key == '-' ||
         key == '*' || key == '/')) {
        if (state->pending != 0 && state->entering) {
            if (!calculator_apply(
                    state->left, state->value,
                    state->pending, &state->left)) {
                state->error = true;
            } else {
                state->value = state->left;
            }
        } else {
            state->left = state->value;
        }
        state->pending = key;
        state->entering = false;
        return true;
    }
    if (!state->error &&
        (key == '=' || key == '\n' || key == '\r')) {
        if (state->pending != 0 &&
            !calculator_apply(
                state->left, state->value,
                state->pending, &state->value)) {
            state->error = true;
        }
        state->left = state->value;
        state->pending = 0;
        /* The next digit starts a new expression after a result. */
        state->entering = false;
        return true;
    }
    if (key == '\b' && !state->error) {
        state->value /= 10;
        return true;
    }
    return false;
}

static void calculator_render(struct app_ui* ui, int64_t value,
                              char pending, bool error) {
    app_ui_clear(ui, 0xff172033u);
    app_ui_fill(ui, 12, 12, (int32_t)ui->width - 24, 42, 0xff0a0f18u);
    app_ui_text(ui, 18, 20, "CALCULATOR", 0xff66d9efu, 2);

    app_ui_fill(ui, 12, 68, (int32_t)ui->width - 24, 54, 0xfff2f3f5u);
    char number[28];
    if (error) {
        app_text_copy(number, sizeof(number), "ERROR");
    } else {
        (void)app_int_to_text(value, number, sizeof(number));
    }
    const size_t number_length = app_text_length(number);
    const uint32_t number_scale =
        number_length > 13u ? 2u : 3u;
    int32_t number_x =
        (int32_t)ui->width - 22 -
        (int32_t)(number_length * 6u * number_scale);
    if (number_x < 22) number_x = 22;
    app_ui_text(
        ui, number_x, number_scale == 3u ? 82 : 86,
        number, error ? 0xffc62828u : 0xff152238u,
        number_scale);

    char operation[] = "OP: NONE";
    if (pending != 0) {
        operation[4] = pending;
        operation[5] = '\0';
    }
    app_ui_text(ui, 18, 140, operation, 0xfff6c85fu, 2);
    app_ui_text(ui, 18, 172, "TYPE 0-9  + - * /", 0xffe5e9f0u, 1);
    app_ui_text(ui, 18, 188, "ENTER OR = TO SOLVE", 0xffe5e9f0u, 1);
    app_ui_text(ui, 18, 204, "C CLEARS", 0xff9fb3c8u, 1);
    (void)app_ui_present(ui);
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    struct app_ui ui;
    if (!app_ui_open(&ui, 300, 232, "Calculator (isolated)")) {
        app_log("Calculator: open it from the graphical desktop.");
        app_exit(1);
    }

    struct calculator_state state = {0};
    calculator_render(
        &ui, state.value, state.pending, state.error);

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
        if (calculator_handle_key(&state, key)) {
            calculator_render(
                &ui, state.value, state.pending, state.error);
        }
    }

    app_ui_close(&ui);
    app_exit(0);
}
