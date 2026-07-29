#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "user_return.h"

static struct user_return_frame valid_frame(void) {
    return (struct user_return_frame) {
        .rip = PAGING_USER_BASE,
        .cs = USER_RETURN_CODE_SELECTOR,
        .rflags = 0x202,
        .rsp = PAGING_USER_LIMIT - 1u,
        .ss = USER_RETURN_DATA_SELECTOR,
    };
}

static void test_rflags_policy(void) {
    assert(user_return_sanitize_rflags(0) ==
           USER_RETURN_RFLAGS_REQUIRED);
    assert(user_return_sanitize_rflags(UINT64_MAX) ==
           USER_RETURN_RFLAGS_ALLOWED);

    const uint64_t forbidden =
        (3ull << 12) | (1ull << 14) | (1ull << 17) |
        (1ull << 19) | (1ull << 20) | (1ull << 63);
    const uint64_t ordinary =
        (1ull << 0) | (1ull << 6) | (1ull << 8) |
        (1ull << 10) | (1ull << 11) | (1ull << 16) |
        (1ull << 18) | (1ull << 21);
    const uint64_t sanitized =
        user_return_sanitize_rflags(forbidden | ordinary);
    assert((sanitized & forbidden) == 0);
    assert((sanitized & ordinary) == ordinary);
    assert((sanitized & USER_RETURN_RFLAGS_REQUIRED) ==
           USER_RETURN_RFLAGS_REQUIRED);
}

static void test_valid_user_frame(void) {
    struct user_return_frame frame = valid_frame();
    frame.rflags = UINT64_MAX;
    assert(user_return_frame_is_user(&frame));
    assert(user_return_frame_normalize(&frame));
    assert(frame.rip == PAGING_USER_BASE);
    assert(frame.rsp == PAGING_USER_LIMIT - 1u);
    assert(frame.rflags == USER_RETURN_RFLAGS_ALLOWED);
}

static void assert_poisoned(struct user_return_frame frame) {
    assert(!user_return_frame_normalize(&frame));
    assert(frame.rip == 0);
    assert(frame.rsp == 0);
    assert(frame.cs == USER_RETURN_CODE_SELECTOR);
    assert(frame.ss == USER_RETURN_DATA_SELECTOR);
    assert(frame.rflags == USER_RETURN_RFLAGS_REQUIRED);
}

static void test_invalid_user_frames_are_poisoned(void) {
    struct user_return_frame frame = valid_frame();
    frame.rsp = PAGING_USER_BASE - 1u;
    assert_poisoned(frame);

    frame = valid_frame();
    frame.rsp = PAGING_USER_LIMIT;
    assert_poisoned(frame);

    frame = valid_frame();
    frame.rsp = 0x0000800000000000ull;
    assert_poisoned(frame);

    frame = valid_frame();
    frame.rip = PAGING_USER_LIMIT;
    assert_poisoned(frame);

    frame = valid_frame();
    frame.cs = USER_RETURN_CODE_SELECTOR + 8u;
    assert_poisoned(frame);

    frame = valid_frame();
    frame.ss = 0;
    assert_poisoned(frame);
}

static void test_kernel_frame_prefix_is_untouched(void) {
    struct user_return_frame frame = {
        .rip = 0x100000,
        .cs = 0x08,
        .rflags = UINT64_MAX,
        .rsp = UINT64_MAX,
        .ss = UINT64_MAX,
    };
    const struct user_return_frame original = frame;
    assert(!user_return_frame_is_user(&frame));
    assert(user_return_frame_normalize(&frame));
    assert(frame.rip == original.rip);
    assert(frame.cs == original.cs);
    assert(frame.rflags == original.rflags);
    assert(frame.rsp == original.rsp);
    assert(frame.ss == original.ss);
}

int main(void) {
    test_rflags_policy();
    test_valid_user_frame();
    test_invalid_user_frames_are_poisoned();
    test_kernel_frame_prefix_is_untouched();
    puts("user_return_test: all tests passed");
    return 0;
}
