#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_catalog.h"
#include "fs.h"

#define TEST_STATE_MAGIC 0x41505053u
#define TEST_STATE_VERSION 1u
#define TEST_STATE_CAPACITY 8u

struct test_state_entry {
    char executable[APP_EXECUTABLE_CAPACITY];
    uint32_t size;
    uint32_t checksum;
};

struct test_state {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t checksum;
    struct test_state_entry entries[TEST_STATE_CAPACITY];
};

static struct fs_file g_files[FS_MAX_FILES];
static size_t g_log_count;

static struct fs_file* find_mutable(const char* name) {
    if (name == NULL) return NULL;
    for (size_t index = 0; index < FS_MAX_FILES; index++) {
        if (g_files[index].in_use &&
            strcmp(g_files[index].name, name) == 0) {
            return &g_files[index];
        }
    }
    return NULL;
}

const struct fs_file* fs_find(const char* name) {
    return find_mutable(name);
}

bool fs_system_remove(const char* name) {
    struct fs_file* file = find_mutable(name);
    if (file == NULL) return false;
    memset(file, 0, sizeof(*file));
    return true;
}

bool fs_system_rename(const char* old_name, const char* new_name) {
    struct fs_file* file = find_mutable(old_name);
    if (file == NULL || new_name == NULL ||
        strlen(new_name) >= FS_MAX_FILENAME ||
        find_mutable(new_name) != NULL) {
        return false;
    }
    strcpy(file->name, new_name);
    return true;
}

void syslog_write(const char* message) {
    assert(message != NULL);
    g_log_count++;
}

static void reset_files(void) {
    memset(g_files, 0, sizeof(g_files));
    g_log_count = 0;
}

static struct fs_file* seed_file(
    const char* name, const void* bytes, size_t size) {
    assert(name != NULL);
    assert(bytes != NULL || size == 0);
    assert(strlen(name) < FS_MAX_FILENAME);
    assert(size < FS_MAX_FILE_SIZE);
    assert(find_mutable(name) == NULL);
    for (size_t index = 0; index < FS_MAX_FILES; index++) {
        if (g_files[index].in_use) continue;
        g_files[index].in_use = true;
        strcpy(g_files[index].name, name);
        g_files[index].size = size;
        if (size != 0) memcpy(g_files[index].data, bytes, size);
        return &g_files[index];
    }
    assert(!"test filesystem is full");
    return NULL;
}

static uint32_t test_checksum(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < length; index++) {
        value ^= bytes[index];
        value *= 16777619u;
    }
    return value;
}

static void finalize_state(struct test_state* state) {
    assert(state != NULL);
    state->checksum = 0;
    state->checksum = test_checksum(state, sizeof(*state));
}

static uint8_t* read_image(const char* path, size_t* out_size) {
    FILE* file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0);
    assert((unsigned long)length < FS_MAX_FILE_SIZE);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t* bytes = (uint8_t*)malloc((size_t)length);
    assert(bytes != NULL);
    assert(fread(bytes, 1, (size_t)length, file) ==
           (size_t)length);
    assert(fclose(file) == 0);
    *out_size = (size_t)length;
    return bytes;
}

static void install_hello(const uint8_t* image, size_t image_size) {
    app_catalog_reset();
    assert(app_catalog_install_hello(
               image, image_size, NULL) == APP_CATALOG_OK);
}

static void test_exact_mirror_is_removed(
    const uint8_t* image, size_t image_size) {
    reset_files();
    install_hello(image, image_size);
    seed_file("hello.elf", image, image_size);

    assert(app_catalog_reclaim_legacy_filesystem_images() == 1u);
    assert(fs_find("hello.elf") == NULL);
    assert(fs_find("recovered-hello.elf") == NULL);
    assert(g_log_count != 0);
}

static void test_unknown_collision_is_quarantined(
    const uint8_t* image, size_t image_size) {
    static const char unknown[] = "not an embedded application";
    reset_files();
    install_hello(image, image_size);
    seed_file("hello.elf", unknown, sizeof(unknown) - 1u);

    assert(app_catalog_reclaim_legacy_filesystem_images() == 0u);
    assert(fs_find("hello.elf") == NULL);
    const struct fs_file* recovered =
        fs_find("recovered-hello.elf");
    assert(recovered != NULL);
    assert(recovered->size == sizeof(unknown) - 1u);
    assert(memcmp(recovered->data, unknown,
                  sizeof(unknown) - 1u) == 0);
}

static void test_collision_uses_numbered_fallback(
    const uint8_t* image, size_t image_size) {
    static const char unknown[] = "unknown";
    static const char occupied[] = "keep me";
    reset_files();
    install_hello(image, image_size);
    seed_file("hello.elf", unknown, sizeof(unknown) - 1u);
    seed_file("recovered-hello.elf",
              occupied, sizeof(occupied) - 1u);

    assert(app_catalog_reclaim_legacy_filesystem_images() == 0u);
    const struct fs_file* fallback =
        fs_find("recovered-app-0.elf");
    assert(fallback != NULL);
    assert(fallback->size == sizeof(unknown) - 1u);
    assert(memcmp(fallback->data, unknown,
                  sizeof(unknown) - 1u) == 0);
}

static void test_prior_package_and_metadata_are_removed(
    const uint8_t* image, size_t image_size) {
    static const uint8_t prior_image[] = {
        0x7f, 'E', 'L', 'F', 1, 2, 3, 4,
    };
    struct test_state state = {
        .magic = TEST_STATE_MAGIC,
        .version = TEST_STATE_VERSION,
        .count = 1,
    };
    strcpy(state.entries[0].executable, "hello.elf");
    state.entries[0].size = sizeof(prior_image);
    state.entries[0].checksum =
        test_checksum(prior_image, sizeof(prior_image));
    finalize_state(&state);

    reset_files();
    install_hello(image, image_size);
    seed_file("hello.elf", prior_image, sizeof(prior_image));
    seed_file("system-apps.db", &state, sizeof(state));

    assert(app_catalog_reclaim_legacy_filesystem_images() == 2u);
    assert(fs_find("hello.elf") == NULL);
    assert(fs_find("system-apps.db") == NULL);
    assert(fs_find("recovered-hello.elf") == NULL);
}

static void test_invalid_metadata_is_preserved(
    const uint8_t* image, size_t image_size) {
    static const char invalid[] = "invalid state";
    reset_files();
    install_hello(image, image_size);
    seed_file("system-apps.db", invalid, sizeof(invalid) - 1u);

    assert(app_catalog_reclaim_legacy_filesystem_images() == 0u);
    assert(fs_find("system-apps.db") == NULL);
    const struct fs_file* recovered =
        fs_find("recovered-system-apps.db");
    assert(recovered != NULL);
    assert(recovered->size == sizeof(invalid) - 1u);
    assert(memcmp(recovered->data, invalid,
                  sizeof(invalid) - 1u) == 0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    size_t image_size = 0;
    uint8_t* image = read_image(argv[1], &image_size);

    test_exact_mirror_is_removed(image, image_size);
    test_unknown_collision_is_quarantined(image, image_size);
    test_collision_uses_numbered_fallback(image, image_size);
    test_prior_package_and_metadata_are_removed(image, image_size);
    test_invalid_metadata_is_preserved(image, image_size);

    app_catalog_reset();
    free(image);
    puts("app catalog reclaim tests passed");
    return 0;
}
