#ifndef USER_RETURN_H
#define USER_RETURN_H

#include <stdbool.h>
#include <stdint.h>

#include "paging.h"

/*
 * These selectors must match the user descriptors installed by gdt_init()
 * and the initial IRET frame built by enter_user_mode().
 */
#define USER_RETURN_DATA_SELECTOR 0x1bull
#define USER_RETURN_CODE_SELECTOR 0x23ull

/*
 * Preserve ordinary user arithmetic/control flags while clearing IOPL, NT,
 * VM, VIF, VIP, and every reserved or high bit. Bit 1 and IF are mandatory
 * for every return to an application.
 */
#define USER_RETURN_RFLAGS_ALLOWED  0x0000000000250fd7ull
#define USER_RETURN_RFLAGS_REQUIRED 0x0000000000000202ull

struct user_return_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static inline bool user_return_frame_is_user(
    const struct user_return_frame* frame) {
    return frame != NULL && (frame->cs & 3u) == 3u;
}

static inline uint64_t user_return_sanitize_rflags(uint64_t rflags) {
    return (rflags & USER_RETURN_RFLAGS_ALLOWED) |
           USER_RETURN_RFLAGS_REQUIRED;
}

/*
 * Prepare a hardware frame for IRETQ. A ring-3 task can place a noncanonical
 * or privileged address in RSP without touching memory; returning to that
 * frame would fault while CPL0 is still active. Poison any invalid user frame
 * with canonical zero addresses and known selectors instead. IRETQ then
 * completes the privilege transition and the instruction fetch at address
 * zero becomes an ordinary contained user page fault.
 *
 * The caller must inspect only CS before invoking this for a same-privilege
 * kernel frame because such a frame has no saved RSP or SS.
 */
static inline bool user_return_frame_normalize(
    struct user_return_frame* frame) {
    if (!user_return_frame_is_user(frame)) return true;

    frame->rflags = user_return_sanitize_rflags(frame->rflags);
    const bool valid =
        frame->cs == USER_RETURN_CODE_SELECTOR &&
        frame->ss == USER_RETURN_DATA_SELECTOR &&
        frame->rip >= PAGING_USER_BASE &&
        frame->rip < PAGING_USER_LIMIT &&
        frame->rsp >= PAGING_USER_BASE &&
        frame->rsp < PAGING_USER_LIMIT;
    if (valid) return true;

    frame->rip = 0;
    frame->rsp = 0;
    frame->cs = USER_RETURN_CODE_SELECTOR;
    frame->ss = USER_RETURN_DATA_SELECTOR;
    frame->rflags = USER_RETURN_RFLAGS_REQUIRED;
    return false;
}

#endif /* USER_RETURN_H */
