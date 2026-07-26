#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

void interrupts_init(void);

/* Unmasks the specified IRQ (0-15) on the PIC */
void interrupts_enable_irq(uint8_t irq);

#endif /* INTERRUPTS_H */
