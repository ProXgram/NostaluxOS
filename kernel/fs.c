#include "fs.h"

#include "kstring.h"
#include "os_info.h"
#include "syslog.h"
#include "ata.h"

#define FS_STORAGE_LBA 2048
#define FS_MAGIC_VAL   0xBA5EBA11
#define FS_LEGACY_DISK_VERSION 1u
#define FS_DISK_VERSION 2u
#define FS_SLOT_COUNT 2u
#define FS_DISK_RECORD_BYTES (1u + FS_MAX_FILENAME + 4u + FS_MAX_FILE_SIZE)
#define FS_DISK_TABLE_BYTES (FS_DISK_RECORD_BYTES * FS_MAX_FILES)
#define FS_DISK_TABLE_SECTORS ((FS_DISK_TABLE_BYTES + 511u) / 512u)
#define FS_DISK_TABLE_PAD (FS_DISK_TABLE_SECTORS * 512u - FS_DISK_TABLE_BYTES)
#define FS_SLOT_SECTORS (1u + FS_DISK_TABLE_SECTORS)

static struct fs_file FILES[FS_MAX_FILES];

struct fs_disk_header {
    uint32_t magic;
    uint32_t version;
    uint32_t table_bytes;
    uint32_t checksum;
    uint32_t generation;
    uint8_t reserved[512 - 20];
} __attribute__((packed));

struct fs_disk_record {
    uint8_t in_use;
    char name[FS_MAX_FILENAME];
    uint32_t size;
    char data[FS_MAX_FILE_SIZE];
} __attribute__((packed));

struct fs_disk_table {
    struct fs_disk_record records[FS_MAX_FILES];
    uint8_t padding[FS_DISK_TABLE_PAD];
} __attribute__((packed));

_Static_assert(sizeof(struct fs_disk_header) == 512, "FS header must occupy one sector");
_Static_assert(sizeof(struct fs_disk_table) == FS_DISK_TABLE_SECTORS * 512u,
               "FS table must be sector aligned");
_Static_assert(FS_DISK_TABLE_SECTORS <= 255u, "ATA PIO count is one byte");

static struct fs_disk_table g_disk_table;
static uint8_t g_active_slot = FS_SLOT_COUNT;
static uint32_t g_active_generation = 0;

static bool fs_is_valid_name(const char* name);

static uint32_t fs_slot_header_lba(uint8_t slot) {
    return FS_STORAGE_LBA + (uint32_t)slot * FS_SLOT_SECTORS;
}

static bool fs_header_is_valid(const struct fs_disk_header* header) {
    return header->magic == FS_MAGIC_VAL &&
           (header->version == FS_LEGACY_DISK_VERSION ||
            header->version == FS_DISK_VERSION) &&
           header->table_bytes == FS_DISK_TABLE_BYTES;
}

static uint32_t fs_header_generation(const struct fs_disk_header* header) {
    return header->version == FS_LEGACY_DISK_VERSION ? 0u : header->generation;
}

static bool fs_generation_is_newer(uint32_t lhs, uint32_t rhs) {
    uint32_t distance = lhs - rhs;
    return distance != 0u && distance < 0x80000000u;
}

static uint32_t fs_checksum(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        value ^= bytes[i];
        value *= 16777619u;
    }
    return value;
}

static void fs_serialize(void) {
    uint8_t* raw = (uint8_t*)&g_disk_table;
    for (size_t i = 0; i < sizeof(g_disk_table); i++) raw[i] = 0;

    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use) continue;
        struct fs_disk_record* record = &g_disk_table.records[i];
        record->in_use = 1;
        record->size = (uint32_t)FILES[i].size;

        size_t n = 0;
        while (FILES[i].name[n] != '\0' && n + 1 < FS_MAX_FILENAME) {
            record->name[n] = FILES[i].name[n];
            n++;
        }

        for (size_t j = 0; j < FILES[i].size; j++) {
            record->data[j] = FILES[i].data[j];
        }
    }
}

static bool fs_deserialize(void) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        const struct fs_disk_record* record = &g_disk_table.records[i];
        if (record->in_use == 0) {
            FILES[i].in_use = false;
            FILES[i].name[0] = '\0';
            FILES[i].size = 0;
            FILES[i].data[0] = '\0';
            continue;
        }
        if (record->in_use != 1 || record->size >= FS_MAX_FILE_SIZE ||
            record->name[FS_MAX_FILENAME - 1] != '\0' ||
            record->data[record->size] != '\0' ||
            !fs_is_valid_name(record->name)) {
            return false;
        }

        for (size_t j = 0; j < record->size; j++) {
            if (record->data[j] == '\0') return false;
        }
        for (size_t previous = 0; previous < i; previous++) {
            if (FILES[previous].in_use &&
                kstrcmp(FILES[previous].name, record->name) == 0) {
                return false;
            }
        }

        FILES[i].in_use = true;
        size_t n = 0;
        while (record->name[n] != '\0') {
            FILES[i].name[n] = record->name[n];
            n++;
        }
        FILES[i].name[n] = '\0';
        FILES[i].size = record->size;
        for (size_t j = 0; j < record->size; j++) {
            FILES[i].data[j] = record->data[j];
        }
        FILES[i].data[record->size] = '\0';
    }
    return true;
}

