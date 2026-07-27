#include "ata.h"
#include "fs.h"
#include "syslog.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SECTOR_SIZE 512u
#define TEST_DISK_SECTORS 4096u
#define TEST_FS_STORAGE_LBA 2048u
#define TEST_FS_RECORD_BYTES \
    (1u + FS_MAX_FILENAME + 4u + FS_MAX_FILE_SIZE)
#define TEST_FS_TABLE_BYTES (TEST_FS_RECORD_BYTES * FS_MAX_FILES)
#define TEST_FS_TABLE_SECTORS \
    ((TEST_FS_TABLE_BYTES + TEST_SECTOR_SIZE - 1u) / TEST_SECTOR_SIZE)
#define TEST_FS_SLOT_SECTORS (1u + TEST_FS_TABLE_SECTORS)

static const uint8_t TEST_SELF_TEST_PAYLOAD[] = {
    0x42u, 0x00u, 0x4Du, 0xFFu
};

static uint8_t g_disk[TEST_DISK_SECTORS * TEST_SECTOR_SIZE];
static bool g_drive_available;
static bool g_fail_reads;
static bool g_fail_writes;
static uint32_t g_fail_read_lba;
static uint32_t g_fail_write_lba;
static size_t g_read_calls;
static size_t g_write_calls;
static size_t g_drop_writes_after;

bool ata_init(void) {
    return g_drive_available;
}

bool ata_read(uint32_t lba, uint8_t count, uint8_t* buffer) {
    g_read_calls++;
    if (!g_drive_available || g_fail_reads || buffer == NULL ||
        count == 0 || lba + count > TEST_DISK_SECTORS ||
        (g_fail_read_lba >= lba &&
         g_fail_read_lba < lba + (uint32_t)count)) {
        return false;
    }
    memcpy(buffer, &g_disk[lba * TEST_SECTOR_SIZE],
           (size_t)count * TEST_SECTOR_SIZE);
    return true;
}

bool ata_write(uint32_t lba, uint8_t count, const uint8_t* buffer) {
    g_write_calls++;
    if (!g_drive_available || g_fail_writes || buffer == NULL ||
        count == 0 || lba + count > TEST_DISK_SECTORS) {
        return false;
    }
    if (g_fail_write_lba >= lba &&
        g_fail_write_lba < lba + (uint32_t)count) {
        return false;
    }
    if (g_write_calls > g_drop_writes_after) {
        return true;
    }
    memcpy(&g_disk[lba * TEST_SECTOR_SIZE], buffer,
           (size_t)count * TEST_SECTOR_SIZE);
    return true;
}

static void reset_storage(void) {
    memset(g_disk, 0, sizeof(g_disk));
    g_drive_available = true;
    g_fail_reads = false;
    g_fail_writes = false;
    g_fail_read_lba = UINT32_MAX;
    g_fail_write_lba = UINT32_MAX;
    g_read_calls = 0;
    g_write_calls = 0;
    g_drop_writes_after = (size_t)-1;
    syslog_init();
}

struct test_fs_header {
    uint32_t magic;
    uint32_t version;
    uint32_t table_bytes;
    uint32_t checksum;
    uint32_t generation;
    uint8_t reserved[TEST_SECTOR_SIZE - 20u];
} __attribute__((packed));

struct test_fs_record {
    uint8_t in_use;
    char name[FS_MAX_FILENAME];
    uint32_t size;
    char data[FS_MAX_FILE_SIZE];
} __attribute__((packed));

_Static_assert(sizeof(struct test_fs_header) == TEST_SECTOR_SIZE,
               "test header layout must match fs.c");
_Static_assert(sizeof(struct test_fs_record) == TEST_FS_RECORD_BYTES,
               "test record layout must match fs.c");

static uint32_t test_checksum(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        value ^= bytes[i];
        value *= 16777619u;
    }
    return value;
}

static uint32_t test_slot_header_lba(uint8_t slot) {
    return TEST_FS_STORAGE_LBA + (uint32_t)slot * TEST_FS_SLOT_SECTORS;
}

static struct test_fs_header* test_header(uint8_t slot) {
    uint32_t header_lba = test_slot_header_lba(slot);
    return (struct test_fs_header*)
        &g_disk[header_lba * TEST_SECTOR_SIZE];
}

