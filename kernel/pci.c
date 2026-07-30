#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CONFIG_ADDRESS_PORT 0x0CF8u
#define PCI_CONFIG_DATA_PORT    0x0CFCu
#define PCI_CONFIG_ENABLE       0x80000000u

#define PCI_VENDOR_DEVICE_OFFSET 0x00u
#define PCI_COMMAND_STATUS_OFFSET 0x04u
#define PCI_CLASS_REVISION_OFFSET 0x08u
#define PCI_HEADER_OFFSET 0x0Cu
#define PCI_BAR_OFFSET 0x10u
#define PCI_SUBSYSTEM_OFFSET 0x2Cu
#define PCI_INTERRUPT_OFFSET 0x3Cu

#define PCI_HEADER_MULTIFUNCTION 0x80u
#define PCI_HEADER_LAYOUT_MASK   0x7Fu
#define PCI_HEADER_LAYOUT_DEVICE 0x00u
#define PCI_HEADER_LAYOUT_BRIDGE 0x01u

static uint64_t pci_irq_save(void) {
#if defined(__x86_64__)
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
#else
    return 0;
#endif
}

static void pci_irq_restore(uint64_t flags) {
#if defined(__x86_64__)
    if ((flags & (1ull << 9)) != 0) {
        __asm__ volatile("sti" ::: "memory");
    }
#else
    (void)flags;
#endif
}

static void pci_out32(uint16_t port, uint32_t value) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("outl %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
#else
    (void)port;
    (void)value;
#endif
}

static void pci_out16(uint16_t port, uint16_t value) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("outw %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
#else
    (void)port;
    (void)value;
#endif
}

static void pci_out8(uint16_t port, uint8_t value) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
#else
    (void)port;
    (void)value;
#endif
}

static uint32_t pci_in32(uint16_t port) {
#if defined(__i386__) || defined(__x86_64__)
    uint32_t value;
    __asm__ volatile("inl %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
#else
    (void)port;
    return UINT32_MAX;
#endif
}

static bool pci_location_valid(struct pci_location location) {
    return location.device < 32u && location.function < 8u;
}

static uint32_t pci_config_address(struct pci_location location,
                                   uint8_t aligned_offset) {
    return PCI_CONFIG_ENABLE |
           ((uint32_t)location.bus << 16) |
           ((uint32_t)location.device << 11) |
           ((uint32_t)location.function << 8) |
           ((uint32_t)aligned_offset & 0xFCu);
}

static uint32_t pci_port_read32(void* context,
                                struct pci_location location,
                                uint8_t aligned_offset) {
    (void)context;
    if (!pci_location_valid(location)) return UINT32_MAX;

    const uint64_t flags = pci_irq_save();
    pci_out32(PCI_CONFIG_ADDRESS_PORT,
              pci_config_address(location, aligned_offset));
    const uint32_t value = pci_in32(PCI_CONFIG_DATA_PORT);
    pci_irq_restore(flags);
    return value;
}

static void pci_port_write32(void* context,
                             struct pci_location location,
                             uint8_t aligned_offset,
                             uint32_t value) {
    (void)context;
    if (!pci_location_valid(location)) return;

    const uint64_t flags = pci_irq_save();
    pci_out32(PCI_CONFIG_ADDRESS_PORT,
              pci_config_address(location, aligned_offset));
    pci_out32(PCI_CONFIG_DATA_PORT, value);
    pci_irq_restore(flags);
}

static const struct pci_config_access g_port_access = {
    .context = NULL,
    .read32 = pci_port_read32,
    .write32 = pci_port_write32,
};

static bool pci_access_valid(const struct pci_config_access* access) {
    return access != NULL && access->read32 != NULL;
}

static uint32_t pci_access_read32(const struct pci_config_access* access,
                                  struct pci_location location,
                                  uint8_t offset) {
    if (!pci_access_valid(access) || !pci_location_valid(location)) {
        return UINT32_MAX;
    }
    return access->read32(access->context, location,
                          (uint8_t)(offset & 0xFCu));
}

uint32_t pci_config_read32(struct pci_location location, uint8_t offset) {
    if ((offset & 3u) != 0u) return UINT32_MAX;
    return pci_access_read32(&g_port_access, location, offset);
}

uint16_t pci_config_read16(struct pci_location location, uint8_t offset) {
    if ((offset & 1u) != 0u) return UINT16_MAX;
    const uint32_t value =
        pci_config_read32(location, (uint8_t)(offset & 0xFCu));
    const uint8_t shift = (uint8_t)((offset & 2u) * 8u);
    return (uint16_t)(value >> shift);
}

uint8_t pci_config_read8(struct pci_location location, uint8_t offset) {
    const uint32_t value =
        pci_config_read32(location, (uint8_t)(offset & 0xFCu));
    const uint8_t shift = (uint8_t)((offset & 3u) * 8u);
    return (uint8_t)(value >> shift);
}

void pci_config_write32(struct pci_location location,
                        uint8_t offset,
                        uint32_t value) {
    if ((offset & 3u) != 0u || !pci_location_valid(location)) return;
    g_port_access.write32(g_port_access.context, location, offset, value);
}

void pci_config_write16(struct pci_location location,
                        uint8_t offset,
                        uint16_t value) {
    if ((offset & 1u) != 0u || !pci_location_valid(location)) return;
    const uint8_t aligned = (uint8_t)(offset & 0xFCu);
    const uint64_t flags = pci_irq_save();
    pci_out32(PCI_CONFIG_ADDRESS_PORT,
              pci_config_address(location, aligned));
    /*
     * Use the native word lane instead of a DWORD read/modify/write. In
     * particular, updating PCI COMMAND at 0x04 must not write ones back to
     * the adjacent write-one-to-clear STATUS register at 0x06.
     */
    pci_out16((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 2u)), value);
    pci_irq_restore(flags);
}

