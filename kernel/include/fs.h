#ifndef FS_H
#define FS_H

#include <stdbool.h>
#include <stddef.h>

#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILE_SIZE 1024

struct fs_file {
    bool in_use;
    char name[FS_MAX_FILENAME];
    size_t size;
    char data[FS_MAX_FILE_SIZE];
};

void fs_init(void);
size_t fs_file_count(void);
const struct fs_file* fs_file_at(size_t index);
const struct fs_file* fs_find(const char* name);
bool fs_touch(const char* name);
bool fs_write(const char* name, const char* contents);
bool fs_append(const char* name, const char* contents);
bool fs_remove(const char* name);

#endif /* FS_H */
