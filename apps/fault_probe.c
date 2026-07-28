#include <stdint.h>

/*
 * Reading the supervisor-only kernel mapping gives the launcher and QEMU
 * smoke tests a deterministic page fault. If isolation were accidentally
 * disabled, the UD2 fallback would still stop the probe rather than letting
 * it continue. This app is never run automatically during boot.
 */
__attribute__((noreturn)) void nostalux_app_entry(void) {
    const volatile uint8_t* kernel_text =
        (const volatile uint8_t*)(uintptr_t)0x00100000u;
    uint8_t observed = *kernel_text;
    __asm__ volatile("" : : "r"(observed) : "memory");
    __asm__ volatile("ud2");
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}