static uint8_t* test_table(uint8_t slot) {
    uint32_t table_lba = test_slot_header_lba(slot) + 1u;
    return &g_disk[table_lba * TEST_SECTOR_SIZE];
}

static void update_slot_checksum(uint8_t slot) {
    test_header(slot)->checksum =
        test_checksum(test_table(slot),
                      TEST_FS_TABLE_SECTORS * TEST_SECTOR_SIZE);
}

static size_t rename_file_on_disk(
    const char* old_name, const char* new_name) {
    size_t renamed = 0;
    for (uint8_t slot = 0; slot < 2; slot++) {
        struct test_fs_header* header = test_header(slot);
        if (header->magic == 0) continue;

        struct test_fs_record* records =
            (struct test_fs_record*)test_table(slot);
        bool slot_changed = false;
        for (size_t i = 0; i < FS_MAX_FILES; i++) {
            if (records[i].in_use == 1 &&
                strcmp(records[i].name, old_name) == 0) {
                memset(records[i].name, 0, sizeof(records[i].name));
                strcpy(records[i].name, new_name);
                renamed++;
                slot_changed = true;
                break;
            }
        }
        if (slot_changed) update_slot_checksum(slot);
    }
    return renamed;
}

static uint8_t newest_slot(void) {
    return test_header(1)->generation > test_header(0)->generation
               ? 1u : 0u;
}

static struct test_fs_record* disk_record(
    uint8_t slot, const char* name) {
    struct test_fs_record* records =
        (struct test_fs_record*)test_table(slot);
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (records[i].in_use == 1 &&
            strcmp(records[i].name, name) == 0) {
            return &records[i];
        }
    }
    return NULL;
}

static size_t disk_record_index(uint8_t slot, const char* name) {
    struct test_fs_record* records =
        (struct test_fs_record*)test_table(slot);
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (records[i].in_use == 1 &&
            strcmp(records[i].name, name) == 0) {
            return i;
        }
    }
    return FS_MAX_FILES;
}

static bool disk_contains_name(const char* name) {
    for (uint8_t slot = 0; slot < 2; slot++) {
        if (disk_record(slot, name) != NULL) return true;
    }
    return false;
}

static void assert_self_test_payload(const struct fs_file* file) {
    assert(file != NULL);
    assert(file->size == sizeof(TEST_SELF_TEST_PAYLOAD));
    assert(memcmp(file->data, TEST_SELF_TEST_PAYLOAD,
                  sizeof(TEST_SELF_TEST_PAYLOAD)) == 0);
}

static void fill_all_slots_without_system_log(void) {
    for (uint8_t slot = 0; slot < 2; slot++) {
        uint8_t* table = test_table(slot);
        memset(table, 0, TEST_FS_TABLE_SECTORS * TEST_SECTOR_SIZE);
        struct test_fs_record* records =
            (struct test_fs_record*)table;
        for (size_t i = 0; i < FS_MAX_FILES; i++) {
            records[i].in_use = 1;
            snprintf(records[i].name, sizeof(records[i].name),
                     "user%02zu.txt", i);
            records[i].size = 1;
            records[i].data[0] = 'x';
            records[i].data[1] = '\0';
        }
        update_slot_checksum(slot);
    }
}

static void reset_io_observation(void) {
    g_read_calls = 0;
    g_write_calls = 0;
    g_fail_reads = false;
    g_fail_writes = false;
    g_fail_read_lba = UINT32_MAX;
    g_fail_write_lba = UINT32_MAX;
    g_drop_writes_after = (size_t)-1;
    syslog_init();
}

static bool log_contains(const char* needle) {
    for (size_t i = 0; i < syslog_length(); i++) {
        const char* entry = syslog_entry(i);
        if (entry != NULL && strstr(entry, needle) != NULL) return true;
    }
    return false;
}

