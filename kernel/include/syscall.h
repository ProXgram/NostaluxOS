#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

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

// This ABI is reserved for a future isolated user mode. No DPL3 IDT gate is
// installed while all mappings are supervisor-only; the dispatcher remains a
// normal C entry point so its number/result contract can be tested safely.
// Unknown syscall IDs return this value in RAX.
#define SYSCALL_RESULT_UNSUPPORTED UINT64_MAX

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
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

uint64_t syscall_dispatcher(struct syscall_regs* regs);

#endif
