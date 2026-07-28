#include <stddef.h>
#include <stdint.h>

#include "app_abi.h"

static uint64_t app_call2(uint64_t syscall_id,
                          uint64_t first_argument,
                          uint64_t second_argument) {
    uint64_t result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "D"(syscall_id),
          "S"(first_argument),
          "d"(second_argument)
        : "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
    return result;
}

static bool app_call_succeeded(uint64_t result) {
    return (int64_t)result >= 0;
}

static void log_bytes(const char* bytes, size_t length) {
    (void)app_call2(APP_SYSCALL_LOG_WRITE,
                    (uint64_t)(uintptr_t)bytes,
                    (uint64_t)length);
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    static const char greeting[] =
        "Hello from a separate Nostalux ELF app.";
    static const char abi_error[] =
        "Apps ABI query failed.";
    struct app_abi_info abi;

    uint64_t result =
        app_call2(APP_SYSCALL_ABI_QUERY,
                  (uint64_t)(uintptr_t)&abi,
                  sizeof(abi));
    if (!app_call_succeeded(result) ||
        abi.abi_version != NOSTALUX_APP_ABI_VERSION) {
        log_bytes(abi_error, sizeof(abi_error) - 1u);
    } else {
        log_bytes(greeting, sizeof(greeting) - 1u);
    }

    struct app_time time;
    result = app_call2(APP_SYSCALL_TIME_GET,
                       (uint64_t)(uintptr_t)&time,
                       sizeof(time));
    if (app_call_succeeded(result)) {
        char time_line[] = "Guest time: 00:00:00";
        time_line[12] = (char)('0' + time.hour / 10u);
        time_line[13] = (char)('0' + time.hour % 10u);
        time_line[15] = (char)('0' + time.minute / 10u);
        time_line[16] = (char)('0' + time.minute % 10u);
        time_line[18] = (char)('0' + time.second / 10u);
        time_line[19] = (char)('0' + time.second % 10u);
        log_bytes(time_line, sizeof(time_line) - 1u);
    }

    (void)app_call2(APP_SYSCALL_YIELD, 0, 0);
    (void)app_call2(APP_SYSCALL_EXIT, 0, 0);

    /*
     * EXIT must not return. Remaining in a cooperative yield loop gives a
     * future kernel a safe failure mode if it rejects the request.
     */
    for (;;) {
        (void)app_call2(APP_SYSCALL_YIELD, 0, 0);
    }
}
