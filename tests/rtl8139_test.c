#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "interrupts.h"
#include "rtl8139.h"

#define TEST_IO_BASE 0xC000u
#define TEST_COMMAND 0x37u
#define TEST_CAPR 0x38u
#define TEST_ISR 0x3Eu
#define TEST_BMSR 0x64u
#define TEST_TSD0 0x10u
#define TEST_TSAD0 0x20u
#define TEST_BUFFER_EMPTY 0x01u
#define TEST_TX_OWN (1u << 13)
#define TEST_TX_OK (1u << 15)
#define TEST_ISR_RX_OK 0x0001u
#define TEST_ISR_TX_OK 0x0004u
#define TEST_ISR_RX_OVERFLOW 0x0010u
#define TEST_ISR_RX_MASK 0x0053u

struct fake_dma_mapping {
    const void* virtual_address;
    size_t length;
    uint32_t bus_address;
};

struct fake_rtl {
    uint8_t registers[256];
    struct fake_dma_mapping mappings[8];
    size_t mapping_count;
    size_t write_barriers;
    size_t read_barriers;
    size_t last_tx_length;
    size_t rx_consumer;
    bool unaligned_dma;
    rtl8139_irq_handler_t irq_handler;
    void* irq_handler_context;
    uint8_t irq_line;
    bool irq_registered;
    bool irq_unmasked;
    bool irq_registration_fails;
    size_t irq_save_calls;
    size_t irq_restore_calls;
};

static void require(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

void syslog_write(const char* message) {
    (void)message;
}

bool interrupts_register_irq(uint8_t irq,
                             irq_registry_handler_t handler,
                             void* context) {
    (void)irq;
    (void)handler;
    (void)context;
    return false;
}

bool interrupts_unregister_irq(uint8_t irq,
                               irq_registry_handler_t handler,
                               void* context) {
    (void)irq;
    (void)handler;
    (void)context;
    return false;
}

void interrupts_enable_irq(uint8_t irq) {
    (void)irq;
}

void interrupts_disable_irq(uint8_t irq) {
    (void)irq;
}

static size_t register_offset(uint16_t port) {
    if (port < TEST_IO_BASE || port >= TEST_IO_BASE + 256u) {
        fprintf(stderr, "FAIL: invalid fake I/O port 0x%04x\n", port);
        exit(1);
    }
    return (size_t)(port - TEST_IO_BASE);
}

static uint16_t load16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t load32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void store16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void store32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint8_t fake_read8(void* opaque, uint16_t port) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    return fake->registers[register_offset(port)];
}

static uint16_t fake_read16(void* opaque, uint16_t port) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    return load16(fake->registers + register_offset(port));
}

static uint32_t fake_read32(void* opaque, uint16_t port) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    return load32(fake->registers + register_offset(port));
}

static void fake_write8(void* opaque, uint16_t port, uint8_t value) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    const size_t offset = register_offset(port);
    if (offset == TEST_COMMAND && (value & 0x10u) != 0u) {
        fake->registers[offset] = 0;
        for (size_t index = 0; index < 4; index++) {
            store32(fake->registers + TEST_TSD0 + index * 4u,
                    TEST_TX_OWN);
        }
        return;
    }
    fake->registers[offset] = value;
}

static void fake_write16(void* opaque, uint16_t port, uint16_t value) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    const size_t offset = register_offset(port);
    if (offset == TEST_ISR) {
        const uint16_t current = load16(fake->registers + offset);
        store16(fake->registers + offset, (uint16_t)(current & ~value));
        return;
    }
    store16(fake->registers + offset, value);
    if (offset == TEST_CAPR) {
        fake->rx_consumer = ((size_t)value + 16u) & 0x1FFFu;
        fake->registers[TEST_COMMAND] |= TEST_BUFFER_EMPTY;
    }
}

