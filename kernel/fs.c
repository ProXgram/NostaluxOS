#include "fs.h"

#include "kstring.h"
#include "os_info.h"
#include "syslog.h"
#include "ata.h"

#ifndef FS_STORAGE_LBA
#define FS_STORAGE_LBA 2048u
#endif
#define FS_MAGIC_VAL   0xBA5EBA11
#define FS_LEGACY_DISK_VERSION 1u
#define FS_GENERATION_DISK_VERSION 2u
#define FS_CONTENT_DISK_VERSION 3u
#define FS_DISK_VERSION 4u
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
    /*
     * Version 4 covers every ordering field above with an integrity checksum.
     * In versions 1-3 these bytes were part of the zero-filled reserved area.
     */
    uint32_t header_checksum;
    uint8_t reserved[512 - 24];
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
static uint32_t g_loaded_version = 0;
static fs_backend_status_t g_backend_status = FS_BACKEND_UNINITIALIZED;
static fs_legacy_upgrade_status_t g_legacy_upgrade_status =
    FS_LEGACY_UPGRADE_UNAVAILABLE;
static bool g_self_test_active = false;
static struct fs_file g_virtual_system_log;
static bool g_system_log_is_virtual = false;
static const uint8_t FS_SELF_TEST_PAYLOAD[] = {
    0x42u, 0x00u, 0x4Du, 0xFFu
};

enum fs_disk_load_result {
    FS_DISK_LOADED,
    FS_DISK_LOADED_IO_ERROR,
    FS_DISK_LOADED_CORRUPT,
    FS_DISK_NO_DRIVE,
    FS_DISK_BLANK,
    FS_DISK_CORRUPT,
    FS_DISK_IO_ERROR,
};

enum fs_slot_load_result {
    FS_SLOT_LOADED,
    FS_SLOT_CORRUPT,
    FS_SLOT_IO_ERROR,
};

enum fs_sync_result {
    FS_SYNC_COMMITTED,
    FS_SYNC_FAILED,
    FS_SYNC_UNCERTAIN,
};

enum fs_reconcile_result {
    FS_RECONCILE_MATCH,
    FS_RECONCILE_MISMATCH,
    FS_RECONCILE_IO_ERROR,
};

enum fs_compare_result {
    FS_COMPARE_EQUAL,
    FS_COMPARE_DIFFERENT,
    FS_COMPARE_IO_ERROR,
};

static bool fs_is_valid_name(const char* name);
static struct fs_file* fs_find_mutable(const char* name);
static bool fs_seed_file(const char* name, const char* contents);
static void fs_refresh_system_log(void);
static uint32_t fs_checksum(const void* data, size_t length);
static uint32_t fs_header_checksum(const struct fs_disk_header* header);

static enum fs_disk_load_result fs_loaded_result(
    bool saw_io_error, bool saw_corruption) {
    if (saw_io_error) return FS_DISK_LOADED_IO_ERROR;
    if (saw_corruption) return FS_DISK_LOADED_CORRUPT;
    return FS_DISK_LOADED;
}

static uint32_t fs_slot_header_lba(uint8_t slot) {
    return FS_STORAGE_LBA + (uint32_t)slot * FS_SLOT_SECTORS;
}

static bool fs_header_is_valid(const struct fs_disk_header* header) {
    if (header->magic != FS_MAGIC_VAL ||
        (header->version != FS_LEGACY_DISK_VERSION &&
         header->version != FS_GENERATION_DISK_VERSION &&
         header->version != FS_CONTENT_DISK_VERSION &&
         header->version != FS_DISK_VERSION) ||
        header->table_bytes != FS_DISK_TABLE_BYTES) {
        return false;
    }

    /*
     * Legacy headers remain readable for an in-place migration. Their
     * generation was not integrity-protected, so once any v4 slot exists it
     * is always preferred over a legacy slot.
     */
    return header->version != FS_DISK_VERSION ||
           header->header_checksum == fs_header_checksum(header);
}

