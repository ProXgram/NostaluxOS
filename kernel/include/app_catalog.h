#ifndef APP_CATALOG_H
#define APP_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include "app_manifest.h"
#include "elf64_loader.h"

#define APP_CATALOG_CAPACITY APP_REGISTRY_CAPACITY

struct app_catalog_entry {
    struct app_manifest manifest;
    struct elf64_image_plan image_plan;
    const uint8_t* image;
    size_t image_size;
};

enum app_catalog_result {
    APP_CATALOG_OK = 0,
    APP_CATALOG_INVALID_ARGUMENT,
    APP_CATALOG_MANIFEST_REJECTED,
    APP_CATALOG_ELF_REJECTED,
    APP_CATALOG_REGISTRY_REJECTED,
    APP_CATALOG_FULL,
    APP_CATALOG_EMBEDDED_IMAGE_UNAVAILABLE,
};

/*
 * The image bytes are not copied. They must remain immutable and valid until
 * app_catalog_reset() is called.
 */
enum app_catalog_result app_catalog_install(
    const struct app_manifest* manifest,
    const void* image,
    size_t image_size,
    size_t* out_index);

enum app_catalog_result app_catalog_install_hello(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_fault_probe(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_hang_probe(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_rflags_probe(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_stack_probe(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_calculator(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_notepad(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_image_viewer(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_ai_assistant(
    const void* image,
    size_t image_size,
    size_t* out_index);
enum app_catalog_result app_catalog_install_browser(
    const void* image,
    size_t image_size,
    size_t* out_index);

/*
 * Resets the catalog, validates the ELFs embedded by the build, and registers
 * their manifests. Host tests install the real build artifacts explicitly.
 */
enum app_catalog_result app_catalog_initialize_embedded(void);

/*
 * Reclaims redundant ELF mirrors created by an earlier development build.
 * Known package bytes are removed; unknown collisions are preserved under a
 * recovery name. Current apps run from the independently built ELF package
 * embedded in the read-only OS image and consume no user-filesystem extents.
 */
size_t app_catalog_reclaim_legacy_filesystem_images(void);

void app_catalog_reset(void);
size_t app_catalog_count(void);
const struct app_catalog_entry* app_catalog_at(size_t index);
const struct app_catalog_entry* app_catalog_find_id(const char* id);

const char* app_catalog_result_text(enum app_catalog_result result);

#endif /* APP_CATALOG_H */