static void test_blank_disk_formats_and_reloads(void) {
    reset_storage();
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_PERSISTENT);
    assert(fs_backend_is_persistent());
    assert(g_write_calls >= 8);
    assert(g_read_calls >= 8);
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);
    assert(log_contains("persistent reload sequence complete"));

    const struct fs_file* system_log = fs_find("system.log");
    assert(system_log != NULL);
    assert(strstr(system_log->data, "filesystem formatted and saved") != NULL);
    assert(strstr(system_log->data, "Use the 'logs' command") == NULL);

    size_t writes_before_log_read = g_write_calls;
    syslog_write("unit-test live event");
    system_log = fs_find("system.log");
    assert(strstr(system_log->data, "unit-test live event") != NULL);
    assert(g_write_calls == writes_before_log_read);
    const uint8_t replacement[] = {0x10u, 0x20u, 0x30u};
    assert(!fs_touch("system.log"));
    assert(!fs_write("system.log", "not a real log"));
    assert(!fs_write_bytes("system.log", replacement,
                           sizeof(replacement)));
    assert(!fs_append("system.log", "not a real log"));
    assert(!fs_rename("system.log", "moved.log"));
    assert(!fs_rename("readme.txt", "system.log"));
    assert(!fs_remove("system.log"));
    assert(g_write_calls == writes_before_log_read);
    assert(!disk_contains_name("system.log"));

    assert(fs_write("persist.txt", "survives reload"));
    assert(!disk_contains_name("system.log"));
    syslog_init();
    fs_init();
    const struct fs_file* persisted = fs_find("persist.txt");
    assert(fs_backend_is_persistent());
    assert(persisted != NULL);
    assert(strcmp(persisted->data, "survives reload") == 0);
}

static void test_corrupt_disk_is_preserved(void) {
    reset_storage();
    uint8_t* first_header = &g_disk[TEST_FS_STORAGE_LBA * TEST_SECTOR_SIZE];
    first_header[0] = 0xA5;
    first_header[1] = 0x5A;
    uint8_t original_header[TEST_SECTOR_SIZE];
    memcpy(original_header, first_header, sizeof(original_header));

    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_CORRUPT);
    assert(!fs_backend_is_persistent());
    assert(g_write_calls == 0);
    assert(memcmp(first_header, original_header, sizeof(original_header)) == 0);
    assert(log_contains("corrupt storage preserved"));

    assert(fs_write("session.txt", "memory only"));
    assert(strcmp(fs_find("session.txt")->data, "memory only") == 0);
    assert(g_write_calls == 0);
    assert(memcmp(first_header, original_header, sizeof(original_header)) == 0);
}

static void test_zero_headers_with_data_are_not_formatted(void) {
    reset_storage();
    uint32_t table_lba = TEST_FS_STORAGE_LBA + 1u;
    uint8_t* recovery_data = &g_disk[table_lba * TEST_SECTOR_SIZE];
    recovery_data[37] = 0xC7;

    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_CORRUPT);
    assert(g_write_calls == 0);
    assert(recovery_data[37] == 0xC7);
    assert(fs_write("session.txt", "memory only"));
    assert(g_write_calls == 0);
    assert(recovery_data[37] == 0xC7);
}

static void test_blank_headers_prioritize_table_io_over_corruption(void) {
    reset_storage();
    uint8_t* recovery_data = test_table(0);
    recovery_data[37] = 0xC7;
    g_fail_read_lba = test_slot_header_lba(1) + 1u;

    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(g_read_calls >= 4);
    assert(g_write_calls == 0);
    assert(recovery_data[37] == 0xC7);
    assert(log_contains("storage I/O error"));
    assert(!log_contains("corrupt storage preserved"));

    assert(fs_write("session.txt", "memory only"));
    assert(g_write_calls == 0);
    assert(recovery_data[37] == 0xC7);
}

static void test_valid_older_slot_preserves_corrupt_newer_slot(void) {
    reset_storage();
    fs_init();
    assert(fs_write("newest-only.txt", "latest generation"));
    uint8_t corrupt_slot = newest_slot();
    uint8_t* corrupt_byte =
        &test_table(corrupt_slot)
            [TEST_FS_TABLE_SECTORS * TEST_SECTOR_SIZE - 1u];
    *corrupt_byte ^= 0x5Au;
    uint8_t preserved = *corrupt_byte;

    reset_io_observation();
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_CORRUPT);
    assert(g_write_calls == 0);
    assert(*corrupt_byte == preserved);
    assert(fs_write("session.txt", "recovered snapshot"));
    assert(g_write_calls == 0);
    assert(*corrupt_byte == preserved);
}

