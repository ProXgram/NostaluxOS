#include "syslog.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "io.h"

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

    // Check current privilege level (CPL) from CS register
    // If CPL is 3 (User Mode), we cannot execute 'outb' without GPF.
    // We skip the debug port output in that case.
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    bool is_kernel = (cs & 3) == 0;

    if (is_kernel) {
        for (const char* c = message; *c != '\0'; c++) {
            outb(0xE9, (uint8_t)*c);
        }
        outb(0xE9, '\n');
    }

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
