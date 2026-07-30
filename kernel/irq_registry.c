#include "irq_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void irq_registry_init(struct irq_registry* registry) {
    if (registry == NULL) return;
    for (size_t irq = 0; irq < IRQ_REGISTRY_LINE_COUNT; irq++) {
        for (size_t slot = 0;
             slot < IRQ_REGISTRY_HANDLERS_PER_LINE;
             slot++) {
            registry->lines[irq][slot].handler = NULL;
            registry->lines[irq][slot].context = NULL;
        }
    }
}

bool irq_registry_register(struct irq_registry* registry,
                           uint8_t irq,
                           irq_registry_handler_t handler,
                           void* context) {
    if (registry == NULL || irq >= IRQ_REGISTRY_LINE_COUNT ||
        handler == NULL) {
        return false;
    }

    size_t free_slot = IRQ_REGISTRY_HANDLERS_PER_LINE;
    for (size_t slot = 0;
         slot < IRQ_REGISTRY_HANDLERS_PER_LINE;
         slot++) {
        struct irq_registry_slot* entry = &registry->lines[irq][slot];
        if (entry->handler == handler && entry->context == context) {
            return true;
        }
        if (entry->handler == NULL &&
            free_slot == IRQ_REGISTRY_HANDLERS_PER_LINE) {
            free_slot = slot;
        }
    }
    if (free_slot == IRQ_REGISTRY_HANDLERS_PER_LINE) return false;

    registry->lines[irq][free_slot].context = context;
    registry->lines[irq][free_slot].handler = handler;
    return true;
}

bool irq_registry_unregister(struct irq_registry* registry,
                             uint8_t irq,
                             irq_registry_handler_t handler,
                             void* context) {
    if (registry == NULL || irq >= IRQ_REGISTRY_LINE_COUNT ||
        handler == NULL) {
        return false;
    }

    for (size_t slot = 0;
         slot < IRQ_REGISTRY_HANDLERS_PER_LINE;
         slot++) {
        struct irq_registry_slot* entry = &registry->lines[irq][slot];
        if (entry->handler != handler || entry->context != context) {
            continue;
        }
        /*
         * Do not compact the line. A handler may unregister itself while the
         * dispatcher is walking later slots, and stable indices avoid skipping
         * or unexpectedly repeating another shared handler.
         */
        entry->handler = NULL;
        entry->context = NULL;
        return true;
    }
    return false;
}

bool irq_registry_dispatch(struct irq_registry* registry, uint8_t irq) {
    if (registry == NULL || irq >= IRQ_REGISTRY_LINE_COUNT) return false;

    bool claimed = false;
    for (size_t slot = 0;
         slot < IRQ_REGISTRY_HANDLERS_PER_LINE;
         slot++) {
        /*
         * Snapshot both fields before the callback. IRQ registration changes
         * are serialized by the interrupt layer, while this also permits a
         * callback to remove its own stable slot safely.
         */
        irq_registry_handler_t handler =
            registry->lines[irq][slot].handler;
        void* context = registry->lines[irq][slot].context;
        if (handler != NULL && handler(irq, context)) claimed = true;
    }
    return claimed;
}

size_t irq_registry_count(const struct irq_registry* registry, uint8_t irq) {
    if (registry == NULL || irq >= IRQ_REGISTRY_LINE_COUNT) return 0;
    size_t count = 0;
    for (size_t slot = 0;
         slot < IRQ_REGISTRY_HANDLERS_PER_LINE;
         slot++) {
        if (registry->lines[irq][slot].handler != NULL) count++;
    }
    return count;
}