static void test_valid_slot_preserves_unsupported_alternate_header(void) {
    reset_storage();
    fs_init();
    uint8_t unsupported_slot = newest_slot();
    test_header(unsupported_slot)->version = 99u;

    reset_io_observation();
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_CORRUPT);
    assert(g_write_calls == 0);
    assert(test_header(unsupported_slot)->version == 99u);
    assert(fs_write("session.txt", "recovered snapshot"));
    assert(g_write_calls == 0);
    assert(test_header(unsupported_slot)->version == 99u);
}

static void test_unavailable_backends_stay_volatile(void) {
    reset_storage();
    g_drive_available = false;
    fs_init();
    assert(fs_backend_status() == FS_BACKEND_VOLATILE_NO_DRIVE);
    assert(fs_touch("session.txt"));
    assert(g_write_calls == 0);

    reset_storage();
    g_fail_reads = true;
    fs_init();
    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(fs_write("session.txt", "read failure"));
    assert(g_write_calls == 0);
}

static void test_partial_header_read_recovers_without_writes(void) {
    reset_storage();
    fs_init();
    assert(fs_write("persist.txt", "trusted data"));
    assert(fs_write("legacy-source.txt", "legacy physical log") );
    assert(rename_file_on_disk("legacy-source.txt", "system.log") != 0);
    uint8_t loaded_slot = newest_slot();
    uint8_t unreadable_slot = (uint8_t)(loaded_slot ^ 1u);

    reset_io_observation();
    g_fail_read_lba = test_slot_header_lba(unreadable_slot);
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(strcmp(fs_find("persist.txt")->data, "trusted data") == 0);
    const struct fs_file* recovered =
        fs_find("recovered-system-log.txt");
    assert(recovered != NULL);
    assert(strcmp(recovered->data, "legacy physical log") == 0);
    const struct fs_file* system_log = fs_find("system.log");
    assert(system_log != NULL);
    assert(strstr(system_log->data, "incomplete header reads") != NULL);
    assert(g_write_calls == 0);

    assert(fs_write("session.txt", "volatile data"));
    assert(g_write_calls == 0);
}

static void test_physical_system_log_is_preserved_on_trusted_volume(void) {
    reset_storage();
    fs_init();
    assert(fs_write("legacy-source.txt", "legacy physical log"));
    assert(rename_file_on_disk("legacy-source.txt", "system.log") != 0);

    reset_io_observation();
    fs_init();

    assert(fs_backend_is_persistent());
    const struct fs_file* recovered =
        fs_find("recovered-system-log.txt");
    assert(recovered != NULL);
    assert(strcmp(recovered->data, "legacy physical log") == 0);
    assert(fs_find("system.log") != NULL);
    assert(g_write_calls != 0);
    assert(log_contains("preserved legacy system.log"));

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert(fs_find("system.log") != NULL);
    recovered = fs_find("recovered-system-log.txt");
    assert(recovered != NULL);
    assert(strcmp(recovered->data, "legacy physical log") == 0);
}

static void test_physical_system_log_uses_collision_free_recovery_name(void) {
    reset_storage();
    fs_init();
    assert(fs_write("recovered-system-log.txt", "existing recovery"));
    assert(fs_write("legacy-source.txt", "legacy physical log"));
    assert(rename_file_on_disk("legacy-source.txt", "system.log") != 0);

    reset_io_observation();
    fs_init();

    assert(fs_backend_is_persistent());
    assert(strcmp(fs_find("recovered-system-log.txt")->data,
                  "existing recovery") == 0);
    const struct fs_file* recovered =
        fs_find("recovered-system-log-1.txt");
    assert(recovered != NULL);
    assert(strcmp(recovered->data, "legacy physical log") == 0);
    assert(g_write_calls != 0);
}

