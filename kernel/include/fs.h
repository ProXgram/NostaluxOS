#ifndef FS_H
#define FS_H

#include <stdbool.h>
#include <stddef.h>

/* Persistent record capacity; virtual read-only files may add public entries. */
#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILE_SIZE 8192
#define FS_LEGACY_UPGRADE_CONFIRMATION "CONFIRM-LEGACY-SNAPSHOT"

struct fs_file {
    bool in_use;
    char name[FS_MAX_FILENAME];
    size_t size;
    char data[FS_MAX_FILE_SIZE];
};

typedef enum {
    FS_BACKEND_UNINITIALIZED = 0,
    FS_BACKEND_PERSISTENT,
    FS_BACKEND_VOLATILE_NO_DRIVE,
    FS_BACKEND_VOLATILE_CORRUPT,
    FS_BACKEND_VOLATILE_IO_ERROR,
} fs_backend_status_t;

typedef enum {
    FS_LEGACY_UPGRADE_UNAVAILABLE = 0,
    FS_LEGACY_UPGRADE_AVAILABLE,
    FS_LEGACY_UPGRADE_OUTCOME_UNCERTAIN,
} fs_legacy_upgrade_status_t;

typedef enum {
    FS_LEGACY_UPGRADE_SUCCEEDED = 0,
    FS_LEGACY_UPGRADE_NOT_AVAILABLE,
    FS_LEGACY_UPGRADE_CONFIRMATION_REQUIRED,
    FS_LEGACY_UPGRADE_FAILED,
    FS_LEGACY_UPGRADE_UNCERTAIN,
} fs_legacy_upgrade_result_t;

void fs_init(void);
fs_backend_status_t fs_backend_status(void);
bool fs_backend_is_persistent(void);
const char* fs_backend_status_text(void);
fs_legacy_upgrade_status_t fs_legacy_upgrade_status(void);
const char* fs_legacy_upgrade_status_text(void);
fs_legacy_upgrade_result_t fs_upgrade_legacy_snapshot(
    const char* confirmation);
size_t fs_file_count(void);
const struct fs_file* fs_file_at(size_t index);
const struct fs_file* fs_find(const char* name);
bool fs_touch(const char* name);
bool fs_write_bytes(const char* name, const void* contents, size_t length);
/* Trusted kernel path for OS-managed catalog files. */
bool fs_system_write_bytes(
    const char* name, const void* contents, size_t length);
bool fs_write(const char* name, const char* contents);
bool fs_append(const char* name, const char* contents);
bool fs_rename(const char* old_name, const char* new_name);
bool fs_remove(const char* name);
bool fs_system_rename(const char* old_name, const char* new_name);
bool fs_system_remove(const char* name);
bool fs_name_is_system_managed(const char* name);

#endif /* FS_H */
