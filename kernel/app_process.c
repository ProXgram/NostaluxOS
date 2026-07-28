#include "app_process.h"

static struct app_process_info g_processes[APP_PROCESS_CAPACITY];
static size_t g_process_count = 0;
static uint64_t g_next_process_id = 1;

static void clear_bytes(void* destination, size_t count) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void copy_string(char* destination, size_t capacity,
                        const char* source) {
    size_t index = 0;
    if (source != NULL) {
        while (source[index] != '\0' && index + 1 < capacity) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static struct app_process_info* find_process(uint64_t process_id) {
    if (process_id == 0) return NULL;
    for (size_t index = 0; index < APP_PROCESS_CAPACITY; index++) {
        if (g_processes[index].state != APP_PROCESS_UNUSED &&
            g_processes[index].process_id == process_id) {
            return &g_processes[index];
        }
    }
    return NULL;
}

static struct app_process_info* find_free_slot(void) {
    for (size_t index = 0; index < APP_PROCESS_CAPACITY; index++) {
        if (g_processes[index].state == APP_PROCESS_UNUSED) {
            return &g_processes[index];
        }
    }
    return NULL;
}

static struct app_process_info* find_oldest_terminal_slot(void) {
    struct app_process_info* oldest = NULL;
    for (size_t index = 0; index < APP_PROCESS_CAPACITY; index++) {
        struct app_process_info* process = &g_processes[index];
        if (process->state != APP_PROCESS_EXITED &&
            process->state != APP_PROCESS_FAULTED) {
            continue;
        }
        if (oldest == NULL ||
            process->process_id < oldest->process_id) {
            oldest = process;
        }
    }
    return oldest;
}

static bool plan_is_consistent(const struct elf64_image_plan* image) {
    if (image == NULL || image->segment_count == 0 ||
        image->segment_count > ELF64_LOADER_MAX_LOAD_SEGMENTS ||
        image->virtual_base < ELF64_LOADER_MIN_USER_VADDR ||
        image->virtual_base >= image->virtual_end ||
        image->virtual_end - 1u > ELF64_LOADER_MAX_USER_VADDR ||
        image->virtual_end - image->virtual_base != image->image_span ||
        image->image_span > ELF64_LOADER_MAX_IMAGE_SPAN ||
        image->entry_point < image->virtual_base ||
        image->entry_point >= image->virtual_end) {
        return false;
    }

    bool executable_entry = false;
    for (size_t index = 0; index < image->segment_count; index++) {
        const struct elf64_load_segment* segment = &image->segments[index];
        if (segment->memory_size == 0 ||
            segment->file_size > segment->memory_size ||
            segment->virtual_address < image->virtual_base ||
            segment->virtual_address >= image->virtual_end ||
            segment->memory_size >
                image->virtual_end - segment->virtual_address ||
            (segment->flags & ~(uint32_t)(
                ELF64_SEGMENT_READ |
                ELF64_SEGMENT_WRITE |
                ELF64_SEGMENT_EXECUTE)) != 0 ||
            ((segment->flags & ELF64_SEGMENT_WRITE) != 0 &&
             (segment->flags & ELF64_SEGMENT_EXECUTE) != 0)) {
            return false;
        }
        const uint64_t segment_end =
            segment->virtual_address + segment->memory_size;
        for (size_t other_index = 0; other_index < index; other_index++) {
            const struct elf64_load_segment* other =
                &image->segments[other_index];
            const uint64_t other_end =
                other->virtual_address + other->memory_size;
            if (segment->virtual_address < other_end &&
                other->virtual_address < segment_end) {
                return false;
            }
        }
        if ((segment->flags & ELF64_SEGMENT_EXECUTE) != 0 &&
            image->entry_point >= segment->virtual_address &&
            image->entry_point - segment->virtual_address <
                segment->file_size) {
            executable_entry = true;
        }
    }
    return executable_entry;
}

void app_process_table_reset(void) {
    clear_bytes(g_processes, sizeof(g_processes));
    g_process_count = 0;
    g_next_process_id = 1;
}

enum app_process_result app_process_track_loaded(
    const struct app_manifest* manifest,
    const struct elf64_image_plan* image,
    uint64_t* out_process_id) {
    if (out_process_id == NULL ||
        app_manifest_validate(manifest) != APP_MANIFEST_OK ||
        !plan_is_consistent(image)) {
        return APP_PROCESS_INVALID_ARGUMENT;
    }
    if (g_next_process_id == 0) {
        return APP_PROCESS_ID_EXHAUSTED;
    }

    struct app_process_info* process = find_free_slot();
    if (process == NULL) {
        /*
         * Keep the newest bounded history without allowing repeated launches
         * to exhaust the table forever. Running/starting entries are never
         * reclaimed.
         */
        process = find_oldest_terminal_slot();
        if (process == NULL) return APP_PROCESS_TABLE_FULL;
        clear_bytes(process, sizeof(*process));
        g_process_count--;
    }

    clear_bytes(process, sizeof(*process));
    process->process_id = g_next_process_id;
    if (g_next_process_id == UINT64_MAX) {
        g_next_process_id = 0;
    } else {
        g_next_process_id++;
    }
    copy_string(process->app_id, sizeof(process->app_id), manifest->id);
    copy_string(process->display_name, sizeof(process->display_name),
                manifest->display_name);
    process->state = APP_PROCESS_LOADED;
    process->capabilities = manifest->capabilities;
    process->image_virtual_base = image->virtual_base;
    process->image_virtual_end = image->virtual_end;
    process->entry_point = image->entry_point;
    g_process_count++;
    *out_process_id = process->process_id;
    return APP_PROCESS_OK;
}

enum app_process_result app_process_mark_starting(uint64_t process_id) {
    struct app_process_info* process = find_process(process_id);
    if (process == NULL) return APP_PROCESS_NOT_FOUND;
    if (process->state != APP_PROCESS_LOADED) {
        return APP_PROCESS_INVALID_TRANSITION;
    }
    process->state = APP_PROCESS_STARTING;
    return APP_PROCESS_OK;
}

enum app_process_result app_process_mark_running(uint64_t process_id) {
    struct app_process_info* process = find_process(process_id);
    if (process == NULL) return APP_PROCESS_NOT_FOUND;
    if (process->state != APP_PROCESS_STARTING) {
        return APP_PROCESS_INVALID_TRANSITION;
    }
    process->state = APP_PROCESS_RUNNING;
    return APP_PROCESS_OK;
}

enum app_process_result app_process_mark_exited(uint64_t process_id,
                                                int64_t exit_code) {
    struct app_process_info* process = find_process(process_id);
    if (process == NULL) return APP_PROCESS_NOT_FOUND;
    if (process->state != APP_PROCESS_STARTING &&
        process->state != APP_PROCESS_RUNNING) {
        return APP_PROCESS_INVALID_TRANSITION;
    }
    process->exit_code = exit_code;
    process->state = APP_PROCESS_EXITED;
    return APP_PROCESS_OK;
}

enum app_process_result app_process_record_fault(
    uint64_t process_id,
    const struct app_fault_record* fault) {
    if (fault == NULL) return APP_PROCESS_INVALID_ARGUMENT;

    struct app_process_info* process = find_process(process_id);
    if (process == NULL) return APP_PROCESS_NOT_FOUND;
    if (process->state != APP_PROCESS_STARTING &&
        process->state != APP_PROCESS_RUNNING) {
        return APP_PROCESS_INVALID_TRANSITION;
    }
    process->fault = *fault;
    process->state = APP_PROCESS_FAULTED;
    return APP_PROCESS_OK;
}

enum app_process_result app_process_release(uint64_t process_id) {
    struct app_process_info* process = find_process(process_id);
    if (process == NULL) return APP_PROCESS_NOT_FOUND;
    if (process->state != APP_PROCESS_LOADED &&
        process->state != APP_PROCESS_EXITED &&
        process->state != APP_PROCESS_FAULTED) {
        return APP_PROCESS_INVALID_TRANSITION;
    }

    clear_bytes(process, sizeof(*process));
    g_process_count--;
    return APP_PROCESS_OK;
}

size_t app_process_count(void) {
    return g_process_count;
}

bool app_process_snapshot(size_t index, struct app_process_info* out_info) {
    if (out_info == NULL || index >= g_process_count) return false;

    /*
     * Terminal entries reuse their oldest storage slot, so physical slot
     * order stops matching launch order after the table fills. Select by
     * monotonically increasing process id to keep `apps` chronological.
     */
    uint64_t previous_id = 0;
    for (size_t visible_index = 0;
         visible_index <= index; visible_index++) {
        const struct app_process_info* next = NULL;
        for (size_t slot = 0; slot < APP_PROCESS_CAPACITY; slot++) {
            const struct app_process_info* candidate =
                &g_processes[slot];
            if (candidate->state == APP_PROCESS_UNUSED ||
                candidate->process_id <= previous_id) {
                continue;
            }
            if (next == NULL ||
                candidate->process_id < next->process_id) {
                next = candidate;
            }
        }
        if (next == NULL) return false;
        if (visible_index == index) {
            *out_info = *next;
            return true;
        }
        previous_id = next->process_id;
    }
    return false;
}

bool app_process_find(uint64_t process_id,
                      struct app_process_info* out_info) {
    if (out_info == NULL) return false;
    const struct app_process_info* process = find_process(process_id);
    if (process == NULL) return false;
    *out_info = *process;
    return true;
}

uint32_t app_runtime_features(void) {
    return APP_RUNTIME_ELF_LOADING |
           APP_RUNTIME_MANIFEST_REGISTRY |
           APP_RUNTIME_PROCESS_METADATA |
           APP_RUNTIME_USER_EXECUTION |
           APP_RUNTIME_ADDRESS_ISOLATION |
           APP_RUNTIME_FAULT_RECOVERY;
}

bool app_runtime_execution_implemented(void) {
    return (app_runtime_features() & APP_RUNTIME_USER_EXECUTION) != 0;
}

const char* app_process_result_text(enum app_process_result result) {
    switch (result) {
        case APP_PROCESS_OK: return "ok";
        case APP_PROCESS_INVALID_ARGUMENT: return "invalid argument";
        case APP_PROCESS_TABLE_FULL: return "application process table is full";
        case APP_PROCESS_ID_EXHAUSTED:
            return "application process ids are exhausted";
        case APP_PROCESS_NOT_FOUND: return "application process not found";
        case APP_PROCESS_INVALID_TRANSITION:
            return "invalid application process state transition";
        default: return "unknown application process result";
    }
}
