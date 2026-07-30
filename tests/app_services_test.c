#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_abi.h"
#include "app_services.h"
#include "fs.h"
#include "graphics.h"
#include "heap.h"
#include "keyboard.h"
#include "mouse.h"
#include "paging.h"

struct paging_address_space {
    unsigned marker;
};

static struct fs_file g_test_files[FS_MAX_FILES];
static bool g_test_pages[
    (0x04000000u / (size_t)PAGING_PAGE_SIZE)];
static uint64_t g_rejected_user_address;
static MouseState g_mouse;
static char g_key;

void app_network_services_release_process(uint64_t process_id) {
    (void)process_id;
}

static uint64_t status_value(enum app_status status) {
    return (uint64_t)(int64_t)status;
}

static bool is_dynamic_address(uint64_t address) {
    return address >= PAGING_USER_BASE + 0x10000000ull &&
           address < PAGING_USER_BASE + 0x14000000ull;
}

static size_t dynamic_page_index(uint64_t address) {
    return (size_t)(
        (address - (PAGING_USER_BASE + 0x10000000ull)) /
        PAGING_PAGE_SIZE);
}

static bool host_user_range_valid(uint64_t address, size_t count,
                                  bool require_mapping) {
    if (count == 0) return true;
    if (address == 0 || address == g_rejected_user_address ||
        count - 1u > UINT64_MAX - address) {
        return false;
    }
    if (!is_dynamic_address(address)) return !require_mapping;

    size_t checked = 0;
    while (checked < count) {
        const uint64_t current = address + (uint64_t)checked;
        if (!is_dynamic_address(current) ||
            !g_test_pages[dynamic_page_index(current)]) {
            return false;
        }
        size_t chunk =
            (size_t)(PAGING_PAGE_SIZE -
                     (current & (PAGING_PAGE_SIZE - 1u)));
        if (chunk > count - checked) chunk = count - checked;
        checked += chunk;
    }
    return true;
}

bool paging_user_range_mapped(
    const struct paging_address_space* space,
    uint64_t address, size_t count, bool require_writable) {
    (void)require_writable;
    if (space == NULL) return false;
    return host_user_range_valid(
        address, count, is_dynamic_address(address));
}

bool paging_copy_to_user(
    const struct paging_address_space* space,
    uint64_t user_destination,
    const void* kernel_source, size_t count) {
    if (space == NULL || kernel_source == NULL ||
        !host_user_range_valid(user_destination, count, false) ||
        is_dynamic_address(user_destination)) {
        return false;
    }
    memcpy((void*)(uintptr_t)user_destination, kernel_source, count);
    return true;
}

bool paging_copy_from_user(
    const struct paging_address_space* space,
    void* kernel_destination,
    uint64_t user_source, size_t count) {
    if (space == NULL || kernel_destination == NULL ||
        !host_user_range_valid(user_source, count, false) ||
        is_dynamic_address(user_source)) {
        return false;
    }
    memcpy(kernel_destination, (const void*)(uintptr_t)user_source,
           count);
    return true;
}

bool paging_user_map_anonymous(
    struct paging_address_space* space, uint64_t address,
    size_t size, bool writable, bool executable) {
    (void)writable;
    (void)executable;
    if (space == NULL || size == 0 ||
        (address & (PAGING_PAGE_SIZE - 1u)) != 0 ||
        (size & (PAGING_PAGE_SIZE - 1u)) != 0 ||
        !is_dynamic_address(address) ||
        !is_dynamic_address(address + (uint64_t)size - 1u)) {
        return false;
    }
    for (size_t offset = 0; offset < size;
         offset += (size_t)PAGING_PAGE_SIZE) {
        if (g_test_pages[
                dynamic_page_index(address + (uint64_t)offset)]) {
            return false;
        }
    }
    for (size_t offset = 0; offset < size;
         offset += (size_t)PAGING_PAGE_SIZE) {
        g_test_pages[
            dynamic_page_index(address + (uint64_t)offset)] = true;
    }
    return true;
}

