#ifndef APP_MANIFEST_H
#define APP_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define NOSTALUX_APP_MANIFEST_VERSION 1u
#define NOSTALUX_APP_ABI_VERSION      1u

#define APP_ID_CAPACITY          32u
#define APP_DISPLAY_CAPACITY     48u
#define APP_EXECUTABLE_CAPACITY  32u
#define APP_DESCRIPTION_CAPACITY 96u
#define APP_REGISTRY_CAPACITY    32u

enum app_capability {
    APP_CAPABILITY_LOG        = 1ull << 0,
    APP_CAPABILITY_TIME       = 1ull << 1,
    APP_CAPABILITY_FILE_READ  = 1ull << 2,
    APP_CAPABILITY_FILE_WRITE = 1ull << 3,
    APP_CAPABILITY_INPUT      = 1ull << 4,
    APP_CAPABILITY_WINDOW     = 1ull << 5,
    APP_CAPABILITY_MEMORY     = 1ull << 6,
};

#define APP_CAPABILITY_ALL \
    (APP_CAPABILITY_LOG | APP_CAPABILITY_TIME | APP_CAPABILITY_FILE_READ | \
     APP_CAPABILITY_FILE_WRITE | APP_CAPABILITY_INPUT | \
     APP_CAPABILITY_WINDOW | APP_CAPABILITY_MEMORY)

/*
 * This fixed-width representation can be populated by a future filesystem
 * manifest parser without retaining pointers into mutable file buffers.
 */
struct app_manifest {
    uint32_t manifest_version;
    uint32_t abi_version;
    uint64_t capabilities;
    char id[APP_ID_CAPACITY];
    char display_name[APP_DISPLAY_CAPACITY];
    char executable[APP_EXECUTABLE_CAPACITY];
    char description[APP_DESCRIPTION_CAPACITY];
};

enum app_manifest_result {
    APP_MANIFEST_OK = 0,
    APP_MANIFEST_INVALID_ARGUMENT,
    APP_MANIFEST_UNSUPPORTED_VERSION,
    APP_MANIFEST_UNSUPPORTED_ABI,
    APP_MANIFEST_INVALID_CAPABILITY,
    APP_MANIFEST_INVALID_ID,
    APP_MANIFEST_INVALID_DISPLAY_NAME,
    APP_MANIFEST_INVALID_EXECUTABLE,
    APP_MANIFEST_INVALID_DESCRIPTION,
    APP_MANIFEST_DUPLICATE_ID,
    APP_MANIFEST_DUPLICATE_EXECUTABLE,
    APP_MANIFEST_REGISTRY_FULL,
};

enum app_manifest_result app_manifest_validate(
    const struct app_manifest* manifest);

void app_registry_reset(void);
enum app_manifest_result app_registry_register(
    const struct app_manifest* manifest);
size_t app_registry_count(void);
const struct app_manifest* app_registry_at(size_t index);
const struct app_manifest* app_registry_find_id(const char* id);
const struct app_manifest* app_registry_find_executable(
    const char* executable);

const char* app_manifest_result_text(enum app_manifest_result result);

#endif /* APP_MANIFEST_H */
