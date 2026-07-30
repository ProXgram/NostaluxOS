#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_VENDOR_ID_REALTEK 0x10ECu
#define PCI_DEVICE_ID_RTL8139 0x8139u

#define PCI_COMMAND_IO_SPACE      (1u << 0)
#define PCI_COMMAND_MEMORY_SPACE  (1u << 1)
#define PCI_COMMAND_BUS_MASTER    (1u << 2)
#define PCI_COMMAND_INTX_DISABLE  (1u << 10)

#define PCI_MAX_BARS 6u

struct pci_location {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct pci_device {
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision;
    uint8_t programming_interface;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint32_t bar[PCI_MAX_BARS];
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
};

enum pci_bar_kind {
    PCI_BAR_NONE = 0,
    PCI_BAR_IO,
    PCI_BAR_MEMORY32,
    PCI_BAR_MEMORY64,
};

struct pci_bar {
    enum pci_bar_kind kind;
    uint64_t address;
    uint8_t index;
    bool prefetchable;
};

/*
 * A configuration-space seam for host tests and alternate PCI mechanisms.
 * Offsets passed to these callbacks are always DWORD aligned and below 256.
 */
struct pci_config_access {
    void* context;
    uint32_t (*read32)(void* context,
                       struct pci_location location,
                       uint8_t aligned_offset);
    void (*write32)(void* context,
                    struct pci_location location,
                    uint8_t aligned_offset,
                    uint32_t value);
};

/*
 * Return true from a visitor to continue scanning or false to stop early.
 * The return value is the number of present functions delivered to visitor.
 */
typedef bool (*pci_device_visitor_t)(const struct pci_device* device,
                                     void* context);

uint8_t pci_config_read8(struct pci_location location, uint8_t offset);
uint16_t pci_config_read16(struct pci_location location, uint8_t offset);
uint32_t pci_config_read32(struct pci_location location, uint8_t offset);
void pci_config_write8(struct pci_location location,
                       uint8_t offset,
                       uint8_t value);
void pci_config_write16(struct pci_location location,
                        uint8_t offset,
                        uint16_t value);
void pci_config_write32(struct pci_location location,
                        uint8_t offset,
                        uint32_t value);

size_t pci_scan(pci_device_visitor_t visitor, void* context);
size_t pci_scan_with_access(const struct pci_config_access* access,
                            pci_device_visitor_t visitor,
                            void* context);

bool pci_find_device(uint16_t vendor_id,
                     uint16_t device_id,
                     struct pci_device* out_device);
bool pci_find_device_with_access(const struct pci_config_access* access,
                                 uint16_t vendor_id,
                                 uint16_t device_id,
                                 struct pci_device* out_device);

/*
 * Sets command-register bits and verifies that the requested capabilities
 * became enabled. Existing command bits are preserved.
 */
bool pci_enable_command_bits(struct pci_location location, uint16_t bits);

/*
 * Decodes an already assigned BAR without destructively sizing it. A 64-bit
 * BAR consumes its following slot; requesting that upper slot is rejected.
 */
bool pci_decode_bar(const struct pci_device* device,
                    uint8_t index,
                    struct pci_bar* out_bar);

#endif /* PCI_H */
