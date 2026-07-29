#include <stdint.h>

#include "app_abi.h"

#define RFLAGS_NT (1ull << 14)
#define RFLAGS_OK_MARKER 0x52464c4147534f4bull

static inline __attribute__((always_inline)) void set_nested_task_flag(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "orq %0, (%%rsp)\n\t"
        "popfq"
        :
        : "i"(RFLAGS_NT)
        : "memory", "cc");
}

/*
 * Exercises both syscall and interrupt return with the user-controlled NT
 * flag set. A safe return path clears NT before IRETQ. R15 becomes the
 * observable success marker only after the cooperative yield returns.
 */
__attribute__((noreturn)) void nostalux_app_entry(void) {
    set_nested_task_flag();
    (void)app_syscall5(APP_SYSCALL_YIELD, 0, 0, 0, 0, 0);

    __asm__ volatile(
        "movabsq $0x52464c4147534f4b, %%r15"
        :
        :
        : "r15", "memory");

    for (;;) {
        set_nested_task_flag();
        __asm__ volatile("pause" ::: "memory");
    }
}