void pci_config_write8(struct pci_location location,
                       uint8_t offset,
                       uint8_t value) {
    if (!pci_location_valid(location)) return;
    const uint8_t aligned = (uint8_t)(offset & 0xFCu);
    const uint64_t flags = pci_irq_save();
    pci_out32(PCI_CONFIG_ADDRESS_PORT,
              pci_config_address(location, aligned));
    pci_out8((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 3u)), value);
    pci_irq_restore(flags);
}

static void pci_read_device(const struct pci_config_access* access,
                            struct pci_location location,
                            struct pci_device* out_device) {
    const uint32_t identity =
        pci_access_read32(access, location, PCI_VENDOR_DEVICE_OFFSET);
    const uint32_t command =
        pci_access_read32(access, location, PCI_COMMAND_STATUS_OFFSET);
    const uint32_t class_revision =
        pci_access_read32(access, location, PCI_CLASS_REVISION_OFFSET);
    const uint32_t header =
        pci_access_read32(access, location, PCI_HEADER_OFFSET);

    out_device->location = location;
    out_device->vendor_id = (uint16_t)identity;
    out_device->device_id = (uint16_t)(identity >> 16);
    out_device->command = (uint16_t)command;
    out_device->status = (uint16_t)(command >> 16);
    out_device->revision = (uint8_t)class_revision;
    out_device->programming_interface =
        (uint8_t)(class_revision >> 8);
    out_device->subclass = (uint8_t)(class_revision >> 16);
    out_device->class_code = (uint8_t)(class_revision >> 24);
    out_device->header_type = (uint8_t)(header >> 16);

    for (size_t index = 0; index < PCI_MAX_BARS; index++) {
        out_device->bar[index] = 0;
    }
    out_device->subsystem_vendor_id = 0;
    out_device->subsystem_id = 0;
    out_device->interrupt_line = 0xFFu;
    out_device->interrupt_pin = 0;

    const uint8_t layout =
        (uint8_t)(out_device->header_type & PCI_HEADER_LAYOUT_MASK);
    size_t bar_count = 0;
    if (layout == PCI_HEADER_LAYOUT_DEVICE) {
        bar_count = 6;
    } else if (layout == PCI_HEADER_LAYOUT_BRIDGE) {
        bar_count = 2;
    }
    for (size_t index = 0; index < bar_count; index++) {
        out_device->bar[index] =
            pci_access_read32(access, location,
                              (uint8_t)(PCI_BAR_OFFSET + index * 4u));
    }

    if (layout == PCI_HEADER_LAYOUT_DEVICE) {
        const uint32_t subsystem =
            pci_access_read32(access, location, PCI_SUBSYSTEM_OFFSET);
        out_device->subsystem_vendor_id = (uint16_t)subsystem;
        out_device->subsystem_id = (uint16_t)(subsystem >> 16);
    }

    const uint32_t interrupt =
        pci_access_read32(access, location, PCI_INTERRUPT_OFFSET);
    out_device->interrupt_line = (uint8_t)interrupt;
    out_device->interrupt_pin = (uint8_t)(interrupt >> 8);
}

static size_t pci_visit_function(const struct pci_config_access* access,
                                 struct pci_location location,
                                 pci_device_visitor_t visitor,
                                 void* context,
                                 bool* keep_scanning) {
    const uint32_t identity =
        pci_access_read32(access, location, PCI_VENDOR_DEVICE_OFFSET);
    if ((uint16_t)identity == 0xFFFFu) return 0;

    struct pci_device device;
    pci_read_device(access, location, &device);
    if (!visitor(&device, context)) *keep_scanning = false;
    return 1;
}

