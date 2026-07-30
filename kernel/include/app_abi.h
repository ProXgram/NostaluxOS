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
 *   ARGUMENT_GET     (buffer, capacity) -> startup-argument byte count
 *   FILE_REPLACE     (path, path_length, bytes, length) -> byte count
 *   NETWORK_STATUS   (out_status, out_size)
 *   NETWORK_HTTP_START (request, request_size) -> request handle
 *   NETWORK_REQUEST_STATUS (handle, out_status, out_size)
 *   NETWORK_REQUEST_READ (handle, buffer, length, offset) -> byte count
 *   NETWORK_REQUEST_CANCEL (handle)
 *   NETWORK_REQUEST_CLOSE (handle)
 *   FILE_CREATE_EXCLUSIVE (path, path_length, bytes, length) -> byte count
 *
 * Ring-3 apps reach this ABI through the checked INT 0x80 gate. All pointers
 * are user virtual addresses and are copied through the current process page
 * tables. Handles are opaque, process-owned values; an app must never infer a
 * kernel pointer from them.
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
    APP_SYSCALL_ARGUMENT_GET,
    APP_SYSCALL_FILE_REPLACE,
    APP_SYSCALL_NETWORK_STATUS,
    APP_SYSCALL_NETWORK_HTTP_START,
    APP_SYSCALL_NETWORK_REQUEST_STATUS,
    APP_SYSCALL_NETWORK_REQUEST_READ,
    APP_SYSCALL_NETWORK_REQUEST_CANCEL,
    APP_SYSCALL_NETWORK_REQUEST_CLOSE,
    /*
     * Append-only ABI: keep every existing syscall number stable.
     * Creates a new file only when the path is still absent.
     */
    APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
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
    APP_STATUS_ALREADY_EXISTS = -9,
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

enum app_input_flags {
    APP_INPUT_FLAG_PRESSED  = 1u << 0,
    APP_INPUT_FLAG_RELEASED = 1u << 1,
};

enum app_pointer_button {
    APP_POINTER_BUTTON_LEFT = 1,
    APP_POINTER_BUTTON_RIGHT = 2,
};

#define APP_FILE_PATH_MAX          31u
#define APP_FILE_TRANSFER_MAX      4096u
#define APP_MEMORY_MAP_MAX         (1024u * 1024u)
#define APP_MEMORY_PROCESS_MAX     (4u * 1024u * 1024u)
#define APP_WINDOW_TITLE_MAX       31u
#define APP_WINDOW_MIN_WIDTH       64u
#define APP_WINDOW_MIN_HEIGHT      48u
#define APP_WINDOW_MAX_WIDTH       480u
#define APP_WINDOW_MAX_HEIGHT      360u
#define APP_STARTUP_ARGUMENT_MAX   255u
#define APP_NETWORK_URL_MAX        511u
#define APP_NETWORK_RESPONSE_MAX   8191u
#define APP_NETWORK_TRANSFER_MAX   4096u
#define APP_NETWORK_REDIRECT_MAX   5u

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

enum app_network_flags {
    APP_NETWORK_DEVICE_PRESENT = 1u << 0,
    APP_NETWORK_DEVICE_READY   = 1u << 1,
    APP_NETWORK_LINK_UP        = 1u << 2,
    APP_NETWORK_CONFIGURED     = 1u << 3,
    APP_NETWORK_DHCP_ACTIVE    = 1u << 4,
};

struct app_network_status {
    uint32_t flags;
    uint8_t mac[6];
    uint8_t reserved[2];
    /* Canonical IPv4 values: a.b.c.d is a<<24 | b<<16 | c<<8 | d. */
    uint32_t ipv4_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint64_t received_packets;
    uint64_t transmitted_packets;
    uint64_t dropped_packets;
};

enum app_network_http_flags {
    APP_NETWORK_HTTP_FOLLOW_REDIRECTS = 1u << 0,
};

enum app_network_request_state {
    APP_NETWORK_REQUEST_WAITING = 0,
    APP_NETWORK_REQUEST_CONFIGURING,
    APP_NETWORK_REQUEST_RESOLVING,
    APP_NETWORK_REQUEST_CONNECTING,
    APP_NETWORK_REQUEST_SENDING,
    APP_NETWORK_REQUEST_RECEIVING,
    APP_NETWORK_REQUEST_REDIRECTING,
    APP_NETWORK_REQUEST_COMPLETE,
    APP_NETWORK_REQUEST_FAILED,
    APP_NETWORK_REQUEST_CANCELED,
};