static void test_failed_legacy_log_migration_preserves_disk(void) {
    reset_storage();
    fs_init();
    assert(fs_write("legacy-source.txt", "legacy physical log"));
    assert(rename_file_on_disk("legacy-source.txt", "system.log") != 0);
    uint8_t source_slot = newest_slot();
    struct test_fs_record* source =
        disk_record(source_slot, "system.log");
    assert(source != NULL);

    reset_io_observation();
    g_fail_writes = true;
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(g_write_calls != 0);
    assert(strcmp(fs_find("recovered-system-log.txt")->data,
                  "legacy physical log") == 0);
    assert(fs_find("system.log") != NULL);
    source = disk_record(source_slot, "system.log");
    assert(source != NULL);
    assert(strcmp(source->data, "legacy physical log") == 0);
    size_t writes_after_failure = g_write_calls;
    assert(fs_write("session.txt", "memory only"));
    assert(g_write_calls == writes_after_failure);

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    const struct fs_file* recovered =
        fs_find("recovered-system-log.txt");
    assert(recovered != NULL);
    assert(strcmp(recovered->data, "legacy physical log") == 0);
    assert(disk_record(newest_slot(), "system.log") == NULL);
    assert(disk_record(newest_slot(), "recovered-system-log.txt") != NULL);
}

static void test_self_test_detects_unstored_writes(void) {
    reset_storage();
    /*
     * Preserve the initial format but acknowledge and discard the self-test
     * write. An in-memory-only self-test would pass this broken ATA backend.
     */
    g_drop_writes_after = 2;
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(log_contains("persisted binary read"));
    size_t writes_after_failure = g_write_calls;
    assert(fs_write("session.txt", "safe after failure"));
    assert(g_write_calls == writes_after_failure);
}

static void test_dropped_self_test_rename_is_hidden_and_quarantined(void) {
    reset_storage();
    g_drop_writes_after = 4;
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(log_contains("old name persisted"));
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);
    assert(!fs_touch("__fs_self_test__"));

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);
    const struct fs_file* recovered =
        fs_find("recovered-fs-test.bin");
    assert_self_test_payload(recovered);
    assert(log_contains("preserved stale self-test data"));
    assert(g_write_calls != 0);

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);
    assert_self_test_payload(fs_find("recovered-fs-test.bin"));
}

static void test_dropped_self_test_remove_is_hidden_and_quarantined(void) {
    reset_storage();
    g_drop_writes_after = 6;
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(log_contains("persisted remove"));
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert(fs_find("__fs_self_test__") == NULL);
    assert(fs_find("__fs_self_renamed__") == NULL);
    assert_self_test_payload(fs_find("recovered-fs-test.bin"));
    assert(log_contains("preserved stale self-test data"));
    assert(g_write_calls != 0);
}

static void test_exact_self_test_signature_is_preserved_in_place(void) {
    reset_storage();
    fs_init();
    assert(fs_write("recovered-fs-test.bin", "existing recovery"));
    assert(fs_write_bytes("legacy-source.bin", TEST_SELF_TEST_PAYLOAD,
                          sizeof(TEST_SELF_TEST_PAYLOAD)));
    assert(rename_file_on_disk(
               "legacy-source.bin", "__fs_self_test__") != 0);
    uint8_t source_slot = newest_slot();
    size_t source_index =
        disk_record_index(source_slot, "__fs_self_test__");
    assert(source_index < FS_MAX_FILES);

    reset_io_observation();
    fs_init();

    assert(fs_backend_is_persistent());
    assert(fs_find("__fs_self_test__") == NULL);
    assert(strcmp(fs_find("recovered-fs-test.bin")->data,
                  "existing recovery") == 0);
    assert_self_test_payload(fs_find("recovered-fs-test-1.bin"));
    assert(g_write_calls != 0);
    assert(disk_record_index(newest_slot(),
                             "recovered-fs-test-1.bin") ==
           source_index);

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert(strcmp(fs_find("recovered-fs-test.bin")->data,
                  "existing recovery") == 0);
    assert_self_test_payload(fs_find("recovered-fs-test-1.bin"));
}