size_t pci_scan_with_access(const struct pci_config_access* access,
                            pci_device_visitor_t visitor,
                            void* context) {
    if (!pci_access_valid(access) || visitor == NULL) return 0;

    size_t visited = 0;
    bool keep_scanning = true;
    for (uint16_t bus = 0; bus < 256u && keep_scanning; bus++) {
        for (uint8_t slot = 0; slot < 32u && keep_scanning; slot++) {
            const struct pci_location function_zero = {
                .bus = (uint8_t)bus,
                .device = slot,
                .function = 0,
            };
            const uint32_t identity =
                pci_access_read32(access, function_zero,
                                  PCI_VENDOR_DEVICE_OFFSET);
            if ((uint16_t)identity == 0xFFFFu) continue;

            visited += pci_visit_function(access, function_zero, visitor,
                                          context, &keep_scanning);
            if (!keep_scanning) break;

            const uint32_t header =
                pci_access_read32(access, function_zero, PCI_HEADER_OFFSET);
            const uint8_t header_type = (uint8_t)(header >> 16);
            if ((header_type & PCI_HEADER_MULTIFUNCTION) == 0) continue;

            for (uint8_t function = 1;
                 function < 8u && keep_scanning;
                 function++) {
                const struct pci_location location = {
                    .bus = (uint8_t)bus,
                    .device = slot,
                    .function = function,
                };
                visited += pci_visit_function(access, location, visitor,
                                              context, &keep_scanning);
            }
        }
    }
    return visited;
}

size_t pci_scan(pci_device_visitor_t visitor, void* context) {
    return pci_scan_with_access(&g_port_access, visitor, context);
}

struct pci_find_context {
    uint16_t vendor_id;
    uint16_t device_id;
    struct pci_device* out_device;
    bool found;
};

static bool pci_find_visitor(const struct pci_device* device, void* context) {
    struct pci_find_context* find = (struct pci_find_context*)context;
    if (device->vendor_id != find->vendor_id ||
        device->device_id != find->device_id) {
        return true;
    }
    *find->out_device = *device;
    find->found = true;
    return false;
}

bool pci_find_device_with_access(const struct pci_config_access* access,
                                 uint16_t vendor_id,
                                 uint16_t device_id,
                                 struct pci_device* out_device) {
    if (out_device == NULL || !pci_access_valid(access) ||
        vendor_id == 0xFFFFu) {
        return false;
    }
    struct pci_find_context context = {
        .vendor_id = vendor_id,
        .device_id = device_id,
        .out_device = out_device,
        .found = false,
    };
    (void)pci_scan_with_access(access, pci_find_visitor, &context);
    return context.found;
}

bool pci_find_device(uint16_t vendor_id,
                     uint16_t device_id,
                     struct pci_device* out_device) {
    return pci_find_device_with_access(&g_port_access, vendor_id, device_id,
                                       out_device);
}

bool pci_enable_command_bits(struct pci_location location, uint16_t bits) {
    if (!pci_location_valid(location)) return false;
    const uint16_t vendor = pci_config_read16(location, 0);
    if (vendor == 0xFFFFu) return false;

    const uint16_t command = pci_config_read16(location, 4);
    pci_config_write16(location, 4, (uint16_t)(command | bits));
    return (pci_config_read16(location, 4) & bits) == bits;
}

bool pci_decode_bar(const struct pci_device* device,
                    uint8_t index,
                    struct pci_bar* out_bar) {
    if (device == NULL || out_bar == NULL || index >= PCI_MAX_BARS) {
        return false;
    }

    const uint8_t layout =
        (uint8_t)(device->header_type & PCI_HEADER_LAYOUT_MASK);
    const uint8_t bar_count =
        layout == PCI_HEADER_LAYOUT_DEVICE
            ? 6u
            : (layout == PCI_HEADER_LAYOUT_BRIDGE ? 2u : 0u);
    if (index >= bar_count) return false;

    if (index > 0) {
        const uint32_t previous = device->bar[index - 1u];
        if ((previous & 1u) == 0u &&
            ((previous >> 1) & 3u) == 2u) {
            return false;
        }
    }

    const uint32_t raw = device->bar[index];
    if (raw == 0u || raw == UINT32_MAX) return false;

    struct pci_bar decoded = {
        .kind = PCI_BAR_NONE,
        .address = 0,
        .index = index,
        .prefetchable = false,
    };
    if ((raw & 1u) != 0u) {
        decoded.kind = PCI_BAR_IO;
        decoded.address = (uint64_t)(raw & ~3u);
    } else {
        const uint8_t memory_type = (uint8_t)((raw >> 1) & 3u);
        decoded.prefetchable = (raw & (1u << 3)) != 0u;
        decoded.address = (uint64_t)(raw & ~0xFu);
        if (memory_type == 0u || memory_type == 1u) {
            decoded.kind = PCI_BAR_MEMORY32;
        } else if (memory_type == 2u && index + 1u < bar_count) {
            decoded.kind = PCI_BAR_MEMORY64;
            decoded.address |=
                (uint64_t)device->bar[index + 1u] << 32;
        } else {
            return false;
        }
    }
    if (decoded.address == 0) return false;
    *out_bar = decoded;
    return true;
}
