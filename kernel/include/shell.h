#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stddef.h>

void shell_run(void);

/*
 * Execute one shell command through the normal command dispatcher while
 * capturing its text output. The output buffer is always NUL-terminated when
 * output_capacity is nonzero. Returns false only for invalid arguments or
 * when another terminal capture is already active.
 */
bool shell_execute_capture(const char* command_line,
                           char* output,
                           size_t output_capacity,
                           size_t* output_length,
                           bool* truncated);

#endif /* SHELL_H */