static bool fs_header_is_blank(const struct fs_disk_header* header) {
    const uint8_t* bytes = (const uint8_t*)header;
    for (size_t i = 0; i < sizeof(*header); i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

static bool fs_bytes_are_blank(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
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

static uint32_t fs_header_checksum(const struct fs_disk_header* header) {
    const uint8_t* bytes = (const uint8_t*)header;
    const size_t checksum_offset =
        offsetof(struct fs_disk_header, header_checksum);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < sizeof(*header); i++) {
        uint8_t byte =
            i >= checksum_offset &&
                    i < checksum_offset + sizeof(header->header_checksum)
                ? 0u
                : bytes[i];
        value ^= byte;
        value *= 16777619u;
    }
    return value;
}

static bool fs_bytes_equal(
    const void* lhs_data, const void* rhs_data, size_t length) {
    const uint8_t* lhs = (const uint8_t*)lhs_data;
    const uint8_t* rhs = (const uint8_t*)rhs_data;
    for (size_t i = 0; i < length; i++) {
        if (lhs[i] != rhs[i]) return false;
    }
    return true;
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
    /*
     * Validate the complete table before touching the mounted view. A failed
     * reload must not leave FILES half-populated from corrupt media.
     */
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        const struct fs_disk_record* record = &g_disk_table.records[i];
        if (record->in_use == 0) continue;
        if (record->in_use != 1 || record->size >= FS_MAX_FILE_SIZE ||
            record->name[FS_MAX_FILENAME - 1] != '\0' ||
            record->data[record->size] != '\0' ||
            !fs_is_valid_name(record->name)) {
            return false;
        }

        for (size_t previous = 0; previous < i; previous++) {
            const struct fs_disk_record* prior =
                &g_disk_table.records[previous];
            if (prior->in_use == 1 &&
                kstrcmp(prior->name, record->name) == 0) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        const struct fs_disk_record* record = &g_disk_table.records[i];
        if (record->in_use == 0) {
            FILES[i].in_use = false;
            FILES[i].name[0] = '\0';
            FILES[i].size = 0;
            FILES[i].data[0] = '\0';
            continue;
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

static enum fs_slot_load_result fs_load_slot(
    uint8_t slot, const struct fs_disk_header* header) {
    uint32_t header_lba = fs_slot_header_lba(slot);
    if (!ata_read(header_lba + 1u, (uint8_t)FS_DISK_TABLE_SECTORS,
                  (uint8_t*)&g_disk_table)) {
        return FS_SLOT_IO_ERROR;
    }

    if (header->checksum != fs_checksum(&g_disk_table, sizeof(g_disk_table))) {
        syslog_write("FS: rejected slot with corrupt checksum");
        return FS_SLOT_CORRUPT;
    }
    if (!fs_deserialize()) {
        syslog_write("FS: rejected slot with invalid records");
        return FS_SLOT_CORRUPT;
    }

    g_active_slot = slot;
    g_active_generation = fs_header_generation(header);
    g_loaded_version = header->version;
    return FS_SLOT_LOADED;
}

static enum fs_reconcile_result fs_reconcile_table(uint32_t target_lba) {
    uint8_t observed[512];
    const uint8_t* expected = (const uint8_t*)&g_disk_table;
    for (uint32_t sector = 0; sector < FS_DISK_TABLE_SECTORS; sector++) {
        if (!ata_read(target_lba + 1u + sector, 1u, observed)) {
            return FS_RECONCILE_IO_ERROR;
        }
        if (!fs_bytes_equal(
                observed, expected + sector * sizeof(observed),
                sizeof(observed))) {
            return FS_RECONCILE_MISMATCH;
        }
    }
    return FS_RECONCILE_MATCH;
}

static enum fs_reconcile_result fs_reconcile_slot(
    uint32_t target_lba, const struct fs_disk_header* expected_header) {
    struct fs_disk_header observed_header;
    if (!ata_read(target_lba, 1u, (uint8_t*)&observed_header)) {
        return FS_RECONCILE_IO_ERROR;
    }
    if (!fs_bytes_equal(
            &observed_header, expected_header, sizeof(observed_header))) {
        return FS_RECONCILE_MISMATCH;
    }
    return fs_reconcile_table(target_lba);
}

static enum fs_compare_result fs_compare_slot_tables(
    uint8_t first_slot, uint8_t second_slot) {
    uint8_t first_sector[512];
    uint8_t second_sector[512];
    const uint32_t first_lba = fs_slot_header_lba(first_slot) + 1u;
    const uint32_t second_lba = fs_slot_header_lba(second_slot) + 1u;

    for (uint32_t sector = 0; sector < FS_DISK_TABLE_SECTORS; sector++) {
        if (!ata_read(first_lba + sector, 1u, first_sector) ||
            !ata_read(second_lba + sector, 1u, second_sector)) {
            return FS_COMPARE_IO_ERROR;
        }
        if (!fs_bytes_equal(first_sector, second_sector,
                            sizeof(first_sector))) {
            return FS_COMPARE_DIFFERENT;
        }
    }
    return FS_COMPARE_EQUAL;
}

static void fs_accept_commit(
    uint8_t target_slot, uint32_t next_generation) {
    g_active_slot = target_slot;
    g_active_generation = next_generation;
    g_loaded_version = FS_DISK_VERSION;
}

/*
 * Persist a complete table first and make it visible with one header sector.
 * Explicit legacy recovery requires exact read-back even when ATA reports both
 * writes as complete; normal commits still reconcile every uncertain write.
 */
static enum fs_sync_result fs_sync_to_disk_verified(
    bool require_verified_readback) {
    ata_init_result_t init_result = ata_init();
    if (init_result != ATA_INIT_READY) {
        syslog_write(
            init_result == ATA_INIT_NO_DEVICE
                ? "FS: Disk sync failed (no ATA drive)"
                : "FS: Disk sync failed (ATA initialization error)");
        return FS_SYNC_FAILED;
    }

    uint8_t target_slot =
        g_active_slot < FS_SLOT_COUNT ? (uint8_t)(g_active_slot ^ 1u) : 0u;
    uint32_t target_lba = fs_slot_header_lba(target_slot);
    uint32_t next_generation = g_active_generation + 1u;

    fs_serialize();
    ata_write_result_t table_write =
        ata_write(target_lba + 1u, (uint8_t)FS_DISK_TABLE_SECTORS,
                  (const uint8_t*)&g_disk_table);
    if (table_write == ATA_WRITE_FAILED) {
        syslog_write("FS: Disk sync failed (write data)");
        return FS_SYNC_FAILED;
    }
    if (table_write == ATA_WRITE_UNCERTAIN ||
        require_verified_readback) {
        enum fs_reconcile_result reconciliation =
            fs_reconcile_table(target_lba);
        if (reconciliation == FS_RECONCILE_IO_ERROR) {
            syslog_write(
                "FS: data write status is uncertain and read-back failed");
            return FS_SYNC_UNCERTAIN;
        }
        if (reconciliation == FS_RECONCILE_MISMATCH) {
            syslog_write(
                "FS: Disk sync failed (data read-back mismatch)");
            return FS_SYNC_FAILED;
        }
        if (table_write == ATA_WRITE_UNCERTAIN) {
            syslog_write("FS: reconciled uncertain data write");
        }
    }

    struct fs_disk_header header = {
        .magic = FS_MAGIC_VAL,
        .version = FS_DISK_VERSION,
        .table_bytes = FS_DISK_TABLE_BYTES,
        .checksum = fs_checksum(&g_disk_table, sizeof(g_disk_table)),
        .generation = next_generation,
        .header_checksum = 0u,
        .reserved = {0},
    };
    header.header_checksum = fs_header_checksum(&header);

    ata_write_result_t header_write =
        ata_write(target_lba, 1u, (const uint8_t*)&header);
    if (header_write == ATA_WRITE_FAILED) {
        syslog_write("FS: Disk sync failed (commit header)");
        return FS_SYNC_FAILED;
    }
    if (header_write == ATA_WRITE_UNCERTAIN ||
        require_verified_readback) {
        enum fs_reconcile_result reconciliation =
            fs_reconcile_slot(target_lba, &header);
        if (reconciliation == FS_RECONCILE_IO_ERROR) {
            syslog_write(
                "FS: commit status is uncertain and read-back failed");
            return FS_SYNC_UNCERTAIN;
        }
        if (reconciliation == FS_RECONCILE_MISMATCH) {
            syslog_write(
                "FS: Disk sync failed (commit read-back mismatch)");
            return FS_SYNC_FAILED;
        }
        if (header_write == ATA_WRITE_UNCERTAIN) {
            syslog_write("FS: reconciled uncertain commit header");
        }
    }

    fs_accept_commit(target_slot, next_generation);
    return FS_SYNC_COMMITTED;
}

static enum fs_sync_result fs_sync_to_disk(void) {
    return fs_sync_to_disk_verified(false);
}

// Inspect and load storage without ever formatting it as a side effect.
static enum fs_disk_load_result fs_load_from_disk(void) {
    g_legacy_upgrade_status = FS_LEGACY_UPGRADE_UNAVAILABLE;

    ata_init_result_t init_result = ata_init();
    if (init_result == ATA_INIT_NO_DEVICE) return FS_DISK_NO_DRIVE;
    if (init_result != ATA_INIT_READY) return FS_DISK_IO_ERROR;

    g_active_slot = FS_SLOT_COUNT;
    g_active_generation = 0;
    g_loaded_version = 0;

    struct fs_disk_header headers[FS_SLOT_COUNT] = {{0}};
    bool valid[FS_SLOT_COUNT] = {false, false};
    bool saw_nonblank_invalid_header = false;
    bool saw_io_error = false;
    for (uint8_t slot = 0; slot < FS_SLOT_COUNT; slot++) {
        uint32_t header_lba = fs_slot_header_lba(slot);
        if (ata_read(header_lba, 1, (uint8_t*)&headers[slot])) {
            valid[slot] = fs_header_is_valid(&headers[slot]);
            if (!valid[slot] && !fs_header_is_blank(&headers[slot])) {
                saw_nonblank_invalid_header = true;
            }
        } else {
            saw_io_error = true;
        }
    }

    /*
     * Versions 1-3 checksum their tables but do not integrity-protect the
     * generation field. If both legacy snapshots are valid yet different, no
     * ordering rule can prove which one is newest. We may load the legacy
     * generation winner as a recovery view, but must mark it corrupt/volatile
     * so neither snapshot is overwritten. Identical copies are safe to
     * migrate automatically.
     */
    bool legacy_order_is_ambiguous = false;
    if (valid[0] && valid[1] &&
        headers[0].version != FS_DISK_VERSION &&
        headers[1].version != FS_DISK_VERSION) {
        enum fs_compare_result comparison = fs_compare_slot_tables(0u, 1u);
        if (comparison == FS_COMPARE_IO_ERROR) {
            saw_io_error = true;
        } else if (comparison == FS_COMPARE_DIFFERENT) {
            legacy_order_is_ambiguous = true;
            syslog_write(
                "FS: legacy snapshots differ; generation order is untrusted");
        }
    }

    uint8_t first = 0;
    uint8_t second = 1;
    bool first_integrity_protected =
        valid[0] && headers[0].version == FS_DISK_VERSION;
    bool second_integrity_protected =
        valid[1] && headers[1].version == FS_DISK_VERSION;
    if (valid[1] &&
        (!valid[0] ||
         (second_integrity_protected && !first_integrity_protected) ||
         (second_integrity_protected == first_integrity_protected &&
          fs_generation_is_newer(fs_header_generation(&headers[1]),
                                 fs_header_generation(&headers[0]))))) {
        first = 1;
        second = 0;
    }

    bool saw_corrupt_slot = false;
    if (valid[first]) {
        enum fs_slot_load_result result =
            fs_load_slot(first, &headers[first]);
        if (result == FS_SLOT_LOADED) {
            if (legacy_order_is_ambiguous) {
                g_legacy_upgrade_status = FS_LEGACY_UPGRADE_AVAILABLE;
            }
            return fs_loaded_result(
                saw_io_error,
                saw_nonblank_invalid_header || legacy_order_is_ambiguous);
        }
        if (result == FS_SLOT_CORRUPT) saw_corrupt_slot = true;
        else saw_io_error = true;
    }
    if (valid[second]) {
        enum fs_slot_load_result result =
            fs_load_slot(second, &headers[second]);
        if (result == FS_SLOT_LOADED) {
            syslog_write("FS: recovered previous committed slot");
            if (legacy_order_is_ambiguous) {
                g_legacy_upgrade_status = FS_LEGACY_UPGRADE_AVAILABLE;
            }
            return fs_loaded_result(
                saw_io_error,
                saw_corrupt_slot || saw_nonblank_invalid_header ||
                    legacy_order_is_ambiguous);
        }
        if (result == FS_SLOT_CORRUPT) saw_corrupt_slot = true;
        else saw_io_error = true;
    }

    if (saw_io_error) return FS_DISK_IO_ERROR;
    if (saw_corrupt_slot || saw_nonblank_invalid_header) {
        return FS_DISK_CORRUPT;
    }

    bool saw_blank_table_io_error = false;
    bool saw_nonblank_table = false;
    for (uint8_t slot = 0; slot < FS_SLOT_COUNT; slot++) {
        uint32_t table_lba = fs_slot_header_lba(slot) + 1u;
        if (!ata_read(table_lba, (uint8_t)FS_DISK_TABLE_SECTORS,
                      (uint8_t*)&g_disk_table)) {
            saw_blank_table_io_error = true;
            continue;
        }
        if (!fs_bytes_are_blank(&g_disk_table, sizeof(g_disk_table))) {
            saw_nonblank_table = true;
        }
    }
    if (saw_blank_table_io_error) return FS_DISK_IO_ERROR;
    if (saw_nonblank_table) return FS_DISK_CORRUPT;
    return FS_DISK_BLANK;
}

static bool fs_commit_changes(void) {
    if (g_backend_status != FS_BACKEND_PERSISTENT) {
        /*
         * A volatile mount remains useful, but it must never write into media
         * that was rejected as corrupt or unreadable during initialization.
         */
        return true;
    }
    enum fs_sync_result result = fs_sync_to_disk();
    if (result == FS_SYNC_COMMITTED) return true;

    g_backend_status = FS_BACKEND_VOLATILE_IO_ERROR;
    if (result == FS_SYNC_UNCERTAIN) {
        /*
         * The header may already expose this exact mutation on disk. Keep the
         * same mutation in RAM and stop all further disk writes; rolling it
         * back here could make the running view contradict the next boot.
         */
        syslog_write(
            "FS: commit outcome uncertain; mutation retained on a volatile volume");
        return true;
    }

    syslog_write("FS: persistence failed; continuing on a volatile volume");
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
    if (file == NULL || name == NULL) return;
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

static bool fs_name_is_system_log(const char* name) {
    return name != NULL && kstrcmp(name, "system.log") == 0;
}

static bool fs_name_is_self_test_file(const char* name) {
    return name != NULL &&
           (kstrcmp(name, "__fs_self_test__") == 0 ||
            kstrcmp(name, "__fs_self_renamed__") == 0);
}

static bool fs_file_is_self_test_artifact(const struct fs_file* file) {
    if (file == NULL || !file->in_use ||
        !fs_name_is_self_test_file(file->name) ||
        file->size != sizeof(FS_SELF_TEST_PAYLOAD)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(FS_SELF_TEST_PAYLOAD); i++) {
        if ((uint8_t)file->data[i] != FS_SELF_TEST_PAYLOAD[i]) {
            return false;
        }
    }
    return true;
}

static bool fs_clear_self_test_artifacts(void) {
    bool changed = false;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (fs_file_is_self_test_artifact(&FILES[i])) {
            fs_clear(&FILES[i]);
            changed = true;
        }
    }
    return changed;
}

static void fs_recovered_self_test_name(
    size_t attempt, char output[FS_MAX_FILENAME]) {
    const char* base = attempt == 0
                           ? "recovered-fs-test.bin"
                           : "recovered-fs-test-";
    size_t position = 0;
    while (base[position] != '\0' &&
           position + 1 < FS_MAX_FILENAME) {
        output[position] = base[position];
        position++;
    }
    if (attempt != 0) {
        if (attempt >= 10u) {
            output[position++] =
                (char)('0' + (attempt / 10u) % 10u);
        }
        output[position++] = (char)('0' + attempt % 10u);
        const char* suffix = ".bin";
        for (size_t i = 0;
             suffix[i] != '\0' && position + 1 < FS_MAX_FILENAME;
             i++) {
            output[position++] = suffix[i];
        }
    }
    output[position] = '\0';
}

static bool fs_quarantine_self_test_artifacts(void) {
    bool changed = false;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_file_is_self_test_artifact(&FILES[i])) continue;

        for (size_t attempt = 0; attempt <= FS_MAX_FILES; attempt++) {
            char candidate[FS_MAX_FILENAME];
            fs_recovered_self_test_name(attempt, candidate);
            if (fs_find_mutable(candidate) == NULL) {
                /*
                 * Rename the existing record in place. Its slot, payload,
                 * size, and every byte outside the name remain untouched.
                 */
                fs_copy_name(&FILES[i], candidate);
                changed = true;
                break;
            }
        }
    }
    return changed;
}

static void fs_activate_virtual_system_log(void) {
    fs_clear(&g_virtual_system_log);
    g_virtual_system_log.in_use = true;
    fs_copy_name(&g_virtual_system_log, "system.log");
    g_system_log_is_virtual = true;
}

static void fs_recovered_log_name(
    size_t attempt, char output[FS_MAX_FILENAME]) {
    const char* base = attempt == 0
                           ? "recovered-system-log.txt"
                           : "recovered-system-log-";
    size_t position = 0;
    while (base[position] != '\0' &&
           position + 1 < FS_MAX_FILENAME) {
        output[position] = base[position];
        position++;
    }
    if (attempt != 0) {
        if (attempt >= 10u) {
            output[position++] =
                (char)('0' + (attempt / 10u) % 10u);
        }
        output[position++] = (char)('0' + attempt % 10u);
        const char* suffix = ".txt";
        for (size_t i = 0;
             suffix[i] != '\0' && position + 1 < FS_MAX_FILENAME;
             i++) {
            output[position++] = suffix[i];
        }
    }
    output[position] = '\0';
}

static bool fs_quarantine_physical_system_log(void) {
    struct fs_file* physical = fs_find_mutable("system.log");
    if (physical == NULL) return false;

    for (size_t attempt = 0; attempt <= FS_MAX_FILES; attempt++) {
        char candidate[FS_MAX_FILENAME];
        fs_recovered_log_name(attempt, candidate);
        if (fs_find_mutable(candidate) == NULL) {
            fs_copy_name(physical, candidate);
            return true;
        }
    }
    return false;
}

static void fs_refresh_system_log(void) {
    if (!g_system_log_is_virtual) return;
    g_virtual_system_log.size =
        syslog_copy_text(g_virtual_system_log.data,
                         sizeof(g_virtual_system_log.data));
}

static struct fs_file* fs_allocate_slot(void) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use) return &FILES[i];
    }
    return NULL;
}