bool paging_user_unmap_anonymous(
    struct paging_address_space* space, uint64_t address,
    size_t size) {
    if (space == NULL || size == 0 ||
        (address & (PAGING_PAGE_SIZE - 1u)) != 0 ||
        (size & (PAGING_PAGE_SIZE - 1u)) != 0 ||
        !is_dynamic_address(address) ||
        !is_dynamic_address(address + (uint64_t)size - 1u)) {
        return false;
    }
    for (size_t offset = 0; offset < size;
         offset += (size_t)PAGING_PAGE_SIZE) {
        if (!g_test_pages[
                dynamic_page_index(address + (uint64_t)offset)]) {
            return false;
        }
    }
    for (size_t offset = 0; offset < size;
         offset += (size_t)PAGING_PAGE_SIZE) {
        g_test_pages[
            dynamic_page_index(address + (uint64_t)offset)] = false;
    }
    return true;
}

const struct fs_file* fs_find(const char* name) {
    if (name == NULL) return NULL;
    for (size_t index = 0; index < FS_MAX_FILES; index++) {
        if (g_test_files[index].in_use &&
            strcmp(g_test_files[index].name, name) == 0) {
            return &g_test_files[index];
        }
    }
    return NULL;
}

size_t fs_file_count(void) {
    size_t count = 0;
    for (size_t index = 0; index < FS_MAX_FILES; index++) {
        if (g_test_files[index].in_use) count++;
    }
    return count;
}

bool fs_write_bytes(const char* name, const void* contents,
                    size_t length) {
    if (name == NULL || name[0] == '\0' ||
        length >= FS_MAX_FILE_SIZE ||
        (contents == NULL && length != 0) ||
        strcmp(name, "system.log") == 0) {
        return false;
    }
    struct fs_file* file = NULL;
    for (size_t index = 0; index < FS_MAX_FILES; index++) {
        if (g_test_files[index].in_use &&
            strcmp(g_test_files[index].name, name) == 0) {
            file = &g_test_files[index];
            break;
        }
        if (file == NULL && !g_test_files[index].in_use) {
            file = &g_test_files[index];
        }
    }
    if (file == NULL) return false;
    memset(file, 0, sizeof(*file));
    file->in_use = true;
    strcpy(file->name, name);
    if (length != 0) memcpy(file->data, contents, length);
    file->size = length;
    file->data[length] = '\0';
    return true;
}

void* kmalloc(size_t size) {
    return malloc(size);
}

void kfree(void* pointer) {
    free(pointer);
}

char keyboard_poll_char(void) {
    const char result = g_key;
    g_key = 0;
    return result;
}

void keyboard_discard_pending(void) {
    g_key = 0;
}

MouseState mouse_get_state(void) {
    return g_mouse;
}

uint32_t graphics_get_width(void) {
    return 800;
}

uint32_t graphics_get_height(void) {
    return 600;
}

void graphics_fill_rect(int x, int y, int width, int height,
                        uint32_t color) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

void graphics_draw_char(int x, int y, char character,
                        uint32_t foreground, uint32_t background) {
    (void)x;
    (void)y;
    (void)character;
    (void)foreground;
    (void)background;
}

void graphics_put_pixel(int x, int y, uint32_t color) {
    (void)x;
    (void)y;
    (void)color;
}

static uint64_t call_service(
    struct paging_address_space* space, uint64_t process_id,
    uint64_t capabilities, uint64_t syscall_id,
    uint64_t argument1, uint64_t argument2, uint64_t argument3,
    uint64_t argument4) {
    return app_services_dispatch(
        syscall_id, space, process_id, capabilities,
        argument1, argument2, argument3, argument4, 0);
}

static uint64_t open_file(
    struct paging_address_space* space, uint64_t process_id,
    uint64_t capabilities, const char* path, uint32_t flags) {
    return call_service(
        space, process_id, capabilities, APP_SYSCALL_FILE_OPEN,
        (uint64_t)(uintptr_t)path, strlen(path), flags, 0);
}

