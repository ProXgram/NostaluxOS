#include "app_services.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_network_services.h"
#include "app_abi.h"
#include "app_manifest.h"
#include "fs.h"
#include "graphics.h"
#include "heap.h"
#include "keyboard.h"
#include "mouse.h"
#include "paging.h"

#define APP_FILE_HANDLE_CAPACITY 64u
#define APP_FILE_HANDLES_PER_PROCESS 4u
#define APP_WINDOW_CAPACITY 8u
#define APP_WINDOWS_PER_PROCESS 2u
#define APP_MEMORY_RECORD_CAPACITY 128u
#define APP_MEMORY_RECORDS_PER_PROCESS 8u
#define APP_MEMORY_ARENA_BASE \
    (PAGING_USER_BASE + 0x10000000ull)
#define APP_MEMORY_ARENA_LIMIT \
    (APP_MEMORY_ARENA_BASE + 0x04000000ull)
#define APP_WINDOW_MAX_STRIDE_PIXELS 1024u
#define APP_WINDOW_BORDER 2
#define APP_WINDOW_TITLE_HEIGHT 22
#define APP_INPUT_QUEUE_CAPACITY 8u
#define APP_DESKTOP_BOTTOM_MARGIN 40u

struct app_file_handle {
    bool in_use;
    uint64_t handle;
    uint64_t process_id;
    uint32_t flags;
    char path[FS_MAX_FILENAME];
};

struct app_memory_record {
    bool in_use;
    uint64_t process_id;
    uint64_t address;
    size_t mapped_size;
    struct paging_address_space* address_space;
};

struct app_window_record {
    bool in_use;
    uint64_t handle;
    uint64_t process_id;
    uint32_t width;
    uint32_t height;
    int x;
    int y;
    uint64_t z_order;
    char title[APP_WINDOW_TITLE_MAX + 1u];
    uint32_t* pixels;
    struct app_input_event input_queue[APP_INPUT_QUEUE_CAPACITY];
    size_t input_count;
    bool dragging;
    int drag_offset_x;
    int drag_offset_y;
};

static struct app_file_handle g_files[APP_FILE_HANDLE_CAPACITY];
static struct app_memory_record g_memory[APP_MEMORY_RECORD_CAPACITY];
static struct app_window_record g_windows[APP_WINDOW_CAPACITY];
static uint64_t g_next_handle = 1;
static uint64_t g_next_z_order = 1;
static uint64_t g_focused_window_handle;
static uint64_t g_hidden_focused_window_handle;
static uint64_t g_launch_focus_process_id;
static uint64_t g_pointer_capture_handle;
static bool g_pointer_gesture_consumed;
static bool g_desktop_active;
static bool g_windows_hidden;
static bool g_input_mouse_initialized;
static MouseState g_input_last_mouse;

static void clear_bytes(void* destination, size_t count);

static uint64_t app_error(enum app_status status) {
    return (uint64_t)(int64_t)status;
}

static void queue_window_input(
    struct app_window_record* window,
    const struct app_input_event* event) {
    if (window == NULL || event == NULL || !window->in_use) return;

    if (event->type == APP_INPUT_WINDOW_CLOSE) {
        window->input_count = 1;
        window->input_queue[0] = *event;
        return;
    }
    for (size_t index = 0; index < window->input_count; index++) {
        if (window->input_queue[index].type ==
            APP_INPUT_WINDOW_CLOSE) {
            return;
        }
    }
    if (event->type == APP_INPUT_POINTER_MOTION &&
        window->input_count != 0 &&
        window->input_queue[window->input_count - 1u].type ==
            APP_INPUT_POINTER_MOTION) {
        window->input_queue[window->input_count - 1u] = *event;
        return;
    }
    if (window->input_count == APP_INPUT_QUEUE_CAPACITY) {
        if (event->type == APP_INPUT_POINTER_MOTION) return;
        for (size_t index = 1;
             index < window->input_count; index++) {
            window->input_queue[index - 1u] =
                window->input_queue[index];
        }
        window->input_count--;
    }
    window->input_queue[window->input_count++] = *event;
}

static bool dequeue_window_input(
    struct app_window_record* window,
    struct app_input_event* out_event) {
    if (window == NULL || out_event == NULL ||
        window->input_count == 0) {
        return false;
    }
    *out_event = window->input_queue[0];
    for (size_t index = 1; index < window->input_count; index++) {
        window->input_queue[index - 1u] =
            window->input_queue[index];
    }
    window->input_count--;
    clear_bytes(
        &window->input_queue[window->input_count],
        sizeof(window->input_queue[window->input_count]));
    return true;
}

static bool has_capability(uint64_t granted, uint64_t required) {
    return (granted & required) == required;
}

static void clear_bytes(void* destination, size_t count) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void copy_bytes(void* destination, const void* source,
                       size_t count) {
    uint8_t* out = (uint8_t*)destination;
    const uint8_t* in = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        out[index] = in[index];
    }
}

static bool strings_equal(const char* first, const char* second) {
    if (first == NULL || second == NULL) return false;
    size_t index = 0;
    while (first[index] != '\0' && second[index] != '\0') {
        if (first[index] != second[index]) return false;
        index++;
    }
    return first[index] == second[index];
}

static uint64_t allocate_handle(void) {
    if (g_next_handle == 0) return 0;
    const uint64_t handle = g_next_handle;
    if (g_next_handle == UINT64_MAX) {
        g_next_handle = 0;
    } else {
        g_next_handle++;
    }
    return handle;
}

