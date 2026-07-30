#include "app_network_services.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_abi.h"
#include "app_manifest.h"
#include "network.h"
#include "paging.h"

#define APP_NETWORK_TIMEOUT_MIN_MS 100u
#define APP_NETWORK_TIMEOUT_MAX_MS 60000u

struct app_network_record {
    bool in_use;
    bool cleanup_pending;
    uint64_t process_id;
    uint64_t app_handle;
    uint64_t network_handle;
    size_t response_capacity;
    char url[APP_NETWORK_URL_MAX + 1u];
};

static struct app_network_record g_request;
static uint64_t g_next_handle = 1u;

static uint64_t app_error(enum app_status status) {
    return (uint64_t)(int64_t)status;
}

static bool has_network_capability(uint64_t capabilities) {
    return (capabilities & APP_CAPABILITY_NETWORK) != 0u;
}

static void clear_record(void) {
    uint8_t* bytes = (uint8_t*)&g_request;
    for (size_t index = 0; index < sizeof(g_request); index++) {
        bytes[index] = 0u;
    }
}

/*
 * A process can disappear while the backend is temporarily unable to close
 * its request.  Retain the kernel-owned URL and backend handle, but remove all
 * process ownership immediately.  A later start retries this cleanup before
 * the single-request backend can be reused.
 */
static bool retry_pending_cleanup(void) {
    if (!g_request.cleanup_pending) return true;
    (void)network_http_request_cancel(g_request.network_handle);
    if (network_http_request_close(g_request.network_handle) != NETWORK_OK) {
        return false;
    }
    clear_record();
    return true;
}

static void retire_backend_request(void) {
    const uint64_t network_handle = g_request.network_handle;
    g_request.in_use = false;
    g_request.cleanup_pending = true;
    g_request.process_id = 0u;
    g_request.app_handle = 0u;
    (void)network_http_request_cancel(network_handle);
    if (network_http_request_close(network_handle) == NETWORK_OK) {
        clear_record();
    }
}

static struct app_network_record* find_record(
    uint64_t process_id, uint64_t handle) {
    if (!g_request.in_use || process_id == 0 || handle == 0 ||
        g_request.process_id != process_id ||
        g_request.app_handle != handle) {
        return NULL;
    }
    return &g_request;
}

static uint64_t allocate_handle(void) {
    if (g_next_handle == 0u ||
        g_next_handle > (uint64_t)INT64_MAX) {
        g_next_handle = 1u;
    }
    const uint64_t handle = g_next_handle;
    if (g_next_handle == (uint64_t)INT64_MAX) {
        g_next_handle = 1u;
    } else {
        g_next_handle++;
    }
    return handle;
}

static enum app_status start_error(enum network_result result) {
    switch (result) {
        case NETWORK_BUSY:
            return APP_STATUS_WOULD_BLOCK;
        case NETWORK_INVALID_ARGUMENT:
            return APP_STATUS_INVALID_ARGUMENT;
        case NETWORK_UNSUPPORTED:
            return APP_STATUS_UNSUPPORTED;
        case NETWORK_RESPONSE_TOO_LARGE:
            return APP_STATUS_NO_SPACE;
        case NETWORK_NOT_READY:
        case NETWORK_LINK_DOWN:
        case NETWORK_TIMEOUT:
        case NETWORK_DNS_FAILED:
        case NETWORK_CONNECTION_FAILED:
        case NETWORK_PROTOCOL_ERROR:
        case NETWORK_IO_ERROR:
        case NETWORK_CANCELED:
            return APP_STATUS_IO_ERROR;
        case NETWORK_OK:
            return APP_STATUS_OK;
    }
    return APP_STATUS_IO_ERROR;
}

static uint32_t app_request_state(
    enum network_http_request_state state) {
    switch (state) {
        case NETWORK_HTTP_REQUEST_WAITING:
            return APP_NETWORK_REQUEST_WAITING;
        case NETWORK_HTTP_REQUEST_CONFIGURING:
            return APP_NETWORK_REQUEST_CONFIGURING;
        case NETWORK_HTTP_REQUEST_RESOLVING:
            return APP_NETWORK_REQUEST_RESOLVING;
        case NETWORK_HTTP_REQUEST_CONNECTING:
            return APP_NETWORK_REQUEST_CONNECTING;
        case NETWORK_HTTP_REQUEST_SENDING:
            return APP_NETWORK_REQUEST_SENDING;
        case NETWORK_HTTP_REQUEST_RECEIVING:
            return APP_NETWORK_REQUEST_RECEIVING;
        case NETWORK_HTTP_REQUEST_REDIRECTING:
            return APP_NETWORK_REQUEST_REDIRECTING;
        case NETWORK_HTTP_REQUEST_COMPLETE:
            return APP_NETWORK_REQUEST_COMPLETE;
        case NETWORK_HTTP_REQUEST_FAILED:
            return APP_NETWORK_REQUEST_FAILED;
        case NETWORK_HTTP_REQUEST_CANCELED:
            return APP_NETWORK_REQUEST_CANCELED;
    }
    return APP_NETWORK_REQUEST_FAILED;
}

