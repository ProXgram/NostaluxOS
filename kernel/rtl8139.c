#include "rtl8139.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts.h"
#include "syslog.h"

#define RTL_REG_MAC0       0x00u
#define RTL_REG_TSD0       0x10u
#define RTL_REG_TSAD0      0x20u
#define RTL_REG_RBSTART    0x30u
#define RTL_REG_COMMAND    0x37u
#define RTL_REG_CAPR       0x38u
#define RTL_REG_IMR        0x3Cu
#define RTL_REG_ISR        0x3Eu
#define RTL_REG_TCR        0x40u
#define RTL_REG_RCR        0x44u
#define RTL_REG_CONFIG1    0x52u
#define RTL_REG_MEDIA      0x58u
#define RTL_REG_BMSR       0x64u

#define RTL_COMMAND_BUFFER_EMPTY 0x01u
#define RTL_COMMAND_TX_ENABLE    0x04u
#define RTL_COMMAND_RX_ENABLE    0x08u
#define RTL_COMMAND_RESET        0x10u

#define RTL_ISR_RX_OK        0x0001u
#define RTL_ISR_RX_ERROR     0x0002u
#define RTL_ISR_TX_OK        0x0004u
#define RTL_ISR_TX_ERROR     0x0008u
#define RTL_ISR_RX_OVERFLOW  0x0010u
#define RTL_ISR_LINK_CHANGE  0x0020u
#define RTL_ISR_FIFO_OVERFLOW 0x0040u
#define RTL_ISR_SYSTEM_ERROR 0x8000u
#define RTL_ISR_KNOWN_MASK \
    (RTL_ISR_RX_OK | RTL_ISR_RX_ERROR | RTL_ISR_TX_OK | RTL_ISR_TX_ERROR | \
     RTL_ISR_RX_OVERFLOW | RTL_ISR_LINK_CHANGE | RTL_ISR_FIFO_OVERFLOW | \
     RTL_ISR_SYSTEM_ERROR)
#define RTL_ISR_RX_MASK \
    (RTL_ISR_RX_OK | RTL_ISR_RX_ERROR | RTL_ISR_RX_OVERFLOW | \
     RTL_ISR_FIFO_OVERFLOW)
#define RTL_ISR_ENABLED_MASK RTL_ISR_KNOWN_MASK

#define RTL_RCR_ACCEPT_PHYSICAL  (1u << 1)
#define RTL_RCR_ACCEPT_MULTICAST (1u << 2)
#define RTL_RCR_ACCEPT_BROADCAST (1u << 3)
#define RTL_RCR_NO_WRAP          (1u << 7)
#define RTL_RCR_DMA_UNLIMITED    (7u << 8)
#define RTL_RCR_FIFO_NONE        (7u << 13)
#define RTL_RCR_VALUE \
    (RTL_RCR_ACCEPT_PHYSICAL | RTL_RCR_ACCEPT_MULTICAST | \
     RTL_RCR_ACCEPT_BROADCAST | RTL_RCR_NO_WRAP | \
     RTL_RCR_DMA_UNLIMITED | RTL_RCR_FIFO_NONE)

#define RTL_TCR_DMA_UNLIMITED (7u << 8)
#define RTL_TCR_IFG_96        (3u << 24)
#define RTL_TCR_VALUE (RTL_TCR_DMA_UNLIMITED | RTL_TCR_IFG_96)

#define RTL_TSD_HOST_OWNS    (1u << 13)
#define RTL_TSD_UNDERRUN     (1u << 14)
#define RTL_TSD_OK           (1u << 15)
#define RTL_TSD_OUT_OF_WINDOW (1u << 29)
#define RTL_TSD_ABORTED      (1u << 30)
#define RTL_TSD_CARRIER_LOST (1u << 31)
#define RTL_TSD_ERROR_MASK \
    (RTL_TSD_UNDERRUN | RTL_TSD_OUT_OF_WINDOW | RTL_TSD_ABORTED | \
     RTL_TSD_CARRIER_LOST)
#define RTL_TSD_EARLY_THRESHOLD (0x3Fu << 16)

#define RTL_RX_STATUS_OK 0x0001u

