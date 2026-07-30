#include "app_network_services.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_abi.h"
#include "app_manifest.h"
#include "network.h"
#include "paging.h"

#define TEST_USER_BASE (PAGING_USER_BASE + 0x100000u)
#define TEST_USER_MEMORY_SIZE 0x4000u
#define TEST_REQUEST_ADDRESS (TEST_USER_BASE + 0x100u)
#define TEST_URL_ADDRESS (TEST_USER_BASE + 0x300u)
#define TEST_STATUS_ADDRESS (TEST_USER_BASE + 0x1000u)
#define TEST_READ_ADDRESS (TEST_USER_BASE + 0x1800u)

_Static_assert(sizeof(struct app_network_http_request) == 32u,
               "network request ABI size changed");
_Static_assert(offsetof(struct app_network_http_request, url_length) == 8u,
               "network request URL length offset changed");
_Static_assert(offsetof(struct app_network_http_request,
                        response_capacity) == 16u,
               "network request response capacity offset changed");
_Static_assert(offsetof(struct app_network_http_request, flags) == 24u,
               "network request flags offset changed");
_Static_assert(offsetof(struct app_network_http_request,
                        timeout_milliseconds) == 28u,
               "network request timeout offset changed");
_Static_assert(sizeof(struct app_network_request_status) == 40u,
               "network request status ABI size changed");
_Static_assert(offsetof(struct app_network_request_status,
                        received_bytes) == 24u,
               "network status received byte offset changed");
_Static_assert(offsetof(struct app_network_request_status,
                        total_bytes) == 32u,
               "network status total byte offset changed");

struct paging_address_space {
    uint8_t memory[TEST_USER_MEMORY_SIZE];
    bool reject_copy_from;
    bool reject_copy_to;
    bool reject_range_check;
    unsigned int copy_from_calls;
    unsigned int copy_to_calls;
    unsigned int range_check_calls;
};

struct mock_network_backend {
    enum network_result start_result;
    uint64_t start_handle;
    const char* retained_url;
    size_t start_capacity;
    bool start_follow_redirects;
    uint32_t start_timeout;
    unsigned int start_calls;

    bool status_succeeds;
    struct network_http_request_status status;
    unsigned int status_calls;
    uint64_t status_handle;

    enum network_result read_result;
    uint8_t read_bytes[APP_NETWORK_TRANSFER_MAX];
    size_t read_bytes_to_write;
    size_t read_reported_length;
    unsigned int read_calls;
    uint64_t read_handle;
    size_t read_capacity;
    size_t read_offset;

    enum network_result cancel_result;
    unsigned int cancel_calls;
    uint64_t cancel_handle;

    enum network_result close_result;
    enum network_result temporary_close_result;
    unsigned int close_failures_remaining;
    unsigned int close_calls;
    uint64_t close_handle;
};

static struct mock_network_backend g_backend;

static uint64_t app_error_value(enum app_status status) {
    return (uint64_t)(int64_t)status;
}

static bool user_range(const struct paging_address_space* space,
                       uint64_t address, size_t count, size_t* offset) {
    if (space == NULL || address < TEST_USER_BASE) return false;
    uint64_t relative = address - TEST_USER_BASE;
    if (relative > TEST_USER_MEMORY_SIZE ||
        count > TEST_USER_MEMORY_SIZE - (size_t)relative) {
        return false;
    }
    if (offset != NULL) *offset = (size_t)relative;
    return true;
}

bool paging_copy_from_user(const struct paging_address_space* space,
                           void* kernel_destination,
                           uint64_t user_source, size_t count) {
    struct paging_address_space* mutable_space =
        (struct paging_address_space*)(uintptr_t)space;
    mutable_space->copy_from_calls++;
    size_t offset = 0u;
    if (space->reject_copy_from ||
        (count != 0u && kernel_destination == NULL) ||
        !user_range(space, user_source, count, &offset)) {
        return false;
    }
    memcpy(kernel_destination, space->memory + offset, count);
    return true;
}

bool paging_copy_to_user(const struct paging_address_space* space,
                         uint64_t user_destination,
                         const void* kernel_source, size_t count) {
    struct paging_address_space* mutable_space =
        (struct paging_address_space*)(uintptr_t)space;
    mutable_space->copy_to_calls++;
    size_t offset = 0u;
    if (space->reject_copy_to ||
        (count != 0u && kernel_source == NULL) ||
        !user_range(space, user_destination, count, &offset)) {
        return false;
    }
    memcpy(mutable_space->memory + offset, kernel_source, count);
    return true;
}

