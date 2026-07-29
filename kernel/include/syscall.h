#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include "user_return.h"

/*
 * Historical kernel-call numbers. They are retained only as source
 * compatibility constants and are never accepted through the DPL3 gate.
 */
enum syscall_id {
    SYSCALL_YIELD = 0,
    SYSCALL_EXIT = 1,
    SYSCALL_LOG = 2,
    SYSCALL_SHUTDOWN = 4,
    SYSCALL_GET_MOUSE = 5,
    SYSCALL_ALLOC = 6,
    SYSCALL_FREE = 7,
    SYSCALL_GET_TIME = 8,
};

/* Unknown and legacy calls return the Apps v1 unsupported status value. */
#define SYSCALL_RESULT_UNSUPPORTED ((uint64_t)(int64_t)-2)

// Register order must match the pushes in entry.asm:isr_syscall.
struct syscall_regs {
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;
    struct user_return_frame return_frame;
};

_Static_assert(
    offsetof(struct syscall_regs, return_frame) == 14u * sizeof(uint64_t),
    "INT 0x80 register pushes no longer match struct syscall_regs");
_Static_assert(
    sizeof(struct syscall_regs) == 19u * sizeof(uint64_t),
    "INT 0x80 hardware-frame layout changed unexpectedly");

uint64_t syscall_dispatcher(struct syscall_regs* regs);

#endif