static bool fs_seed_bytes(const char* name, const void* contents, size_t length) {
    if (!fs_is_valid_name(name) ||
        (contents == NULL && length != 0) ||
        length >= FS_MAX_FILE_SIZE) {
        return false;
    }

    struct fs_file* existing = fs_find_mutable(name);
    struct fs_file* target = existing;

    if (target == NULL) {
        target = fs_allocate_slot();
        if (target == NULL) return false;
        target->in_use = true;
        fs_copy_name(target, name);
    }

    const uint8_t* bytes = (const uint8_t*)contents;
    for (size_t i = 0; i < length; i++) {
        target->data[i] = (char)bytes[i];
    }
    target->data[length] = '\0';
    target->size = length;
    return true;
}

static bool fs_seed_file(const char* name, const char* contents) {
    if (contents == NULL) return false;
    return fs_seed_bytes(name, contents, kstrlen(contents));
}

static void write_u16_le(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

#define DEFAULT_BMP_WIDTH 17u
#define DEFAULT_BMP_HEIGHT 17u
#define DEFAULT_BMP_ROW_STRIDE 52u
#define DEFAULT_BMP_PIXEL_OFFSET 54u
#define DEFAULT_BMP_SIZE \
    (DEFAULT_BMP_PIXEL_OFFSET + DEFAULT_BMP_ROW_STRIDE * DEFAULT_BMP_HEIGHT)

_Static_assert(DEFAULT_BMP_SIZE < FS_MAX_FILE_SIZE,
               "Default BMP must fit in one filesystem record");

static bool fs_seed_default_bmp(void) {
    uint8_t bmp[DEFAULT_BMP_SIZE];
    for (size_t i = 0; i < sizeof(bmp); i++) bmp[i] = 0;

    bmp[0] = 'B';
    bmp[1] = 'M';
    write_u32_le(bmp + 2, (uint32_t)sizeof(bmp));
    write_u32_le(bmp + 10, DEFAULT_BMP_PIXEL_OFFSET);
    write_u32_le(bmp + 14, 40u);
    write_u32_le(bmp + 18, DEFAULT_BMP_WIDTH);
    write_u32_le(bmp + 22, DEFAULT_BMP_HEIGHT);
    write_u16_le(bmp + 26, 1u);
    write_u16_le(bmp + 28, 24u);
    write_u32_le(bmp + 34,
                 DEFAULT_BMP_ROW_STRIDE * DEFAULT_BMP_HEIGHT);
    write_u32_le(bmp + 38, 2835u);
    write_u32_le(bmp + 42, 2835u);

    for (uint32_t y = 0; y < DEFAULT_BMP_HEIGHT; y++) {
        uint32_t file_row = DEFAULT_BMP_HEIGHT - 1u - y;
        uint8_t* row = bmp + DEFAULT_BMP_PIXEL_OFFSET +
                       file_row * DEFAULT_BMP_ROW_STRIDE;
        for (uint32_t x = 0; x < DEFAULT_BMP_WIDTH; x++) {
            uint8_t red = (uint8_t)(12u + x * 3u);
            uint8_t green = (uint8_t)(28u + y * 4u);
            uint8_t blue = (uint8_t)(58u + (x + y) * 2u);
            bool logo = y >= 2u && y <= 14u &&
                        (x == 3u || x == 13u ||
                         (x + 1u >= y && x <= y + 1u));
            bool border = x == 0u || x == DEFAULT_BMP_WIDTH - 1u ||
                          y == 0u || y == DEFAULT_BMP_HEIGHT - 1u;
            if (logo) {
                red = 86u;
                green = 238u;
                blue = 196u;
            } else if (border) {
                red = 36u;
                green = 126u;
                blue = 148u;
            }
            row[x * 3u] = blue;
            row[x * 3u + 1u] = green;
            row[x * 3u + 2u] = red;
        }
    }

    return fs_seed_bytes("nostalux.bmp", bmp, sizeof(bmp));
}

void fs_init(void) {
    fs_activate_virtual_system_log();

    enum fs_disk_load_result load_result = fs_load_from_disk();
    if (load_result == FS_DISK_LOADED ||
        load_result == FS_DISK_LOADED_IO_ERROR ||
        load_result == FS_DISK_LOADED_CORRUPT) {
        bool trusted = load_result == FS_DISK_LOADED;
        bool recovered_corrupt =
            load_result == FS_DISK_LOADED_CORRUPT;
        uint32_t mounted_version = g_loaded_version;
        g_backend_status =
            trusted ? FS_BACKEND_PERSISTENT
                    : recovered_corrupt
                          ? FS_BACKEND_VOLATILE_CORRUPT
                          : FS_BACKEND_VOLATILE_IO_ERROR;
        syslog_write(
            trusted
                ? "FS: loaded from persistent storage"
                : recovered_corrupt
                      ? "FS: recovered a volume while preserving corrupt storage"
                      : "FS: recovered a volume with incomplete header reads");

        bool quarantined_test_files =
            fs_quarantine_self_test_artifacts();
        bool had_physical_system_log =
            fs_find_mutable("system.log") != NULL;
        bool quarantined_system_log =
            had_physical_system_log &&
            fs_quarantine_physical_system_log();
        if (had_physical_system_log && !quarantined_system_log) {
            syslog_write(
                "FS: could not preserve the legacy physical system.log name");
        }

        bool added_default_bmp = false;
        if (trusted && mounted_version < FS_CONTENT_DISK_VERSION &&
            fs_find_mutable("nostalux.bmp") == NULL) {
            added_default_bmp = fs_seed_default_bmp();
        }
        bool needs_integrity_protected_header =
            trusted && mounted_version < FS_DISK_VERSION;

        bool metadata_changed =
            quarantined_test_files || quarantined_system_log ||
            added_default_bmp || needs_integrity_protected_header;
        if (trusted && metadata_changed) {
            if (fs_commit_changes()) {
                if (quarantined_test_files) {
                    syslog_write(
                        "FS: preserved stale self-test data under a recovery name");
                }
                if (quarantined_system_log) {
                    syslog_write(
                        "FS: preserved legacy system.log under a recovery name");
                }
                if (added_default_bmp) {
                    syslog_write("FS: installed default BMP image");
                }
                if (needs_integrity_protected_header &&
                    fs_backend_is_persistent()) {
                    syslog_write(
                        "FS: upgraded to integrity-protected commit metadata");
                }
            } else {
                if (added_default_bmp) {
                    struct fs_file* image =
                        fs_find_mutable("nostalux.bmp");
                    if (image != NULL) fs_clear(image);
                }
                syslog_write(
                    "FS: metadata recovery is available for this session only");
            }
        } else if (!trusted) {
            if (g_legacy_upgrade_status == FS_LEGACY_UPGRADE_AVAILABLE) {
                syslog_write(
                    "FS: explicit legacy snapshot upgrade available; "
                    "run fsupgrade for details");
            }
            if (quarantined_test_files) {
                syslog_write(
                    "FS: self-test recovery name is session-only");
            }
            if (quarantined_system_log) {
                syslog_write(
                    "FS: legacy system.log recovery name is session-only");
            }
        }
        return;
    }

    /*
     * Mount a clean in-memory view for every non-loadable backend. Only an
     * entirely blank pair of complete slots is eligible for formatting.
     * Invalid, partially readable, and corrupt media are preserved for
     * recovery.
     */
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

    fs_seed_default_bmp();

    if (load_result == FS_DISK_BLANK) {
        syslog_write("FS: blank storage detected; mounted fresh volume");
        enum fs_sync_result format_result = fs_sync_to_disk();
        if (format_result == FS_SYNC_COMMITTED) {
            g_backend_status = FS_BACKEND_PERSISTENT;
            syslog_write("FS: filesystem formatted and saved");
            fs_self_test();
        } else {
            g_backend_status = FS_BACKEND_VOLATILE_IO_ERROR;
            syslog_write(format_result == FS_SYNC_UNCERTAIN
                             ? "FS: format outcome uncertain; continuing on a volatile volume"
                             : "FS: format failed; continuing on a volatile volume");
        }
        return;
    }

    if (load_result == FS_DISK_NO_DRIVE) {
        g_backend_status = FS_BACKEND_VOLATILE_NO_DRIVE;
        syslog_write("FS: no ATA drive; mounted a volatile volume");
    } else if (load_result == FS_DISK_CORRUPT) {
        g_backend_status = FS_BACKEND_VOLATILE_CORRUPT;
        syslog_write(
            "FS: corrupt storage preserved; mounted a volatile volume");
    } else {
        g_backend_status = FS_BACKEND_VOLATILE_IO_ERROR;
        syslog_write(
            "FS: storage I/O error; mounted a volatile volume without writes");
    }
}

fs_backend_status_t fs_backend_status(void) {
    return g_backend_status;
}

bool fs_backend_is_persistent(void) {
    return g_backend_status == FS_BACKEND_PERSISTENT;
}

const char* fs_backend_status_text(void) {
    switch (g_backend_status) {
        case FS_BACKEND_PERSISTENT:
            return "Persistent ATA storage";
        case FS_BACKEND_VOLATILE_NO_DRIVE:
            return "Volatile memory (no ATA drive)";
        case FS_BACKEND_VOLATILE_CORRUPT:
            if (g_legacy_upgrade_status == FS_LEGACY_UPGRADE_AVAILABLE) {
                return "Volatile memory (legacy snapshots differ; run fsupgrade)";
            }
            if (g_legacy_upgrade_status ==
                FS_LEGACY_UPGRADE_OUTCOME_UNCERTAIN) {
                return "Volatile memory (legacy upgrade unverified; reboot)";
            }
            return "Volatile memory (corrupt disk preserved)";
        case FS_BACKEND_VOLATILE_IO_ERROR:
            return "Volatile memory (storage I/O error)";
        case FS_BACKEND_UNINITIALIZED:
        default:
            return "Filesystem not initialized";
    }
}

fs_legacy_upgrade_status_t fs_legacy_upgrade_status(void) {
    return g_legacy_upgrade_status;
}

const char* fs_legacy_upgrade_status_text(void) {
    switch (g_legacy_upgrade_status) {
        case FS_LEGACY_UPGRADE_AVAILABLE:
            return "Legacy snapshots differ; explicit recovery upgrade available";
        case FS_LEGACY_UPGRADE_OUTCOME_UNCERTAIN:
            return "Legacy upgrade outcome is unverified; reboot before retrying";
        case FS_LEGACY_UPGRADE_UNAVAILABLE:
        default:
            return "No legacy recovery upgrade is available";
    }
}

fs_legacy_upgrade_result_t fs_upgrade_legacy_snapshot(
    const char* confirmation) {
    if (g_legacy_upgrade_status != FS_LEGACY_UPGRADE_AVAILABLE ||
        g_backend_status != FS_BACKEND_VOLATILE_CORRUPT ||
        g_active_slot >= FS_SLOT_COUNT ||
        g_loaded_version >= FS_DISK_VERSION) {
        return FS_LEGACY_UPGRADE_NOT_AVAILABLE;
    }
    if (confirmation == NULL ||
        kstrcmp(confirmation, FS_LEGACY_UPGRADE_CONFIRMATION) != 0) {
        return FS_LEGACY_UPGRADE_CONFIRMATION_REQUIRED;
    }

    enum fs_sync_result result = fs_sync_to_disk_verified(true);
    if (result == FS_SYNC_COMMITTED) {
        g_backend_status = FS_BACKEND_PERSISTENT;
        g_legacy_upgrade_status = FS_LEGACY_UPGRADE_UNAVAILABLE;
        syslog_write(
            "FS: explicitly upgraded recovered legacy snapshot to "
            "verified v4 metadata");
        return FS_LEGACY_UPGRADE_SUCCEEDED;
    }
    if (result == FS_SYNC_UNCERTAIN) {
        g_legacy_upgrade_status = FS_LEGACY_UPGRADE_OUTCOME_UNCERTAIN;
        syslog_write(
            "FS: legacy upgrade outcome could not be verified; reboot required");
        return FS_LEGACY_UPGRADE_UNCERTAIN;
    }

    syslog_write(
        "FS: legacy upgrade failed; selected legacy snapshot remains mounted "
        "read-only");
    return FS_LEGACY_UPGRADE_FAILED;
}

size_t fs_file_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (FILES[i].in_use &&
            !fs_file_is_self_test_artifact(&FILES[i]) &&
            !(g_system_log_is_virtual &&
              fs_name_is_system_log(FILES[i].name))) {
            count++;
        }
    }
    if (g_system_log_is_virtual) count++;
    return count;
}