enum app_network_request_error {
    APP_NETWORK_ERROR_NONE = 0,
    APP_NETWORK_ERROR_NOT_READY,
    APP_NETWORK_ERROR_LINK_DOWN,
    APP_NETWORK_ERROR_BUSY,
    APP_NETWORK_ERROR_INVALID_URL,
    APP_NETWORK_ERROR_TIMEOUT,
    APP_NETWORK_ERROR_DNS,
    APP_NETWORK_ERROR_CONNECTION,
    APP_NETWORK_ERROR_PROTOCOL,
    APP_NETWORK_ERROR_RESPONSE_TOO_LARGE,
    APP_NETWORK_ERROR_UNSUPPORTED,
    APP_NETWORK_ERROR_IO,
    APP_NETWORK_ERROR_CANCELED,
};

enum app_network_request_status_flags {
    APP_NETWORK_STATUS_TOTAL_KNOWN = 1u << 0,
};

struct app_network_http_request {
    const char* url;
    size_t url_length;
    size_t response_capacity;
    uint32_t flags;
    uint32_t timeout_milliseconds;
};

struct app_network_request_status {
    uint32_t state;
    uint32_t error;
    uint32_t http_status;
    uint32_t redirect_count;
    uint32_t flags;
    uint32_t reserved;
    uint64_t received_bytes;
    uint64_t total_bytes;
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

/*
 * Raw Apps v1 call helper. App code may use the typed wrappers below or call
 * this directly. The kernel never calls these wrappers.
 */
#if defined(__x86_64__) && !defined(NOSTALUX_HOST_TEST)
static inline uint64_t app_syscall5(uint64_t syscall_id,
                                    uint64_t argument1,
                                    uint64_t argument2,
                                    uint64_t argument3,
                                    uint64_t argument4,
                                    uint64_t argument5) {
    uint64_t result;
    register uint64_t register_r8 __asm__("r8") = argument4;
    register uint64_t register_r9 __asm__("r9") = argument5;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "D"(syscall_id), "S"(argument1), "d"(argument2),
          "c"(argument3), "r"(register_r8), "r"(register_r9)
        : "memory", "cc");
    return result;
}

static inline uint64_t app_syscall2(uint64_t syscall_id,
                                    uint64_t argument1,
                                    uint64_t argument2) {
    return app_syscall5(syscall_id, argument1, argument2, 0, 0, 0);
}

static inline bool app_result_is_error(uint64_t result) {
    return (int64_t)result < 0;
}

static inline uint64_t app_abi_query(struct app_abi_info* info) {
    return app_syscall2(
        APP_SYSCALL_ABI_QUERY, (uint64_t)(uintptr_t)info,
        sizeof(*info));
}

static inline uint64_t app_log_write(const void* bytes,
                                     size_t length) {
    return app_syscall2(
        APP_SYSCALL_LOG_WRITE, (uint64_t)(uintptr_t)bytes,
        (uint64_t)length);
}

static inline uint64_t app_time_get(struct app_time* time) {
    return app_syscall2(
        APP_SYSCALL_TIME_GET, (uint64_t)(uintptr_t)time,
        sizeof(*time));
}

static inline uint64_t app_argument_get(char* buffer, size_t capacity) {
    return app_syscall2(
        APP_SYSCALL_ARGUMENT_GET, (uint64_t)(uintptr_t)buffer,
        (uint64_t)capacity);
}

static inline uint64_t app_network_status_get(
    struct app_network_status* status) {
    return app_syscall2(
        APP_SYSCALL_NETWORK_STATUS,
        (uint64_t)(uintptr_t)status, sizeof(*status));
}

static inline uint64_t app_network_http_start(
    const struct app_network_http_request* request) {
    return app_syscall2(
        APP_SYSCALL_NETWORK_HTTP_START,
        (uint64_t)(uintptr_t)request, sizeof(*request));
}

