#include "app_manifest.h"

#include <stdbool.h>

static struct app_manifest g_registry[APP_REGISTRY_CAPACITY];
static size_t g_registry_count = 0;

static bool string_is_terminated(const char* text, size_t capacity,
                                 size_t* out_length) {
    if (text == NULL) return false;
    for (size_t index = 0; index < capacity; index++) {
        if (text[index] == '\0') {
            if (out_length != NULL) *out_length = index;
            return true;
        }
    }
    return false;
}

static bool strings_equal_bounded(const char* first, const char* second,
                                  size_t capacity) {
    if (first == NULL || second == NULL) return false;
    for (size_t index = 0; index < capacity; index++) {
        if (first[index] != second[index]) return false;
        if (first[index] == '\0') return true;
    }
    return false;
}

static bool is_lower_letter(char value) {
    return value >= 'a' && value <= 'z';
}

static bool is_digit(char value) {
    return value >= '0' && value <= '9';
}

static bool is_printable_ascii(char value) {
    return value >= 0x20 && value <= 0x7e;
}

static bool id_is_valid(const char id[APP_ID_CAPACITY]) {
    size_t length = 0;
    if (!string_is_terminated(id, APP_ID_CAPACITY, &length) ||
        length == 0 || !is_lower_letter(id[0])) {
        return false;
    }

    for (size_t index = 1; index < length; index++) {
        const char value = id[index];
        if (!is_lower_letter(value) && !is_digit(value) &&
            value != '-' && value != '_') {
            return false;
        }
    }
    return true;
}

static bool display_name_is_valid(
    const char display_name[APP_DISPLAY_CAPACITY]) {
    size_t length = 0;
    if (!string_is_terminated(display_name, APP_DISPLAY_CAPACITY, &length) ||
        length == 0 || display_name[0] == ' ' ||
        display_name[length - 1] == ' ') {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_printable_ascii(display_name[index])) return false;
    }
    return true;
}

static bool executable_is_valid(
    const char executable[APP_EXECUTABLE_CAPACITY]) {
    size_t length = 0;
    if (!string_is_terminated(executable, APP_EXECUTABLE_CAPACITY, &length) ||
        length <= 4) {
        return false;
    }

    for (size_t index = 0; index < length; index++) {
        const char value = executable[index];
        const bool allowed =
            is_lower_letter(value) || is_digit(value) ||
            value == '-' || value == '_' || value == '.';
        if (!allowed) return false;
    }

    return executable[length - 4] == '.' &&
           executable[length - 3] == 'e' &&
           executable[length - 2] == 'l' &&
           executable[length - 1] == 'f';
}

static bool description_is_valid(
    const char description[APP_DESCRIPTION_CAPACITY]) {
    size_t length = 0;
    if (!string_is_terminated(description, APP_DESCRIPTION_CAPACITY,
                              &length)) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_printable_ascii(description[index])) return false;
    }
    return true;
}

enum app_manifest_result app_manifest_validate(
    const struct app_manifest* manifest) {
    if (manifest == NULL) return APP_MANIFEST_INVALID_ARGUMENT;
    if (manifest->manifest_version != NOSTALUX_APP_MANIFEST_VERSION) {
        return APP_MANIFEST_UNSUPPORTED_VERSION;
    }
    if (manifest->abi_version != NOSTALUX_APP_ABI_VERSION) {
        return APP_MANIFEST_UNSUPPORTED_ABI;
    }
    if ((manifest->capabilities & ~APP_CAPABILITY_ALL) != 0) {
        return APP_MANIFEST_INVALID_CAPABILITY;
    }
    if (!id_is_valid(manifest->id)) {
        return APP_MANIFEST_INVALID_ID;
    }
    if (!display_name_is_valid(manifest->display_name)) {
        return APP_MANIFEST_INVALID_DISPLAY_NAME;
    }
    if (!executable_is_valid(manifest->executable)) {
        return APP_MANIFEST_INVALID_EXECUTABLE;
    }
    if (!description_is_valid(manifest->description)) {
        return APP_MANIFEST_INVALID_DESCRIPTION;
    }
    return APP_MANIFEST_OK;
}

void app_registry_reset(void) {
    for (size_t index = 0; index < APP_REGISTRY_CAPACITY; index++) {
        g_registry[index].id[0] = '\0';
    }
    g_registry_count = 0;
}

enum app_manifest_result app_registry_register(
    const struct app_manifest* manifest) {
    enum app_manifest_result result = app_manifest_validate(manifest);
    if (result != APP_MANIFEST_OK) return result;

    for (size_t index = 0; index < g_registry_count; index++) {
        if (strings_equal_bounded(g_registry[index].id, manifest->id,
                                  APP_ID_CAPACITY)) {
            return APP_MANIFEST_DUPLICATE_ID;
        }
        if (strings_equal_bounded(g_registry[index].executable,
                                  manifest->executable,
                                  APP_EXECUTABLE_CAPACITY)) {
            return APP_MANIFEST_DUPLICATE_EXECUTABLE;
        }
    }

    if (g_registry_count >= APP_REGISTRY_CAPACITY) {
        return APP_MANIFEST_REGISTRY_FULL;
    }

    g_registry[g_registry_count] = *manifest;
    g_registry_count++;
    return APP_MANIFEST_OK;
}

size_t app_registry_count(void) {
    return g_registry_count;
}

const struct app_manifest* app_registry_at(size_t index) {
    if (index >= g_registry_count) return NULL;
    return &g_registry[index];
}

const struct app_manifest* app_registry_find_id(const char* id) {
    if (id == NULL) return NULL;
    for (size_t index = 0; index < g_registry_count; index++) {
        if (strings_equal_bounded(g_registry[index].id, id,
                                  APP_ID_CAPACITY)) {
            return &g_registry[index];
        }
    }
    return NULL;
}

const struct app_manifest* app_registry_find_executable(
    const char* executable) {
    if (executable == NULL) return NULL;
    for (size_t index = 0; index < g_registry_count; index++) {
        if (strings_equal_bounded(g_registry[index].executable, executable,
                                  APP_EXECUTABLE_CAPACITY)) {
            return &g_registry[index];
        }
    }
    return NULL;
}

const char* app_manifest_result_text(enum app_manifest_result result) {
    switch (result) {
        case APP_MANIFEST_OK: return "ok";
        case APP_MANIFEST_INVALID_ARGUMENT: return "invalid argument";
        case APP_MANIFEST_UNSUPPORTED_VERSION:
            return "unsupported manifest version";
        case APP_MANIFEST_UNSUPPORTED_ABI:
            return "unsupported application ABI";
        case APP_MANIFEST_INVALID_CAPABILITY:
            return "manifest requests an unknown capability";
        case APP_MANIFEST_INVALID_ID: return "invalid application id";
        case APP_MANIFEST_INVALID_DISPLAY_NAME:
            return "invalid application display name";
        case APP_MANIFEST_INVALID_EXECUTABLE:
            return "invalid application executable";
        case APP_MANIFEST_INVALID_DESCRIPTION:
            return "invalid application description";
        case APP_MANIFEST_DUPLICATE_ID:
            return "application id is already registered";
        case APP_MANIFEST_DUPLICATE_EXECUTABLE:
            return "application executable is already registered";
        case APP_MANIFEST_REGISTRY_FULL:
            return "application registry is full";
        default: return "unknown application manifest result";
    }
}