static void fake_write32(void* opaque, uint16_t port, uint32_t value) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    const size_t offset = register_offset(port);
    if (offset >= TEST_TSD0 && offset < TEST_TSD0 + 16u) {
        fake->last_tx_length = value & 0x1FFFu;
        store32(fake->registers + offset,
                value | TEST_TX_OWN | TEST_TX_OK);
        const uint16_t isr =
            (uint16_t)(load16(fake->registers + TEST_ISR) |
                       TEST_ISR_TX_OK);
        store16(fake->registers + TEST_ISR, isr);
        return;
    }
    store32(fake->registers + offset, value);
}

static bool fake_dma_map(void* opaque,
                         const void* virtual_address,
                         size_t length,
                         uint32_t* out_bus_address) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    for (size_t index = 0; index < fake->mapping_count; index++) {
        if (fake->mappings[index].virtual_address == virtual_address &&
            fake->mappings[index].length == length) {
            *out_bus_address = fake->mappings[index].bus_address;
            return true;
        }
    }
    if (fake->mapping_count >= 8) return false;
    struct fake_dma_mapping* mapping =
        &fake->mappings[fake->mapping_count++];
    mapping->virtual_address = virtual_address;
    mapping->length = length;
    mapping->bus_address =
        (uint32_t)(0x00100000u +
                   (uint32_t)(fake->mapping_count - 1u) * 0x10000u);
    if (fake->unaligned_dma) mapping->bus_address++;
    *out_bus_address = mapping->bus_address;
    return true;
}

static void fake_write_barrier(void* opaque) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    fake->write_barriers++;
}

static void fake_read_barrier(void* opaque) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    fake->read_barriers++;
}

static bool fake_irq_register(void* opaque,
                              uint8_t irq,
                              rtl8139_irq_handler_t handler,
                              void* handler_context) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    if (fake->irq_registration_fails ||
        fake->irq_registered || handler == NULL) {
        return false;
    }
    fake->irq_handler = handler;
    fake->irq_handler_context = handler_context;
    fake->irq_line = irq;
    fake->irq_registered = true;
    return true;
}

static bool fake_irq_unregister(void* opaque,
                                uint8_t irq,
                                rtl8139_irq_handler_t handler,
                                void* handler_context) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    if (!fake->irq_registered || fake->irq_line != irq ||
        fake->irq_handler != handler ||
        fake->irq_handler_context != handler_context) {
        return false;
    }
    fake->irq_handler = NULL;
    fake->irq_handler_context = NULL;
    fake->irq_registered = false;
    fake->irq_unmasked = false;
    return true;
}

static void fake_irq_unmask(void* opaque, uint8_t irq) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    require(fake->irq_registered && fake->irq_line == irq,
            "PIC line should unmask only after handler registration");
    fake->irq_unmasked = true;
}

static uint64_t fake_irq_save(void* opaque) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    fake->irq_save_calls++;
    return 0x202u;
}

static void fake_irq_restore(void* opaque, uint64_t saved_state) {
    struct fake_rtl* fake = (struct fake_rtl*)opaque;
    require(saved_state == 0x202u,
            "deferred collector should restore the saved IRQ state");
    fake->irq_restore_calls++;
}

static void initialize_fake(struct fake_rtl* fake) {
    for (size_t index = 0; index < sizeof(fake->registers); index++) {
        fake->registers[index] = 0;
    }
    fake->mapping_count = 0;
    fake->write_barriers = 0;
    fake->read_barriers = 0;
    fake->last_tx_length = 0;
    fake->rx_consumer = 0;
    fake->unaligned_dma = false;
    fake->irq_handler = NULL;
    fake->irq_handler_context = NULL;
    fake->irq_line = 0xFFu;
    fake->irq_registered = false;
    fake->irq_unmasked = false;
    fake->irq_registration_fails = false;
    fake->irq_save_calls = 0;
    fake->irq_restore_calls = 0;

    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    for (size_t index = 0; index < 6; index++) {
        fake->registers[index] = mac[index];
    }
    store16(fake->registers + TEST_BMSR, 0x0004u);
}

static struct rtl8139_platform fake_platform(struct fake_rtl* fake) {
    const struct rtl8139_platform platform = {
        .context = fake,
        .read8 = fake_read8,
        .read16 = fake_read16,
        .read32 = fake_read32,
        .write8 = fake_write8,
        .write16 = fake_write16,
        .write32 = fake_write32,
        .dma_map = fake_dma_map,
        .dma_write_barrier = fake_write_barrier,
        .dma_read_barrier = fake_read_barrier,
    };
    return platform;
}

