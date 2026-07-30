#include "app_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#if !defined(NOSTALUX_HOST_TEST) || \
    defined(NOSTALUX_APP_CATALOG_FS_TEST)
#define NOSTALUX_APP_CATALOG_FS_ENABLED 1
#include "fs.h"
#include "syslog.h"
#endif

static struct app_catalog_entry g_catalog[APP_CATALOG_CAPACITY];
static size_t g_catalog_count = 0;

static const struct app_manifest HELLO_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_TIME,
    .id = "hello",
    .display_name = "Hello Nostalux",
    .executable = "hello.elf",
    .description = "Separate ELF app that exercises the Apps v1 ABI",
};

static const struct app_manifest FAULT_PROBE_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = 0,
    .id = "fault-probe",
    .display_name = "Fault Isolation Probe",
    .executable = "fault-probe.elf",
    .description = "Touches supervisor memory to verify ring-3 isolation",
};

static const struct app_manifest HANG_PROBE_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = 0,
    .id = "hang-probe",
    .display_name = "Preemption Hang Probe",
    .executable = "hang-probe.elf",
    .description = "Never yields; verifies timer preemption and appkill",
};

static const struct app_manifest RFLAGS_PROBE_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = 0,
    .id = "rflags-probe",
    .display_name = "RFLAGS Return Probe",
    .executable = "rflags-probe.elf",
    .description = "Sets RFLAGS.NT to verify safe syscall and IRQ return",
};

static const struct app_manifest STACK_PROBE_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = 0,
    .id = "stack-probe",
    .display_name = "Stack Return Probe",
    .executable = "stack-probe.elf",
    .description = "Uses a noncanonical RSP to verify safe IRQ return",
};

static const struct app_manifest CALCULATOR_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_INPUT |
                    APP_CAPABILITY_WINDOW | APP_CAPABILITY_MEMORY,
    .id = "calculator",
    .display_name = "Calculator",
    .executable = "calculator.elf",
    .description = "Interactive arithmetic in an isolated ELF process",
};

static const struct app_manifest NOTEPAD_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_FILE_READ |
                    APP_CAPABILITY_FILE_WRITE | APP_CAPABILITY_INPUT |
                    APP_CAPABILITY_WINDOW | APP_CAPABILITY_MEMORY,
    .id = "notepad",
    .display_name = "Notepad",
    .executable = "notepad.elf",
    .description = "Persistent text editor in an isolated ELF process",
};

static const struct app_manifest IMAGE_VIEWER_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_FILE_READ |
                    APP_CAPABILITY_INPUT | APP_CAPABILITY_WINDOW |
                    APP_CAPABILITY_MEMORY,
    .id = "image-viewer",
    .display_name = "Image Viewer",
    .executable = "image-viewer.elf",
    .description = "Renders a real filesystem BMP in an isolated process",
};

static const struct app_manifest AI_ASSISTANT_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_TIME |
                    APP_CAPABILITY_INPUT | APP_CAPABILITY_WINDOW |
                    APP_CAPABILITY_MEMORY | APP_CAPABILITY_NETWORK,
    .id = "ai-assistant",
    .display_name = "AI Assistant",
    .executable = "ai-assistant.elf",
    .description = "Truthful offline intent helper in an isolated process",
};

static const struct app_manifest BROWSER_MANIFEST = {
    .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
    .abi_version = NOSTALUX_APP_ABI_VERSION,
    .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_TIME |
                    APP_CAPABILITY_FILE_READ |
                    APP_CAPABILITY_FILE_WRITE | APP_CAPABILITY_INPUT |
                    APP_CAPABILITY_WINDOW | APP_CAPABILITY_MEMORY |
                    APP_CAPABILITY_NETWORK,
    .id = "browser",
    .display_name = "Browser",
    .executable = "browser.elf",
    .description = "Asynchronous HTTP and file viewer in an isolated process",
};

