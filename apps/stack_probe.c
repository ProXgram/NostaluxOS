#include <stdint.h>

#define STACK_BAD_MARKER 0x535441434b424144ull
#define NONCANONICAL_STACK_POINTER 0x0000800000000000ull

/*
 * Deliberately leaves ring 3 with a noncanonical RSP. The timer return path
 * must reject the saved frame instead of attempting an unsafe IRETQ.
 */
__attribute__((naked, noreturn)) void nostalux_app_entry(void) {
    __asm__(
        "movabsq $0x535441434b424144, %r15\n\t"
        "movabsq $0x0000800000000000, %rsp\n"
        "1:\n\t"
        "pause\n\t"
        "jmp 1b");
}
