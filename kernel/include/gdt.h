#ifndef GDT_H
#define GDT_H

#include <stdint.h>

extern uint8_t g_kernel_stack[];
extern uint8_t* const g_kernel_stack_top;

void gdt_init(void);

// Used by the scheduler to update the RSP0 in TSS when switching tasks
void gdt_set_kernel_stack(uint64_t stack_top);

#endif /* GDT_H */
