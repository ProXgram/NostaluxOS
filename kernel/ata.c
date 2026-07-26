#include "ata.h"
#include "io.h"
#include "syslog.h"
#include "kstdio.h"
#include <stddef.h>

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define CMD_READ_PIO    0x20
#define CMD_WRITE_PIO   0x30
#define CMD_IDENTIFY    0xEC
#define CMD_CACHE_FLUSH 0xE7

#define STATUS_BSY      0x80
#define STATUS_DF       0x20
#define STATUS_DRQ      0x08
#define STATUS_ERR      0x01

/*
 * A fresh raw image can make QEMU hold BSY while the host allocates and
 * flushes backing-file pages. The old 100,000-read budget timed out during
 * the filesystem's first multi-sector commit even though the drive was
 * healthy. Keep the wait bounded, but allow normal slow-path I/O to finish.
 */
#define ATA_TIMEOUT     10000000

static bool ata_status_failed(uint8_t status) {
    if (status & STATUS_DF) {
        syslog_write("ATA: Device fault");
        return true;
    }
    if (status & STATUS_ERR) {
        syslog_write("ATA: Command error");
        return true;
    }
    return false;
}

static bool ata_wait_not_busy(void) {
    int timeout = ATA_TIMEOUT;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFF) return false;
        if ((status & STATUS_BSY) == 0) {
            return !ata_status_failed(status);
        }
    }
    syslog_write("ATA: Timeout waiting for BSY to clear");
    return false;
}

static bool ata_wait_drq(void) {
    int timeout = ATA_TIMEOUT;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFF) return false;
        if ((status & STATUS_BSY) == 0) {
            if (ata_status_failed(status)) return false;
            if (status & STATUS_DRQ) return true;
        }
    }
    syslog_write("ATA: Timeout waiting for DRQ to set");
    return false;
}

static void ata_select_drive(void) {
    outb(ATA_DRIVE_HEAD, 0xE0);
    // ATA requires roughly 400 ns after drive selection.
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
}

static void ata_select_lba(uint32_t lba) {
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
    (void)inb(ATA_STATUS);
}

static bool ata_valid_request(uint32_t lba, uint8_t count, const void* buffer) {
    if (buffer == NULL || count == 0) return false;
    uint64_t final_lba = (uint64_t)lba + (uint64_t)count - 1u;
    return final_lba <= 0x0FFFFFFFull;
}

bool ata_init(void) {
    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) {
        // Floating bus, no drive
        return false;
    }

    ata_select_drive();
    io_wait();
    
    outb(ATA_SECTOR_CNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    
    status = inb(ATA_STATUS);
    if (status == 0) return false;

    if (!ata_wait_not_busy()) return false;

    // A non-zero signature here indicates an ATAPI rather than ATA device.
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return false;
    if (!ata_wait_drq()) return false;
    
    // Read Identify data
    uint16_t tmp[256];
    insw(ATA_DATA, tmp, 256);
    return ata_wait_not_busy();
}

bool ata_read(uint32_t lba, uint8_t count, uint8_t* buffer) {
    if (!ata_valid_request(lba, count, buffer)) return false;
    if (!ata_wait_not_busy()) return false;
    
    ata_select_lba(lba);
    outb(ATA_SECTOR_CNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, CMD_READ_PIO);

    for (uint16_t i = 0; i < count; i++) {
        if (!ata_wait_drq()) return false;
        insw(ATA_DATA, buffer + (i * 512), 256);
    }
    return ata_wait_not_busy();
}

bool ata_write(uint32_t lba, uint8_t count, const uint8_t* buffer) {
    if (!ata_valid_request(lba, count, buffer)) return false;
    if (!ata_wait_not_busy()) return false;

    ata_select_lba(lba);
    outb(ATA_SECTOR_CNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, CMD_WRITE_PIO);

    for (uint16_t i = 0; i < count; i++) {
        if (!ata_wait_drq()) return false;
        outsw(ATA_DATA, buffer + (i * 512), 256);
    }

    if (!ata_wait_not_busy()) return false;
    outb(ATA_COMMAND, CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}