#define RTL_MEDIA_LINK_DOWN (1u << 2)
#define RTL_BMSR_LINK_UP     (1u << 2)

#define RTL_TX_DESCRIPTOR_COUNT 4u
#define RTL_TX_BUFFER_SIZE 1536u
#define RTL_RX_RING_SIZE 8192u
#define RTL_RX_RING_GUARD 16u
#define RTL_RX_PACKET_SLACK 2048u
#define RTL_RX_BUFFER_SIZE \
    (RTL_RX_RING_SIZE + RTL_RX_RING_GUARD + RTL_RX_PACKET_SLACK)
#define RTL_RESET_POLL_LIMIT 1000000u

_Static_assert((RTL_RX_RING_SIZE & (RTL_RX_RING_SIZE - 1u)) == 0u,
               "RTL8139 RX ring size must be a power of two");
_Static_assert(RTL_RX_PACKET_SLACK >= RTL8139_MAX_FRAME_SIZE + 4u,
               "RTL8139 RX no-wrap slack must hold a frame and CRC");
_Static_assert(RTL_TX_BUFFER_SIZE >= RTL8139_MAX_FRAME_SIZE,
               "RTL8139 TX buffers must hold the maximum frame");

static uint8_t g_rx_buffer[RTL_RX_BUFFER_SIZE]
    __attribute__((aligned(256)));
static uint8_t
    g_tx_buffer[RTL_TX_DESCRIPTOR_COUNT][RTL_TX_BUFFER_SIZE]
    __attribute__((aligned(256)));

static struct rtl8139_platform g_platform;
static struct rtl8139_status g_status;
static bool g_tx_pending[RTL_TX_DESCRIPTOR_COUNT];
static size_t g_tx_length[RTL_TX_DESCRIPTOR_COUNT];
static uint16_t g_rx_offset;
static uint8_t g_next_tx;
static uint16_t g_deferred_interrupts;
static bool g_interrupt_mode;
static uint8_t g_irq_line;

static bool rtl_interrupt_entry(uint8_t irq, void* context);