const struct fs_file* fs_file_at(size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!FILES[i].in_use ||
            fs_file_is_self_test_artifact(&FILES[i]) ||
            (g_system_log_is_virtual &&
             fs_name_is_system_log(FILES[i].name))) {
            continue;
        }
        if (seen == index) {
            return &FILES[i];
        }
        seen++;
    }
    if (g_system_log_is_virtual && seen == index) {
        fs_refresh_system_log();
        return &g_virtual_system_log;
    }
    return NULL;
}

const struct fs_file* fs_find(const char* name) {
    if (fs_name_is_system_log(name) && g_system_log_is_virtual) {
        fs_refresh_system_log();
        return &g_virtual_system_log;
    }
    const struct fs_file* file = fs_find_mutable(name);
    return fs_file_is_self_test_artifact(file) ? NULL : file;
}

bool fs_touch(const char* name) {
    if (fs_name_is_system_log(name)) return false;
    if (!fs_is_valid_name(name) ||
        (fs_name_is_self_test_file(name) && !g_self_test_active)) {
        return false;
    }

    struct fs_file* existing = fs_find_mutable(name);
    if (existing != NULL) return true;

    struct fs_file* slot = fs_allocate_slot();
    if (slot == NULL) return false;

    slot->in_use = true;
    fs_copy_name(slot, name);
    slot->size = 0;
    slot->data[0] = '\0';

    if (!fs_commit_changes()) {
        fs_clear(slot);
        return false;
    }
    return true;
}