bool paging_user_range_mapped(const struct paging_address_space* space,
                              uint64_t address, size_t count,
                              bool require_writable) {
    (void)require_writable;
    struct paging_address_space* mutable_space =
        (struct paging_address_space*)(uintptr_t)space;
    mutable_space->range_check_calls++;
    return !space->reject_range_check &&
           user_range(space, address, count, NULL);
}

enum network_result network_http_request_start(
    const char* url, size_t body_capacity, bool follow_redirects,
    uint32_t timeout_milliseconds, uint64_t* out_handle) {
    g_backend.start_calls++;
    g_backend.retained_url = url;
    g_backend.start_capacity = body_capacity;
    g_backend.start_follow_redirects = follow_redirects;
    g_backend.start_timeout = timeout_milliseconds;
    if (out_handle != NULL) *out_handle = g_backend.start_handle;
    return g_backend.start_result;
}

bool network_http_request_get_status(
    uint64_t handle, struct network_http_request_status* out_status) {
    g_backend.status_calls++;
    g_backend.status_handle = handle;
    if (!g_backend.status_succeeds || out_status == NULL) return false;
    *out_status = g_backend.status;
    return true;
}

enum network_result network_http_request_read(
    uint64_t handle, void* buffer, size_t capacity, size_t offset,
    size_t* out_length) {
    g_backend.read_calls++;
    g_backend.read_handle = handle;
    g_backend.read_capacity = capacity;
    g_backend.read_offset = offset;
    if (g_backend.read_result != NETWORK_OK) return g_backend.read_result;
    size_t write_size = g_backend.read_bytes_to_write;
    if (write_size > capacity) write_size = capacity;
    if (write_size != 0u) memcpy(buffer, g_backend.read_bytes, write_size);
    if (out_length != NULL) {
        *out_length = g_backend.read_reported_length;
    }
    return NETWORK_OK;
}

enum network_result network_http_request_cancel(uint64_t handle) {
    g_backend.cancel_calls++;
    g_backend.cancel_handle = handle;
    return g_backend.cancel_result;
}

enum network_result network_http_request_close(uint64_t handle) {
    g_backend.close_calls++;
    g_backend.close_handle = handle;
    if (g_backend.close_failures_remaining != 0u) {
        g_backend.close_failures_remaining--;
        return g_backend.temporary_close_result;
    }
    return g_backend.close_result;
}

static void reset_space(struct paging_address_space* space) {
    memset(space, 0, sizeof(*space));
}

static void reset_backend(void) {
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend.start_result = NETWORK_OK;
    g_backend.start_handle = 0xABCDEFu;
    g_backend.status_succeeds = true;
    g_backend.status.state = NETWORK_HTTP_REQUEST_COMPLETE;
    g_backend.status.result = NETWORK_OK;
    g_backend.status.http_status = 200u;
    g_backend.status.received_bytes = 4u;
    g_backend.status.total_bytes = 4u;
    g_backend.status.total_known = true;
    g_backend.read_result = NETWORK_OK;
    memcpy(g_backend.read_bytes, "DATA", 4u);
    g_backend.read_bytes_to_write = 4u;
    g_backend.read_reported_length = 4u;
    g_backend.cancel_result = NETWORK_OK;
    g_backend.close_result = NETWORK_OK;
    g_backend.temporary_close_result = NETWORK_BUSY;
}

static void store_user_bytes(struct paging_address_space* space,
                             uint64_t address, const void* source,
                             size_t count) {
    size_t offset = 0u;
    assert(user_range(space, address, count, &offset));
    memcpy(space->memory + offset, source, count);
}

static void load_user_bytes(const struct paging_address_space* space,
                            uint64_t address, void* destination,
                            size_t count) {
    size_t offset = 0u;
    assert(user_range(space, address, count, &offset));
    memcpy(destination, space->memory + offset, count);
}

