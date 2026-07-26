#include "syscall.h"
#include "kstdio.h"
#include "scheduler.h"
#include "syslog.h"
#include "io.h"
#include "mouse.h"
#include "heap.h"
#include "rtc.h"

static void sys_yield(void) { schedule(); }

static void sys_exit(void) {
    syslog_write("Syscall: Task exited");
    exit_current_task();
}

static void sys_log(const char* msg) { syslog_write(msg); }

static void sys_shutdown(void) {
    syslog_write("Syscall: Shutdown");
    outw(0x604, 0x2000); 
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}

static void sys_get_mouse(MouseState* user_struct) {
    if (user_struct) {
        MouseState k_state = mouse_get_state();
        *user_struct = k_state;
    }
}

static void* sys_malloc(size_t size) { return kmalloc(size); }
static void sys_free(void* ptr) { kfree(ptr); }

static void sys_get_time(char* buffer) {
    if (buffer == NULL) return;
    struct rtc_time time;
    if (!rtc_read_time(&time)) {
        buffer[0] = '\0';
        return;
    }
    buffer[0] = (char)('0' + time.hour / 10u);
    buffer[1] = (char)('0' + time.hour % 10u);
    buffer[2] = ':';
    buffer[3] = (char)('0' + time.minute / 10u);
    buffer[4] = (char)('0' + time.minute % 10u);
    buffer[5] = '\0';
}

// Returns value to be placed in RAX
uint64_t syscall_dispatcher(struct syscall_regs* regs) {
    if (regs == NULL) {
        return SYSCALL_RESULT_UNSUPPORTED;
    }

    uint64_t syscall_num = regs->rdi;
    uint64_t ret = 0;

    switch (syscall_num) {
        case SYSCALL_YIELD: sys_yield(); break;
        case SYSCALL_EXIT: sys_exit(); break;
        case SYSCALL_LOG: sys_log((const char*)regs->rsi); break;
        case SYSCALL_SHUTDOWN: sys_shutdown(); break;
        case SYSCALL_GET_MOUSE: sys_get_mouse((MouseState*)regs->rsi); break;
        case SYSCALL_ALLOC: ret = (uint64_t)sys_malloc((size_t)regs->rsi); break;
        case SYSCALL_FREE: sys_free((void*)regs->rsi); break;
        case SYSCALL_GET_TIME: sys_get_time((char*)regs->rsi); break;
        default: ret = SYSCALL_RESULT_UNSUPPORTED; break;
    }
    return ret;
}