bool fs_write_bytes(const char* name, const void* contents, size_t length) {
    if (!fs_is_valid_name(name) ||
        fs_name_is_system_log(name) ||
        (fs_name_is_self_test_file(name) && !g_self_test_active) ||
        (contents == NULL && length != 0) ||
        length >= FS_MAX_FILE_SIZE) {
        return false;
    }

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

    const uint8_t* bytes = (const uint8_t*)contents;
    for (size_t i = 0; i < length; i++) {
        file->data[i] = (char)bytes[i];
    }
    file->data[length] = '\0';
    file->size = length;

    if (!fs_commit_changes()) {
        if (existed) *file = backup;
        else fs_clear(file);
        return false;
    }
    return true;
}

bool fs_write(const char* name, const char* contents) {
    if (contents == NULL) return false;
    return fs_write_bytes(name, contents, kstrlen(contents));
}

bool fs_append(const char* name, const char* contents) {
    if (!fs_is_valid_name(name) || fs_name_is_system_log(name) ||
        (fs_name_is_self_test_file(name) && !g_self_test_active) ||
        contents == NULL) {
        return false;
    }
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

    if (!fs_commit_changes()) {
        if (existed) *file = backup;
        else fs_clear(file);
        return false;
    }
    return true;
}

bool fs_rename(const char* old_name, const char* new_name) {
    if (!fs_is_valid_name(old_name) || !fs_is_valid_name(new_name))
        return false;
    if (fs_name_is_system_log(old_name) ||
        fs_name_is_system_log(new_name)) {
        return false;
    }
    if (fs_name_is_self_test_file(new_name) &&
        !g_self_test_active) {
        return false;
    }

    struct fs_file* file = fs_find_mutable(old_name);
    if (file == NULL) return false;
    if (fs_name_is_self_test_file(old_name) &&
        !g_self_test_active &&
        fs_file_is_self_test_artifact(file)) {
        return false;
    }
    if (kstrcmp(old_name, new_name) == 0) return true;
    if (fs_find_mutable(new_name) != NULL) return false;

    char previous_name[FS_MAX_FILENAME];
    size_t i = 0;
    while (file->name[i] != '\0' && i + 1 < sizeof(previous_name)) {
        previous_name[i] = file->name[i];
        i++;
    }
    previous_name[i] = '\0';
    fs_copy_name(file, new_name);
    if (!fs_commit_changes()) {
        fs_copy_name(file, previous_name);
        return false;
    }
    return true;
}

