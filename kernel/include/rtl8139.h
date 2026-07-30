#ifndef RTL8139_H
#define RTL8139_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"

#define RTL8139_MAC_LENGTH 6u
#define RTL8139_MIN_FRAME_SIZE 60u
#define RTL8139_MAX_FRAME_SIZE 1518u

enum rtl8139_init_result {
    RTL8139_INIT_READY = 0,
    RTL8139_INIT_NOT_FOUND,
    RTL8139_INIT_PCI_ERROR,
    RTL8139_INIT_UNSUPPORTED_BAR,
    RTL8139_INIT_INVALID_PLATFORM,
    RTL8139_INIT_DMA_UNAVAILABLE,
    RTL8139_INIT_RESET_TIMEOUT,
    RTL8139_INIT_INVALID_MAC,
};

enum rtl8139_tx_result {
    RTL8139_TX_QUEUED = 0,
    RTL8139_TX_NOT_READY,
    RTL8139_TX_INVALID_FRAME,
    RTL8139_TX_BUSY,
};

enum rtl8139_rx_result {
    RTL8139_RX_RECEIVED = 0,
    RTL8139_RX_NONE,
    RTL8139_RX_NOT_READY,
    RTL8139_RX_BUFFER_TOO_SMALL,
    RTL8139_RX_ERROR,
};

struct rtl8139_status {
    bool present;
    bool ready;
    bool link_up;
    bool interrupt_mode;
    uint16_t io_base;
    uint8_t irq_line;
    uint8_t mac[RTL8139_MAC_LENGTH];
    uint64_t interrupts;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_dropped;
    uint64_t rx_errors;
    uint64_t rx_overflows;
};

typedef bool (*rtl8139_irq_handler_t)(uint8_t irq, void* context);

/*
 * Host-test and platform seam. The driver uses port addresses relative to the
 * decoded BAR, and every callback receives the complete 16-bit port number.
 * dma_map must return a stable 32-bit PCI bus address for the entire range.
 * IRQ callbacks are optional as a group; without them the driver remains in
 * its fully supported polling mode.
 */
struct rtl8139_platform {
    void* context;
    uint8_t (*read8)(void* context, uint16_t port);
    uint16_t (*read16)(void* context, uint16_t port);
    uint32_t (*read32)(void* context, uint16_t port);
    void (*write8)(void* context, uint16_t port, uint8_t value);
    void (*write16)(void* context, uint16_t port, uint16_t value);
    void (*write32)(void* context, uint16_t port, uint32_t value);
    bool (*dma_map)(void* context,
                    const void* virtual_address,
                    size_t length,
                    uint32_t* out_bus_address);
    void (*dma_write_barrier)(void* context);
    void (*dma_read_barrier)(void* context);
    bool (*irq_register)(void* context,
                         uint8_t irq,
                         rtl8139_irq_handler_t handler,
                         void* handler_context);
    bool (*irq_unregister)(void* context,
                           uint8_t irq,
                           rtl8139_irq_handler_t handler,
                           void* handler_context);
    void (*irq_unmask)(void* context, uint8_t irq);
    uint64_t (*irq_save)(void* context);
    void (*irq_restore)(void* context, uint64_t saved_state);
};

/*
 * Discovers a 10EC:8139 function, enables PCI I/O and bus mastering, and
 * initializes it with deferred legacy interrupts when the platform supports
 * them. Polling remains the supported fallback.
 */
enum rtl8139_init_result rtl8139_init(void);

/*
 * Initializes a supplied function with injected platform operations. The
 * caller is responsible for enabling its PCI I/O and bus-master command bits.
 */
enum rtl8139_init_result rtl8139_init_device(
    const struct pci_device* device,
    const struct rtl8139_platform* platform);

enum rtl8139_tx_result rtl8139_send(const void* frame, size_t length);
enum rtl8139_rx_result rtl8139_receive(void* buffer,
                                      size_t capacity,
                                      size_t* out_length);

/* Reaps completed transmits, acknowledges polling status, and handles faults. */
void rtl8139_poll(void);

bool rtl8139_get_status(struct rtl8139_status* out_status);
const char* rtl8139_init_result_text(enum rtl8139_init_result result);

#endif /* RTL8139_H */
