#ifndef APP_ABI_H
#define APP_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_manifest.h"

/*
 * Provisional Nostalux Apps v1 register ABI.
 *
 *   RDI: syscall number
 *   RSI, RDX, RCX, R8, R9: arguments 1-5
 *   RAX: non-negative result or a sign-extended enum app_status value
 *
 * Calls:
 *   ABI_QUERY       (out_info, out_size)
 *   EXIT            (exit_code)
 *   YIELD           ()
 *   LOG_WRITE       (bytes, length)
 *   TIME_GET        (out_time, out_size)
 *   FILE_OPEN       (path, path_length, open_flags) -> file handle
 *   FILE_READ       (handle, buffer, length, offset) -> byte count
 *   FILE_WRITE      (handle, buffer, length, offset) -> byte count
 *   FILE_CLOSE      (handle)
 *   WINDOW_CREATE   (create_request, request_size) -> window handle
 *   WINDOW_PRESENT  (present_request, request_size)
 *   WINDOW_CLOSE    (window_handle)
 *   INPUT_POLL      (window_handle, out_event, out_size) -> 0 or 1 event
 *   MEMORY_MAP      (byte_count, protection_flags) -> user address
 *   MEMORY_UNMAP    (user_address, byte_count)
 *
 * Ring-3 apps reach this ABI through the checked INT 0x80 gate. ABI_QUERY,
 * EXIT, YIELD, LOG_WRITE, and TIME_GET are implemented. The remaining
 * reserved v1 calls return APP_STATUS_UNSUPPORTED until their backing
 * subsystems and handle validation are complete.
 */
enum app_syscall_id {
    APP_SYSCALL_ABI_QUERY = 0x1000,
    APP_SYSCALL_EXIT,
    APP_SYSCALL_YIELD,
    APP_SYSCALL_LOG_WRITE,
    APP_SYSCALL_TIME_GET,
    APP_SYSCALL_FILE_OPEN,
    APP_SYSCALL_FILE_READ,
    APP_SYSCALL_FILE_WRITE,
    APP_SYSCALL_FILE_CLOSE,
    APP_SYSCALL_WINDOW_CREATE,
    APP_SYSCALL_WINDOW_PRESENT,
    APP_SYSCALL_WINDOW_CLOSE,
    APP_SYSCALL_INPUT_POLL,
    APP_SYSCALL_MEMORY_MAP,
    APP_SYSCALL_MEMORY_UNMAP,
};

enum app_status {
    APP_STATUS_OK = 0,
    APP_STATUS_INVALID_ARGUMENT = -1,
    APP_STATUS_UNSUPPORTED = -2,
    APP_STATUS_PERMISSION_DENIED = -3,
    APP_STATUS_NOT_FOUND = -4,
    APP_STATUS_NO_SPACE = -5,
    APP_STATUS_IO_ERROR = -6,
    APP_STATUS_WOULD_BLOCK = -7,
    APP_STATUS_BAD_HANDLE = -8,
};

enum app_file_open_flags {
    APP_FILE_OPEN_READ     = 1u << 0,
    APP_FILE_OPEN_WRITE    = 1u << 1,
    APP_FILE_OPEN_CREATE   = 1u << 2,
    APP_FILE_OPEN_TRUNCATE = 1u << 3,
};

enum app_memory_protection {
    APP_MEMORY_READ  = 1u << 0,
    APP_MEMORY_WRITE = 1u << 1,
};

enum app_input_type {
    APP_INPUT_NONE = 0,
    APP_INPUT_KEY,
    APP_INPUT_POINTER_MOTION,
    APP_INPUT_POINTER_BUTTON,
    APP_INPUT_WINDOW_CLOSE,
};

struct app_abi_info {
    uint32_t abi_version;
    uint32_t page_size;
    uint64_t granted_capabilities;
};

struct app_time {
    uint64_t monotonic_milliseconds;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t reserved[5];
};

struct app_window_create {
    uint32_t width;
    uint32_t height;
    const char* title;
    size_t title_length;
};

struct app_window_present {
    uint64_t window_handle;
    const uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
};

struct app_input_event {
    uint32_t type;
    uint32_t flags;
    int32_t x;
    int32_t y;
    uint32_t key;
    uint32_t button;
};

bool app_abi_syscall_known(uint64_t syscall_id);
uint64_t app_abi_required_capability(uint64_t syscall_id);
const char* app_abi_syscall_name(uint64_t syscall_id);

#endif /* APP_ABI_H */
