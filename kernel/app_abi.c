#include "app_abi.h"

bool app_abi_syscall_known(uint64_t syscall_id) {
    return syscall_id >= APP_SYSCALL_ABI_QUERY &&
           syscall_id <= APP_SYSCALL_FILE_CREATE_EXCLUSIVE;
}

uint64_t app_abi_required_capability(uint64_t syscall_id) {
    switch (syscall_id) {
        case APP_SYSCALL_ABI_QUERY:
        case APP_SYSCALL_EXIT:
        case APP_SYSCALL_YIELD:
        case APP_SYSCALL_ARGUMENT_GET:
            return 0;
        case APP_SYSCALL_LOG_WRITE:
            return APP_CAPABILITY_LOG;
        case APP_SYSCALL_TIME_GET:
            return APP_CAPABILITY_TIME;
        case APP_SYSCALL_NETWORK_STATUS:
        case APP_SYSCALL_NETWORK_HTTP_START:
        case APP_SYSCALL_NETWORK_REQUEST_STATUS:
        case APP_SYSCALL_NETWORK_REQUEST_READ:
        case APP_SYSCALL_NETWORK_REQUEST_CANCEL:
        case APP_SYSCALL_NETWORK_REQUEST_CLOSE:
            return APP_CAPABILITY_NETWORK;
        case APP_SYSCALL_FILE_OPEN:
            /*
             * OPEN is capability-neutral. The kernel checks the requested
             * open flags against read/write capabilities before returning a
             * handle.
             */
            return 0;
        case APP_SYSCALL_FILE_READ:
            return APP_CAPABILITY_FILE_READ;
        case APP_SYSCALL_FILE_WRITE:
        case APP_SYSCALL_FILE_REPLACE:
        case APP_SYSCALL_FILE_CREATE_EXCLUSIVE:
            return APP_CAPABILITY_FILE_WRITE;
        case APP_SYSCALL_FILE_CLOSE:
            return 0;
        case APP_SYSCALL_WINDOW_CREATE:
        case APP_SYSCALL_WINDOW_PRESENT:
        case APP_SYSCALL_WINDOW_CLOSE:
            return APP_CAPABILITY_WINDOW;
        case APP_SYSCALL_INPUT_POLL:
            return APP_CAPABILITY_INPUT;
        case APP_SYSCALL_MEMORY_MAP:
        case APP_SYSCALL_MEMORY_UNMAP:
            return APP_CAPABILITY_MEMORY;
        default:
            return UINT64_MAX;
    }
}

const char* app_abi_syscall_name(uint64_t syscall_id) {
    switch (syscall_id) {
        case APP_SYSCALL_ABI_QUERY: return "abi_query";
        case APP_SYSCALL_EXIT: return "exit";
        case APP_SYSCALL_YIELD: return "yield";
        case APP_SYSCALL_LOG_WRITE: return "log_write";
        case APP_SYSCALL_TIME_GET: return "time_get";
        case APP_SYSCALL_FILE_OPEN: return "file_open";
        case APP_SYSCALL_FILE_READ: return "file_read";
        case APP_SYSCALL_FILE_WRITE: return "file_write";
        case APP_SYSCALL_FILE_CLOSE: return "file_close";
        case APP_SYSCALL_WINDOW_CREATE: return "window_create";
        case APP_SYSCALL_WINDOW_PRESENT: return "window_present";
        case APP_SYSCALL_WINDOW_CLOSE: return "window_close";
        case APP_SYSCALL_INPUT_POLL: return "input_poll";
        case APP_SYSCALL_MEMORY_MAP: return "memory_map";
        case APP_SYSCALL_MEMORY_UNMAP: return "memory_unmap";
        case APP_SYSCALL_ARGUMENT_GET: return "argument_get";
        case APP_SYSCALL_FILE_REPLACE: return "file_replace";
        case APP_SYSCALL_NETWORK_STATUS: return "network_status";
        case APP_SYSCALL_NETWORK_HTTP_START:
            return "network_http_start";
        case APP_SYSCALL_NETWORK_REQUEST_STATUS:
            return "network_request_status";
        case APP_SYSCALL_NETWORK_REQUEST_READ:
            return "network_request_read";
        case APP_SYSCALL_NETWORK_REQUEST_CANCEL:
            return "network_request_cancel";
        case APP_SYSCALL_NETWORK_REQUEST_CLOSE:
            return "network_request_close";
        case APP_SYSCALL_FILE_CREATE_EXCLUSIVE:
            return "file_create_exclusive";
        default: return "unknown";
    }
}