static bool strings_equal_bounded(const char* first,
                                  const char* second,
                                  size_t capacity) {
    if (first == NULL || second == NULL) return false;
    for (size_t index = 0; index < capacity; index++) {
        if (first[index] != second[index]) return false;
        if (first[index] == '\0') return true;
    }
    return false;
}

void app_catalog_reset(void) {
    for (size_t index = 0; index < APP_CATALOG_CAPACITY; index++) {
        g_catalog[index].image = NULL;
        g_catalog[index].image_size = 0;
    }
    g_catalog_count = 0;
    app_registry_reset();
}

enum app_catalog_result app_catalog_install(
    const struct app_manifest* manifest,
    const void* image,
    size_t image_size,
    size_t* out_index) {
    if (manifest == NULL || image == NULL || image_size == 0) {
        return APP_CATALOG_INVALID_ARGUMENT;
    }
    if (g_catalog_count >= APP_CATALOG_CAPACITY) {
        return APP_CATALOG_FULL;
    }
    if (app_manifest_validate(manifest) != APP_MANIFEST_OK) {
        return APP_CATALOG_MANIFEST_REJECTED;
    }

    struct elf64_image_plan image_plan;
    if (elf64_inspect(image, image_size, &image_plan) != ELF64_LOAD_OK) {
        return APP_CATALOG_ELF_REJECTED;
    }
    if (app_registry_register(manifest) != APP_MANIFEST_OK) {
        return APP_CATALOG_REGISTRY_REJECTED;
    }

    struct app_catalog_entry* entry = &g_catalog[g_catalog_count];
    entry->manifest = *manifest;
    entry->image_plan = image_plan;
    entry->image = (const uint8_t*)image;
    entry->image_size = image_size;
    if (out_index != NULL) *out_index = g_catalog_count;
    g_catalog_count++;
    return APP_CATALOG_OK;
}

enum app_catalog_result app_catalog_install_hello(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(&HELLO_MANIFEST, image, image_size, out_index);
}

enum app_catalog_result app_catalog_install_fault_probe(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(&FAULT_PROBE_MANIFEST, image, image_size,
                               out_index);
}

enum app_catalog_result app_catalog_install_hang_probe(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(&HANG_PROBE_MANIFEST, image, image_size,
                               out_index);
}

enum app_catalog_result app_catalog_install_rflags_probe(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(&RFLAGS_PROBE_MANIFEST, image, image_size,
                               out_index);
}

enum app_catalog_result app_catalog_install_stack_probe(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(&STACK_PROBE_MANIFEST, image, image_size,
                               out_index);
}

enum app_catalog_result app_catalog_install_calculator(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(
        &CALCULATOR_MANIFEST, image, image_size, out_index);
}

enum app_catalog_result app_catalog_install_notepad(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(
        &NOTEPAD_MANIFEST, image, image_size, out_index);
}

enum app_catalog_result app_catalog_install_image_viewer(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(
        &IMAGE_VIEWER_MANIFEST, image, image_size, out_index);
}

enum app_catalog_result app_catalog_install_ai_assistant(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(
        &AI_ASSISTANT_MANIFEST, image, image_size, out_index);
}

enum app_catalog_result app_catalog_install_browser(
    const void* image,
    size_t image_size,
    size_t* out_index) {
    return app_catalog_install(
        &BROWSER_MANIFEST, image, image_size, out_index);
}

#ifndef NOSTALUX_HOST_TEST
extern const uint8_t nostalux_hello_app_start[];
extern const uint8_t nostalux_hello_app_end[];
extern const uint8_t nostalux_fault_probe_app_start[];
extern const uint8_t nostalux_fault_probe_app_end[];
extern const uint8_t nostalux_hang_probe_app_start[];
extern const uint8_t nostalux_hang_probe_app_end[];
extern const uint8_t nostalux_rflags_probe_app_start[];
extern const uint8_t nostalux_rflags_probe_app_end[];
extern const uint8_t nostalux_stack_probe_app_start[];
extern const uint8_t nostalux_stack_probe_app_end[];
extern const uint8_t nostalux_calculator_app_start[];
extern const uint8_t nostalux_calculator_app_end[];
extern const uint8_t nostalux_notepad_app_start[];
extern const uint8_t nostalux_notepad_app_end[];
extern const uint8_t nostalux_image_viewer_app_start[];
extern const uint8_t nostalux_image_viewer_app_end[];
extern const uint8_t nostalux_ai_assistant_app_start[];
extern const uint8_t nostalux_ai_assistant_app_end[];
extern const uint8_t nostalux_browser_app_start[];
extern const uint8_t nostalux_browser_app_end[];