static void test_self_test_quarantine_is_session_only_on_io_recovery(void) {
    reset_storage();
    fs_init();
    assert(fs_write_bytes("legacy-source.bin", TEST_SELF_TEST_PAYLOAD,
                          sizeof(TEST_SELF_TEST_PAYLOAD)));
    assert(rename_file_on_disk(
               "legacy-source.bin", "__fs_self_test__") != 0);
    uint8_t source_slot = newest_slot();
    uint8_t unreadable_slot = (uint8_t)(source_slot ^ 1u);

    reset_io_observation();
    g_fail_read_lba = test_slot_header_lba(unreadable_slot);
    fs_init();

    assert(fs_backend_status() == FS_BACKEND_VOLATILE_IO_ERROR);
    assert(fs_find("__fs_self_test__") == NULL);
    assert_self_test_payload(fs_find("recovered-fs-test.bin"));
    assert(g_write_calls == 0);
    assert(disk_record(source_slot, "__fs_self_test__") != NULL);
    assert(!disk_contains_name("recovered-fs-test.bin"));
    assert(log_contains("self-test recovery name is session-only"));

    reset_io_observation();
    fs_init();
    assert(fs_backend_is_persistent());
    assert_self_test_payload(fs_find("recovered-fs-test.bin"));
    assert(disk_record(newest_slot(), "__fs_self_test__") == NULL);
}

static void test_legacy_reserved_name_user_files_are_preserved(void) {
    reset_storage();
    fs_init();
    assert(fs_write("legacy-source.txt", "important user data"));
    assert(rename_file_on_disk(
               "legacy-source.txt", "__fs_self_test__") != 0);

    reset_io_observation();
    fs_init();

    assert(fs_backend_is_persistent());
    const struct fs_file* legacy = fs_find("__fs_self_test__");
    assert(legacy != NULL);
    assert(strcmp(legacy->data, "important user data") == 0);
    assert(g_write_calls == 0);
    assert(!fs_touch("__fs_self_test__"));
    assert(!fs_write("__fs_self_test__", "replacement"));
    assert(!fs_rename("readme.txt", "__fs_self_test__"));
    assert(fs_rename("__fs_self_test__", "rescued.txt"));
    assert(strcmp(fs_find("rescued.txt")->data,
                  "important user data") == 0);

    assert(fs_write("second-source.txt", "remove me safely"));
    assert(rename_file_on_disk(
               "second-source.txt", "__fs_self_renamed__") != 0);
    reset_io_observation();
    fs_init();
    legacy = fs_find("__fs_self_renamed__");
    assert(legacy != NULL);
    assert(strcmp(legacy->data, "remove me safely") == 0);
    assert(fs_remove("__fs_self_renamed__"));
    assert(fs_find("__fs_self_renamed__") == NULL);
}

static void test_full_volume_gets_one_virtual_system_log(void) {
    reset_storage();
    fs_init();
    fill_all_slots_without_system_log();

    reset_io_observation();
    fs_init();

    assert(fs_backend_is_persistent());
    assert(g_write_calls == 0);
    assert(fs_file_count() == FS_MAX_FILES + 1u);
    assert(fs_find("user00.txt") != NULL);
    assert(fs_find("user31.txt") != NULL);

    size_t system_log_count = 0;
    size_t count = fs_file_count();
    for (size_t i = 0; i < count; i++) {
        const struct fs_file* file = fs_file_at(i);
        assert(file != NULL);
        if (strcmp(file->name, "system.log") == 0) {
            system_log_count++;
        }
    }
    assert(system_log_count == 1);
    assert(fs_file_at(count) == NULL);

    size_t writes_before_touch = g_write_calls;
    assert(!fs_touch("system.log"));
    assert(g_write_calls == writes_before_touch);
    assert(!fs_write("system.log", "not writable"));
    assert(fs_write("user00.txt", "changed"));
    assert(fs_file_count() == FS_MAX_FILES + 1u);

    reset_io_observation();
    fs_init();
    assert(fs_file_count() == FS_MAX_FILES + 1u);
    assert(strcmp(fs_find("user00.txt")->data, "changed") == 0);
    assert(fs_find("system.log") != NULL);
}

enum public_mutation {
    MUTATION_TOUCH,
    MUTATION_WRITE_BYTES,
    MUTATION_WRITE_TEXT,
    MUTATION_APPEND,
    MUTATION_RENAME,
    MUTATION_REMOVE,
};

