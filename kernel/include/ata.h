#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ATA_INIT_ERROR = 0,
    ATA_INIT_READY,
    ATA_INIT_NO_DEVICE,
} ata_init_result_t;

typedef enum {
    ATA_WRITE_FAILED = 0,
    ATA_WRITE_COMPLETE,
    /*
     * At least one payload sector was handed to the controller, but a later
     * transfer status or cache flush failed. The caller must read back before
     * deciding whether the write committed.
     */
    ATA_WRITE_UNCERTAIN,
} ata_write_result_t;

/*
 * Initializes the ATA driver (Primary Bus, Master Drive). Distinguishing an
 * absent device from an initialization/I/O error prevents callers from
 * labelling every controller failure as "no drive".
 */
ata_init_result_t ata_init(void);

/*
 * Reads 'count' sectors starting at LBA 'lba' into 'buffer'.
 * Returns true on success, false on timeout/error.
 */
bool ata_read(uint32_t lba, uint8_t count, uint8_t* buffer);

/*
 * Writes 'count' sectors starting at LBA 'lba' from 'buffer'. Once any sector
 * has been transferred, a later status/flush failure is reported as uncertain
 * so transactional callers can reconcile it with a read-back.
 */
ata_write_result_t ata_write(
    uint32_t lba, uint8_t count, const uint8_t* buffer);

#endif /* ATA_H */
