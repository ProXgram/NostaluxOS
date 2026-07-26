#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

enum {
    KEYBOARD_MAX_LINE = 128,
    KEYBOARD_HISTORY_LIMIT = 16,
};

typedef void (*keyboard_idle_callback_t)(void);

/* Initializes the keyboard driver and unmasks IRQ 1 */
void keyboard_init(void);

/* Called by the interrupt handler (IRQ1) to push raw scancodes */
void keyboard_push_byte(uint8_t byte);

/* Blocking: Waits for a key */
char keyboard_get_char(void);

/* Non-Blocking: Returns character if available, or 0 if buffer empty */
char keyboard_poll_char(void);

/* Modified to accept an idle callback */
void keyboard_read_line_ex(char* buffer, size_t size, keyboard_idle_callback_t on_idle);
void keyboard_read_line(char* buffer, size_t size); // Legacy wrapper

void keyboard_history_record(const char* line);
size_t keyboard_history_length(void);
const char* keyboard_history_entry(size_t index);
const char* keyboard_history_step(int direction);
void keyboard_history_reset_iteration(void);

#endif /* KEYBOARD_H */