static void prepare_request(struct paging_address_space* space,
                            const char* url, size_t response_capacity,
                            uint32_t flags, uint32_t timeout_ms) {
    size_t url_length = strlen(url);
    assert(url_length <= APP_NETWORK_URL_MAX);
    store_user_bytes(space, TEST_URL_ADDRESS, url, url_length);
    const struct app_network_http_request request = {
        .url = (const char*)(uintptr_t)TEST_URL_ADDRESS,
        .url_length = url_length,
        .response_capacity = response_capacity,
        .flags = flags,
        .timeout_milliseconds = timeout_ms,
    };
    store_user_bytes(space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
}

static uint64_t dispatch(struct paging_address_space* space,
                         uint64_t process_id, uint64_t capabilities,
                         uint64_t syscall_id, uint64_t argument1,
                         uint64_t argument2, uint64_t argument3,
                         uint64_t argument4) {
    return app_network_services_dispatch(
        syscall_id, space, process_id, capabilities, argument1, argument2,
        argument3, argument4, 0u);
}

static uint64_t start_request(struct paging_address_space* space,
                              uint64_t process_id,
                              uint64_t capabilities) {
    return dispatch(
        space, process_id, capabilities, APP_SYSCALL_NETWORK_HTTP_START,
        TEST_REQUEST_ADDRESS, sizeof(struct app_network_http_request), 0u,
        0u);
}

static void test_start_snapshot_and_isolation(void) {
    struct paging_address_space space;
    reset_space(&space);
    reset_backend();
    prepare_request(&space, "http://example.com/path", 64u,
                    APP_NETWORK_HTTP_FOLLOW_REDIRECTS, 5000u);

    const uint64_t capability = APP_CAPABILITY_NETWORK;
    const uint64_t process_id = 101u;
    assert(start_request(&space, process_id, 0u) ==
           app_error_value(APP_STATUS_PERMISSION_DENIED));
    assert(g_backend.start_calls == 0u);
    assert(dispatch(&space, 0u, capability,
                    APP_SYSCALL_NETWORK_HTTP_START,
                    TEST_REQUEST_ADDRESS,
                    sizeof(struct app_network_http_request), 0u, 0u) ==
           app_error_value(APP_STATUS_UNSUPPORTED));
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_HTTP_START,
                    TEST_REQUEST_ADDRESS,
                    sizeof(struct app_network_http_request) - 1u, 0u, 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_HTTP_START,
                    TEST_USER_BASE + TEST_USER_MEMORY_SIZE + 1u,
                    sizeof(struct app_network_http_request), 0u, 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(g_backend.start_calls == 0u);

    struct app_network_http_request request;
    load_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    const struct app_network_http_request valid_request = request;
    request.url =
        (const char*)(uintptr_t)(TEST_USER_BASE + TEST_USER_MEMORY_SIZE + 1u);
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    request = valid_request;
    request.response_capacity = 0u;
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    request.response_capacity = APP_NETWORK_RESPONSE_MAX + 1u;
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    request = valid_request;
    request.timeout_milliseconds = 99u;
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    request = valid_request;
    request.flags = 1u << 31;
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &request, sizeof(request));
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    store_user_bytes(&space, TEST_REQUEST_ADDRESS, &valid_request,
                     sizeof(valid_request));
    const uint8_t embedded_zero = 0u;
    store_user_bytes(&space, TEST_URL_ADDRESS + 5u, &embedded_zero, 1u);
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    prepare_request(&space, "http://example.com/path", 64u,
                    APP_NETWORK_HTTP_FOLLOW_REDIRECTS, 5000u);
    assert(g_backend.start_calls == 0u);

    uint64_t handle = start_request(&space, process_id, capability);
    assert((int64_t)handle > 0);
    assert(g_backend.start_calls == 1u);
    assert(g_backend.start_capacity == 64u);
    assert(g_backend.start_follow_redirects);
    assert(g_backend.start_timeout == 5000u);
    assert(g_backend.retained_url !=
           (const char*)(uintptr_t)TEST_URL_ADDRESS);
    assert(strcmp(g_backend.retained_url,
                  "http://example.com/path") == 0);

    const char replacement[] = "XXXXXXXXXXXXXXXXXXXXXXX";
    store_user_bytes(&space, TEST_URL_ADDRESS, replacement,
                     sizeof(replacement) - 1u);
    assert(strcmp(g_backend.retained_url,
                  "http://example.com/path") == 0);

    prepare_request(&space, "http://second.example/", 64u, 0u, 5000u);
    assert(start_request(&space, process_id + 1u, capability) ==
           app_error_value(APP_STATUS_WOULD_BLOCK));
    assert(g_backend.start_calls == 1u);

    assert(dispatch(&space, process_id + 1u, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CANCEL, handle, 0u, 0u,
                    0u) == app_error_value(APP_STATUS_BAD_HANDLE));
    assert(g_backend.cancel_calls == 0u);
    assert(dispatch(&space, process_id, 0u,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) ==
           app_error_value(APP_STATUS_PERMISSION_DENIED));
    assert(g_backend.close_calls == 0u);

    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == APP_STATUS_OK);
    assert(g_backend.close_calls == 1u);
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == app_error_value(APP_STATUS_BAD_HANDLE));
}