static inline uint64_t app_network_request_status_get(
    uint64_t handle, struct app_network_request_status* status) {
    return app_syscall5(
        APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
        (uint64_t)(uintptr_t)status, sizeof(*status), 0, 0);
}

static inline uint64_t app_network_request_read(
    uint64_t handle, void* buffer, size_t length, size_t offset) {
    return app_syscall5(
        APP_SYSCALL_NETWORK_REQUEST_READ, handle,
        (uint64_t)(uintptr_t)buffer, length, offset, 0);
}

static inline uint64_t app_network_request_cancel(uint64_t handle) {
    return app_syscall2(
        APP_SYSCALL_NETWORK_REQUEST_CANCEL, handle, 0);
}

static inline uint64_t app_network_request_close(uint64_t handle) {
    return app_syscall2(
        APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0);
}

static inline uint64_t app_file_open(const char* path,
                                     size_t path_length,
                                     uint32_t flags) {
    return app_syscall5(APP_SYSCALL_FILE_OPEN,
                        (uint64_t)(uintptr_t)path,
                        (uint64_t)path_length, flags, 0, 0);
}

static inline uint64_t app_file_read(uint64_t handle, void* buffer,
                                     size_t length, size_t offset) {
    return app_syscall5(APP_SYSCALL_FILE_READ, handle,
                        (uint64_t)(uintptr_t)buffer,
                        (uint64_t)length, (uint64_t)offset, 0);
}

static inline uint64_t app_file_write(uint64_t handle,
                                      const void* buffer,
                                      size_t length, size_t offset) {
    return app_syscall5(APP_SYSCALL_FILE_WRITE, handle,
                        (uint64_t)(uintptr_t)buffer,
                        (uint64_t)length, (uint64_t)offset, 0);
}

static inline uint64_t app_file_replace(
    const char* path, size_t path_length,
    const void* bytes, size_t length) {
    return app_syscall5(
        APP_SYSCALL_FILE_REPLACE,
        (uint64_t)(uintptr_t)path, (uint64_t)path_length,
        (uint64_t)(uintptr_t)bytes, (uint64_t)length, 0);
}

static inline uint64_t app_file_create_exclusive(
    const char* path, size_t path_length,
    const void* bytes, size_t length) {
    return app_syscall5(
        APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
        (uint64_t)(uintptr_t)path, (uint64_t)path_length,
        (uint64_t)(uintptr_t)bytes, (uint64_t)length, 0);
}

static inline uint64_t app_file_close(uint64_t handle) {
    return app_syscall2(APP_SYSCALL_FILE_CLOSE, handle, 0);
}

static inline uint64_t app_window_create(
    const struct app_window_create* request) {
    return app_syscall2(
        APP_SYSCALL_WINDOW_CREATE,
        (uint64_t)(uintptr_t)request, sizeof(*request));
}

static inline uint64_t app_window_present(
    const struct app_window_present* request) {
    return app_syscall2(
        APP_SYSCALL_WINDOW_PRESENT,
        (uint64_t)(uintptr_t)request, sizeof(*request));
}

static inline uint64_t app_window_close(uint64_t handle) {
    return app_syscall2(APP_SYSCALL_WINDOW_CLOSE, handle, 0);
}

static inline uint64_t app_input_poll(
    uint64_t window_handle, struct app_input_event* event) {
    return app_syscall5(
        APP_SYSCALL_INPUT_POLL, window_handle,
        (uint64_t)(uintptr_t)event, sizeof(*event), 0, 0);
}

static inline uint64_t app_memory_map(size_t byte_count,
                                      uint32_t protection_flags) {
    return app_syscall2(APP_SYSCALL_MEMORY_MAP,
                        (uint64_t)byte_count, protection_flags);
}

static inline uint64_t app_memory_unmap(void* address,
                                        size_t byte_count) {
    return app_syscall2(APP_SYSCALL_MEMORY_UNMAP,
                        (uint64_t)(uintptr_t)address,
                        (uint64_t)byte_count);
}
#endif

bool app_abi_syscall_known(uint64_t syscall_id);
uint64_t app_abi_required_capability(uint64_t syscall_id);
const char* app_abi_syscall_name(uint64_t syscall_id);

#endif /* APP_ABI_H */