bool fs_remove(const char* name) {
    if (fs_name_is_system_log(name)) return false;
    struct fs_file* file = fs_find_mutable(name);
    if (file == NULL) return false;
    if (fs_name_is_self_test_file(name) &&
        !g_self_test_active &&
        fs_file_is_self_test_artifact(file)) {
        return false;
    }

    struct fs_file backup = *file;
    fs_clear(file);
    if (!fs_commit_changes()) {
        *file = backup;
        return false;
    }
    return true;
}

static bool fs_self_test_reload(const char* failure_message) {
    enum fs_disk_load_result result = fs_load_from_disk();
    if (result == FS_DISK_LOADED) {
        g_backend_status = FS_BACKEND_PERSISTENT;
        return true;
    }

    if (result == FS_DISK_NO_DRIVE) {
        g_backend_status = FS_BACKEND_VOLATILE_NO_DRIVE;
    } else if (result == FS_DISK_CORRUPT ||
               result == FS_DISK_LOADED_CORRUPT) {
        g_backend_status = FS_BACKEND_VOLATILE_CORRUPT;
    } else {
        g_backend_status = FS_BACKEND_VOLATILE_IO_ERROR;
    }
    fs_clear_self_test_artifacts();
    g_self_test_active = false;
    syslog_write(failure_message);
    return false;
}

