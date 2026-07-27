#include "syslog.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef NOSTALUX_HOST_TEST
#include "io.h"
#endif

#define SYSLOG_CAPACITY 64
#define SYSLOG_MESSAGE_LEN 80

static char g_entries[SYSLOG_CAPACITY][SYSLOG_MESSAGE_LEN];
static size_t g_start = 0;
static size_t g_count = 0;

static void copy_message(char* dest, const char* src) {
    size_t i = 0;
    if (dest == NULL || src == NULL) {
        return;
    }
    for (; i < SYSLOG_MESSAGE_LEN - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

void syslog_init(void) {
    g_start = 0;
    g_count = 0;
    for (size_t i = 0; i < SYSLOG_CAPACITY; i++) {
        g_entries[i][0] = '\0';
    }
}

void syslog_write(const char* message) {
    if (message == NULL) {
        return;
    }

#ifndef NOSTALUX_HOST_TEST
    /*
     * Check the current privilege level before using the x86 debug port.
     * Host-side tests compile this module natively, including on ARM64, and
     * exercise only the in-memory ring below.
     */
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    bool is_kernel = (cs & 3) == 0;

    if (is_kernel) {
        for (const char* c = message; *c != '\0'; c++) {
            outb(0xE9, (uint8_t)*c);
        }
        outb(0xE9, '\n');
    }
#endif

    size_t index;
    if (g_count < SYSLOG_CAPACITY) {
        index = (g_start + g_count) % SYSLOG_CAPACITY;
        g_count++;
    } else {
        index = g_start;
        g_start = (g_start + 1) % SYSLOG_CAPACITY;
    }

    copy_message(g_entries[index], message);
}

size_t syslog_length(void) {
    return g_count;
}

const char* syslog_entry(size_t index) {
    if (index >= g_count) {
        return NULL;
    }
    size_t actual = (g_start + index) % SYSLOG_CAPACITY;
    return g_entries[actual];
}

size_t syslog_copy_text(char* buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) {
        return 0;
    }

    size_t available = capacity - 1;
    size_t first = g_count;
    size_t required = 0;

    /*
     * A filesystem log file is smaller than the complete ring buffer. Keep
     * the newest complete entries that fit rather than presenting stale boot
     * messages while omitting the event that the user is investigating.
     */
    while (first > 0) {
        const char* message = syslog_entry(first - 1);
        size_t length = 0;
        while (message != NULL && message[length] != '\0') length++;
        if (length + 1 > available - required) break;
        required += length + 1;
        first--;
    }

    size_t written = 0;
    for (size_t i = first; i < g_count; i++) {
        const char* message = syslog_entry(i);
        for (size_t j = 0;
             message != NULL && message[j] != '\0' && written < available;
             j++) {
            buffer[written++] = message[j];
        }
        if (written < available) buffer[written++] = '\n';
    }

    /*
     * Very small callers may not have room for one complete entry. They
     * still receive a truthful, truncated copy of the newest message.
     */
    if (written == 0 && g_count != 0 && available != 0) {
        const char* message = syslog_entry(g_count - 1);
        while (message != NULL && *message != '\0' && written < available) {
            buffer[written++] = *message++;
        }
    }

    buffer[written] = '\0';
    return written;
}