static bool copy_file_path(
    const struct paging_address_space* space,
    uint64_t user_path,
    uint64_t requested_length,
    char out_path[FS_MAX_FILENAME]) {
    if (out_path == NULL || user_path == 0 ||
        requested_length == 0 ||
        requested_length > APP_FILE_PATH_MAX ||
        requested_length >= FS_MAX_FILENAME ||
        requested_length > (uint64_t)SIZE_MAX) {
        return false;
    }

    const size_t length = (size_t)requested_length;
    if (!paging_copy_from_user(space, out_path, user_path, length)) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        const char character = out_path[index];
        if (character == '\0' || character == ' ' ||
            character == '\t' || character == '/' ||
            character == '\\') {
            return false;
        }
    }
    out_path[length] = '\0';
    return true;
}

static size_t file_handle_count(uint64_t process_id) {
    size_t count = 0;
    for (size_t index = 0;
         index < APP_FILE_HANDLE_CAPACITY; index++) {
        if (g_files[index].in_use &&
            g_files[index].process_id == process_id) {
            count++;
        }
    }
    return count;
}

static struct app_file_handle* find_file_handle(
    uint64_t process_id, uint64_t handle) {
    if (process_id == 0 || handle == 0) return NULL;
    for (size_t index = 0;
         index < APP_FILE_HANDLE_CAPACITY; index++) {
        if (g_files[index].in_use &&
            g_files[index].process_id == process_id &&
            g_files[index].handle == handle) {
            return &g_files[index];
        }
    }
    return NULL;
}

static struct app_file_handle* allocate_file_slot(
    uint64_t process_id) {
    if (file_handle_count(process_id) >=
        APP_FILE_HANDLES_PER_PROCESS) {
        return NULL;
    }
    for (size_t index = 0;
         index < APP_FILE_HANDLE_CAPACITY; index++) {
        if (!g_files[index].in_use) return &g_files[index];
    }
    return NULL;
}