static void test_status_and_read_validation(void) {
    struct paging_address_space space;
    reset_space(&space);
    reset_backend();
    prepare_request(&space, "http://status.example/", 64u, 0u, 5000u);
    const uint64_t process_id = 201u;
    const uint64_t capability = APP_CAPABILITY_NETWORK;
    uint64_t handle = start_request(&space, process_id, capability);
    assert((int64_t)handle > 0);

    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status) - 1u, 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(g_backend.status_calls == 0u);
    space.reject_range_check = true;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(g_backend.status_calls == 0u);
    space.reject_range_check = false;

    g_backend.status.state = NETWORK_HTTP_REQUEST_CANCELED;
    g_backend.status.result = NETWORK_CANCELED;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           APP_STATUS_OK);
    struct app_network_request_status status;
    load_user_bytes(&space, TEST_STATUS_ADDRESS, &status, sizeof(status));
    assert(status.state == APP_NETWORK_REQUEST_CANCELED);
    assert(status.error == APP_NETWORK_ERROR_CANCELED);
    assert(status.http_status == 200u);
    assert(status.received_bytes == 4u);
    assert(status.total_bytes == 4u);
    assert((status.flags & APP_NETWORK_STATUS_TOTAL_KNOWN) != 0u);
    assert(status.reserved == 0u);

    unsigned int status_calls = g_backend.status_calls;
    g_backend.status.received_bytes = 65u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    assert(g_backend.status_calls == status_calls + 1u);
    g_backend.status.received_bytes = 4u;
    g_backend.status.redirect_count = APP_NETWORK_REDIRECT_MAX + 1u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.status.redirect_count = 0u;
    g_backend.status.total_bytes = 3u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.status.total_bytes = 4u;
    g_backend.status.total_bytes = 65u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.status.total_bytes = 4u;
    g_backend.status.http_status = 1000u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.status.http_status = 200u;
    g_backend.status.result =
        (enum network_result)(NETWORK_CANCELED + 1u);
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.status.result = NETWORK_CANCELED;

    space.reject_copy_to = true;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    space.reject_copy_to = false;

    unsigned int read_calls = g_backend.read_calls;
    assert(dispatch(&space, process_id + 1u, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_BAD_HANDLE));
    assert(dispatch(&space, process_id, 0u,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_PERMISSION_DENIED));
    assert(g_backend.read_calls == read_calls);
    space.reject_range_check = true;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(g_backend.read_calls == read_calls);
    space.reject_range_check = false;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 2u, 63u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle, 0u, 0u,
                    64u) == 0u);
    assert(g_backend.read_calls == read_calls);

    g_backend.read_reported_length = 5u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_IO_ERROR));
    g_backend.read_reported_length = 4u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 8u) == 4u);
    uint8_t bytes[4];
    load_user_bytes(&space, TEST_READ_ADDRESS, bytes, sizeof(bytes));
    assert(memcmp(bytes, "DATA", 4u) == 0);
    assert(g_backend.read_capacity == 4u);
    assert(g_backend.read_offset == 8u);

    g_backend.read_result = NETWORK_BUSY;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_WOULD_BLOCK));
    g_backend.read_result = NETWORK_OK;
    space.reject_copy_to = true;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_READ, handle,
                    TEST_READ_ADDRESS, 4u, 0u) ==
           app_error_value(APP_STATUS_INVALID_ARGUMENT));
    space.reject_copy_to = false;

    g_backend.cancel_result = NETWORK_CANCELED;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CANCEL, handle, 0u, 0u,
                    0u) == APP_STATUS_OK);
    assert(g_backend.cancel_handle == g_backend.read_handle);
    g_backend.cancel_result = NETWORK_OK;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == APP_STATUS_OK);
}

