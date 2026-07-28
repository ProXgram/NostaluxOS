#include "app_catalog.h"

#include <stdbool.h>
#include <stdint.h>

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

#ifndef NOSTALUX_HOST_TEST
extern const uint8_t nostalux_hello_app_start[];
extern const uint8_t nostalux_hello_app_end[];
extern const uint8_t nostalux_fault_probe_app_start[];
extern const uint8_t nostalux_fault_probe_app_end[];
extern const uint8_t nostalux_hang_probe_app_start[];
extern const uint8_t nostalux_hang_probe_app_end[];

enum app_catalog_result app_catalog_initialize_embedded(void) {
    const uintptr_t hello_start = (uintptr_t)nostalux_hello_app_start;
    const uintptr_t hello_end = (uintptr_t)nostalux_hello_app_end;
    const uintptr_t fault_start =
        (uintptr_t)nostalux_fault_probe_app_start;
    const uintptr_t fault_end =
        (uintptr_t)nostalux_fault_probe_app_end;
    const uintptr_t hang_start =
        (uintptr_t)nostalux_hang_probe_app_start;
    const uintptr_t hang_end =
        (uintptr_t)nostalux_hang_probe_app_end;
    app_catalog_reset();
    if (hello_end <= hello_start ||
        hello_end - hello_start > SIZE_MAX ||
        fault_end <= fault_start ||
        fault_end - fault_start > SIZE_MAX ||
        hang_end <= hang_start ||
        hang_end - hang_start > SIZE_MAX) {
        return APP_CATALOG_EMBEDDED_IMAGE_UNAVAILABLE;
    }
    enum app_catalog_result result =
        app_catalog_install_hello((const void*)hello_start,
                                  (size_t)(hello_end - hello_start),
                                  NULL);
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_fault_probe(
            (const void*)fault_start,
            (size_t)(fault_end - fault_start),
            NULL);
    }
    if (result == APP_CATALOG_OK) {
        result = app_catalog_install_hang_probe(
            (const void*)hang_start,
            (size_t)(hang_end - hang_start),
            NULL);
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