static uint64_t dispatch_file_open(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t user_path,
    uint64_t path_length,
    uint64_t requested_flags) {
    const uint32_t known_flags =
        APP_FILE_OPEN_READ | APP_FILE_OPEN_WRITE |
        APP_FILE_OPEN_CREATE | APP_FILE_OPEN_TRUNCATE;
    if (requested_flags > UINT32_MAX) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t flags = (uint32_t)requested_flags;
    if ((flags & ~known_flags) != 0 ||
        (flags & (APP_FILE_OPEN_READ | APP_FILE_OPEN_WRITE)) == 0 ||
        ((flags & (APP_FILE_OPEN_CREATE |
                   APP_FILE_OPEN_TRUNCATE)) != 0 &&
         (flags & APP_FILE_OPEN_WRITE) == 0)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if (((flags & APP_FILE_OPEN_READ) != 0 &&
         !has_capability(capabilities, APP_CAPABILITY_FILE_READ)) ||
        ((flags & APP_FILE_OPEN_WRITE) != 0 &&
         !has_capability(capabilities, APP_CAPABILITY_FILE_WRITE))) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }

    char path[FS_MAX_FILENAME];
    if (!copy_file_path(space, user_path, path_length, path)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if ((flags & APP_FILE_OPEN_WRITE) != 0 &&
        (strings_equal(path, "system.log") ||
         app_registry_find_executable(path) != NULL)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }

    struct app_file_handle* slot = allocate_file_slot(process_id);
    if (slot == NULL) return app_error(APP_STATUS_NO_SPACE);

    const uint64_t handle = allocate_handle();
    if (handle == 0) return app_error(APP_STATUS_NO_SPACE);

    const struct fs_file* file = fs_find(path);
    if (file == NULL && (flags & APP_FILE_OPEN_CREATE) == 0) {
        return app_error(APP_STATUS_NOT_FOUND);
    }
    if (file == NULL || (flags & APP_FILE_OPEN_TRUNCATE) != 0) {
        if (!fs_write_bytes(path, NULL, 0)) {
            return app_error(
                file == NULL ? APP_STATUS_NO_SPACE :
                               APP_STATUS_IO_ERROR);
        }
    }

    clear_bytes(slot, sizeof(*slot));
    slot->in_use = true;
    slot->handle = handle;
    slot->process_id = process_id;
    slot->flags = flags;
    size_t index = 0;
    while (path[index] != '\0' &&
           index + 1u < sizeof(slot->path)) {
        slot->path[index] = path[index];
        index++;
    }
    slot->path[index] = '\0';
    return handle;
}

static uint64_t dispatch_file_read(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle,
    uint64_t user_buffer,
    uint64_t requested_length,
    uint64_t requested_offset) {
    if (!has_capability(capabilities, APP_CAPABILITY_FILE_READ)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_file_handle* open =
        find_file_handle(process_id, handle);
    if (open == NULL ||
        (open->flags & APP_FILE_OPEN_READ) == 0) {
        return app_error(APP_STATUS_BAD_HANDLE);
    }
    if (requested_length > APP_FILE_TRANSFER_MAX ||
        requested_length > (uint64_t)SIZE_MAX ||
        requested_offset > (uint64_t)SIZE_MAX) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    const struct fs_file* file = fs_find(open->path);
    if (file == NULL) return app_error(APP_STATUS_NOT_FOUND);
    const size_t offset = (size_t)requested_offset;
    size_t length = (size_t)requested_length;
    if (offset >= file->size) return 0;
    if (length > file->size - offset) {
        length = file->size - offset;
    }
    if (length == 0) return 0;
    if (user_buffer == 0 ||
        !paging_copy_to_user(space, user_buffer,
                             file->data + offset, length)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return (uint64_t)length;
}

static uint64_t dispatch_file_write(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle,
    uint64_t user_buffer,
    uint64_t requested_length,
    uint64_t requested_offset) {
    if (!has_capability(capabilities, APP_CAPABILITY_FILE_WRITE)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_file_handle* open =
        find_file_handle(process_id, handle);
    if (open == NULL ||
        (open->flags & APP_FILE_OPEN_WRITE) == 0) {
        return app_error(APP_STATUS_BAD_HANDLE);
    }
    if (requested_length > APP_FILE_TRANSFER_MAX ||
        requested_length > (uint64_t)SIZE_MAX ||
        requested_offset > (uint64_t)SIZE_MAX) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    const size_t storage_limit = FS_MAX_FILE_SIZE - 1u;
    const size_t offset = (size_t)requested_offset;
    const size_t length = (size_t)requested_length;
    if (offset > storage_limit ||
        length > storage_limit - offset) {
        return app_error(APP_STATUS_NO_SPACE);
    }
    if (length == 0) return 0;
    if (user_buffer == 0 ||
        !paging_user_range_mapped(space, user_buffer,
                                  length, false)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    const struct fs_file* file = fs_find(open->path);
    if (file == NULL) return app_error(APP_STATUS_NOT_FOUND);

    uint8_t* contents = (uint8_t*)kmalloc(FS_MAX_FILE_SIZE);
    if (contents == NULL) return app_error(APP_STATUS_NO_SPACE);
    clear_bytes(contents, FS_MAX_FILE_SIZE);
    copy_bytes(contents, file->data, file->size);
    if (!paging_copy_from_user(space, contents + offset,
                               user_buffer, length)) {
        kfree(contents);
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    size_t final_size = file->size;
    if (offset + length > final_size) {
        final_size = offset + length;
    }
    const bool written =
        fs_write_bytes(open->path, contents, final_size);
    kfree(contents);
    if (!written) return app_error(APP_STATUS_IO_ERROR);
    return (uint64_t)length;
}

static uint64_t dispatch_file_store(
    struct paging_address_space* space,
    uint64_t capabilities,
    uint64_t user_path,
    uint64_t path_length,
    uint64_t user_buffer,
    uint64_t requested_length,
    bool require_absent) {
    if (!has_capability(capabilities, APP_CAPABILITY_FILE_WRITE)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (requested_length >= FS_MAX_FILE_SIZE ||
        requested_length > (uint64_t)SIZE_MAX) {
        return app_error(APP_STATUS_NO_SPACE);
    }

    char path[FS_MAX_FILENAME];
    if (!copy_file_path(space, user_path, path_length, path)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    if (strings_equal(path, "system.log") ||
        app_registry_find_executable(path) != NULL) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    /*
     * App service calls run as one kernel operation. Keeping the existence
     * check and write inside this dispatch makes create-if-absent atomic with
     * respect to every other app filesystem call.
     */
    if (require_absent && fs_find(path) != NULL) {
        return app_error(APP_STATUS_ALREADY_EXISTS);
    }

    const size_t length = (size_t)requested_length;
    uint8_t* contents = NULL;
    if (length != 0) {
        if (user_buffer == 0 ||
            !paging_user_range_mapped(
                space, user_buffer, length, false)) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
        contents = (uint8_t*)kmalloc(length);
        if (contents == NULL) return app_error(APP_STATUS_NO_SPACE);
        if (!paging_copy_from_user(
                space, contents, user_buffer, length)) {
            kfree(contents);
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
    }

    const bool written = fs_write_bytes(path, contents, length);
    kfree(contents);
    if (!written) return app_error(APP_STATUS_IO_ERROR);
    return (uint64_t)length;
}

static uint64_t dispatch_file_replace(
    struct paging_address_space* space,
    uint64_t capabilities,
    uint64_t user_path,
    uint64_t path_length,
    uint64_t user_buffer,
    uint64_t requested_length) {
    return dispatch_file_store(
        space, capabilities, user_path, path_length,
        user_buffer, requested_length, false);
}

static uint64_t dispatch_file_create_exclusive(
    struct paging_address_space* space,
    uint64_t capabilities,
    uint64_t user_path,
    uint64_t path_length,
    uint64_t user_buffer,
    uint64_t requested_length) {
    return dispatch_file_store(
        space, capabilities, user_path, path_length,
        user_buffer, requested_length, true);
}

static uint64_t dispatch_file_close(uint64_t process_id,
                                    uint64_t handle) {
    struct app_file_handle* open =
        find_file_handle(process_id, handle);
    if (open == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    clear_bytes(open, sizeof(*open));
    return APP_STATUS_OK;
}

static bool round_up_page(uint64_t byte_count, size_t* out_size) {
    if (out_size == NULL || byte_count == 0 ||
        byte_count > APP_MEMORY_MAP_MAX ||
        byte_count > (uint64_t)SIZE_MAX ||
        byte_count >
            UINT64_MAX - (PAGING_PAGE_SIZE - 1u)) {
        return false;
    }
    const uint64_t rounded =
        (byte_count + PAGING_PAGE_SIZE - 1u) &
        ~(PAGING_PAGE_SIZE - 1u);
    if (rounded == 0 || rounded > SIZE_MAX) return false;
    *out_size = (size_t)rounded;
    return true;
}

static size_t memory_record_count(uint64_t process_id,
                                  size_t* out_total_bytes) {
    size_t count = 0;
    size_t total = 0;
    for (size_t index = 0;
         index < APP_MEMORY_RECORD_CAPACITY; index++) {
        if (g_memory[index].in_use &&
            g_memory[index].process_id == process_id) {
            count++;
            if (g_memory[index].mapped_size <= SIZE_MAX - total) {
                total += g_memory[index].mapped_size;
            } else {
                total = SIZE_MAX;
            }
        }
    }
    if (out_total_bytes != NULL) *out_total_bytes = total;
    return count;
}

static struct app_memory_record* allocate_memory_record(
    uint64_t process_id, size_t mapped_size) {
    size_t total;
    if (memory_record_count(process_id, &total) >=
            APP_MEMORY_RECORDS_PER_PROCESS ||
        mapped_size > APP_MEMORY_PROCESS_MAX ||
        total > APP_MEMORY_PROCESS_MAX - mapped_size) {
        return NULL;
    }
    for (size_t index = 0;
         index < APP_MEMORY_RECORD_CAPACITY; index++) {
        if (!g_memory[index].in_use) return &g_memory[index];
    }
    return NULL;
}

static uint64_t find_memory_range(
    const struct paging_address_space* space, size_t size) {
    if ((uint64_t)size >
        APP_MEMORY_ARENA_LIMIT - APP_MEMORY_ARENA_BASE) {
        return 0;
    }
    uint64_t run_start = 0;
    size_t run_size = 0;
    for (uint64_t page = APP_MEMORY_ARENA_BASE;
         page < APP_MEMORY_ARENA_LIMIT;
         page += PAGING_PAGE_SIZE) {
        if (paging_user_range_mapped(space, page, 1, false)) {
            run_start = 0;
            run_size = 0;
            continue;
        }
        if (run_size == 0) run_start = page;
        run_size += (size_t)PAGING_PAGE_SIZE;
        if (run_size >= size) {
            return run_start;
        }
    }
    return 0;
}

static uint64_t dispatch_memory_map(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t byte_count,
    uint64_t protection_flags) {
    if (!has_capability(capabilities, APP_CAPABILITY_MEMORY)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (protection_flags > UINT32_MAX ||
        ((uint32_t)protection_flags &
         ~(APP_MEMORY_READ | APP_MEMORY_WRITE)) != 0 ||
        ((uint32_t)protection_flags & APP_MEMORY_READ) == 0) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    size_t mapped_size;
    if (!round_up_page(byte_count, &mapped_size)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    struct app_memory_record* record =
        allocate_memory_record(process_id, mapped_size);
    if (record == NULL) return app_error(APP_STATUS_NO_SPACE);

    const uint64_t address = find_memory_range(space, mapped_size);
    if (address == 0) return app_error(APP_STATUS_NO_SPACE);

    size_t mapped = 0;
    const bool writable =
        ((uint32_t)protection_flags & APP_MEMORY_WRITE) != 0;
    while (mapped < mapped_size) {
        if (!paging_user_map_anonymous(
                space, address + (uint64_t)mapped,
                (size_t)PAGING_PAGE_SIZE, writable, false)) {
            if (mapped != 0) {
                (void)paging_user_unmap_anonymous(
                    space, address, mapped);
            }
            return app_error(APP_STATUS_NO_SPACE);
        }
        mapped += (size_t)PAGING_PAGE_SIZE;
    }

    clear_bytes(record, sizeof(*record));
    record->in_use = true;
    record->process_id = process_id;
    record->address = address;
    record->mapped_size = mapped_size;
    record->address_space = space;
    return address;
}

static uint64_t dispatch_memory_unmap(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t address,
    uint64_t byte_count) {
    if (!has_capability(capabilities, APP_CAPABILITY_MEMORY)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    size_t mapped_size;
    if (!round_up_page(byte_count, &mapped_size) ||
        (address & (PAGING_PAGE_SIZE - 1u)) != 0) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    for (size_t index = 0;
         index < APP_MEMORY_RECORD_CAPACITY; index++) {
        struct app_memory_record* record = &g_memory[index];
        if (!record->in_use ||
            record->process_id != process_id ||
            record->address != address ||
            record->mapped_size != mapped_size ||
            record->address_space != space) {
            continue;
        }
        if (!paging_user_unmap_anonymous(
                space, address, mapped_size)) {
            return app_error(APP_STATUS_IO_ERROR);
        }
        clear_bytes(record, sizeof(*record));
        return APP_STATUS_OK;
    }
    return app_error(APP_STATUS_BAD_HANDLE);
}

static size_t window_count(uint64_t process_id) {
    size_t count = 0;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use &&
            g_windows[index].process_id == process_id) {
            count++;
        }
    }
    return count;
}

static struct app_window_record* find_window(
    uint64_t process_id, uint64_t handle) {
    if (process_id == 0 || handle == 0) return NULL;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use &&
            g_windows[index].process_id == process_id &&
            g_windows[index].handle == handle) {
            return &g_windows[index];
        }
    }
    return NULL;
}

static struct app_window_record* find_window_handle(uint64_t handle) {
    if (handle == 0) return NULL;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use &&
            g_windows[index].handle == handle) {
            return &g_windows[index];
        }
    }
    return NULL;
}

static struct app_window_record* top_window_at(int x, int y) {
    if (g_windows_hidden) return NULL;
    struct app_window_record* top = NULL;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        struct app_window_record* window = &g_windows[index];
        if (!window->in_use) continue;
        const int total_width = (int)window->width + 4;
        const int total_height =
            (int)window->height + APP_WINDOW_TITLE_HEIGHT + 4;
        if (x < window->x || x >= window->x + total_width ||
            y < window->y || y >= window->y + total_height) {
            continue;
        }
        if (top == NULL || window->z_order > top->z_order) {
            top = window;
        }
    }
    return top;
}

static bool raise_window(struct app_window_record* window) {
    if (window == NULL || !window->in_use ||
        g_next_z_order == 0) {
        return false;
    }
    window->z_order = g_next_z_order;
    if (g_next_z_order == UINT64_MAX) {
        g_next_z_order = 0;
    } else {
        g_next_z_order++;
    }
    return true;
}

static bool focus_window(struct app_window_record* window) {
    /*
     * A pointer-driven focus decision supersedes any delayed launch. An
     * authorized create also consumes its grant here before returning.
     */
    g_launch_focus_process_id = 0;
    if (!raise_window(window)) return false;
    if (g_focused_window_handle != window->handle) {
        keyboard_discard_pending();
        g_focused_window_handle = window->handle;
    }
    return true;
}

static bool place_window_in_background(
    struct app_window_record* window) {
    if (!raise_window(window)) return false;

    /*
     * Keep the currently focused app above a programmatically created
     * background window. With no app focus, the new window is still visible
     * and can be activated by clicking it.
     */
    struct app_window_record* focused =
        find_window_handle(g_focused_window_handle);
    return focused == NULL || focused == window ||
           raise_window(focused);
}

static struct app_window_record* allocate_window_slot(
    uint64_t process_id, size_t* out_index) {
    if (window_count(process_id) >= APP_WINDOWS_PER_PROCESS) {
        return NULL;
    }
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (!g_windows[index].in_use) {
            if (out_index != NULL) *out_index = index;
            return &g_windows[index];
        }
    }
    return NULL;
}

static void close_window(struct app_window_record* window);

static bool copy_window_title(
    const struct paging_address_space* space,
    uint64_t user_title,
    size_t title_length,
    char out_title[APP_WINDOW_TITLE_MAX + 1u]) {
    if (out_title == NULL ||
        title_length > APP_WINDOW_TITLE_MAX ||
        (title_length != 0 && user_title == 0)) {
        return false;
    }
    if (title_length == 0) {
        const char fallback[] = "Application";
        copy_bytes(out_title, fallback, sizeof(fallback));
        return true;
    }
    if (!paging_copy_from_user(space, out_title,
                               user_title, title_length)) {
        return false;
    }
    for (size_t index = 0; index < title_length; index++) {
        if ((uint8_t)out_title[index] < 32u ||
            (uint8_t)out_title[index] > 126u) {
            return false;
        }
    }
    out_title[title_length] = '\0';
    return true;
}

static uint64_t dispatch_window_create(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t user_request,
    uint64_t request_size) {
    if (!has_capability(capabilities, APP_CAPABILITY_WINDOW)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (!g_desktop_active) {
        return app_error(APP_STATUS_WOULD_BLOCK);
    }
    if (user_request == 0 ||
        request_size < sizeof(struct app_window_create)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    struct app_window_create request;
    if (!paging_copy_from_user(space, &request, user_request,
                               sizeof(request)) ||
        request.width < APP_WINDOW_MIN_WIDTH ||
        request.width > APP_WINDOW_MAX_WIDTH ||
        request.height < APP_WINDOW_MIN_HEIGHT ||
        request.height > APP_WINDOW_MAX_HEIGHT) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    char title[APP_WINDOW_TITLE_MAX + 1u];
    if (!copy_window_title(
            space, (uint64_t)(uintptr_t)request.title,
            request.title_length, title)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    size_t slot_index;
    struct app_window_record* window =
        allocate_window_slot(process_id, &slot_index);
    if (window == NULL) return app_error(APP_STATUS_NO_SPACE);
    const size_t pixel_count =
        (size_t)request.width * (size_t)request.height;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    uint32_t* pixels =
        (uint32_t*)kmalloc(pixel_count * sizeof(uint32_t));
    if (pixels == NULL) return app_error(APP_STATUS_NO_SPACE);
    clear_bytes(pixels, pixel_count * sizeof(uint32_t));

    const uint64_t handle = allocate_handle();
    if (handle == 0) {
        kfree(pixels);
        return app_error(APP_STATUS_NO_SPACE);
    }

    const MouseState mouse = mouse_get_state();
    clear_bytes(window, sizeof(*window));
    window->in_use = true;
    window->handle = handle;
    window->process_id = process_id;
    window->width = request.width;
    window->height = request.height;
    window->x = 44 + (int)slot_index * 28;
    window->y = 42 + (int)slot_index * 24;
    window->pixels = pixels;
    size_t title_index = 0;
    while (title[title_index] != '\0' &&
           title_index + 1u < sizeof(window->title)) {
        window->title[title_index] = title[title_index];
        title_index++;
    }
    window->title[title_index] = '\0';
    if (!g_input_mouse_initialized) {
        g_input_last_mouse = mouse;
        g_input_mouse_initialized = true;
    }
    const bool focus_authorized =
        g_launch_focus_process_id == process_id;
    if (focus_authorized) {
        g_windows_hidden = false;
        g_hidden_focused_window_handle = 0;
    }
    const bool placed = focus_authorized
        ? focus_window(window)
        : place_window_in_background(window);
    if (!placed) {
        close_window(window);
        return app_error(APP_STATUS_NO_SPACE);
    }
    return handle;
}

static uint64_t dispatch_window_present(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t user_request,
    uint64_t request_size) {
    if (!has_capability(capabilities, APP_CAPABILITY_WINDOW)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (user_request == 0 ||
        request_size < sizeof(struct app_window_present)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    struct app_window_present request;
    if (!paging_copy_from_user(space, &request, user_request,
                               sizeof(request))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    struct app_window_record* window =
        find_window(process_id, request.window_handle);
    if (window == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    if (request.pixels == NULL ||
        request.width != window->width ||
        request.height != window->height ||
        request.stride_pixels < request.width ||
        request.stride_pixels > APP_WINDOW_MAX_STRIDE_PIXELS) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    const uint64_t user_pixels =
        (uint64_t)(uintptr_t)request.pixels;
    const size_t row_bytes =
        (size_t)request.width * sizeof(uint32_t);
    const uint64_t stride_bytes =
        (uint64_t)request.stride_pixels * sizeof(uint32_t);
    /*
     * Validate the complete strided source first. A malformed later row must
     * not leave a partially updated retained surface.
     */
    for (uint32_t row = 0; row < request.height; row++) {
        if ((uint64_t)row >
            (UINT64_MAX - user_pixels) / stride_bytes) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
        const uint64_t row_address =
            user_pixels + (uint64_t)row * stride_bytes;
        if (!paging_user_range_mapped(
                space, row_address, row_bytes, false)) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
    }
    for (uint32_t row = 0; row < request.height; row++) {
        const uint64_t row_address =
            user_pixels + (uint64_t)row * stride_bytes;
        if (!paging_copy_from_user(
                space,
                window->pixels + (size_t)row * window->width,
                row_address, row_bytes)) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
    }
    return APP_STATUS_OK;
}

static void close_window(struct app_window_record* window) {
    if (window == NULL || !window->in_use) return;
    const uint64_t closed_handle = window->handle;
    kfree(window->pixels);
    clear_bytes(window, sizeof(*window));
    if (g_pointer_capture_handle == closed_handle) {
        g_pointer_capture_handle = 0;
    }
    if (g_focused_window_handle == closed_handle) {
        keyboard_discard_pending();
        g_launch_focus_process_id = 0;
        struct app_window_record* newest = NULL;
        for (size_t index = 0;
             index < APP_WINDOW_CAPACITY; index++) {
            struct app_window_record* candidate = &g_windows[index];
            if (candidate->in_use &&
                (newest == NULL ||
                 candidate->z_order > newest->z_order)) {
                newest = candidate;
            }
        }
        g_focused_window_handle =
            newest == NULL ? 0 : newest->handle;
    }
    if (g_hidden_focused_window_handle == closed_handle) {
        g_hidden_focused_window_handle = 0;
    }
    bool any_window = false;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use) {
            any_window = true;
            break;
        }
    }
    if (!any_window) {
        g_focused_window_handle = 0;
        g_pointer_capture_handle = 0;
        g_input_mouse_initialized = false;
    }
}

static uint64_t dispatch_window_close(
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t handle) {
    if (!has_capability(capabilities, APP_CAPABILITY_WINDOW)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_window_record* window =
        find_window(process_id, handle);
    if (window == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    close_window(window);
    return APP_STATUS_OK;
}

static bool point_in_close_button(
    const struct app_window_record* window, int x, int y) {
    if (window == NULL) return false;
    const int left =
        window->x + (int)window->width - 18;
    const int top = window->y + 3;
    return x >= left && x < left + 16 &&
           y >= top && y < top + 16;
}

static bool point_in_title_bar(
    const struct app_window_record* window, int x, int y) {
    return window != NULL &&
           x >= window->x &&
           x < window->x + (int)window->width + 4 &&
           y >= window->y &&
           y < window->y + APP_WINDOW_TITLE_HEIGHT + 2;
}

static void publish_pointer_event(MouseState mouse) {
    if (!g_input_mouse_initialized) {
        g_input_last_mouse = mouse;
        g_input_mouse_initialized = true;
        if (!mouse.left_button && !mouse.right_button) {
            g_pointer_capture_handle = 0;
            g_pointer_gesture_consumed = false;
        }
        return;
    }

    const bool left_changed =
        mouse.left_button != g_input_last_mouse.left_button;
    const bool right_changed =
        mouse.right_button != g_input_last_mouse.right_button;
    const bool moved =
        mouse.x != g_input_last_mouse.x ||
        mouse.y != g_input_last_mouse.y;
    if (!left_changed && !right_changed && !moved) return;

    struct app_window_record* target = NULL;
    struct app_input_event event;
    bool chrome_gesture = false;
    clear_bytes(&event, sizeof(event));

    if (left_changed) {
        if (mouse.left_button) {
            if (g_pointer_gesture_consumed) {
                target =
                    find_window_handle(g_pointer_capture_handle);
            } else if (!g_input_last_mouse.right_button) {
                target = top_window_at(mouse.x, mouse.y);
                if (target != NULL) {
                    (void)focus_window(target);
                    g_pointer_capture_handle = target->handle;
                    g_pointer_gesture_consumed = true;
                }
            }
            if (target != NULL) {
                if (!point_in_close_button(
                        target, mouse.x, mouse.y) &&
                    point_in_title_bar(
                        target, mouse.x, mouse.y)) {
                    target->dragging = true;
                    target->drag_offset_x = mouse.x - target->x;
                    target->drag_offset_y = mouse.y - target->y;
                    chrome_gesture = true;
                }
            }
        } else {
            target = find_window_handle(g_pointer_capture_handle);
            if (target != NULL && target->dragging) {
                target->dragging = false;
                chrome_gesture = true;
            }
        }
        event.type = APP_INPUT_POINTER_BUTTON;
        event.flags = mouse.left_button
            ? APP_INPUT_FLAG_PRESSED
            : APP_INPUT_FLAG_RELEASED;
        event.button = APP_POINTER_BUTTON_LEFT;
        g_input_last_mouse.left_button = mouse.left_button;
    } else if (right_changed) {
        if (mouse.right_button) {
            if (g_pointer_gesture_consumed) {
                target =
                    find_window_handle(g_pointer_capture_handle);
            } else if (!g_input_last_mouse.left_button) {
                target = top_window_at(mouse.x, mouse.y);
                if (target != NULL) {
                    (void)focus_window(target);
                    g_pointer_capture_handle = target->handle;
                    g_pointer_gesture_consumed = true;
                }
            }
        } else {
            target = find_window_handle(g_pointer_capture_handle);
        }
        event.type = APP_INPUT_POINTER_BUTTON;
        event.flags = mouse.right_button
            ? APP_INPUT_FLAG_PRESSED
            : APP_INPUT_FLAG_RELEASED;
        event.button = APP_POINTER_BUTTON_RIGHT;
        g_input_last_mouse.right_button = mouse.right_button;
    } else {
        target = find_window_handle(g_pointer_capture_handle);
        if (target == NULL &&
            !mouse.left_button && !mouse.right_button) {
            target = top_window_at(mouse.x, mouse.y);
        }
        event.type = APP_INPUT_POINTER_MOTION;
        if (target != NULL && target->dragging) {
            target->x = mouse.x - target->drag_offset_x;
            target->y = mouse.y - target->drag_offset_y;
            chrome_gesture = true;
        }
    }

    g_input_last_mouse.x = mouse.x;
    g_input_last_mouse.y = mouse.y;
    if (!mouse.left_button && !mouse.right_button) {
        g_pointer_capture_handle = 0;
        g_pointer_gesture_consumed = false;
    }
    if (target == NULL) return;

    event.x =
        mouse.x - (target->x + APP_WINDOW_BORDER);
    event.y =
        mouse.y -
        (target->y + APP_WINDOW_TITLE_HEIGHT +
         APP_WINDOW_BORDER);
    if (left_changed && mouse.left_button &&
        point_in_close_button(target, mouse.x, mouse.y)) {
        clear_bytes(&event, sizeof(event));
        event.type = APP_INPUT_WINDOW_CLOSE;
    }
    if (chrome_gesture) return;
    queue_window_input(target, &event);
}

void app_services_route_pointer(
    int x, int y, bool left_button, bool right_button) {
    if (!g_desktop_active) return;
    const MouseState snapshot = {
        .x = x,
        .y = y,
        .left_button = left_button,
        .right_button = right_button,
    };
    publish_pointer_event(snapshot);
}

static uint64_t dispatch_input_poll(
    struct paging_address_space* space,
    uint64_t process_id,
    uint64_t capabilities,
    uint64_t window_handle,
    uint64_t user_event,
    uint64_t event_size) {
    if (!has_capability(capabilities, APP_CAPABILITY_INPUT)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    struct app_window_record* window =
        find_window(process_id, window_handle);
    if (window == NULL) return app_error(APP_STATUS_BAD_HANDLE);
    if (user_event == 0 ||
        event_size < sizeof(struct app_input_event) ||
        !paging_user_range_mapped(
            space, user_event, sizeof(struct app_input_event), true)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    if (g_desktop_active) {
        const MouseState mouse = mouse_get_state();
        app_services_route_pointer(
            mouse.x, mouse.y,
            mouse.left_button, mouse.right_button);
    }
    struct app_input_event event;
    if (dequeue_window_input(window, &event)) {
        if (!paging_copy_to_user(space, user_event,
                                 &event, sizeof(event))) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
        return 1;
    }

    if (!g_desktop_active ||
        g_focused_window_handle != window->handle) return 0;
    const char key = keyboard_poll_char();
    if (key == 0) return 0;
    clear_bytes(&event, sizeof(event));
    event.type = APP_INPUT_KEY;
    event.flags = APP_INPUT_FLAG_PRESSED;
    event.key = (uint8_t)key;
    if (!paging_copy_to_user(space, user_event,
                             &event, sizeof(event))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return 1;
}

uint64_t app_services_dispatch(
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
        case APP_SYSCALL_FILE_OPEN:
            return dispatch_file_open(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3);
        case APP_SYSCALL_FILE_READ:
            return dispatch_file_read(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3, argument4);
        case APP_SYSCALL_FILE_WRITE:
            return dispatch_file_write(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3, argument4);
        case APP_SYSCALL_FILE_CLOSE:
            return dispatch_file_close(process_id, argument1);
        case APP_SYSCALL_FILE_REPLACE:
            return dispatch_file_replace(
                address_space, granted_capabilities,
                argument1, argument2, argument3, argument4);
        case APP_SYSCALL_FILE_CREATE_EXCLUSIVE:
            return dispatch_file_create_exclusive(
                address_space, granted_capabilities,
                argument1, argument2, argument3, argument4);
        case APP_SYSCALL_WINDOW_CREATE:
            return dispatch_window_create(
                address_space, process_id, granted_capabilities,
                argument1, argument2);
        case APP_SYSCALL_WINDOW_PRESENT:
            return dispatch_window_present(
                address_space, process_id, granted_capabilities,
                argument1, argument2);
        case APP_SYSCALL_WINDOW_CLOSE:
            return dispatch_window_close(
                process_id, granted_capabilities, argument1);
        case APP_SYSCALL_INPUT_POLL:
            return dispatch_input_poll(
                address_space, process_id, granted_capabilities,
                argument1, argument2, argument3);
        case APP_SYSCALL_MEMORY_MAP:
            return dispatch_memory_map(
                address_space, process_id, granted_capabilities,
                argument1, argument2);
        case APP_SYSCALL_MEMORY_UNMAP:
            return dispatch_memory_unmap(
                address_space, process_id, granted_capabilities,
                argument1, argument2);
        default:
            return app_error(APP_STATUS_UNSUPPORTED);
    }
}

void app_services_release_process(uint64_t process_id) {
    if (process_id == 0) return;
    app_network_services_release_process(process_id);
    if (g_launch_focus_process_id == process_id) {
        g_launch_focus_process_id = 0;
    }

    for (size_t index = 0;
         index < APP_FILE_HANDLE_CAPACITY; index++) {
        if (g_files[index].in_use &&
            g_files[index].process_id == process_id) {
            clear_bytes(&g_files[index], sizeof(g_files[index]));
        }
    }
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use &&
            g_windows[index].process_id == process_id) {
            close_window(&g_windows[index]);
        }
    }
    for (size_t index = 0;
         index < APP_MEMORY_RECORD_CAPACITY; index++) {
        struct app_memory_record* record = &g_memory[index];
        if (!record->in_use ||
            record->process_id != process_id) {
            continue;
        }
        if (record->address_space != NULL) {
            (void)paging_user_unmap_anonymous(
                record->address_space, record->address,
                record->mapped_size);
        }
        clear_bytes(record, sizeof(*record));
    }
}

bool app_services_authorize_launch_focus(uint64_t process_id) {
    if (!g_desktop_active || process_id == 0) return false;
    g_launch_focus_process_id = process_id;
    return true;
}

size_t app_services_window_count(void) {
    size_t count = 0;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        if (g_windows[index].in_use) count++;
    }
    return count;
}

static size_t queue_all_window_close_events(void) {
    size_t count = 0;
    for (size_t index = 0; index < APP_WINDOW_CAPACITY; index++) {
        struct app_window_record* window = &g_windows[index];
        if (!window->in_use) continue;
        struct app_input_event event;
        clear_bytes(&event, sizeof(event));
        event.type = APP_INPUT_WINDOW_CLOSE;
        queue_window_input(window, &event);
        count++;
    }
    return count;
}

size_t app_services_request_all_windows_close(void) {
    return g_desktop_active
        ? queue_all_window_close_events()
        : 0;
}

void app_services_set_windows_hidden(bool hidden) {
    if (!g_desktop_active || g_windows_hidden == hidden) return;

    keyboard_discard_pending();
    g_launch_focus_process_id = 0;
    if (hidden) {
        g_hidden_focused_window_handle =
            g_focused_window_handle;
        g_focused_window_handle = 0;
    } else {
        struct app_window_record* previous =
            find_window_handle(g_hidden_focused_window_handle);
        g_focused_window_handle =
            previous == NULL ? 0 : previous->handle;
        g_hidden_focused_window_handle = 0;
    }
    g_windows_hidden = hidden;
}

void app_services_set_desktop_active(bool active) {
    g_desktop_active = active;
    g_input_mouse_initialized = false;
    if (active) return;

    keyboard_discard_pending();
    g_focused_window_handle = 0;
    g_hidden_focused_window_handle = 0;
    g_launch_focus_process_id = 0;
    g_pointer_capture_handle = 0;
    g_pointer_gesture_consumed = false;
    g_windows_hidden = false;
    (void)queue_all_window_close_events();
}

static void draw_title_bounded(
    const struct app_window_record* window,
    int x, int y, int available_width,
    uint32_t foreground, uint32_t background) {
    if (window == NULL || available_width <= 0) return;
    size_t index = 0;
    while (window->title[index] != '\0' &&
           (int)((index + 1u) * 8u) <= available_width) {
        graphics_draw_char(x + (int)index * 8, y,
                           window->title[index],
                           foreground, background);
        index++;
    }
}

void app_services_render_overlay(void) {
    if (!g_desktop_active || g_windows_hidden) return;
    const uint32_t screen_width = graphics_get_width();
    const uint32_t screen_height = graphics_get_height();
    uint64_t previous_z = 0;
    for (size_t visible = 0;
         visible < APP_WINDOW_CAPACITY; visible++) {
        struct app_window_record* window = NULL;
        for (size_t index = 0;
             index < APP_WINDOW_CAPACITY; index++) {
            struct app_window_record* candidate = &g_windows[index];
            if (!candidate->in_use || candidate->pixels == NULL ||
                candidate->z_order <= previous_z) {
                continue;
            }
            if (window == NULL ||
                candidate->z_order < window->z_order) {
                window = candidate;
            }
        }
        if (window == NULL) break;
        previous_z = window->z_order;

        int x = window->x;
        int y = window->y;
        const int total_width = (int)window->width + 4;
        const int total_height =
            (int)window->height + APP_WINDOW_TITLE_HEIGHT + 4;
        if (total_width <= (int)screen_width) {
            if (x < 0) x = 0;
            if (x + total_width > (int)screen_width) {
                x = (int)screen_width - total_width;
            }
        }
        const int desktop_height =
            screen_height > APP_DESKTOP_BOTTOM_MARGIN
                ? (int)(screen_height - APP_DESKTOP_BOTTOM_MARGIN)
                : (int)screen_height;
        if (total_height <= desktop_height) {
            if (y < 0) y = 0;
            if (y + total_height > desktop_height) {
                y = desktop_height - total_height;
            }
        }
        window->x = x;
        window->y = y;

        graphics_fill_rect(x, y, total_width, total_height,
                           0x00181824u);
        graphics_fill_rect(
            x + APP_WINDOW_BORDER, y + APP_WINDOW_BORDER,
            (int)window->width,
            APP_WINDOW_TITLE_HEIGHT, 0x00354f86u);
        draw_title_bounded(
            window, x + 8, y + 9,
            (int)window->width - 36,
            0x00ffffffu, 0x00354f86u);
        graphics_fill_rect(
            x + (int)window->width - 18, y + 3,
            16, 16, 0x00a33b43u);
        graphics_draw_char(
            x + (int)window->width - 14, y + 7,
            'x', 0x00ffffffu, 0x00a33b43u);

        const int content_x = x + APP_WINDOW_BORDER;
        const int content_y =
            y + APP_WINDOW_TITLE_HEIGHT + APP_WINDOW_BORDER;
        for (uint32_t row = 0; row < window->height; row++) {
            const size_t row_offset =
                (size_t)row * window->width;
            for (uint32_t column = 0;
                 column < window->width; column++) {
                graphics_put_pixel(
                    content_x + (int)column,
                    content_y + (int)row,
                    window->pixels[row_offset + column]);
            }
        }
    }
}

bool app_services_pointer_captured(int x, int y) {
    if (!g_desktop_active || g_windows_hidden) return false;
    for (size_t index = APP_WINDOW_CAPACITY; index > 0; index--) {
        const struct app_window_record* window =
            &g_windows[index - 1u];
        if (!window->in_use) continue;
        const int total_width = (int)window->width + 4;
        const int total_height =
            (int)window->height + APP_WINDOW_TITLE_HEIGHT + 4;
        if (x >= window->x && x < window->x + total_width &&
            y >= window->y && y < window->y + total_height) {
            return true;
        }
    }
    return false;
}

bool app_services_pointer_gesture_active(void) {
    return g_desktop_active && !g_windows_hidden &&
           g_pointer_gesture_consumed;
}

bool app_services_has_keyboard_focus(void) {
    return g_desktop_active && !g_windows_hidden &&
           find_window_handle(g_focused_window_handle) != NULL;
}

void app_services_blur_keyboard_focus(void) {
    g_launch_focus_process_id = 0;
    if (g_focused_window_handle != 0 ||
        g_hidden_focused_window_handle != 0) {
        keyboard_discard_pending();
        g_focused_window_handle = 0;
        g_hidden_focused_window_handle = 0;
    }
}
