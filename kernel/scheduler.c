#include "scheduler.h"
#include "heap.h"
#include "syslog.h"
#include "gdt.h"
#include "kstdio.h"

static Task* g_current_task = NULL;
static Task* g_head = NULL;
static uint64_t g_next_pid = 1;

#define STACK_SIZE 16384

static void reap_dead_tasks(void) {
    if (g_head == NULL) return;

    Task* task = g_head;
    do {
        Task* next = (Task*)task->next;
        if (next != g_head && next != g_current_task && next->state == TASK_DEAD) {
            task->next = next->next;
            kfree(next->kernel_stack);
            kfree(next->user_stack);
            kfree(next);
            continue;
        }
        task = next;
    } while (task != g_head);
}

void scheduler_init(void) {
    Task* kmain_task = (Task*)kmalloc(sizeof(Task));
    if (kmain_task == NULL) {
        syslog_write("Scheduler: initialization failed (out of memory)");
        return;
    }

    kmain_task->id = g_next_pid++;
    kmain_task->rsp = 0; 
    kmain_task->is_user = false;
    kmain_task->state = TASK_READY;
    kmain_task->kernel_stack_top = 0;
    kmain_task->kernel_stack = NULL;
    kmain_task->user_stack = NULL;
    kmain_task->next = kmain_task; // Circular list

    g_head = kmain_task;
    g_current_task = kmain_task;
    
    syslog_write("Scheduler: Initialized (Multitasking enabled)");
}

extern void task_return_trampoline(void);

bool spawn_task(void (*entry_point)(void)) {
    if (entry_point == NULL || g_head == NULL) return false;

    Task* new_task = (Task*)kmalloc(sizeof(Task));
    if (new_task == NULL) {
        syslog_write("Scheduler: task allocation failed");
        return false;
    }

    uint8_t* stack = (uint8_t*)kmalloc(STACK_SIZE);
    if (stack == NULL) {
        kfree(new_task);
        syslog_write("Scheduler: stack allocation failed");
        return false;
    }
    
    new_task->id = g_next_pid++;
    new_task->is_user = false;
    new_task->state = TASK_READY;
    new_task->kernel_stack = stack;
    new_task->user_stack = NULL;
    
    uint64_t* sp = (uint64_t*)(stack + STACK_SIZE);
    
    // Synthetic return address used when the task entry function returns.
    *(--sp) = (uint64_t)task_return_trampoline;

    // Return address for context_switch
    *(--sp) = (uint64_t)entry_point;
    
    // Callee saved registers
    *(--sp) = 0; // R15
    *(--sp) = 0; // R14
    *(--sp) = 0; // R13
    *(--sp) = 0; // R12
    *(--sp) = 0; // RBP
    *(--sp) = 0; // RBX
    
    new_task->rsp = (uint64_t)sp;
    new_task->kernel_stack_top = (uint64_t)(stack + STACK_SIZE);

    new_task->next = g_head->next;
    g_head->next = new_task;
    return true;
}

bool spawn_user_task(void (*entry_point)(void)) {
    (void)entry_point;
    /*
     * The old implementation exposed all kernel mappings to ring 3 and had no
     * safe user-pointer contract. Refuse user tasks until dedicated mappings
     * and a real interrupt-frame scheduler exist.
     */
    syslog_write("Scheduler: user tasks are disabled");
    return false;
}

void exit_current_task(void) {
    // We cannot free the stack we are currently using.
    // Mark as dead, and the scheduler will simply skip it.
    // In a real OS, a separate "reaper" thread would free these.
    if (g_current_task) {
        g_current_task->state = TASK_DEAD;
    }
    
    // Yield immediately to switch away
    schedule();
    
    // Should never reach here
    while(1);
}

void schedule(void) {
    if (!g_current_task) return;

    Task* start_task = g_current_task;
    Task* next = (Task*)g_current_task->next;

    // Find next READY task
    while (next != start_task) {
        if (next->state == TASK_READY) break;
        next = (Task*)next->next;
    }

    // If we looped all the way around, check if current is ready
    if (next == start_task && start_task->state != TASK_READY) {
        // All tasks are dead. In a real OS, idle task would run.
        // For now, halt.
        syslog_write("Scheduler: All tasks dead/waiting.");
        while(1) __asm__ volatile("hlt");
    }

    if (next == g_current_task) return; // No switch needed

    Task* prev = g_current_task;
    g_current_task = next;
    
    if (next->kernel_stack_top != 0) {
        gdt_set_kernel_stack(next->kernel_stack_top);
    }

    context_switch(&prev->rsp, next->rsp);
    reap_dead_tasks();
}
