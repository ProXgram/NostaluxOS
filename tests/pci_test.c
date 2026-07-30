#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "pci.h"

struct fake_pci {
    uint32_t rtl[64];
    uint32_t bridge[64];
    uint32_t bridge_function_two[64];
    size_t unexpected_single_function_reads;
};

static void require(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static uint32_t fake_read32(void* opaque,
                            struct pci_location location,
                            uint8_t offset) {
    struct fake_pci* fake = (struct fake_pci*)opaque;
    if (location.bus == 0 && location.device == 3) {
        if (location.function == 0) return fake->rtl[offset / 4u];
        fake->unexpected_single_function_reads++;
        return UINT32_MAX;
    }
    if (location.bus == 0 && location.device == 4) {
        if (location.function == 0) return fake->bridge[offset / 4u];
        if (location.function == 2) {
            return fake->bridge_function_two[offset / 4u];
        }
    }
    return UINT32_MAX;
}

static bool count_visitor(const struct pci_device* device, void* opaque) {
    size_t* count = (size_t*)opaque;
    require(device->vendor_id != 0xFFFFu,
            "scanner must never publish an absent function");
    (*count)++;
    return true;
}

static void initialize_fake(struct fake_pci* fake) {
    for (size_t index = 0; index < 64; index++) {
        fake->rtl[index] = 0;
        fake->bridge[index] = 0;
        fake->bridge_function_two[index] = 0;
    }
    fake->unexpected_single_function_reads = 0;

    fake->rtl[0x00 / 4] =
        ((uint32_t)PCI_DEVICE_ID_RTL8139 << 16) |
        PCI_VENDOR_ID_REALTEK;
    fake->rtl[0x04 / 4] = 0x02B00007u;
    fake->rtl[0x08 / 4] = 0x02000020u;
    fake->rtl[0x0C / 4] = 0x00000000u;
    fake->rtl[0x10 / 4] = 0x0000C001u;
    fake->rtl[0x2C / 4] = 0x813910ECu;
    fake->rtl[0x3C / 4] = 0x0000010Bu;

    fake->bridge[0x00 / 4] = 0x11111234u;
    fake->bridge[0x08 / 4] = 0x06040001u;
    fake->bridge[0x0C / 4] = 0x00810000u;
    fake->bridge[0x10 / 4] = 0xFFF00004u;
    fake->bridge[0x14 / 4] = 0x00000001u;

    fake->bridge_function_two[0x00 / 4] = 0x22221234u;
    fake->bridge_function_two[0x08 / 4] = 0x01010002u;
    fake->bridge_function_two[0x0C / 4] = 0x00000000u;
}

int main(void) {
    struct fake_pci fake;
    initialize_fake(&fake);
    const struct pci_config_access access = {
        .context = &fake,
        .read32 = fake_read32,
        .write32 = NULL,
    };

    size_t callback_count = 0;
    const size_t visited =
        pci_scan_with_access(&access, count_visitor, &callback_count);
    require(visited == 3 && callback_count == 3,
            "scan should enumerate present normal and multifunction devices");
    require(fake.unexpected_single_function_reads == 0,
            "scan must not probe extra functions without multifunction bit");

    struct pci_device rtl;
    require(pci_find_device_with_access(
                &access, PCI_VENDOR_ID_REALTEK, PCI_DEVICE_ID_RTL8139,
                &rtl),
            "find should return the RTL8139 function");
    require(rtl.location.bus == 0 && rtl.location.device == 3 &&
                rtl.location.function == 0,
            "find should preserve bus/device/function coordinates");
    require(rtl.class_code == 0x02 && rtl.subclass == 0x00,
            "device class fields should decode in PCI byte order");
    require(rtl.interrupt_line == 0x0B && rtl.interrupt_pin == 0x01,
            "legacy interrupt fields should decode");

    struct pci_bar bar;
    require(pci_decode_bar(&rtl, 0, &bar),
            "assigned I/O BAR should decode");
    require(bar.kind == PCI_BAR_IO && bar.address == 0xC000u,
            "I/O BAR flags must not leak into its base address");
    require(!pci_decode_bar(&rtl, 1, &bar),
            "an unassigned BAR should be rejected");

    struct pci_device bridge;
    require(pci_find_device_with_access(&access, 0x1234u, 0x1111u,
                                        &bridge),
            "bridge should be discoverable");
    require(pci_decode_bar(&bridge, 0, &bar),
            "64-bit bridge BAR should decode");
    require(bar.kind == PCI_BAR_MEMORY64 &&
                bar.address == 0x00000001FFF00000ull,
            "64-bit BAR should combine both DWORDs");
    require(!pci_decode_bar(&bridge, 1, &bar),
            "upper half of a 64-bit BAR must not decode independently");

    require(!pci_find_device_with_access(&access, 0x9999u, 0x9999u,
                                         &rtl),
            "find should report a missing ID pair");

    puts("PCI discovery and BAR decoding tests passed.");
    return 0;
}