static bool fs_load_slot(uint8_t slot, const struct fs_disk_header* header) {
    uint32_t header_lba = fs_slot_header_lba(slot);
    if (!ata_read(header_lba + 1u, (uint8_t)FS_DISK_TABLE_SECTORS,
                  (uint8_t*)&g_disk_table)) {
        return false;
    }

    if (header->checksum != fs_checksum(&g_disk_table, sizeof(g_disk_table))) {
        syslog_write("FS: rejected slot with corrupt checksum");
        return false;
    }
    if (!fs_deserialize()) {
        syslog_write("FS: rejected slot with invalid records");
        return false;
    }

    g_active_slot = slot;
    g_active_generation = fs_header_generation(header);
    return true;
}

// Helper to persist data to the disk
static bool fs_sync_to_disk(void) {
    if (!ata_init()) {
        syslog_write("FS: Disk sync failed (no ATA drive)");
        return false;
    }

    uint8_t target_slot =
        g_active_slot < FS_SLOT_COUNT ? (uint8_t)(g_active_slot ^ 1u) : 0u;
    uint32_t target_lba = fs_slot_header_lba(target_slot);
    uint32_t next_generation = g_active_generation + 1u;

    fs_serialize();
    if (!ata_write(target_lba + 1u, (uint8_t)FS_DISK_TABLE_SECTORS,
                   (const uint8_t*)&g_disk_table)) {
        syslog_write("FS: Disk sync failed (write data)");
        return false;
    }

    struct fs_disk_header header = {
        .magic = FS_MAGIC_VAL,
        .version = FS_DISK_VERSION,
        .table_bytes = FS_DISK_TABLE_BYTES,
        .checksum = fs_checksum(&g_disk_table, sizeof(g_disk_table)),
        .generation = next_generation,
        .reserved = {0},
    };
    if (!ata_write(target_lba, 1, (const uint8_t*)&header)) {
        syslog_write("FS: Disk sync failed (commit header)");
        return false;
    }

    g_active_slot = target_slot;
    g_active_generation = next_generation;
    return true;
}

// Helper to load data from the disk
static bool fs_load_from_disk(void) {
    if (!ata_init()) return false;

    struct fs_disk_header headers[FS_SLOT_COUNT];
    bool valid[FS_SLOT_COUNT] = {false, false};
    for (uint8_t slot = 0; slot < FS_SLOT_COUNT; slot++) {
        uint32_t header_lba = fs_slot_header_lba(slot);
        if (ata_read(header_lba, 1, (uint8_t*)&headers[slot])) {
            valid[slot] = fs_header_is_valid(&headers[slot]);
        }
    }

    uint8_t first = 0;
    uint8_t second = 1;
    if (valid[1] &&
        (!valid[0] ||
         fs_generation_is_newer(fs_header_generation(&headers[1]),
                                fs_header_generation(&headers[0])))) {
        first = 1;
        second = 0;
    }

    if (valid[first] && fs_load_slot(first, &headers[first])) {
        return true;
    }
    if (valid[second] && fs_load_slot(second, &headers[second])) {
        syslog_write("FS: recovered previous committed slot");
        return true;
    }
    return false;
}

static void fs_self_test(void);

static void fs_clear(struct fs_file* file) {
    if (file == NULL) {
        return;
    }
    file->in_use = false;
    file->name[0] = '\0';
    file->size = 0;
    file->data[0] = '\0';
}

static bool fs_is_valid_name(const char* name) {
    if (name == NULL || *name == '\0') return false;
    size_t length = 0;
    while (name[length] != '\0') {
        char c = name[length];
        if (c == ' ' || c == '\t' || c == '/' || c == '\\') return false;
        length++;
        if (length >= FS_MAX_FILENAME) return false;
    }
    return true;
}

static void fs_copy_name(struct fs_file* file, const char* name) {
    size_t i = 0;
    while (name[i] != '\0' && i + 1 < FS_MAX_FILENAME) {
        file->name[i] = name[i];
        i++;
    }
    file->name[i] = '\0';
}

static struct fs_file* fs_find_mutable(const char* name) {
    if (name == NULL) return NULL;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use) continue;
        if (kstrcmp(FILES[i].name, name) == 0) return &FILES[i];
    }
    return NULL;
}

static struct fs_file* fs_allocate_slot(void) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use) return &FILES[i];
    }
    return NULL;
}

