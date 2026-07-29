#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#include "app_abi.h"
#include "app_manifest.h"
#include "app_process.h"
#include "app_services.h"
#include "paging.h"
#include "rtc.h"
#include "scheduler.h"
#include "syslog.h"
#include "timer.h"

#define APP_LOG_MAX_BYTES 4096u
#define APP_LOG_CHUNK_BYTES 79u

static uint64_t app_error(enum app_status status) {
    return (uint64_t)(int64_t)status;
}

static struct paging_address_space* current_user_space(void) {
    if (!scheduler_current_is_user()) return NULL;
    return scheduler_current_address_space();
}

static bool current_has_capability(uint64_t capability) {
    return (scheduler_current_app_capabilities() & capability) ==
           capability;
}

static uint64_t dispatch_abi_query(const struct syscall_regs* regs) {
    if (regs->rsi == 0 || regs->rdx < sizeof(struct app_abi_info)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    const struct app_abi_info info = {
        .abi_version = NOSTALUX_APP_ABI_VERSION,
        .page_size = (uint32_t)PAGING_PAGE_SIZE,
        .granted_capabilities =
            scheduler_current_app_capabilities(),
    };
    if (!paging_copy_to_user(current_user_space(), regs->rsi,
                             &info, sizeof(info))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return APP_STATUS_OK;
}

static uint64_t dispatch_log_write(const struct syscall_regs* regs) {
    const uint64_t user_bytes = regs->rsi;
    const uint64_t requested = regs->rdx;
    if (!current_has_capability(APP_CAPABILITY_LOG)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (requested > APP_LOG_MAX_BYTES ||
        requested > (uint64_t)SIZE_MAX) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    const size_t length = (size_t)requested;
    if (length == 0) return 0;

    struct paging_address_space* space = current_user_space();
    if (!paging_user_range_mapped(space, user_bytes, length, false)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    size_t offset = 0;
    while (offset < length) {
        char chunk[APP_LOG_CHUNK_BYTES + 1u];
        size_t chunk_length = length - offset;
        if (chunk_length > APP_LOG_CHUNK_BYTES) {
            chunk_length = APP_LOG_CHUNK_BYTES;
        }
        if (!paging_copy_from_user(space, chunk,
                                   user_bytes + (uint64_t)offset,
                                   chunk_length)) {
            return app_error(APP_STATUS_INVALID_ARGUMENT);
        }
        chunk[chunk_length] = '\0';
        syslog_write(chunk);
        offset += chunk_length;
    }
    return (uint64_t)length;
}

static uint64_t dispatch_time_get(const struct syscall_regs* regs) {
    if (!current_has_capability(APP_CAPABILITY_TIME)) {
        return app_error(APP_STATUS_PERMISSION_DENIED);
    }
    if (regs->rsi == 0 || regs->rdx < sizeof(struct app_time)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    struct rtc_time rtc;
    if (!rtc_read_time(&rtc)) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    const struct app_time result = {
        .monotonic_milliseconds = timer_get_milliseconds(),
        .hour = rtc.hour,
        .minute = rtc.minute,
        .second = rtc.second,
        .reserved = {0},
    };
    if (!paging_copy_to_user(current_user_space(), regs->rsi,
                             &result, sizeof(result))) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return APP_STATUS_OK;
}

static uint64_t dispatch_argument_get(const struct syscall_regs* regs) {
    if (regs->rsi == 0 || regs->rdx == 0 ||
        regs->rdx > (uint64_t)SIZE_MAX) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }

    char argument[APP_STARTUP_ARGUMENT_MAX + 1u];
    size_t length = 0;
    if (!app_process_get_startup_argument(
            scheduler_current_app_process_id(),
            argument, sizeof(argument), &length)) {
        return app_error(APP_STATUS_IO_ERROR);
    }
    if (regs->rdx < length + 1u) {
        return app_error(APP_STATUS_NO_SPACE);
    }
    if (!paging_copy_to_user(
            current_user_space(), regs->rsi,
            argument, length + 1u)) {
        return app_error(APP_STATUS_INVALID_ARGUMENT);
    }
    return (uint64_t)length;
}

uint64_t syscall_dispatcher(struct syscall_regs* regs) {
    /*
     * INT 0x80 is an application boundary, not a way to invoke the old raw
     * kernel helpers. Requiring an active user task also makes a CPL0 INT 0x80
     * harmless. Check CS before reading the privilege-transition-only RSP/SS
     * tail: an accidental CPL0 INT 0x80 has a shorter hardware frame.
     */
    if (regs == NULL ||
        !user_return_frame_is_user(&regs->return_frame)) {
        return SYSCALL_RESULT_UNSUPPORTED;
    }

    /*
     * Normalize the frame before any requested side effect. Invalid user
     * state is poisoned so IRET completes at CPL3 and the resulting page fault
     * is contained by the ordinary user-exception path.
     */
    const bool return_frame_valid =
        user_return_frame_normalize(&regs->return_frame);
    if (!return_frame_valid ||
        !scheduler_current_is_user() ||
        current_user_space() == NULL) {
        return SYSCALL_RESULT_UNSUPPORTED;
    }

    switch (regs->rdi) {
        case APP_SYSCALL_ABI_QUERY:
            return dispatch_abi_query(regs);
        case APP_SYSCALL_EXIT:
            scheduler_exit_current_user((int64_t)regs->rsi);
        case APP_SYSCALL_YIELD:
            schedule();
            return APP_STATUS_OK;
        case APP_SYSCALL_LOG_WRITE:
            return dispatch_log_write(regs);
        case APP_SYSCALL_TIME_GET:
            return dispatch_time_get(regs);
        case APP_SYSCALL_ARGUMENT_GET:
            return dispatch_argument_get(regs);

        case APP_SYSCALL_FILE_OPEN:
        case APP_SYSCALL_FILE_READ:
        case APP_SYSCALL_FILE_WRITE:
        case APP_SYSCALL_FILE_CLOSE:
        case APP_SYSCALL_FILE_REPLACE:
        case APP_SYSCALL_WINDOW_CREATE:
        case APP_SYSCALL_WINDOW_PRESENT:
        case APP_SYSCALL_WINDOW_CLOSE:
        case APP_SYSCALL_INPUT_POLL:
        case APP_SYSCALL_MEMORY_MAP:
        case APP_SYSCALL_MEMORY_UNMAP:
            return app_services_dispatch(
                regs->rdi, current_user_space(),
                scheduler_current_app_process_id(),
                scheduler_current_app_capabilities(),
                regs->rsi, regs->rdx, regs->rcx,
                regs->r8, regs->r9);
        default:
            return SYSCALL_RESULT_UNSUPPORTED;
    }
}
