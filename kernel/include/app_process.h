#ifndef APP_PROCESS_H
#define APP_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_abi.h"
#include "app_manifest.h"
#include "elf64_loader.h"

#define APP_PROCESS_CAPACITY 16u

enum app_process_state {
    APP_PROCESS_UNUSED = 0,
    APP_PROCESS_LOADED,
    APP_PROCESS_STARTING,
    APP_PROCESS_RUNNING,
    APP_PROCESS_EXITED,
    APP_PROCESS_FAULTED,
};

enum app_runtime_feature {
    APP_RUNTIME_ELF_LOADING       = 1u << 0,
    APP_RUNTIME_MANIFEST_REGISTRY = 1u << 1,
    APP_RUNTIME_PROCESS_METADATA  = 1u << 2,
    APP_RUNTIME_USER_EXECUTION    = 1u << 3,
    APP_RUNTIME_ADDRESS_ISOLATION = 1u << 4,
    APP_RUNTIME_FAULT_RECOVERY    = 1u << 5,
};

struct app_fault_record {
    uint8_t vector;
    bool has_error_code;
    uint16_t reserved;
    uint32_t reserved2;
    uint64_t error_code;
    uint64_t instruction_pointer;
    uint64_t stack_pointer;
    uint64_t fault_address;
};

struct app_process_info {
    uint64_t process_id;
    char app_id[APP_ID_CAPACITY];
    char display_name[APP_DISPLAY_CAPACITY];
    char startup_argument[APP_STARTUP_ARGUMENT_MAX + 1u];
    enum app_process_state state;
    int64_t exit_code;
    uint64_t capabilities;
    uint64_t image_virtual_base;
    uint64_t image_virtual_end;
    uint64_t entry_point;
    struct app_fault_record fault;
};

enum app_process_result {
    APP_PROCESS_OK = 0,
    APP_PROCESS_INVALID_ARGUMENT,
    APP_PROCESS_TABLE_FULL,
    APP_PROCESS_ID_EXHAUSTED,
    APP_PROCESS_NOT_FOUND,
    APP_PROCESS_INVALID_TRANSITION,
};

void app_process_table_reset(void);

/*
 * Tracks a validated, loaded image. This creates metadata only; it does not
 * schedule or execute the image.
 */
enum app_process_result app_process_track_loaded(
    const struct app_manifest* manifest,
    const struct elf64_image_plan* image,
    uint64_t* out_process_id);

enum app_process_result app_process_mark_starting(uint64_t process_id);
enum app_process_result app_process_set_startup_argument(
    uint64_t process_id, const char* argument);
enum app_process_result app_process_mark_running(uint64_t process_id);
enum app_process_result app_process_mark_exited(uint64_t process_id,
                                                int64_t exit_code);
enum app_process_result app_process_record_fault(
    uint64_t process_id,
    const struct app_fault_record* fault);
/*
 * Releases metadata before a loaded process is scheduled, or after it has
 * exited/faulted. Address-space resources must be freed first.
 */
enum app_process_result app_process_release(uint64_t process_id);

size_t app_process_count(void);
bool app_process_snapshot(size_t index, struct app_process_info* out_info);
bool app_process_find(uint64_t process_id, struct app_process_info* out_info);
bool app_process_get_startup_argument(
    uint64_t process_id,
    char* out_argument,
    size_t capacity,
    size_t* out_length);

/*
 * Reports runtime capabilities implemented by this kernel build. A launch
 * can still be rejected when required CPU support such as NX is unavailable.
 */
uint32_t app_runtime_features(void);
bool app_runtime_execution_implemented(void);

const char* app_process_result_text(enum app_process_result result);

#endif /* APP_PROCESS_H */