static void test_file_services(void) {
    struct paging_address_space space = {.marker = 1};
    const uint64_t process_id = 11;
    const uint64_t read_write =
        APP_CAPABILITY_FILE_READ | APP_CAPABILITY_FILE_WRITE;
    const struct app_manifest system_app = {
        .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
        .abi_version = NOSTALUX_APP_ABI_VERSION,
        .capabilities = APP_CAPABILITY_LOG,
        .id = "calculator",
        .display_name = "Calculator",
        .executable = "calculator.elf",
        .description = "System application",
    };
    app_registry_reset();
    assert(app_registry_register(&system_app) == APP_MANIFEST_OK);

    const char path[] = "notes.txt";
    assert(open_file(&space, process_id, 0, path,
                     APP_FILE_OPEN_READ) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    uint64_t handle = open_file(
        &space, process_id, read_write, path,
        APP_FILE_OPEN_READ | APP_FILE_OPEN_WRITE |
        APP_FILE_OPEN_CREATE | APP_FILE_OPEN_TRUNCATE);
    assert((int64_t)handle > 0);

    const char text[] = "hello";
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_WRITE, handle,
               (uint64_t)(uintptr_t)text, 5, 0) == 5);
    char output[16] = {0};
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_READ, handle,
               (uint64_t)(uintptr_t)output,
               sizeof(output), 0) == 5);
    assert(memcmp(output, "hello", 5) == 0);

    const char tail[] = "!";
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_WRITE, handle,
               (uint64_t)(uintptr_t)tail, 1, 7) == 1);
    const struct fs_file* file = fs_find(path);
    assert(file != NULL && file->size == 8);
    assert(file->data[5] == 0 && file->data[6] == 0);
    assert(file->data[7] == '!');

    char replacement[5000];
    memset(replacement, 'R', sizeof(replacement));
    g_rejected_user_address =
        (uint64_t)(uintptr_t)replacement;
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_REPLACE,
               (uint64_t)(uintptr_t)path, strlen(path),
               (uint64_t)(uintptr_t)replacement,
               sizeof(replacement)) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));
    assert(file->size == 8);
    g_rejected_user_address = 0;
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_REPLACE,
               (uint64_t)(uintptr_t)path, strlen(path),
               (uint64_t)(uintptr_t)replacement,
               sizeof(replacement)) == sizeof(replacement));
    file = fs_find(path);
    assert(file != NULL && file->size == sizeof(replacement));
    assert(file->data[0] == 'R' &&
           file->data[sizeof(replacement) - 1u] == 'R');

    const char exclusive_path[] = "download.txt";
    const char exclusive_contents[] = "first";
    assert(call_service(
               &space, process_id, 0,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)exclusive_path,
               strlen(exclusive_path),
               (uint64_t)(uintptr_t)exclusive_contents,
               sizeof(exclusive_contents) - 1u) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(fs_find(exclusive_path) == NULL);
    g_rejected_user_address =
        (uint64_t)(uintptr_t)exclusive_contents;
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)exclusive_path,
               strlen(exclusive_path),
               (uint64_t)(uintptr_t)exclusive_contents,
               sizeof(exclusive_contents) - 1u) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));
    assert(fs_find(exclusive_path) == NULL);
    g_rejected_user_address = 0;
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)exclusive_path,
               strlen(exclusive_path),
               (uint64_t)(uintptr_t)exclusive_contents,
               sizeof(exclusive_contents) - 1u) ==
           sizeof(exclusive_contents) - 1u);
    const struct fs_file* exclusive_file =
        fs_find(exclusive_path);
    assert(exclusive_file != NULL &&
           exclusive_file->size ==
               sizeof(exclusive_contents) - 1u &&
           memcmp(
               exclusive_file->data, exclusive_contents,
               sizeof(exclusive_contents) - 1u) == 0);

    const char overwrite_attempt[] = "second";
    assert(call_service(
               &space, process_id + 1u, read_write,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)exclusive_path,
               strlen(exclusive_path),
               (uint64_t)(uintptr_t)overwrite_attempt,
               sizeof(overwrite_attempt) - 1u) ==
           status_value(APP_STATUS_ALREADY_EXISTS));
    exclusive_file = fs_find(exclusive_path);
    assert(exclusive_file != NULL &&
           exclusive_file->size ==
               sizeof(exclusive_contents) - 1u &&
           memcmp(
               exclusive_file->data, exclusive_contents,
               sizeof(exclusive_contents) - 1u) == 0);

    assert(call_service(
               &space, process_id + 1u, read_write,
               APP_SYSCALL_FILE_READ, handle,
               (uint64_t)(uintptr_t)output, 1, 0) ==
           status_value(APP_STATUS_BAD_HANDLE));
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CLOSE, handle, 0, 0, 0) == 0);
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CLOSE, handle, 0, 0, 0) ==
           status_value(APP_STATUS_BAD_HANDLE));

    const char bad_path[] = "bad/name";
    assert(open_file(
               &space, process_id, read_write, bad_path,
               APP_FILE_OPEN_WRITE | APP_FILE_OPEN_CREATE) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));
    assert(open_file(
               &space, process_id, read_write, "system.log",
               APP_FILE_OPEN_WRITE) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(open_file(
               &space, process_id, read_write, "calculator.elf",
               APP_FILE_OPEN_WRITE | APP_FILE_OPEN_TRUNCATE) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_REPLACE,
               (uint64_t)(uintptr_t)"calculator.elf",
               strlen("calculator.elf"), 0, 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_REPLACE,
               (uint64_t)(uintptr_t)"system.log",
               strlen("system.log"), 0, 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)"calculator.elf",
               strlen("calculator.elf"), 0, 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, process_id, read_write,
               APP_SYSCALL_FILE_CREATE_EXCLUSIVE,
               (uint64_t)(uintptr_t)"system.log",
               strlen("system.log"), 0, 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));

    app_services_release_process(process_id);
}