static bool fs_seed_file(const char* name, const char* contents) {
    struct fs_file* existing = fs_find_mutable(name);
    struct fs_file* target = existing;

    if (target == NULL) {
        target = fs_allocate_slot();
        if (target == NULL) return false;
        target->in_use = true;
        fs_copy_name(target, name);
    }

    size_t length = kstrlen(contents);
    if (length >= FS_MAX_FILE_SIZE) return false;

    for (size_t i = 0; i < length; i++) {
        target->data[i] = contents[i];
    }
    target->data[length] = '\0';
    target->size = length;
    return true;
}

void fs_init(void) {
    // Try to load existing FS
    if (fs_load_from_disk()) {
        syslog_write("FS: loaded from persistent storage");
        return;
    }

    // Fallback: Fresh init
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        fs_clear(&FILES[i]);
    }

    fs_seed_file(
        "readme.txt",
        OS_NAME " is a retro-themed playground kernel.\n"
        "Use 'help' to explore the built-in utilities.\n");

    fs_seed_file(
        "motd.txt",
        "Hold fast to curiosity and keep building!\n"
        "Type 'history' to revisit previous commands.\n");

    fs_seed_file(
        "colors.map",
        "Color IDs 0-15 follow the standard IBM PC palette.\n"
        "Run 'palette' to preview swatches.\n");

    fs_seed_file(
        "system.log",
        "Use the 'logs' command to view the in-memory event log.\n");

    syslog_write("FS: mounted fresh volume");

    if (fs_sync_to_disk()) {
        syslog_write("FS: filesystem formatted and saved");
        fs_self_test();
    } else {
        syslog_write("FS: running with unsaved in-memory volume");
    }
}

size_t fs_file_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (FILES[i].in_use) count++;
    }
    return count;
}

const struct fs_file* fs_file_at(size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use) continue;
        if (seen == index) return &FILES[i];
        seen++;
    }
    return NULL;
}

const struct fs_file* fs_find(const char* name) {
    return fs_find_mutable(name);
}

bool fs_touch(const char* name) {
    if (!fs_is_valid_name(name)) return false;

    struct fs_file* existing = fs_find_mutable(name);
    if (existing != NULL) return true;

    struct fs_file* slot = fs_allocate_slot();
    if (slot == NULL) return false;

    slot->in_use = true;
    fs_copy_name(slot, name);
    slot->size = 0;
    slot->data[0] = '\0';

    if (!fs_sync_to_disk()) {
        fs_clear(slot);
        return false;
    }
    return true;
}

bool fs_write(const char* name, const char* contents) {
    if (!fs_is_valid_name(name) || contents == NULL) return false;
    size_t length = kstrlen(contents);
    if (length >= FS_MAX_FILE_SIZE) return false;

    struct fs_file* file = fs_find_mutable(name);
    bool existed = file != NULL;
    struct fs_file backup;
    if (existed) {
        backup = *file;
    } else {
        file = fs_allocate_slot();
        if (file == NULL) return false;
        fs_clear(file);
        file->in_use = true;
        fs_copy_name(file, name);
    }

    for (size_t i = 0; i < length; i++) {
        file->data[i] = contents[i];
    }
    file->data[length] = '\0';
    file->size = length;

    if (!fs_sync_to_disk()) {
        if (existed) *file = backup;
        else fs_clear(file);
        return false;
    }
    return true;
}

bool fs_append(const char* name, const char* contents) {
    if (!fs_is_valid_name(name) || contents == NULL) return false;
    size_t length = kstrlen(contents);
    if (length >= FS_MAX_FILE_SIZE) return false;

    struct fs_file* file = fs_find_mutable(name);
    bool existed = file != NULL;
    struct fs_file backup;
    if (existed) {
        backup = *file;
    } else {
        file = fs_allocate_slot();
        if (file == NULL) return false;
        fs_clear(file);
        file->in_use = true;
        fs_copy_name(file, name);
    }

    if (length > (FS_MAX_FILE_SIZE - 1u) - file->size) {
        if (!existed) fs_clear(file);
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        file->data[file->size + i] = contents[i];
    }
    file->size += length;
    file->data[file->size] = '\0';

    if (!fs_sync_to_disk()) {
        if (existed) *file = backup;
        else fs_clear(file);
        return false;
    }
    return true;
}

bool fs_remove(const char* name) {
    struct fs_file* file = fs_find_mutable(name);
    if (file == NULL) return false;

    struct fs_file backup = *file;
    fs_clear(file);
    if (!fs_sync_to_disk()) {
        *file = backup;
        return false;
    }
    return true;
}

static void fs_self_test(void) {
    const char* scratch = "__fs_self_test__";
    struct fs_file* f = fs_find_mutable(scratch);
    if (f) fs_clear(f);
    
    if (!fs_touch(scratch)) {
        syslog_write("FS: self-test (touch) failed");
        return;
    }
    
    if (!fs_remove(scratch)) {
        syslog_write("FS: self-test (remove) failed");
        return;
    }
    
    syslog_write("FS: self-test sequence complete");
}