static void test_close_retry_and_process_release_cleanup(void) {
    struct paging_address_space space;
    reset_space(&space);
    reset_backend();
    prepare_request(&space, "http://close.example/", 64u, 0u, 5000u);
    const uint64_t capability = APP_CAPABILITY_NETWORK;
    const uint64_t process_id = 301u;
    uint64_t handle = start_request(&space, process_id, capability);
    assert((int64_t)handle > 0);

    g_backend.close_failures_remaining = 1u;
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == app_error_value(APP_STATUS_IO_ERROR));
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           APP_STATUS_OK);
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == APP_STATUS_OK);

    reset_backend();
    prepare_request(&space, "http://orphan.example/", 64u, 0u, 5000u);
    const uint64_t released_process = 401u;
    handle = start_request(&space, released_process, capability);
    assert((int64_t)handle > 0);
    const char* retained_url = g_backend.retained_url;
    assert(strcmp(retained_url, "http://orphan.example/") == 0);

    app_network_services_release_process(released_process + 1u);
    assert(g_backend.cancel_calls == 0u);
    assert(g_backend.close_calls == 0u);
    g_backend.close_failures_remaining = 2u;
    app_network_services_release_process(released_process);
    assert(g_backend.cancel_calls == 1u);
    assert(g_backend.close_calls == 1u);
    assert(strcmp(retained_url, "http://orphan.example/") == 0);
    assert(dispatch(&space, released_process, capability,
                    APP_SYSCALL_NETWORK_REQUEST_STATUS, handle,
                    TEST_STATUS_ADDRESS,
                    sizeof(struct app_network_request_status), 0u) ==
           app_error_value(APP_STATUS_BAD_HANDLE));

    prepare_request(&space, "http://replacement.example/", 64u, 0u,
                    5000u);
    assert(start_request(&space, released_process + 1u, capability) ==
           app_error_value(APP_STATUS_WOULD_BLOCK));
    assert(g_backend.start_calls == 1u);
    assert(g_backend.cancel_calls == 2u);
    assert(g_backend.close_calls == 2u);
    assert(strcmp(retained_url, "http://orphan.example/") == 0);

    uint64_t replacement =
        start_request(&space, released_process + 1u, capability);
    assert((int64_t)replacement > 0);
    assert(g_backend.start_calls == 2u);
    assert(g_backend.cancel_calls == 3u);
    assert(g_backend.close_calls == 3u);
    assert(strcmp(g_backend.retained_url,
                  "http://replacement.example/") == 0);
    assert(dispatch(&space, released_process + 1u, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, replacement, 0u,
                    0u, 0u) == APP_STATUS_OK);
}

static void test_backend_start_anomalies_are_contained(void) {
    struct paging_address_space space;
    reset_space(&space);
    reset_backend();
    prepare_request(&space, "http://failure.example/", 64u, 0u, 5000u);
    const uint64_t process_id = 501u;
    const uint64_t capability = APP_CAPABILITY_NETWORK;

    g_backend.start_result = NETWORK_IO_ERROR;
    g_backend.start_handle = 0x999u;
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_IO_ERROR));
    assert(g_backend.cancel_calls == 1u);
    assert(g_backend.cancel_handle == 0x999u);
    assert(g_backend.close_calls == 1u);
    assert(g_backend.close_handle == 0x999u);

    g_backend.start_result = NETWORK_OK;
    g_backend.start_handle = 0u;
    assert(start_request(&space, process_id, capability) ==
           app_error_value(APP_STATUS_IO_ERROR));

    g_backend.start_handle = 0x777u;
    uint64_t handle = start_request(&space, process_id, capability);
    assert((int64_t)handle > 0);
    assert(dispatch(&space, process_id, capability,
                    APP_SYSCALL_NETWORK_REQUEST_CLOSE, handle, 0u, 0u,
                    0u) == APP_STATUS_OK);
}

int main(void) {
    test_start_snapshot_and_isolation();
    test_status_and_read_validation();
    test_close_retry_and_process_release_cleanup();
    test_backend_start_anomalies_are_contained();
    puts("app network services tests passed");
    return 0;
}