static uint32_t app_request_error(enum network_result result) {
    switch (result) {
        case NETWORK_OK:
            return APP_NETWORK_ERROR_NONE;
        case NETWORK_NOT_READY:
            return APP_NETWORK_ERROR_NOT_READY;
        case NETWORK_LINK_DOWN:
            return APP_NETWORK_ERROR_LINK_DOWN;
        case NETWORK_BUSY:
            return APP_NETWORK_ERROR_BUSY;
        case NETWORK_INVALID_ARGUMENT:
            return APP_NETWORK_ERROR_INVALID_URL;
        case NETWORK_TIMEOUT:
            return APP_NETWORK_ERROR_TIMEOUT;
        case NETWORK_DNS_FAILED:
            return APP_NETWORK_ERROR_DNS;
        case NETWORK_CONNECTION_FAILED:
            return APP_NETWORK_ERROR_CONNECTION;
        case NETWORK_PROTOCOL_ERROR:
            return APP_NETWORK_ERROR_PROTOCOL;
        case NETWORK_RESPONSE_TOO_LARGE:
            return APP_NETWORK_ERROR_RESPONSE_TOO_LARGE;
        case NETWORK_UNSUPPORTED:
            return APP_NETWORK_ERROR_UNSUPPORTED;
        case NETWORK_IO_ERROR:
            return APP_NETWORK_ERROR_IO;
        case NETWORK_CANCELED:
            return APP_NETWORK_ERROR_CANCELED;
    }
    return APP_NETWORK_ERROR_IO;
}

