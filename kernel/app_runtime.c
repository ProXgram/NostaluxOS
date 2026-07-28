#include "app_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_process.h"
#include "elf64_loader.h"
#include "paging.h"
#include "scheduler.h"
#include "syslog.h"

#define APP_USER_STACK_SIZE (16u * PAGING_PAGE_SIZE)
#define APP_USER_STACK_TOP  (PAGING_USER_LIMIT - PAGING_PAGE_SIZE)
#define APP_RUN_DISPATCH_BUDGET 8u

static uint64_t align_down_page(uint64_t value) {
    return value & ~(PAGING_PAGE_SIZE - 1u);
}

static bool align_up_page(uint64_t value, uint64_t* out_value) {
    if (out_value == NULL ||
        value > UINT64_MAX - (PAGING_PAGE_SIZE - 1u)) {
        return false;
    }
    *out_value =
        (value + PAGING_PAGE_SIZE - 1u) &
        ~(PAGING_PAGE_SIZE - 1u);
    return true;
}

static bool ranges_overlap(uint64_t first_start, uint64_t first_end,
                           uint64_t second_start, uint64_t second_end) {
    return first_start < second_end && second_start < first_end;
}

static bool plans_equal(const struct elf64_image_plan* first,
                        const struct elf64_image_plan* second) {
    if (first == NULL || second == NULL ||
        first->entry_point != second->entry_point ||
        first->virtual_base != second->virtual_base ||
        first->virtual_end != second->virtual_end ||
        first->image_span != second->image_span ||
        first->segment_count != second->segment_count) {
        return false;
    }
    for (size_t index = 0; index < first->segment_count; index++) {
        const struct elf64_load_segment* a = &first->segments[index];
        const struct elf64_load_segment* b = &second->segments[index];
        if (a->virtual_address != b->virtual_address ||
            a->memory_size != b->memory_size ||
            a->file_size != b->file_size ||
            a->file_offset != b->file_offset ||
            a->alignment != b->alignment ||
            a->flags != b->flags) {
            return false;
        }
    }
    return true;
}

static bool page_permissions(
    const struct elf64_image_plan* plan,
    uint64_t page_start,
    bool* out_used,
    bool* out_writable,
    bool* out_executable) {
    if (plan == NULL || out_used == NULL ||
        out_writable == NULL || out_executable == NULL) {
        return false;
    }

    const uint64_t page_end = page_start + PAGING_PAGE_SIZE;
    bool used = false;
    bool writable = false;
    bool executable = false;
    for (size_t index = 0; index < plan->segment_count; index++) {
        const struct elf64_load_segment* segment =
            &plan->segments[index];
        const uint64_t segment_end =
            segment->virtual_address + segment->memory_size;
        if (!ranges_overlap(page_start, page_end,
                            segment->virtual_address, segment_end)) {
            continue;
        }
        used = true;
        writable =
            writable ||
            (segment->flags & ELF64_SEGMENT_WRITE) != 0;
        executable =
            executable ||
            (segment->flags & ELF64_SEGMENT_EXECUTE) != 0;
    }

    /*
     * Byte-disjoint PT_LOAD ranges can still share a hardware page. Refuse
     * such a layout when their combined permissions would make that page W+X.
     */
    if (writable && executable) return false;
    *out_used = used;
    *out_writable = writable;
    *out_executable = executable;
    return true;
}

static bool map_and_load_image(
    struct paging_address_space* space,
    const struct app_catalog_entry* entry) {
    uint64_t first_page = align_down_page(entry->image_plan.virtual_base);
    uint64_t final_page;
    if (!align_up_page(entry->image_plan.virtual_end, &final_page) ||
        first_page < PAGING_USER_BASE ||
        final_page > PAGING_USER_LIMIT ||
        first_page >= final_page) {
        return false;
    }

    const uint64_t stack_start =
        APP_USER_STACK_TOP - APP_USER_STACK_SIZE;
    if (ranges_overlap(first_page, final_page,
                       stack_start, APP_USER_STACK_TOP)) {
        return false;
    }

    for (uint64_t page = first_page; page < final_page;
         page += PAGING_PAGE_SIZE) {
        bool used;
        bool writable;
        bool executable;
        if (!page_permissions(&entry->image_plan, page,
                              &used, &writable, &executable)) {
            return false;
        }
        if (used &&
            !paging_user_map_anonymous(space, page,
                                       (size_t)PAGING_PAGE_SIZE,
                                       writable, executable)) {
            return false;
        }
    }

    for (size_t index = 0;
         index < entry->image_plan.segment_count; index++) {
        const struct elf64_load_segment* segment =
            &entry->image_plan.segments[index];
        if (segment->file_size == 0) continue;
        if (!paging_initialize_user_memory(
                space, segment->virtual_address,
                entry->image + (size_t)segment->file_offset,
                (size_t)segment->file_size)) {
            return false;
        }
    }

    return paging_user_map_anonymous(
        space, stack_start, (size_t)APP_USER_STACK_SIZE,
        true, false);
}