static void rtl_cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static void rtl_x86_out8(void* context, uint16_t port, uint8_t value) {
    (void)context;
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

static void rtl_x86_out16(void* context, uint16_t port, uint16_t value) {
    (void)context;
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

static void rtl_x86_out32(void* context, uint16_t port, uint32_t value) {
    (void)context;
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

static uint8_t rtl_x86_in8(void* context, uint16_t port) {
    (void)context;
#if defined(__i386__) || defined(__x86_64__)
    uint8_t value;
    __asm__ volatile("inb %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
#else
    (void)port;
    return UINT8_MAX;
#endif
}

static uint16_t rtl_x86_in16(void* context, uint16_t port) {
    (void)context;
#if defined(__i386__) || defined(__x86_64__)
    uint16_t value;
    __asm__ volatile("inw %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
#else
    (void)port;
    return UINT16_MAX;
#endif
}

static uint32_t rtl_x86_in32(void* context, uint16_t port) {
    (void)context;
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

static bool rtl_identity_dma_map(void* context,
                                 const void* virtual_address,
                                 size_t length,
                                 uint32_t* out_bus_address) {
    (void)context;
    if (virtual_address == NULL || out_bus_address == NULL || length == 0) {
        return false;
    }
    const uintptr_t start = (uintptr_t)virtual_address;
    if (start > UINT32_MAX ||
        length - 1u > (size_t)(UINT32_MAX - start)) {
        return false;
    }

    /*
     * Nostalux's kernel and static DMA buffers use the low identity map.
     * This deliberate seam must be replaced if the kernel moves to a higher
     * half or gains an IOMMU/non-identity DMA mapping.
     */
    *out_bus_address = (uint32_t)start;
    return true;
}

static void rtl_x86_dma_barrier(void* context) {
    (void)context;
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("mfence" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static bool rtl_kernel_irq_register(void* context,
                                    uint8_t irq,
                                    rtl8139_irq_handler_t handler,
                                    void* handler_context) {
    (void)context;
    return interrupts_register_irq(irq, handler, handler_context);
}

static bool rtl_kernel_irq_unregister(void* context,
                                      uint8_t irq,
                                      rtl8139_irq_handler_t handler,
                                      void* handler_context) {
    (void)context;
    return interrupts_unregister_irq(irq, handler, handler_context);
}

static void rtl_kernel_irq_unmask(void* context, uint8_t irq) {
    (void)context;
    interrupts_enable_irq(irq);
}

static uint64_t rtl_kernel_irq_save(void* context) {
    (void)context;
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

static void rtl_kernel_irq_restore(void* context, uint64_t saved_state) {
    (void)context;
#if defined(__x86_64__)
    if ((saved_state & (1ull << 9)) != 0u) {
        __asm__ volatile("sti" ::: "memory");
    }
#else
    (void)saved_state;
#endif
}

static const struct rtl8139_platform g_x86_platform = {
    .context = NULL,
    .read8 = rtl_x86_in8,
    .read16 = rtl_x86_in16,
    .read32 = rtl_x86_in32,
    .write8 = rtl_x86_out8,
    .write16 = rtl_x86_out16,
    .write32 = rtl_x86_out32,
    .dma_map = rtl_identity_dma_map,
    .dma_write_barrier = rtl_x86_dma_barrier,
    .dma_read_barrier = rtl_x86_dma_barrier,
    .irq_register = rtl_kernel_irq_register,
    .irq_unregister = rtl_kernel_irq_unregister,
    .irq_unmask = rtl_kernel_irq_unmask,
    .irq_save = rtl_kernel_irq_save,
    .irq_restore = rtl_kernel_irq_restore,
};

static bool rtl_platform_valid(const struct rtl8139_platform* platform) {
    return platform != NULL &&
           platform->read8 != NULL &&
           platform->read16 != NULL &&
           platform->read32 != NULL &&
           platform->write8 != NULL &&
           platform->write16 != NULL &&
           platform->write32 != NULL &&
           platform->dma_map != NULL &&
           platform->dma_write_barrier != NULL &&
           platform->dma_read_barrier != NULL;
}

static bool rtl_platform_has_interrupts(
    const struct rtl8139_platform* platform) {
    return platform != NULL &&
           platform->irq_register != NULL &&
           platform->irq_unregister != NULL &&
           platform->irq_unmask != NULL &&
           platform->irq_save != NULL &&
           platform->irq_restore != NULL;
}

static uint16_t rtl_port(uint16_t offset) {
    return (uint16_t)(g_status.io_base + offset);
}

static uint8_t rtl_read8(uint16_t offset) {
    return g_platform.read8(g_platform.context, rtl_port(offset));
}

static uint16_t rtl_read16(uint16_t offset) {
    return g_platform.read16(g_platform.context, rtl_port(offset));
}

static uint32_t rtl_read32(uint16_t offset) {
    return g_platform.read32(g_platform.context, rtl_port(offset));
}

static void rtl_write8(uint16_t offset, uint8_t value) {
    g_platform.write8(g_platform.context, rtl_port(offset), value);
}

static void rtl_write16(uint16_t offset, uint16_t value) {
    g_platform.write16(g_platform.context, rtl_port(offset), value);
}

static void rtl_write32(uint16_t offset, uint32_t value) {
    g_platform.write32(g_platform.context, rtl_port(offset), value);
}

static void rtl_dma_write_barrier(void) {
    g_platform.dma_write_barrier(g_platform.context);
}

static void rtl_dma_read_barrier(void) {
    g_platform.dma_read_barrier(g_platform.context);
}

static void rtl_detach_interrupt_mode(void) {
    if (!g_interrupt_mode) return;

    /*
     * Quiesce the device before removing its callback. The PIC line may be
     * shared, so it deliberately remains unmasked for any peer handler.
     */
    rtl_write16(RTL_REG_IMR, 0);
    if (g_platform.irq_unregister != NULL) {
        (void)g_platform.irq_unregister(
            g_platform.context, g_irq_line,
            rtl_interrupt_entry, NULL);
    }
    __atomic_store_n(&g_deferred_interrupts, 0, __ATOMIC_RELEASE);
    g_interrupt_mode = false;
    g_irq_line = 0xFFu;
}

static void rtl_clear_bytes(uint8_t* destination, size_t count) {
    for (size_t index = 0; index < count; index++) destination[index] = 0;
}

static void rtl_copy_bytes(void* destination,
                           const void* source,
                           size_t count) {
    uint8_t* output = (uint8_t*)destination;
    const uint8_t* input = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        output[index] = input[index];
    }
}

static uint16_t rtl_rx_read16(size_t offset) {
    return (uint16_t)g_rx_buffer[offset] |
           ((uint16_t)g_rx_buffer[offset + 1u] << 8);
}

static void rtl_reset_software_state(void) {
    g_rx_offset = 0;
    g_next_tx = 0;
    __atomic_store_n(&g_deferred_interrupts, 0, __ATOMIC_RELEASE);
    g_interrupt_mode = false;
    g_irq_line = 0xFFu;
    for (size_t index = 0; index < RTL_TX_DESCRIPTOR_COUNT; index++) {
        g_tx_pending[index] = false;
        g_tx_length[index] = 0;
    }
    rtl_clear_bytes(g_rx_buffer, sizeof(g_rx_buffer));
    rtl_clear_bytes(&g_tx_buffer[0][0], sizeof(g_tx_buffer));
}

static void rtl_clear_status(void) {
    g_status.present = false;
    g_status.ready = false;
    g_status.link_up = false;
    g_status.interrupt_mode = false;
    g_status.io_base = 0;
    g_status.irq_line = 0xFFu;
    g_status.interrupts = 0;
    for (size_t index = 0; index < RTL8139_MAC_LENGTH; index++) {
        g_status.mac[index] = 0;
    }
    g_status.tx_packets = 0;
    g_status.tx_bytes = 0;
    g_status.tx_errors = 0;
    g_status.rx_packets = 0;
    g_status.rx_bytes = 0;
    g_status.rx_dropped = 0;
    g_status.rx_errors = 0;
    g_status.rx_overflows = 0;
}

static bool rtl_find_io_bar(const struct pci_device* device,
                            uint16_t* out_io_base) {
    for (uint8_t index = 0; index < PCI_MAX_BARS; index++) {
        struct pci_bar bar;
        if (!pci_decode_bar(device, index, &bar) ||
            bar.kind != PCI_BAR_IO) {
            continue;
        }
        if (bar.address > 0xFF00u) return false;
        *out_io_base = (uint16_t)bar.address;
        return true;
    }
    return false;
}

static bool rtl_read_mac(void) {
    bool all_zero = true;
    bool all_ones = true;
    for (size_t index = 0; index < RTL8139_MAC_LENGTH; index++) {
        const uint8_t octet = rtl_read8((uint16_t)(RTL_REG_MAC0 + index));
        g_status.mac[index] = octet;
        if (octet != 0) all_zero = false;
        if (octet != 0xFFu) all_ones = false;
    }
    return !all_zero && !all_ones &&
           (g_status.mac[0] & 1u) == 0u;
}

static void rtl_refresh_link(void) {
    /*
     * BMSR link state is latch-low on physical hardware, hence two reads.
     * Some old clones expose only the inverted MEDIA link-down bit.
     */
    (void)rtl_read16(RTL_REG_BMSR);
    const uint16_t bmsr = rtl_read16(RTL_REG_BMSR);
    if (bmsr != 0u && bmsr != UINT16_MAX) {
        g_status.link_up = (bmsr & RTL_BMSR_LINK_UP) != 0u;
    } else {
        g_status.link_up =
            (rtl_read8(RTL_REG_MEDIA) & RTL_MEDIA_LINK_DOWN) == 0u;
    }
}

static bool rtl_interrupt_entry(uint8_t irq, void* context) {
    (void)context;
    if (!g_status.ready || !g_interrupt_mode || irq != g_irq_line) {
        return false;
    }

    const uint16_t status = rtl_read16(RTL_REG_ISR);
    const uint16_t events =
        (uint16_t)(status & RTL_ISR_ENABLED_MASK);
    if (status == UINT16_MAX || events == 0u) return false;

    /*
     * Stop further device interrupts before acknowledging this batch. The
     * receive ring and TX descriptors remain untouched until foreground code
     * calls rtl8139_poll()/rtl8139_receive().
     */
    rtl_write16(RTL_REG_IMR, 0);
    rtl_write16(RTL_REG_ISR, status);
    __atomic_fetch_or(&g_deferred_interrupts, events, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_status.interrupts, 1u, __ATOMIC_RELAXED);
    return true;
}

static uint16_t rtl_collect_events(void) {
    const uint64_t irq_state =
        g_interrupt_mode
            ? g_platform.irq_save(g_platform.context)
            : 0;
    uint16_t events =
        __atomic_exchange_n(&g_deferred_interrupts, 0,
                            __ATOMIC_ACQ_REL);
    const uint16_t live = rtl_read16(RTL_REG_ISR);
    if (live != 0u && live != UINT16_MAX) {
        rtl_write16(RTL_REG_ISR, live);
        events = (uint16_t)(events | live);
    }
    if (g_interrupt_mode) {
        g_platform.irq_restore(g_platform.context, irq_state);
    }
    return events;
}

static void rtl_rearm_interrupts(void) {
    if (!g_status.ready || !g_interrupt_mode) return;

    uint16_t mask = RTL_ISR_ENABLED_MASK;
    /*
     * Acknowledging RxOK does not consume the receive ring. Keep RX causes
     * masked until foreground code drains it, while allowing TX/link faults
     * to continue notifying the kernel.
     */
    if ((rtl_read8(RTL_REG_COMMAND) &
         RTL_COMMAND_BUFFER_EMPTY) == 0u) {
        mask = (uint16_t)(mask & ~RTL_ISR_RX_MASK);
    }
    rtl_write16(RTL_REG_IMR, mask);
}

static bool rtl_irq_can_be_shared(uint8_t irq) {
    /*
     * IRQ0, IRQ1, and IRQ12 enter Nostalux's timer, keyboard, and mouse
     * handlers before shared callbacks are dispatched. A PCI device on one of
     * those lines would therefore manufacture time or input activity. IRQ2 is
     * the legacy PIC cascade and cannot be assigned to a device. Other lines
     * use the generic shared dispatcher, including its IRQ7/IRQ15 spurious
     * checks, so they remain eligible.
     */
    return irq < IRQ_REGISTRY_LINE_COUNT &&
           irq != 0u &&
           irq != 1u &&
           irq != 2u &&
           irq != 12u;
}

static void rtl_try_attach_interrupt(const struct pci_device* device) {
    if (device == NULL ||
        !rtl_platform_has_interrupts(&g_platform) ||
        !rtl_irq_can_be_shared(device->interrupt_line)) {
        return;
    }

    const uint8_t irq = device->interrupt_line;
    if (!g_platform.irq_register(
            g_platform.context, irq, rtl_interrupt_entry, NULL)) {
        return;
    }

    g_irq_line = irq;
    g_interrupt_mode = true;
    g_status.irq_line = irq;
    g_status.interrupt_mode = true;
    __atomic_store_n(&g_deferred_interrupts, 0, __ATOMIC_RELEASE);

    /* Clear reset-era status before exposing the PIC line. */
    rtl_write16(RTL_REG_ISR, UINT16_MAX);
    rtl_write16(RTL_REG_IMR, RTL_ISR_ENABLED_MASK);
    g_platform.irq_unmask(g_platform.context, irq);
}

static void rtl_reap_transmits(void) {
    for (size_t index = 0; index < RTL_TX_DESCRIPTOR_COUNT; index++) {
        if (!g_tx_pending[index]) continue;
        const uint32_t descriptor =
            rtl_read32((uint16_t)(RTL_REG_TSD0 + index * 4u));
        if ((descriptor & RTL_TSD_HOST_OWNS) == 0u) continue;

        if ((descriptor & RTL_TSD_OK) != 0u &&
            (descriptor & RTL_TSD_ERROR_MASK) == 0u) {
            g_status.tx_packets++;
            g_status.tx_bytes += g_tx_length[index];
        } else {
            g_status.tx_errors++;
        }
        g_tx_pending[index] = false;
        g_tx_length[index] = 0;
    }
}

static void rtl_restart_receiver(void) {
    rtl_write8(RTL_REG_COMMAND, RTL_COMMAND_TX_ENABLE);
    bool stopped = false;
    for (size_t attempt = 0; attempt < 1024u; attempt++) {
        if ((rtl_read8(RTL_REG_COMMAND) &
             RTL_COMMAND_RX_ENABLE) == 0u) {
            stopped = true;
            break;
        }
        rtl_cpu_relax();
    }
    if (!stopped) {
        g_status.ready = false;
        return;
    }
    rtl_dma_read_barrier();
    rtl_clear_bytes(g_rx_buffer, sizeof(g_rx_buffer));
    rtl_dma_write_barrier();

    uint32_t rx_bus = 0;
    if (!g_platform.dma_map(g_platform.context, g_rx_buffer,
                            sizeof(g_rx_buffer), &rx_bus) ||
        (rx_bus & 3u) != 0u) {
        g_status.ready = false;
        return;
    }
    g_rx_offset = 0;
    rtl_write32(RTL_REG_RBSTART, rx_bus);
    /*
     * CAPR is encoded 16 bytes behind the consumer. 0xFFF0 therefore
     * represents consumer offset zero after a non-reset restart.
     */
    rtl_write16(RTL_REG_CAPR, 0xFFF0u);
    rtl_write32(RTL_REG_RCR, RTL_RCR_VALUE);
    rtl_write8(RTL_REG_COMMAND,
               RTL_COMMAND_TX_ENABLE | RTL_COMMAND_RX_ENABLE);
}

enum rtl8139_init_result rtl8139_init_device(
    const struct pci_device* device,
    const struct rtl8139_platform* platform) {
    rtl_detach_interrupt_mode();
    rtl_clear_status();
    rtl_reset_software_state();
    if (device == NULL ||
        device->vendor_id != PCI_VENDOR_ID_REALTEK ||
        device->device_id != PCI_DEVICE_ID_RTL8139) {
        return RTL8139_INIT_NOT_FOUND;
    }
    g_status.present = true;
    if (!rtl_platform_valid(platform)) {
        return RTL8139_INIT_INVALID_PLATFORM;
    }
    g_platform = *platform;

    uint16_t io_base = 0;
    if (!rtl_find_io_bar(device, &io_base)) {
        return RTL8139_INIT_UNSUPPORTED_BAR;
    }
    g_status.io_base = io_base;

    uint32_t rx_bus = 0;
    uint32_t tx_bus[RTL_TX_DESCRIPTOR_COUNT];
    if (!g_platform.dma_map(g_platform.context, g_rx_buffer,
                            sizeof(g_rx_buffer), &rx_bus) ||
        (rx_bus & 3u) != 0u) {
        return RTL8139_INIT_DMA_UNAVAILABLE;
    }
    for (size_t index = 0; index < RTL_TX_DESCRIPTOR_COUNT; index++) {
        if (!g_platform.dma_map(g_platform.context, g_tx_buffer[index],
                                sizeof(g_tx_buffer[index]),
                                &tx_bus[index]) ||
            (tx_bus[index] & 3u) != 0u) {
            return RTL8139_INIT_DMA_UNAVAILABLE;
        }
    }

    /* Keep device interrupts quiescent until all DMA state is valid. */
    rtl_write16(RTL_REG_IMR, 0);
    rtl_write16(RTL_REG_ISR, UINT16_MAX);
    rtl_write8(RTL_REG_CONFIG1, 0);
    rtl_write8(RTL_REG_COMMAND, RTL_COMMAND_RESET);
    size_t timeout = RTL_RESET_POLL_LIMIT;
    while (timeout > 0 &&
           (rtl_read8(RTL_REG_COMMAND) & RTL_COMMAND_RESET) != 0u) {
        timeout--;
        rtl_cpu_relax();
    }
    if (timeout == 0) return RTL8139_INIT_RESET_TIMEOUT;

    if (!rtl_read_mac()) return RTL8139_INIT_INVALID_MAC;

    rtl_dma_write_barrier();
    rtl_write32(RTL_REG_RBSTART, rx_bus);
    for (size_t index = 0; index < RTL_TX_DESCRIPTOR_COUNT; index++) {
        rtl_write32((uint16_t)(RTL_REG_TSAD0 + index * 4u), tx_bus[index]);
    }
    rtl_write16(RTL_REG_IMR, 0);
    rtl_write16(RTL_REG_ISR, UINT16_MAX);
    rtl_write32(RTL_REG_TCR, RTL_TCR_VALUE);
    rtl_write32(RTL_REG_RCR, RTL_RCR_VALUE);
    rtl_write8(RTL_REG_COMMAND,
               RTL_COMMAND_TX_ENABLE | RTL_COMMAND_RX_ENABLE);

    g_status.ready = true;
    rtl_refresh_link();
    rtl_try_attach_interrupt(device);
    syslog_write(g_interrupt_mode
                     ? "RTL8139: deferred interrupt mode ready"
                     : "RTL8139: polling network device ready");
    return RTL8139_INIT_READY;
}

enum rtl8139_init_result rtl8139_init(void) {
    rtl_detach_interrupt_mode();
    struct pci_device device;
    if (!pci_find_device(PCI_VENDOR_ID_REALTEK, PCI_DEVICE_ID_RTL8139,
                         &device)) {
        rtl_clear_status();
        return RTL8139_INIT_NOT_FOUND;
    }
    if (!pci_enable_command_bits(
            device.location,
            PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER)) {
        rtl_clear_status();
        g_status.present = true;
        return RTL8139_INIT_PCI_ERROR;
    }
    return rtl8139_init_device(&device, &g_x86_platform);
}

enum rtl8139_tx_result rtl8139_send(const void* frame, size_t length) {
    if (!g_status.ready) return RTL8139_TX_NOT_READY;
    if (frame == NULL || length < 14u ||
        length > RTL8139_MAX_FRAME_SIZE) {
        return RTL8139_TX_INVALID_FRAME;
    }

    rtl_reap_transmits();
    /*
     * Legacy RTL8139 descriptors are consumed in a strict four-entry cycle.
     * In particular, QEMU tracks the same current descriptor internally, so
     * skipping a busy slot and writing a later TSD would stall its queue.
     */
    const size_t descriptor = g_next_tx;
    const uint32_t state =
        rtl_read32((uint16_t)(RTL_REG_TSD0 + descriptor * 4u));
    if (g_tx_pending[descriptor] ||
        (state & RTL_TSD_HOST_OWNS) == 0u) {
        return RTL8139_TX_BUSY;
    }

    const size_t transfer_length =
        length < RTL8139_MIN_FRAME_SIZE ? RTL8139_MIN_FRAME_SIZE : length;
    rtl_copy_bytes(g_tx_buffer[descriptor], frame, length);
    if (transfer_length > length) {
        rtl_clear_bytes(g_tx_buffer[descriptor] + length,
                        transfer_length - length);
    }

    g_tx_pending[descriptor] = true;
    g_tx_length[descriptor] = length;
    g_next_tx = (uint8_t)((descriptor + 1u) % RTL_TX_DESCRIPTOR_COUNT);
    rtl_dma_write_barrier();
    /*
     * Writing a byte count with OWN clear transfers the descriptor to the
     * controller. The buffer stays stable until OWN returns to one.
     */
    rtl_write32((uint16_t)(RTL_REG_TSD0 + descriptor * 4u),
                RTL_TSD_EARLY_THRESHOLD | (uint32_t)transfer_length);
    return RTL8139_TX_QUEUED;
}

static void rtl_consume_receive(size_t controller_length) {
    size_t next = (size_t)g_rx_offset + 4u + controller_length;
    next = (next + 3u) & ~(size_t)3u;
    g_rx_offset = (uint16_t)(next & (RTL_RX_RING_SIZE - 1u));
    rtl_dma_write_barrier();
    rtl_write16(RTL_REG_CAPR, (uint16_t)(g_rx_offset - 16u));
}

enum rtl8139_rx_result rtl8139_receive(void* buffer,
                                      size_t capacity,
                                      size_t* out_length) {
    if (out_length != NULL) *out_length = 0;
    if (!g_status.ready) return RTL8139_RX_NOT_READY;
    if (buffer == NULL && capacity != 0) return RTL8139_RX_ERROR;

    rtl_reap_transmits();
    const uint16_t interrupts = rtl_collect_events();
    if ((interrupts & (RTL_ISR_RX_OVERFLOW | RTL_ISR_FIFO_OVERFLOW)) != 0u) {
        g_status.rx_overflows++;
        g_status.rx_errors++;
        rtl_restart_receiver();
        rtl_rearm_interrupts();
        return RTL8139_RX_ERROR;
    }
    if ((interrupts & RTL_ISR_RX_ERROR) != 0u) {
        g_status.rx_errors++;
    }

    if ((rtl_read8(RTL_REG_COMMAND) &
         RTL_COMMAND_BUFFER_EMPTY) != 0u) {
        rtl_rearm_interrupts();
        return RTL8139_RX_NONE;
    }

    rtl_dma_read_barrier();
    const size_t offset = g_rx_offset;
    const uint16_t receive_status = rtl_rx_read16(offset);
    const size_t controller_length =
        (size_t)rtl_rx_read16(offset + 2u);
    if (controller_length < 4u ||
        controller_length > RTL8139_MAX_FRAME_SIZE + 4u) {
        g_status.rx_errors++;
        rtl_restart_receiver();
        rtl_rearm_interrupts();
        return RTL8139_RX_ERROR;
    }

    const size_t frame_length = controller_length - 4u;
    if (out_length != NULL) *out_length = frame_length;
    if ((receive_status & RTL_RX_STATUS_OK) == 0u) {
        g_status.rx_errors++;
        rtl_consume_receive(controller_length);
        rtl_rearm_interrupts();
        return RTL8139_RX_ERROR;
    }

    if (frame_length > capacity || buffer == NULL) {
        g_status.rx_dropped++;
        rtl_consume_receive(controller_length);
        rtl_rearm_interrupts();
        return RTL8139_RX_BUFFER_TOO_SMALL;
    }

    rtl_copy_bytes(buffer, g_rx_buffer + offset + 4u, frame_length);
    rtl_consume_receive(controller_length);
    g_status.rx_packets++;
    g_status.rx_bytes += frame_length;
    rtl_rearm_interrupts();
    return RTL8139_RX_RECEIVED;
}

void rtl8139_poll(void) {
    if (!g_status.ready) return;
    rtl_reap_transmits();

    const uint16_t interrupts = rtl_collect_events();
    if ((interrupts & (RTL_ISR_RX_OVERFLOW | RTL_ISR_FIFO_OVERFLOW)) != 0u) {
        g_status.rx_overflows++;
        g_status.rx_errors++;
        rtl_restart_receiver();
    }
    if ((interrupts & RTL_ISR_RX_ERROR) != 0u) {
        g_status.rx_errors++;
    }
    if ((interrupts & RTL_ISR_SYSTEM_ERROR) != 0u) {
        g_status.tx_errors++;
        g_status.rx_errors++;
    }
    if ((interrupts & RTL_ISR_LINK_CHANGE) != 0u) rtl_refresh_link();
    rtl_rearm_interrupts();
}

bool rtl8139_get_status(struct rtl8139_status* out_status) {
    if (out_status == NULL) return false;
    const bool irq_locked = g_interrupt_mode;
    const uint64_t irq_state =
        irq_locked
            ? g_platform.irq_save(g_platform.context)
            : 0;
    if (g_status.ready) rtl_refresh_link();
    *out_status = g_status;
    if (irq_locked) {
        g_platform.irq_restore(g_platform.context, irq_state);
    }
    return true;
}

const char* rtl8139_init_result_text(enum rtl8139_init_result result) {
    switch (result) {
        case RTL8139_INIT_READY:
            return "RTL8139: ready";
        case RTL8139_INIT_NOT_FOUND:
            return "RTL8139: PCI device not found";
        case RTL8139_INIT_PCI_ERROR:
            return "RTL8139: PCI I/O or bus mastering unavailable";
        case RTL8139_INIT_UNSUPPORTED_BAR:
            return "RTL8139: no supported I/O BAR";
        case RTL8139_INIT_INVALID_PLATFORM:
            return "RTL8139: invalid platform operations";
        case RTL8139_INIT_DMA_UNAVAILABLE:
            return "RTL8139: DMA buffers are not addressable";
        case RTL8139_INIT_RESET_TIMEOUT:
            return "RTL8139: reset timed out";
        case RTL8139_INIT_INVALID_MAC:
            return "RTL8139: invalid hardware address";
        default:
            return "RTL8139: unknown initialization result";
    }
}