static uint64_t dispatch_start(
    const struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t user_request,
    uint64_t request_size) {
    if (!has_network_capability(capabilities)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (!retry_pending_cleanup()) {
        return app_error(APP_STATUS_WOULD_BLOCK);
    }
    if (g_request.in_use) {
        return app_error(APP_STATUS_WOULD_BLOCK);
    }
    if (space == NULL || user_request == 0 ||
        request_size < sizeof(struct app_network_http_request)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    struct app_network_http_request request;
    if (!paging_copy_from_user(
            space, &request, user_request, sizeof(request))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if (request.url == NULL || request.url_length == 0 ||
        request.url_length > APP_NETWORK_URL_MAX ||
        request.response_capacity == 0 ||
        request.response_capacity > APP_NETWORK_RESPONSE_MAX ||
        request.timeout_milliseconds < APP_NETWORK_TIMEOUT_MIN_MS ||
        request.timeout_milliseconds > APP_NETWORK_TIMEOUT_MAX_MS ||
        (request.flags & ~APP_NETWORK_HTTP_FOLLOW_REDIRECTS) != 0u) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    char url[APP_NETWORK_URL_MAX + 1u];
    if (!paging_copy_from_user(
            space, url, (uint64_t)(uintptr_t)request.url,
            request.url_length)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    for (size_t index = 0; index < request.url_length; index++) {
        const unsigned char character = (unsigned char)url[index];
        if (character == 0 || character <= 0x20u ||
            character == 0x7fu) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
    }
    url[request.url_length] = '\0';

    /*
     * Keep the URL in kernel-owned storage for the entire asynchronous
     * lifetime. This remains safe whether the backend copies the string or
     * retains the pointer until close.
     */
    clear_record();
    for (size_t index = 0; index <= request.url_length; index++) {
        g_request.url[index] = url[index];
    }
    g_request.response_capacity = request.response_capacity;

    uint64_t network_handle = 0;
    enum network_result result = network_http_request_start(
        g_request.url, request.response_capacity,
        (request.flags & APP_NETWORK_HTTP_FOLLOW_REDIRECTS) != 0u,
        request.timeout_milliseconds, &network_handle);
    if (result != NETWORK_OK) {
        if (network_handle != 0u) {
            g_request.network_handle = network_handle;
            retire_backend_request();
        } else {
            clear_record();
        }
        return app_error(start_error(result));
    }
    if (network_handle == 0u) {
        clear_record();
        return app_error(APP_STATUS_IO_ERROR);
    }

    const uint64_t app_handle = allocate_handle();
    if (app_handle == 0) {
        g_request.network_handle = network_handle;
        retire_backend_request();
        return app_error(APP_STATUS_NO_SPACE);
    }
    g_request.in_use = true;
    g_request.cleanup_pending = false;
    g_request.process_id = process_id;
    g_request.app_handle = app_handle;
    g_request.network_handle = network_handle;
    return app_handle;
}

static uint64_t dispatch_status(
    const struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle,
    uint64_t user_status,
    uint64_t status_size) {
    if (!has_network_capability(capabilities)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_network_record* record =
        find_record(process_id, handle);
    if (record == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    if (space == NULL || user_status == 0 ||
        status_size < sizeof(struct app_network_request_status)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if (!paging_user_range_mapped(
            space, user_status,
            sizeof(struct app_network_request_status), true)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    struct network_http_request_status network_status;
    if (!network_http_request_get_status(
            record->network_handle, &network_status)) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    if ((unsigned int)network_status.state >
            (unsigned int)NETWORK_HTTP_REQUEST_CANCELED ||
        (unsigned int)network_status.result >
            (unsigned int)NETWORK_CANCELED ||
        network_status.http_status > 999u ||
        network_status.received_bytes > record->response_capacity ||
        network_status.redirect_count > APP_NETWORK_REDIRECT_MAX ||
        (network_status.total_known &&
         (network_status.total_bytes < network_status.received_bytes ||
          network_status.total_bytes > record->response_capacity))) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    const struct app_network_request_status status = {
        .state = app_request_state(network_status.state),
        .error =
            network_status.state == NETWORK_HTTP_REQUEST_CANCELED
                ? APP_NETWORK_ERROR_CANCELED
                : app_request_error(network_status.result),
        .http_status = network_status.http_status,
        .redirect_count = network_status.redirect_count,
        .flags = network_status.total_known
                   ? APP_NETWORK_STATUS_TOTAL_KNOWN : 0u,
        .reserved = 0,
        .received_bytes = network_status.received_bytes,
        .total_bytes = network_status.total_bytes,
    };
    if (!paging_copy_to_user(
            space, user_status, &status, sizeof(status))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return APP_STATUS_OK;
}

static uint64_t dispatch_read(
    const struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle,
    uint64_t user_buffer,
    uint64_t requested,
    uint64_t requested_offset) {
    if (!has_network_capability(capabilities)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_network_record* record =
        find_record(process_id, handle);
    if (record == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    if (requested > APP_NETWORK_TRANSFER_MAX ||
        requested > (uint64_t)SIZE_MAX ||
        requested_offset > (uint64_t)SIZE_MAX ||
        requested_offset > record->response_capacity ||
        requested > record->response_capacity - requested_offset ||
        (requested != 0 && (space == NULL || user_buffer == 0))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if (requested == 0) return 0;
    if (!paging_user_range_mapped(
            space, user_buffer, (size_t)requested, true)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    uint8_t bytes[APP_NETWORK_TRANSFER_MAX];
    for (size_t index = 0; index < (size_t)requested; index++) {
        bytes[index] = 0u;
    }
    size_t copied = 0;
    enum network_result result = network_http_request_read(
        record->network_handle, bytes, (size_t)requested,
        (size_t)requested_offset, &copied);
    if (result != NETWORK_OK) {
        return app_error(
            result == NETWORK_BUSY
                ? APP_STATUS_WOULD_BLOCK
                : result == NETWORK_INVALID_ARGUMENT
                    ? APP_STATUS_INVALID_ARGUMENT
                    : APP_STATUS_IO_ERROR);
    }
    if (copied > (size_t)requested) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    if (copied != 0 &&
        !paging_copy_to_user(space, user_buffer, bytes, copied)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return copied;
}

static uint64_t dispatch_cancel(
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle) {
    if (!has_network_capability(capabilities)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_network_record* record =
        find_record(process_id, handle);
    if (record == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    enum network_result result =
        network_http_request_cancel(record->network_handle);
    return result == NETWORK_OK || result == NETWORK_CANCELED
         ? APP_STATUS_OK : app_error(APP_STATUS_IO_ERROR);
}

static uint64_t dispatch_close(
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle) {
    if (!has_network_capability(capabilities)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_network_record* record =
        find_record(process_id, handle);
    if (record == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    enum network_result result =
        network_http_request_close(record->network_handle);
    if (result != NETWORK_OK) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    clear_record();
    return APP_STATUS_OK;
}

uint64_t app_network_services_dispatch(
    uint64_t syscall_id,
    struct paging_address_space* address_space,
    uint64_t process_id,
    uint64_t granted_capabilities,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3,
    uint64_t argument4,
    uint64_t argument5) {
    (void)argument5;
    if (address_space == NULL || process_id == 0) {
        return app_error(APP_STATUS_UNSUPPORTED);
    }
    switch (syscall_id) {
        case APP_SYSCALL_NETWORK_HTTP_START:
            return dispatch_start(
                address_space, process_id, granted_capabilities,
                argument1, argument2);
        case APP_SYSCALL_NETWORK_REQUEST_STATUS:
            return dispatch_status(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3);
        case APP_SYSCALL_NETWORK_REQUEST_READ:
            return dispatch_read(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3, argument4);
        case APP_SYSCALL_NETWORK_REQUEST_CANCEL:
            return dispatch_cancel(
                process_id, granted_capabilities, argument1);
        case APP_SYSCALL_NETWORK_REQUEST_CLOSE:
            return dispatch_close(
                process_id, granted_capabilities, argument1);
        default:
            return app_error(APP_STATUS_UNSUPPORTED);
    }
}

void app_network_services_release_process(uint64_t process_id) {
    if (!g_request.in_use || process_id == 0 ||
        g_request.process_id != process_id) {
        return;
    }
    retire_backend_request();
}