static bool run_public_mutation(enum public_mutation mutation) {
    static const uint8_t replacement[] = {
        0x00u, 0x7Fu, 0xFFu
    };
    switch (mutation) {
        case MUTATION_TOUCH:
            return fs_touch("new-file.txt");
        case MUTATION_WRITE_BYTES:
            return fs_write_bytes("stable.txt", replacement,
                                  sizeof(replacement));
        case MUTATION_WRITE_TEXT:
            return fs_write("stable.txt", "replacement");
        case MUTATION_APPEND:
            return fs_append("stable.txt", " appended");
        case MUTATION_RENAME:
            return fs_rename("stable.txt", "renamed.txt");
        case MUTATION_REMOVE:
            return fs_remove("stable.txt");
    }
    return true;
}

static void assert_mutation_baseline(void) {
    const struct fs_file* stable = fs_find("stable.txt");
    assert(stable != NULL);
    assert(stable->size == strlen("original"));
    assert(memcmp(stable->data, "original", strlen("original")) == 0);
    assert(fs_find("new-file.txt") == NULL);
    assert(fs_find("renamed.txt") == NULL);
}

static void test_public_mutations_roll_back_on_sync_failure(void) {
    static const enum public_mutation mutations[] = {
        MUTATION_TOUCH,
        MUTATION_WRITE_BYTES,
        MUTATION_WRITE_TEXT,
        MUTATION_APPEND,
        MUTATION_RENAME,
        MUTATION_REMOVE,
    };

    for (size_t phase = 0; phase < 2; phase++) {
        for (size_t i = 0;
             i < sizeof(mutations) / sizeof(mutations[0]);
             i++) {
            reset_storage();
            fs_init();
            assert(fs_write("stable.txt", "original"));
            assert_mutation_baseline();

            uint8_t target_slot = (uint8_t)(newest_slot() ^ 1u);
            reset_io_observation();
            g_fail_write_lba =
                test_slot_header_lba(target_slot) +
                (phase == 0 ? 1u : 0u);

            assert(!run_public_mutation(mutations[i]));
            assert(fs_backend_status() ==
                   FS_BACKEND_VOLATILE_IO_ERROR);
            assert_mutation_baseline();
            assert(g_write_calls == (phase == 0 ? 1u : 2u));

            size_t writes_after_failure = g_write_calls;
            assert(fs_write("session-only.txt", "volatile"));
            assert(g_write_calls == writes_after_failure);

            reset_io_observation();
            fs_init();
            assert(fs_backend_is_persistent());
            assert_mutation_baseline();
            assert(fs_find("session-only.txt") == NULL);
        }
    }
}

static void test_syslog_text_snapshot(void) {
    syslog_init();
    syslog_write("alpha");
    syslog_write("beta");
    char text[32];
    size_t length = syslog_copy_text(text, sizeof(text));
    assert(length == strlen("alpha\nbeta\n"));
    assert(strcmp(text, "alpha\nbeta\n") == 0);
}

int main(void) {
    test_blank_disk_formats_and_reloads();
    test_corrupt_disk_is_preserved();
    test_zero_headers_with_data_are_not_formatted();
    test_blank_headers_prioritize_table_io_over_corruption();
    test_valid_older_slot_preserves_corrupt_newer_slot();
    test_valid_slot_preserves_unsupported_alternate_header();
    test_unavailable_backends_stay_volatile();
    test_partial_header_read_recovers_without_writes();
    test_physical_system_log_is_preserved_on_trusted_volume();
    test_physical_system_log_uses_collision_free_recovery_name();
    test_failed_legacy_log_migration_preserves_disk();
    test_self_test_detects_unstored_writes();
    test_dropped_self_test_rename_is_hidden_and_quarantined();
    test_dropped_self_test_remove_is_hidden_and_quarantined();
    test_exact_self_test_signature_is_preserved_in_place();
    test_self_test_quarantine_is_session_only_on_io_recovery();
    test_legacy_reserved_name_user_files_are_preserved();
    test_full_volume_gets_one_virtual_system_log();
    test_public_mutations_roll_back_on_sync_failure();
    test_syslog_text_snapshot();
    puts("filesystem persistence tests passed");
    return 0;
}