static bool embedded_size(const uint8_t* start, const uint8_t* end,
                          size_t* size) {
    const uintptr_t first = (uintptr_t)start;
    const uintptr_t last = (uintptr_t)end;
    if (size == NULL || last <= first || last - first > SIZE_MAX) {
        return false;
    }
    *size = (size_t)(last - first);
    return true;
}
#endif

#ifdef NOSTALUX_APP_CATALOG_FS_ENABLED
#define APP_SYSTEM_STATE_MAGIC 0x41505053u
#define APP_SYSTEM_STATE_VERSION 1u
#define APP_SYSTEM_STATE_CAPACITY 8u
#define APP_SYSTEM_STATE_FILE "system-apps.db"

struct app_system_state_entry {
    char executable[APP_EXECUTABLE_CAPACITY];
    uint32_t size;
    uint32_t checksum;
};

struct app_system_state {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t checksum;
    struct app_system_state_entry entries[APP_SYSTEM_STATE_CAPACITY];
};

static void catalog_copy_string(
    char* destination, size_t capacity, const char* source) {
    if (destination == NULL || capacity == 0) return;
    size_t index = 0;
    if (source != NULL) {
        while (source[index] != '\0' && index + 1u < capacity) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static uint32_t catalog_checksum(
    const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < length; index++) {
        value ^= bytes[index];
        value *= 16777619u;
    }
    return value;
}

static uint32_t catalog_state_checksum(
    const struct app_system_state* state) {
    struct app_system_state copy = *state;
    copy.checksum = 0;
    return catalog_checksum(&copy, sizeof(copy));
}

static bool catalog_load_state(struct app_system_state* state) {
    if (state == NULL) return false;
    const struct fs_file* file = fs_find(APP_SYSTEM_STATE_FILE);
    if (file == NULL || file->size != sizeof(*state)) return false;
    const uint8_t* bytes = (const uint8_t*)file->data;
    uint8_t* output = (uint8_t*)state;
    for (size_t index = 0; index < sizeof(*state); index++) {
        output[index] = bytes[index];
    }
    if (state->magic != APP_SYSTEM_STATE_MAGIC ||
        state->version != APP_SYSTEM_STATE_VERSION ||
        state->count > APP_SYSTEM_STATE_CAPACITY ||
        state->checksum != catalog_state_checksum(state)) {
        return false;
    }
    for (size_t index = 0; index < state->count; index++) {
        if (state->entries[index]
                .executable[APP_EXECUTABLE_CAPACITY - 1u] != '\0' ||
            state->entries[index].size >= FS_MAX_FILE_SIZE) {
            return false;
        }
    }
    return true;
}

static const struct app_system_state_entry* catalog_state_find(
    const struct app_system_state* state, const char* executable) {
    if (state == NULL || executable == NULL) return NULL;
    for (size_t index = 0; index < state->count; index++) {
        if (strings_equal_bounded(
                state->entries[index].executable,
                executable, APP_EXECUTABLE_CAPACITY)) {
            return &state->entries[index];
        }
    }
    return NULL;
}

static bool catalog_file_matches(
    const struct fs_file* file,
    const uint8_t* image, size_t image_size) {
    if (file == NULL || image == NULL ||
        file->size != image_size) {
        return false;
    }
    for (size_t index = 0; index < image_size; index++) {
        if ((uint8_t)file->data[index] != image[index]) return false;
    }
    return true;
}

static bool catalog_file_matches_state(
    const struct fs_file* file,
    const struct app_system_state_entry* prior) {
    return file != NULL && prior != NULL &&
           file->size == prior->size &&
           catalog_checksum(file->data, file->size) ==
               prior->checksum;
}

static bool catalog_quarantine_system_file(const char* executable) {
    if (executable == NULL || fs_find(executable) == NULL) return true;

    char recovery[FS_MAX_FILENAME];
    catalog_copy_string(recovery, sizeof(recovery), "recovered-");
    size_t used = 0;
    while (recovery[used] != '\0') used++;
    for (size_t index = 0;
         executable[index] != '\0' &&
         used + 1u < sizeof(recovery);
         index++) {
        recovery[used++] = executable[index];
    }
    recovery[used] = '\0';
    if (fs_find(recovery) == NULL &&
        fs_system_rename(executable, recovery)) {
        return true;
    }

    for (uint8_t suffix = 0; suffix < 10u; suffix++) {
        const char fallback[] = "recovered-app-0.elf";
        catalog_copy_string(recovery, sizeof(recovery), fallback);
        recovery[14] = (char)('0' + suffix);
        if (fs_find(recovery) == NULL &&
            fs_system_rename(executable, recovery)) {
            return true;
        }
    }
    return false;
}
#endif

#ifndef NOSTALUX_HOST_TEST
enum app_catalog_result app_catalog_initialize_embedded(void) {
    size_t hello_size;
    size_t fault_size;
    size_t hang_size;
    size_t rflags_size;
    size_t stack_size;
    size_t calculator_size;
    size_t notepad_size;
    size_t image_viewer_size;
    size_t ai_assistant_size;
    size_t browser_size;
    app_catalog_reset();
    if (!embedded_size(nostalux_hello_app_start,
                       nostalux_hello_app_end, &hello_size) ||
        !embedded_size(nostalux_fault_probe_app_start,
                       nostalux_fault_probe_app_end, &fault_size) ||
        !embedded_size(nostalux_hang_probe_app_start,
                       nostalux_hang_probe_app_end, &hang_size) ||
        !embedded_size(nostalux_rflags_probe_app_start,
                       nostalux_rflags_probe_app_end, &rflags_size) ||
        !embedded_size(nostalux_stack_probe_app_start,
                       nostalux_stack_probe_app_end, &stack_size) ||
        !embedded_size(nostalux_calculator_app_start,
                       nostalux_calculator_app_end, &calculator_size) ||
        !embedded_size(nostalux_notepad_app_start,
                       nostalux_notepad_app_end, &notepad_size) ||
        !embedded_size(nostalux_image_viewer_app_start,
                       nostalux_image_viewer_app_end,
                       &image_viewer_size) ||
        !embedded_size(nostalux_ai_assistant_app_start,
                       nostalux_ai_assistant_app_end,
                       &ai_assistant_size) ||
        !embedded_size(nostalux_browser_app_start,
                       nostalux_browser_app_end,
                       &browser_size)) {
        return APP_CATALOG_EMBEDDED_IMAGE_UNAVAILABLE;
    }
    enum app_catalog_result result =
        app_catalog_install_hello(nostalux_hello_app_start, hello_size,
                                  NULL);
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_fault_probe(
            nostalux_fault_probe_app_start, fault_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_hang_probe(
            nostalux_hang_probe_app_start, hang_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_rflags_probe(
            nostalux_rflags_probe_app_start, rflags_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_stack_probe(
            nostalux_stack_probe_app_start, stack_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_calculator(
            nostalux_calculator_app_start, calculator_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_notepad(
            nostalux_notepad_app_start, notepad_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_image_viewer(
            nostalux_image_viewer_app_start, image_viewer_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_ai_assistant(
            nostalux_ai_assistant_app_start, ai_assistant_size, NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_browser(
            nostalux_browser_app_start, browser_size, NULL);
    }
    if (result != APP_CATALOG_OK) {
        app_catalog_reset();
    }
    return result;
}
#else
enum app_catalog_result app_catalog_initialize_embedded(void) {
    app_catalog_reset();
    return APP_CATALOG_EMBEDDED_IMAGE_UNAVAILABLE;
}
#endif

#ifdef NOSTALUX_APP_CATALOG_FS_ENABLED
size_t app_catalog_reclaim_legacy_filesystem_images(void) {
    size_t reclaimed = 0;
    struct app_system_state prior_state;
    const bool prior_state_valid = catalog_load_state(&prior_state);
    if (!prior_state_valid &&
        fs_find(APP_SYSTEM_STATE_FILE) != NULL &&
        !catalog_quarantine_system_file(APP_SYSTEM_STATE_FILE)) {
        syslog_write(
            "Apps: invalid legacy system-app state was preserved in place");
    }

    for (size_t index = 0; index < g_catalog_count; index++) {
        const struct app_catalog_entry* entry = &g_catalog[index];
        const struct fs_file* existing =
            fs_find(entry->manifest.executable);
        if (existing == NULL) continue;

        const struct app_system_state_entry* prior =
            prior_state_valid
                ? catalog_state_find(
                      &prior_state, entry->manifest.executable)
                : NULL;

        /*
         * An earlier development build mirrored the embedded ELFs into the
         * small user filesystem even though the runtime accepted only
         * byte-identical copies. Reclaim those redundant extents. A file is
         * deleted only when its bytes match the current embedded package or
         * the checksummed metadata written with the previous package.
         */
        if (catalog_file_matches(
                existing, entry->image, entry->image_size) ||
            catalog_file_matches_state(existing, prior)) {
            if (fs_system_remove(entry->manifest.executable)) {
                reclaimed++;
            } else {
                syslog_write(
                    "Apps: unable to reclaim a legacy filesystem ELF");
            }
        } else if (!catalog_quarantine_system_file(
                       entry->manifest.executable)) {
            syslog_write(
                "Apps: unknown reserved ELF was preserved in place");
        }
    }

    if (prior_state_valid &&
        fs_find(APP_SYSTEM_STATE_FILE) != NULL) {
        if (fs_system_remove(APP_SYSTEM_STATE_FILE)) {
            reclaimed++;
        } else {
            syslog_write(
                "Apps: unable to reclaim legacy system-app metadata");
        }
    }
    if (reclaimed != 0) {
        syslog_write(
            "Apps: reclaimed redundant filesystem app-package records");
    }
    return reclaimed;
}
#else
size_t app_catalog_reclaim_legacy_filesystem_images(void) {
    return 0;
}
#endif

size_t app_catalog_count(void) {
    return g_catalog_count;
}

const struct app_catalog_entry* app_catalog_at(size_t index) {
    if (index >= g_catalog_count) return NULL;
    return &g_catalog[index];
}

const struct app_catalog_entry* app_catalog_find_id(const char* id) {
    if (id == NULL) return NULL;
    for (size_t index = 0; index < g_catalog_count; index++) {
        if (strings_equal_bounded(g_catalog[index].manifest.id,
                                  id, APP_ID_CAPACITY)) {
            return &g_catalog[index];
        }
    }
    return NULL;
}

const char* app_catalog_result_text(enum app_catalog_result result) {
    switch (result) {
        case APP_CATALOG_OK: return "ok";
        case APP_CATALOG_INVALID_ARGUMENT: return "invalid argument";
        case APP_CATALOG_MANIFEST_REJECTED:
            return "application manifest rejected";
        case APP_CATALOG_ELF_REJECTED:
            return "application ELF rejected";
        case APP_CATALOG_REGISTRY_REJECTED:
            return "application registry rejected the manifest";
        case APP_CATALOG_FULL: return "application catalog is full";
        case APP_CATALOG_EMBEDDED_IMAGE_UNAVAILABLE:
            return "embedded application image is unavailable";
        default: return "unknown application catalog result";
    }
}
