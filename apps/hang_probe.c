/*
 * This probe deliberately violates the normal Apps v1 idle-loop convention:
 * it sets DF and then spins forever without issuing a syscall or yield.
 *
 * A healthy kernel clears DF on every interrupt entry, preempts the process
 * on the PIT quantum, and lets the shell terminate it with appkill.
 */
__attribute__((noreturn)) void nostalux_app_entry(void) {
    __asm__ volatile("std" ::: "cc");
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
    }
}
