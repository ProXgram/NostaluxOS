#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

#include "irq_registry.h"

void interrupts_init(void);

/*
 * Registers a bounded shared-line callback. Handlers execute with maskable
 * interrupts disabled and must only acknowledge/latch device state; deferred
 * processing belongs in normal kernel context. IRQ2 is the PIC cascade and
 * cannot host a device callback.
 */
bool interrupts_register_irq(uint8_t irq,
                             irq_registry_handler_t handler,
                             void* context);
bool interrupts_unregister_irq(uint8_t irq,
                               irq_registry_handler_t handler,
                               void* context);

/* Masks/unmasks the specified IRQ (0-15) on the legacy PIC. */
void interrupts_enable_irq(uint8_t irq);
void interrupts_disable_irq(uint8_t irq);

#endif /* INTERRUPTS_H */