static void test_memory_services(void) {
    struct paging_address_space space = {.marker = 2};
    const uint64_t process_id = 22;
    const uint64_t capability = APP_CAPABILITY_MEMORY;

    assert(call_service(
               &space, process_id, 0, APP_SYSCALL_MEMORY_MAP,
               5000, APP_MEMORY_READ | APP_MEMORY_WRITE, 0, 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, process_id, capability,
               APP_SYSCALL_MEMORY_MAP, 5000,
               APP_MEMORY_WRITE, 0, 0) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));

    uint64_t address = call_service(
        &space, process_id, capability, APP_SYSCALL_MEMORY_MAP,
        5000, APP_MEMORY_READ | APP_MEMORY_WRITE, 0, 0);
    assert(address >= PAGING_USER_BASE + 0x10000000ull);
    assert(g_test_pages[dynamic_page_index(address)]);
    assert(g_test_pages[dynamic_page_index(
        address + PAGING_PAGE_SIZE)]);

    assert(call_service(
               &space, process_id + 1u, capability,
               APP_SYSCALL_MEMORY_UNMAP,
               address, 5000, 0, 0) ==
           status_value(APP_STATUS_BAD_HANDLE));
    assert(call_service(
               &space, process_id, capability,
               APP_SYSCALL_MEMORY_UNMAP,
               address, 4096, 0, 0) ==
           status_value(APP_STATUS_BAD_HANDLE));
    assert(call_service(
               &space, process_id, capability,
               APP_SYSCALL_MEMORY_UNMAP,
               address, 5000, 0, 0) == 0);
    assert(!g_test_pages[dynamic_page_index(address)]);

    for (size_t index = 0; index < 4; index++) {
        assert((int64_t)call_service(
                   &space, process_id, capability,
                   APP_SYSCALL_MEMORY_MAP,
                   APP_MEMORY_MAP_MAX, APP_MEMORY_READ, 0, 0) > 0);
    }
    assert(call_service(
               &space, process_id, capability,
               APP_SYSCALL_MEMORY_MAP,
               1, APP_MEMORY_READ, 0, 0) ==
           status_value(APP_STATUS_NO_SPACE));
    app_services_release_process(process_id);
}

static uint64_t create_window(
    struct paging_address_space* space, uint64_t process_id,
    uint64_t capabilities, uint32_t width, uint32_t height) {
    static const char title[] = "Test window";
    const struct app_window_create request = {
        .width = width,
        .height = height,
        .title = title,
        .title_length = sizeof(title) - 1u,
    };
    return call_service(
        space, process_id, capabilities,
        APP_SYSCALL_WINDOW_CREATE,
        (uint64_t)(uintptr_t)&request, sizeof(request), 0, 0);
}

static uint64_t poll_window(
    struct paging_address_space* space, uint64_t process_id,
    uint64_t capabilities, uint64_t handle,
    struct app_input_event* event) {
    return call_service(
        space, process_id, capabilities,
        APP_SYSCALL_INPUT_POLL, handle,
        (uint64_t)(uintptr_t)event, sizeof(*event), 0);
}

static void route_pointer(int x, int y,
                          bool left_button, bool right_button) {
    g_mouse = (MouseState){
        .x = x,
        .y = y,
        .left_button = left_button,
        .right_button = right_button,
    };
    app_services_route_pointer(
        x, y, left_button, right_button);
}