enum app_runtime_launch_result app_runtime_spawn(
    const struct app_catalog_entry* entry,
    uint64_t* out_process_id) {
    if (entry == NULL || entry->image == NULL ||
        entry->image_size == 0 || out_process_id == NULL) {
        return APP_RUNTIME_LAUNCH_INVALID_ARGUMENT;
    }
    if (!paging_nx_available()) {
        return APP_RUNTIME_LAUNCH_NX_REQUIRED;
    }

    struct elf64_image_plan verified_plan;
    if (elf64_inspect(entry->image, entry->image_size,
                      &verified_plan) != ELF64_LOAD_OK ||
        !plans_equal(&verified_plan, &entry->image_plan)) {
        return APP_RUNTIME_LAUNCH_INVALID_IMAGE;
    }

    struct paging_address_space* space =
        paging_address_space_create();
    if (space == NULL) return APP_RUNTIME_LAUNCH_OUT_OF_MEMORY;

    if (!map_and_load_image(space, entry)) {
        paging_address_space_destroy(space);
        return APP_RUNTIME_LAUNCH_MAPPING_FAILED;
    }

    uint64_t process_id;
    if (app_process_track_loaded(&entry->manifest,
                                 &entry->image_plan,
                                 &process_id) != APP_PROCESS_OK) {
        paging_address_space_destroy(space);
        return APP_RUNTIME_LAUNCH_PROCESS_TABLE_FAILED;
    }
    if (app_process_mark_starting(process_id) != APP_PROCESS_OK) {
        paging_address_space_destroy(space);
        (void)app_process_release(process_id);
        return APP_RUNTIME_LAUNCH_PROCESS_TABLE_FAILED;
    }

    char task_name[TASK_NAME_MAX] = "app/";
    size_t task_index = 4;
    for (size_t id_index = 0;
         entry->manifest.id[id_index] != '\0' &&
         task_index + 1 < sizeof(task_name);
         id_index++) {
        task_name[task_index++] = entry->manifest.id[id_index];
    }
    task_name[task_index] = '\0';

    if (!scheduler_spawn_user_process(
            task_name, space, entry->image_plan.entry_point,
            APP_USER_STACK_TOP, process_id,
            entry->manifest.capabilities)) {
        (void)app_process_mark_exited(process_id, -1);
        paging_address_space_destroy(space);
        (void)app_process_release(process_id);
        return APP_RUNTIME_LAUNCH_SCHEDULER_FAILED;
    }

    *out_process_id = process_id;
    return APP_RUNTIME_LAUNCH_OK;
}

enum app_runtime_launch_result app_runtime_run_catalog_id(
    const char* app_id) {
    const struct app_catalog_entry* entry =
        app_catalog_find_id(app_id);
    if (entry == NULL) return APP_RUNTIME_LAUNCH_NOT_FOUND;

    uint64_t process_id;
    enum app_runtime_launch_result result =
        app_runtime_spawn(entry, &process_id);
    if (result != APP_RUNTIME_LAUNCH_OK) return result;

    for (size_t dispatch = 0;
         dispatch < APP_RUN_DISPATCH_BUDGET; dispatch++) {
        struct app_process_info process;
        if (!app_process_find(process_id, &process)) break;
        if (process.state == APP_PROCESS_EXITED) {
            return APP_RUNTIME_LAUNCH_OK;
        }
        if (process.state == APP_PROCESS_FAULTED) {
            return APP_RUNTIME_LAUNCH_APP_FAULTED;
        }
        schedule();
    }

    syslog_write("Apps: process remains runnable after dispatch budget");
    return APP_RUNTIME_LAUNCH_DISPATCH_BUDGET;
}

const char* app_runtime_launch_result_text(
    enum app_runtime_launch_result result) {
    switch (result) {
        case APP_RUNTIME_LAUNCH_OK: return "ok";
        case APP_RUNTIME_LAUNCH_INVALID_ARGUMENT:
            return "invalid runtime launch argument";
        case APP_RUNTIME_LAUNCH_INVALID_IMAGE:
            return "ELF changed after catalog validation";
        case APP_RUNTIME_LAUNCH_OUT_OF_MEMORY:
            return "out of memory creating address space";
        case APP_RUNTIME_LAUNCH_MAPPING_FAILED:
            return "unable to map ELF or user stack";
        case APP_RUNTIME_LAUNCH_PROCESS_TABLE_FAILED:
            return "application process metadata rejected";
        case APP_RUNTIME_LAUNCH_SCHEDULER_FAILED:
            return "scheduler rejected user process";
        case APP_RUNTIME_LAUNCH_NOT_FOUND:
            return "application not found";
        case APP_RUNTIME_LAUNCH_APP_FAULTED:
            return "application faulted and was contained";
        case APP_RUNTIME_LAUNCH_DISPATCH_BUDGET:
            return "application remains runnable after dispatch budget";
        case APP_RUNTIME_LAUNCH_NX_REQUIRED:
            return "CPU NX support is required for isolated user apps";
        default: return "unknown runtime launch result";
    }
}