static struct rtl8139_platform fake_interrupt_platform(
    struct fake_rtl* fake) {
    struct rtl8139_platform platform = fake_platform(fake);
    platform.irq_register = fake_irq_register;
    platform.irq_unregister = fake_irq_unregister;
    platform.irq_unmask = fake_irq_unmask;
    platform.irq_save = fake_irq_save;
    platform.irq_restore = fake_irq_restore;
    return platform;
}

static struct pci_device fake_device(void) {
    struct pci_device device = {
        .location = {.bus = 0, .device = 3, .function = 0},
        .vendor_id = PCI_VENDOR_ID_REALTEK,
        .device_id = PCI_DEVICE_ID_RTL8139,
        .header_type = 0,
        .bar = {0x0000C001u, 0, 0, 0, 0, 0},
    };
    return device;
}

static bool fire_fake_irq(struct fake_rtl* fake) {
    if (!fake->irq_registered || !fake->irq_unmasked ||
        fake->irq_handler == NULL) {
        return false;
    }
    const uint16_t pending =
        (uint16_t)(load16(fake->registers + TEST_ISR) &
                   load16(fake->registers + 0x3Cu));
    if (pending == 0u) return false;
    return fake->irq_handler(
        fake->irq_line, fake->irq_handler_context);
}

static uint8_t* receive_buffer(struct fake_rtl* fake) {
    require(fake->mapping_count == 5,
            "initialization should map one RX and four TX buffers");
    return (uint8_t*)(uintptr_t)fake->mappings[0].virtual_address;
}

static void inject_packet(struct fake_rtl* fake,
                          const uint8_t* frame,
                          size_t frame_length) {
    uint8_t* ring = receive_buffer(fake);
    uint8_t* packet = ring + fake->rx_consumer;
    store16(packet, 0x0001u);
    store16(packet + 2, (uint16_t)(frame_length + 4u));
    for (size_t index = 0; index < frame_length; index++) {
        packet[4u + index] = frame[index];
    }
    fake->registers[TEST_COMMAND] &= (uint8_t)~TEST_BUFFER_EMPTY;
    store16(fake->registers + TEST_ISR, TEST_ISR_RX_OK);
}

