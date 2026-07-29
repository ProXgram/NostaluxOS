#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdint.h>

#include "app_catalog.h"

enum app_runtime_launch_result {
    APP_RUNTIME_LAUNCH_OK = 0,
    APP_RUNTIME_LAUNCH_INVALID_ARGUMENT,
    APP_RUNTIME_LAUNCH_INVALID_IMAGE,
    APP_RUNTIME_LAUNCH_OUT_OF_MEMORY,
    APP_RUNTIME_LAUNCH_MAPPING_FAILED,
    APP_RUNTIME_LAUNCH_PROCESS_TABLE_FAILED,
    APP_RUNTIME_LAUNCH_SCHEDULER_FAILED,
    APP_RUNTIME_LAUNCH_NOT_FOUND,
    APP_RUNTIME_LAUNCH_APP_FAULTED,
    APP_RUNTIME_LAUNCH_APP_EXITED_ERROR,
    APP_RUNTIME_LAUNCH_DISPATCH_BUDGET,
    APP_RUNTIME_LAUNCH_NX_REQUIRED,
};

/*
 * Creates an isolated address space, maps a validated ELF plus a guarded user
 * stack, and queues one ring-3 scheduler task. On success the scheduler owns
 * the address space.
 */
enum app_runtime_launch_result app_runtime_spawn(
    const struct app_catalog_entry* entry,
    uint64_t* out_process_id);
enum app_runtime_launch_result app_runtime_spawn_with_argument(
    const struct app_catalog_entry* entry,
    const char* startup_argument,
    uint64_t* out_process_id);

/*
 * Runs the named embedded app until it exits/faults or a bounded number of
 * scheduler dispatches has elapsed. A non-yielding ring-3 app is returned to
 * this caller by timer preemption and remains killable as a background task.
 */
enum app_runtime_launch_result app_runtime_run_catalog_id(const char* app_id);
enum app_runtime_launch_result app_runtime_run_catalog_id_with_argument(
    const char* app_id,
    const char* startup_argument);

const char* app_runtime_launch_result_text(
    enum app_runtime_launch_result result);

#endif /* APP_RUNTIME_H */