static void fs_self_test_fail(const char* message) {
    g_backend_status = FS_BACKEND_VOLATILE_IO_ERROR;
    fs_clear_self_test_artifacts();
    g_self_test_active = false;
    syslog_write(message);
}

static void fs_self_test(void) {
    const char* scratch = "__fs_self_test__";
    const char* renamed = "__fs_self_renamed__";
    g_self_test_active = true;
    fs_clear_self_test_artifacts();
    
    if (!fs_write_bytes(scratch, FS_SELF_TEST_PAYLOAD,
                        sizeof(FS_SELF_TEST_PAYLOAD))) {
        fs_self_test_fail("FS: self-test (binary write) failed");
        return;
    }

    if (!fs_self_test_reload(
            "FS: self-test (binary persistence reload) failed")) {
        return;
    }

    struct fs_file* f = fs_find_mutable(scratch);
    if (!fs_file_is_self_test_artifact(f)) {
        fs_self_test_fail(
            "FS: self-test (persisted binary read) failed");
        return;
    }
    
    if (!fs_rename(scratch, renamed) ||
        fs_find_mutable(scratch) != NULL) {
        fs_self_test_fail("FS: self-test (rename) failed");
        return;
    }

    if (!fs_self_test_reload(
            "FS: self-test (rename persistence reload) failed")) {
        return;
    }

    if (fs_find_mutable(scratch) != NULL) {
        fs_self_test_fail("FS: self-test (old name persisted) failed");
        return;
    }

    f = fs_find_mutable(renamed);
    if (!fs_file_is_self_test_artifact(f)) {
        fs_self_test_fail("FS: self-test (renamed read) failed");
        return;
    }

    if (!fs_remove(renamed)) {
        fs_self_test_fail("FS: self-test (remove) failed");
        return;
    }

    if (!fs_self_test_reload(
            "FS: self-test (remove persistence reload) failed")) {
        return;
    }
    if (fs_find_mutable(scratch) != NULL ||
        fs_find_mutable(renamed) != NULL) {
        fs_self_test_fail("FS: self-test (persisted remove) failed");
        return;
    }

    g_self_test_active = false;
    syslog_write("FS: persistent reload sequence complete");
}