static void test_launch_focus_authorization(void) {
    struct paging_address_space space = {.marker = 4};
    const uint64_t capabilities =
        APP_CAPABILITY_WINDOW | APP_CAPABILITY_INPUT;
    const uint64_t first_process_id = 70;
    struct app_input_event event;

    assert(!app_services_authorize_launch_focus(first_process_id));
    app_services_set_desktop_active(true);
    route_pointer(799, 599, false, false);

    assert(app_services_authorize_launch_focus(first_process_id));
    const uint64_t first_handle = create_window(
        &space, first_process_id, capabilities, 64, 48);
    assert((int64_t)first_handle > 0);
    assert(app_services_has_keyboard_focus());

    /*
     * The launch grant is one-shot. A later window from the same process is
     * retained below the focused window and cannot read keys until clicked.
     */
    const uint64_t background_handle = create_window(
        &space, first_process_id, capabilities, 64, 48);
    assert((int64_t)background_handle > 0);
    app_services_set_windows_hidden(true);
    assert(!app_services_has_keyboard_focus());
    assert(!app_services_pointer_captured(50, 80));
    g_key = 'H';
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 0);
    assert(g_key == 'H');
    app_services_set_windows_hidden(false);
    assert(app_services_has_keyboard_focus());
    /* Keys typed while windows are hidden must not leak on restore. */
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 0);
    assert(g_key == 0);
    g_key = 'H';
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'H');

    /* Explicit desktop interaction cancels a hidden focus snapshot. */
    app_services_set_windows_hidden(true);
    app_services_blur_keyboard_focus();
    app_services_set_windows_hidden(false);
    assert(!app_services_has_keyboard_focus());
    route_pointer(50, 80, true, false);
    route_pointer(50, 80, false, false);
    assert(app_services_has_keyboard_focus());

    assert(app_services_window_count() == 2);
    assert(app_services_request_all_windows_close() == 2);
    assert(app_services_window_count() == 2);
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(event.type == APP_INPUT_WINDOW_CLOSE);
    assert(poll_window(
               &space, first_process_id, capabilities,
               background_handle, &event) == 1);
    assert(event.type == APP_INPUT_WINDOW_CLOSE);
    assert(app_services_has_keyboard_focus());
    assert(app_services_pointer_captured(130, 100));
    g_key = 'A';
    assert(poll_window(
               &space, first_process_id, capabilities,
               background_handle, &event) == 0);
    assert(g_key == 'A');
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'A');

    route_pointer(130, 100, true, false);
    route_pointer(130, 100, false, false);
    assert(poll_window(
               &space, first_process_id, capabilities,
               background_handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.flags == APP_INPUT_FLAG_PRESSED);
    assert(poll_window(
               &space, first_process_id, capabilities,
               background_handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.flags == APP_INPUT_FLAG_RELEASED);
    g_key = 'B';
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 0);
    assert(g_key == 'B');
    assert(poll_window(
               &space, first_process_id, capabilities,
               background_handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'B');

    /*
     * A user focus change invalidates a delayed grant. The new process still
     * gets a visible window, but the clicked window keeps keyboard focus.
     */
    const uint64_t focus_changed_process_id = 71;
    assert(app_services_authorize_launch_focus(
        focus_changed_process_id));
    route_pointer(50, 80, true, false);
    route_pointer(50, 80, false, false);
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    const uint64_t focus_changed_handle = create_window(
        &space, focus_changed_process_id, capabilities, 64, 48);
    assert((int64_t)focus_changed_handle > 0);
    g_key = 'C';
    assert(poll_window(
               &space, focus_changed_process_id, capabilities,
               focus_changed_handle, &event) == 0);
    assert(g_key == 'C');
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'C');
    assert(call_service(
               &space, focus_changed_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               focus_changed_handle, 0, 0, 0) == 0);

    /*
     * Releasing the authorized process also invalidates its unused grant.
     */
    const uint64_t released_process_id = 72;
    assert(app_services_authorize_launch_focus(released_process_id));
    app_services_release_process(released_process_id);
    const uint64_t released_handle = create_window(
        &space, released_process_id, capabilities, 64, 48);
    assert((int64_t)released_handle > 0);
    g_key = 'D';
    assert(poll_window(
               &space, released_process_id, capabilities,
               released_handle, &event) == 0);
    assert(g_key == 'D');
    assert(poll_window(
               &space, first_process_id, capabilities,
               first_handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'D');
    assert(call_service(
               &space, released_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               released_handle, 0, 0, 0) == 0);

    /*
     * Desktop deactivation is another hard focus boundary. A grant cannot
     * survive leaving and re-entering the GUI.
     */
    const uint64_t deactivated_process_id = 73;
    assert(app_services_authorize_launch_focus(
        deactivated_process_id));
    app_services_set_desktop_active(false);
    app_services_set_desktop_active(true);
    const uint64_t deactivated_handle = create_window(
        &space, deactivated_process_id, capabilities, 64, 48);
    assert((int64_t)deactivated_handle > 0);
    assert(!app_services_has_keyboard_focus());
    g_key = 'E';
    assert(poll_window(
               &space, deactivated_process_id, capabilities,
               deactivated_handle, &event) == 0);
    assert(g_key == 'E');

    assert(call_service(
               &space, first_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               first_handle, 0, 0, 0) == 0);
    assert(call_service(
               &space, first_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               background_handle, 0, 0, 0) == 0);
    assert(call_service(
               &space, deactivated_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               deactivated_handle, 0, 0, 0) == 0);
    assert(app_services_window_count() == 0);
    app_services_set_desktop_active(false);
    assert(app_services_request_all_windows_close() == 0);
}

static void test_window_and_input_services(void) {
    struct paging_address_space space = {.marker = 3};
    const uint64_t process_id = 33;
    const uint64_t capabilities =
        APP_CAPABILITY_WINDOW | APP_CAPABILITY_INPUT;

    assert(create_window(
               &space, process_id, 0, 64, 48) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(create_window(
               &space, process_id, capabilities, 64, 48) ==
           status_value(APP_STATUS_WOULD_BLOCK));
    app_services_set_desktop_active(true);
    assert(create_window(
               &space, process_id, capabilities, 63, 48) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));

    g_mouse = (MouseState){
        .x = 100, .y = 100,
        .left_button = false, .right_button = false,
    };
    assert(app_services_authorize_launch_focus(process_id));
    const uint64_t handle = create_window(
        &space, process_id, capabilities, 64, 48);
    assert((int64_t)handle > 0);

    uint32_t pixels[64u * 48u];
    for (size_t index = 0;
         index < sizeof(pixels) / sizeof(pixels[0]); index++) {
        pixels[index] = 0x00112233u;
    }
    struct app_window_present present = {
        .window_handle = handle,
        .pixels = pixels,
        .width = 64,
        .height = 48,
        .stride_pixels = 63,
    };
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_WINDOW_PRESENT,
               (uint64_t)(uintptr_t)&present,
               sizeof(present), 0, 0) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));
    present.stride_pixels = 64;
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_WINDOW_PRESENT,
               (uint64_t)(uintptr_t)&present,
               sizeof(present), 0, 0) == 0);

    const uint64_t second_process_id = process_id + 1u;
    assert(app_services_authorize_launch_focus(second_process_id));
    const uint64_t second_handle = create_window(
        &space, second_process_id, capabilities, 64, 48);
    assert((int64_t)second_handle > 0);
    assert(app_services_has_keyboard_focus());

    struct app_input_event event;
    g_key = 'Q';
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    assert(g_key == 'Q');
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'Q');
    assert(event.flags == APP_INPUT_FLAG_PRESSED);

    /*
     * Leave a motion event pending for the older window. It must not gate a
     * key intended for the still-focused newer window.
     */
    g_mouse.x = 50;
    g_mouse.y = 80;
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    g_key = 'S';
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'S');
    assert(event.flags == APP_INPUT_FLAG_PRESSED);
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_POINTER_MOTION);

    /*
     * (50,80) is inside the older window content but outside the newer
     * cascaded
     * window. Polling the focused newer window first publishes, but does not
     * steal, the pointer press destined for the older window.
     */
    g_mouse.left_button = true;
    assert(call_service(
               &space, process_id, APP_CAPABILITY_WINDOW,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) ==
           status_value(APP_STATUS_PERMISSION_DENIED));
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON);
    assert(event.flags == APP_INPUT_FLAG_PRESSED);
    assert(event.button == APP_POINTER_BUTTON_LEFT);
    assert(app_services_pointer_gesture_active());

    g_mouse.left_button = false;
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.flags == APP_INPUT_FLAG_RELEASED);
    assert(!app_services_pointer_gesture_active());

    /*
     * A second button pressed over another app window remains with the
     * original gesture owner. It must not transfer pointer or keyboard focus.
     */
    route_pointer(50, 80, true, false);
    route_pointer(130, 100, true, false);
    route_pointer(130, 100, true, true);
    assert(poll_window(
               &space, second_process_id, capabilities,
               second_handle, &event) == 0);
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.button == APP_POINTER_BUTTON_LEFT &&
           event.flags == APP_INPUT_FLAG_PRESSED);
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_MOTION);
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.button == APP_POINTER_BUTTON_RIGHT &&
           event.flags == APP_INPUT_FLAG_PRESSED);
    g_key = 'T';
    assert(poll_window(
               &space, second_process_id, capabilities,
               second_handle, &event) == 0);
    assert(g_key == 'T');
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'T');

    route_pointer(130, 100, false, true);
    route_pointer(130, 100, false, false);
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.button == APP_POINTER_BUTTON_LEFT &&
           event.flags == APP_INPUT_FLAG_RELEASED);
    assert(poll_window(
               &space, process_id, capabilities,
               handle, &event) == 1);
    assert(event.type == APP_INPUT_POINTER_BUTTON &&
           event.button == APP_POINTER_BUTTON_RIGHT &&
           event.flags == APP_INPUT_FLAG_RELEASED);
    assert(!app_services_pointer_gesture_active());

    /*
     * If the first button went to kernel UI, a later chord over an app cannot
     * acquire app capture in the middle of that kernel-owned gesture.
     */
    route_pointer(799, 599, true, false);
    route_pointer(130, 100, true, false);
    route_pointer(130, 100, true, true);
    assert(!app_services_pointer_gesture_active());
    assert(poll_window(
               &space, second_process_id, capabilities,
               second_handle, &event) == 0);
    route_pointer(130, 100, false, true);
    route_pointer(130, 100, false, false);

    g_key = 'R';
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    assert(g_key == 'R');
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_KEY && event.key == 'R');
    assert(event.flags == APP_INPUT_FLAG_PRESSED);
    app_services_blur_keyboard_focus();
    assert(!app_services_has_keyboard_focus());

    g_rejected_user_address = (uint64_t)(uintptr_t)&event;
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) ==
           status_value(APP_STATUS_INVALID_ARGUMENT));
    g_rejected_user_address = 0;

    /*
     * A close-button press must survive a release that arrives before the
     * target process polls its queue.
     */
    g_mouse.x = 92;
    g_mouse.y = 48;
    g_mouse.left_button = true;
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    g_mouse.left_button = false;
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, second_handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 0);
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_INPUT_POLL, handle,
               (uint64_t)(uintptr_t)&event,
               sizeof(event), 0) == 1);
    assert(event.type == APP_INPUT_WINDOW_CLOSE);

    app_services_render_overlay();
    assert(app_services_pointer_captured(50, 50));
    assert(!app_services_pointer_captured(799, 599));
    assert(call_service(
               &space, process_id + 2u, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               handle, 0, 0, 0) ==
           status_value(APP_STATUS_BAD_HANDLE));
    assert(call_service(
               &space, second_process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               second_handle, 0, 0, 0) == 0);

    /*
     * Closing a captured app window while the physical button is still held
     * must not hand that same gesture to a kernel window underneath. This
     * remains true when it was the last app window and the release is the
     * first mouse snapshot observed after closing it.
     */
    g_mouse.x = 50;
    g_mouse.y = 80;
    g_mouse.left_button = true;
    app_services_route_pointer(
        g_mouse.x, g_mouse.y,
        g_mouse.left_button, g_mouse.right_button);
    assert(app_services_pointer_gesture_active());
    assert(call_service(
               &space, process_id, capabilities,
               APP_SYSCALL_WINDOW_CLOSE,
               handle, 0, 0, 0) == 0);
    assert(app_services_pointer_gesture_active());
    assert(!app_services_pointer_captured(50, 50));
    g_mouse.left_button = false;
    app_services_route_pointer(
        g_mouse.x, g_mouse.y,
        g_mouse.left_button, g_mouse.right_button);
    assert(!app_services_pointer_gesture_active());
    assert(!app_services_has_keyboard_focus());
    app_services_set_desktop_active(false);
}

int main(void) {
    test_file_services();
    test_memory_services();
    test_launch_focus_authorization();
    test_window_and_input_services();
    puts("app_services_test: all tests passed");
    return 0;
}