int main(void) {
    struct fake_rtl fake;
    initialize_fake(&fake);
    struct rtl8139_platform platform = fake_platform(&fake);
    struct pci_device device = fake_device();

    require(rtl8139_init_device(&device, &platform) ==
                RTL8139_INIT_READY,
            "injected RTL8139 should initialize");
    require(fake.mapping_count == 5,
            "driver should map the RX ring and all four TX buffers");
    require(load32(fake.registers + 0x30) ==
                fake.mappings[0].bus_address,
            "RBSTART must receive a DMA bus address, not an arbitrary pointer");
    for (size_t index = 0; index < 4; index++) {
        require(load32(fake.registers + TEST_TSAD0 + index * 4u) ==
                    fake.mappings[index + 1u].bus_address,
                "each TSAD must receive its stable DMA bus address");
    }

    struct rtl8139_status status;
    require(rtl8139_get_status(&status) && status.ready &&
                status.present && status.link_up,
            "status should publish readiness, presence, and physical link");
    require(status.mac[0] == 0x52 && status.mac[5] == 0x56,
            "status should publish the hardware MAC address");

    uint8_t short_frame[14];
    for (size_t index = 0; index < sizeof(short_frame); index++) {
        short_frame[index] = (uint8_t)(0xA0u + index);
    }
    require(rtl8139_send(short_frame, sizeof(short_frame)) ==
                RTL8139_TX_QUEUED,
            "valid frame should queue");
    require(fake.last_tx_length == RTL8139_MIN_FRAME_SIZE,
            "short Ethernet frames should be zero-padded to 60 bytes");
    const uint8_t* first_tx =
        (const uint8_t*)fake.mappings[1].virtual_address;
    for (size_t index = 0; index < sizeof(short_frame); index++) {
        require(first_tx[index] == short_frame[index],
                "TX DMA buffer should contain the caller's frame");
    }
    for (size_t index = sizeof(short_frame);
         index < RTL8139_MIN_FRAME_SIZE;
         index++) {
        require(first_tx[index] == 0,
                "TX padding must not expose stale kernel memory");
    }
    rtl8139_poll();
    require(rtl8139_get_status(&status) && status.tx_packets == 1 &&
                status.tx_bytes == sizeof(short_frame),
            "poll should account for a completed transmit");

    uint8_t frame[64];
    for (size_t index = 0; index < sizeof(frame); index++) {
        frame[index] = (uint8_t)(index ^ 0x5Au);
    }
    fake.registers[TEST_COMMAND] |= TEST_BUFFER_EMPTY;
    inject_packet(&fake, frame, sizeof(frame));
    uint8_t output[64];
    size_t output_length = 0;
    require(rtl8139_receive(output, sizeof(output), &output_length) ==
                RTL8139_RX_RECEIVED,
            "complete receive-ring entry should be returned");
    require(output_length == sizeof(frame),
            "receive should exclude the controller-appended CRC");
    for (size_t index = 0; index < sizeof(frame); index++) {
        require(output[index] == frame[index],
                "received frame bytes should remain intact");
    }
    require(rtl8139_get_status(&status) && status.rx_packets == 1 &&
                status.rx_bytes == sizeof(frame),
            "receive counters should reflect delivered data");

    inject_packet(&fake, frame, sizeof(frame));
    output_length = 0;
    require(rtl8139_receive(output, 8, &output_length) ==
                RTL8139_RX_BUFFER_TOO_SMALL,
            "undersized caller buffer should be reported");
    require(output_length == sizeof(frame),
            "buffer-too-small result should publish required capacity");
    require(rtl8139_get_status(&status) && status.rx_dropped == 1,
            "discarded oversized delivery should be counted");

    store16(fake.registers + TEST_ISR, TEST_ISR_RX_OVERFLOW);
    require(rtl8139_receive(output, sizeof(output), &output_length) ==
                RTL8139_RX_ERROR,
            "receive overflow should be surfaced and recovered");
    require(rtl8139_get_status(&status) && status.ready &&
                status.rx_overflows == 1,
            "overflow recovery should keep the device ready and count loss");

    require(fake.write_barriers > 0 && fake.read_barriers > 0,
            "DMA ownership changes must pass through ordering barriers");
    require(rtl8139_send(NULL, 64) == RTL8139_TX_INVALID_FRAME,
            "invalid frame pointers should be rejected");

    const uint8_t reserved_irq_lines[] = {
        0u, 1u, 2u, 12u, 16u, 0xFFu
    };
    struct fake_rtl reserved_irq_fakes[
        sizeof(reserved_irq_lines) /
        sizeof(reserved_irq_lines[0])];
    for (size_t index = 0;
         index < sizeof(reserved_irq_lines) /
                     sizeof(reserved_irq_lines[0]);
         index++) {
        initialize_fake(&reserved_irq_fakes[index]);
        struct rtl8139_platform reserved_platform =
            fake_interrupt_platform(&reserved_irq_fakes[index]);
        struct pci_device reserved_device = fake_device();
        reserved_device.interrupt_line =
            reserved_irq_lines[index];

        require(rtl8139_init_device(
                    &reserved_device,
                    &reserved_platform) ==
                    RTL8139_INIT_READY,
                "unsafe PCI IRQ should retain a usable polling NIC");
        require(!reserved_irq_fakes[index].irq_registered &&
                    !reserved_irq_fakes[index].irq_unmasked,
                "unsafe PCI IRQ must never register or unmask");
        require(rtl8139_get_status(&status) &&
                    status.ready &&
                    !status.interrupt_mode &&
                    status.irq_line == 0xFFu,
                "unsafe PCI IRQ should be reported as polling mode");
    }

    struct fake_rtl interrupt_fake;
    initialize_fake(&interrupt_fake);
    struct rtl8139_platform interrupt_platform =
        fake_interrupt_platform(&interrupt_fake);
    struct pci_device interrupt_device = fake_device();
    interrupt_device.interrupt_line = 11;
    require(rtl8139_init_device(
                &interrupt_device, &interrupt_platform) ==
                RTL8139_INIT_READY,
            "valid legacy PCI IRQ should enable deferred interrupt mode");
    require(interrupt_fake.irq_registered &&
                interrupt_fake.irq_unmasked &&
                interrupt_fake.irq_line == 11,
            "handler must register before unmasking the PCI IRQ");
    require(rtl8139_get_status(&status) &&
                status.interrupt_mode && status.irq_line == 11,
            "driver status should distinguish interrupt mode from fallback");

    interrupt_fake.registers[TEST_COMMAND] |= TEST_BUFFER_EMPTY;
    require(rtl8139_send(short_frame, sizeof(short_frame)) ==
                RTL8139_TX_QUEUED,
            "interrupt-mode TX should preserve the public queue API");
    require(fire_fake_irq(&interrupt_fake),
            "RTL8139 TX status should claim its shared IRQ");
    require(load16(interrupt_fake.registers + 0x3Cu) == 0,
            "hard IRQ should mask device causes before returning");
    require(rtl8139_get_status(&status) &&
                status.interrupts == 1 && status.tx_packets == 0,
            "hard IRQ should defer descriptor accounting");
    rtl8139_poll();
    require(rtl8139_get_status(&status) &&
                status.tx_packets == 1 &&
                status.tx_bytes == sizeof(short_frame),
            "foreground poll should reap the deferred transmit");

    inject_packet(&interrupt_fake, frame, sizeof(frame));
    require(fire_fake_irq(&interrupt_fake),
            "RTL8139 RX status should claim its shared IRQ");
    require(rtl8139_get_status(&status) &&
                status.interrupts == 2 && status.rx_packets == 0,
            "hard IRQ must not parse or publish receive data");
    rtl8139_poll();
    require((load16(interrupt_fake.registers + 0x3Cu) &
             TEST_ISR_RX_MASK) == 0,
            "RX causes should remain masked while the ring has data");
    output_length = 0;
    require(rtl8139_receive(output, sizeof(output), &output_length) ==
                RTL8139_RX_RECEIVED &&
                output_length == sizeof(frame),
            "existing receive API should drain deferred RX work");
    require((load16(interrupt_fake.registers + 0x3Cu) &
             TEST_ISR_RX_OK) != 0,
            "draining the ring should rearm receive interrupts");
    require(interrupt_fake.irq_save_calls > 0 &&
                interrupt_fake.irq_save_calls ==
                    interrupt_fake.irq_restore_calls,
            "foreground event handoff should serialize against hard IRQs");

    struct fake_rtl fallback;
    initialize_fake(&fallback);
    fallback.irq_registration_fails = true;
    struct rtl8139_platform fallback_platform =
        fake_interrupt_platform(&fallback);
    require(rtl8139_init_device(
                &interrupt_device, &fallback_platform) ==
                RTL8139_INIT_READY,
            "IRQ registration failure should retain a usable device");
    require(!interrupt_fake.irq_registered,
            "reinitialization should quiesce and unregister the old IRQ");
    require(rtl8139_get_status(&status) &&
                status.ready && !status.interrupt_mode &&
                status.irq_line == 0xFFu &&
                load16(fallback.registers + 0x3Cu) == 0,
            "registration failure should fall back to polling mode");

    struct fake_rtl unaligned;
    initialize_fake(&unaligned);
    unaligned.unaligned_dma = true;
    struct rtl8139_platform unaligned_platform =
        fake_platform(&unaligned);
    require(rtl8139_init_device(&device, &unaligned_platform) ==
                RTL8139_INIT_DMA_UNAVAILABLE,
            "misaligned DMA bus addresses must be rejected");

    puts("RTL8139 polling, deferred IRQ, DMA, TX, RX, and status tests passed.");
    return 0;
}
