#ifndef IRQ_REGISTRY_H
#define IRQ_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRQ_REGISTRY_LINE_COUNT 16u
#define IRQ_REGISTRY_HANDLERS_PER_LINE 4u

/*
 * Return true when the device claimed the interrupt. Shared-line dispatch
 * still invokes every registered handler because more than one device may
 * assert a legacy PIC line at the same time.
 */
typedef bool (*irq_registry_handler_t)(uint8_t irq, void* context);

struct irq_registry_slot {
    irq_registry_handler_t handler;
    void* context;
};

struct irq_registry {
    struct irq_registry_slot
        lines[IRQ_REGISTRY_LINE_COUNT][IRQ_REGISTRY_HANDLERS_PER_LINE];
};

void irq_registry_init(struct irq_registry* registry);
bool irq_registry_register(struct irq_registry* registry,
                           uint8_t irq,
                           irq_registry_handler_t handler,
                           void* context);
bool irq_registry_unregister(struct irq_registry* registry,
                             uint8_t irq,
                             irq_registry_handler_t handler,
                             void* context);
bool irq_registry_dispatch(struct irq_registry* registry, uint8_t irq);
size_t irq_registry_count(const struct irq_registry* registry, uint8_t irq);

#endif /* IRQ_REGISTRY_H */
