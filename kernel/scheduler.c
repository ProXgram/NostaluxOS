#include "scheduler.h"
#include "heap.h"
#include "syslog.h"
#include "gdt.h"
#include "kstdio.h"

static Task* g_current_task = NULL;
static Task* g_head = NULL;
static uint64_t g_next_pid = 1;

#define STACK_SIZE 16384

static bool allocate_task_id(uint64_t* id) {
    if (id == NULL || g_next_pid == 0) return false;

    *id = g_next_pid;
    if (g_next_pid == UINT64_MAX) {
        // PID 0 is reserved as invalid; refuse further spawns after wrap.
        g_next_pid = 0;
    } else {
        g_next_pid++;
    }
    return true;
}

static void copy_task_name(char destination[TASK_NAME_MAX], const char* source) {
    size_t index = 0;

    if (source != NULL) {
        while (source[index] != '\0' && index + 1 < TASK_NAME_MAX) {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static void destroy_task(Task* task) {
    if (task == NULL) return;
    kfree(task->kernel_stack);
    kfree(task->user_stack);
    kfree(task);
}

static void reap_dead_tasks(void) {
    if (g_head == NULL) return;

    /*
     * The list is circular and has no immortal sentinel. If a future caller
     * terminates the original bootstrap task while another task is current,
     * promote the next live list node before doing the normal interior pass.
     */
    while (g_head != g_current_task && g_head->state == TASK_DEAD) {
        Task* dead = g_head;
        Task* next = (Task*)dead->next;
        if (next == dead) {
            g_head = NULL;
            destroy_task(dead);
            return;
        }

        Task* tail = next;
        while (tail->next != dead) {
            tail = (Task*)tail->next;
        }
        g_head = next;
        tail->next = g_head;
        destroy_task(dead);
    }

    Task* previous = g_head;
    Task* task = (Task*)g_head->next;
    while (task != g_head) {
        Task* next = (Task*)task->next;
        if (task != g_current_task && task->state == TASK_DEAD) {
            previous->next = next;
            destroy_task(task);
        } else {
            previous = task;
        }
        task = next;
    }
}

void scheduler_init(void) {
    Task* kmain_task = (Task*)kmalloc(sizeof(Task));
    if (kmain_task == NULL) {
        syslog_write("Scheduler: initialization failed (out of memory)");
        return;
    }

    if (!allocate_task_id(&kmain_task->id)) {
        kfree(kmain_task);
        syslog_write("Scheduler: PID allocation failed");
        return;
    }
    copy_task_name(kmain_task->name, "kernel/main");
    kmain_task->rsp = 0; 
    kmain_task->is_user = false;
    kmain_task->state = TASK_READY;
    kmain_task->kernel_stack_top = 0;
    kmain_task->kernel_stack = NULL;
    kmain_task->user_stack = NULL;
    kmain_task->next = kmain_task; // Circular list

    g_head = kmain_task;
    g_current_task = kmain_task;
    
    syslog_write("Scheduler: Cooperative kernel scheduler initialized");
}

extern void task_return_trampoline(void);

bool spawn_named_task(const char* name, void (*entry_point)(void)) {
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
    
    if (!allocate_task_id(&new_task->id)) {
        kfree(stack);
        kfree(new_task);
        syslog_write("Scheduler: PID allocation failed");
        return false;
    }
    copy_task_name(new_task->name,
                   name != NULL && name[0] != '\0' ? name : "kernel/task");
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

bool spawn_task(void (*entry_point)(void)) {
    return spawn_named_task("kernel/task", entry_point);
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

size_t scheduler_task_count(void) {
    if (g_head == NULL) return 0;

    size_t count = 0;
    Task* task = g_head;
    do {
        count++;
        task = (Task*)task->next;
    } while (task != NULL && task != g_head);
    return count;
}

size_t scheduler_snapshot_tasks_from(size_t offset,
                                     struct scheduler_task_info* tasks,
                                     size_t capacity) {
    if (tasks == NULL || capacity == 0 || g_head == NULL) return 0;

    size_t written = 0;
    size_t index = 0;
    Task* task = g_head;
    do {
        if (index >= offset) {
            if (written >= capacity) break;
            tasks[written].id = task->id;
            copy_task_name(tasks[written].name, task->name);
            tasks[written].state = task->state;
            tasks[written].is_user = task->is_user;
            tasks[written].is_current = task == g_current_task;
            written++;
        }
        index++;
        task = (Task*)task->next;
    } while (task != NULL && task != g_head);

    return written;
}

size_t scheduler_snapshot_tasks(struct scheduler_task_info* tasks, size_t capacity) {
    return scheduler_snapshot_tasks_from(0, tasks, capacity);
}

void scheduler_set_current_name(const char* name) {
    if (g_current_task == NULL) return;
    copy_task_name(g_current_task->name,
                   name != NULL && name[0] != '\0' ? name : "kernel/task");
}

bool scheduler_terminate_task(uint64_t id) {
    if (id == 0 || g_head == NULL) return false;

    Task* task = g_head;
    do {
        if (task->id == id) {
            if (task == g_current_task || task->state == TASK_DEAD) {
                return false;
            }
            task->state = TASK_DEAD;
            reap_dead_tasks();
            return true;
        }
        task = (Task*)task->next;
    } while (task != NULL && task != g_head);

    return false;
}

void exit_current_task(void) {
    // We cannot free the stack we are currently using. Mark it dead; the
    // scheduler reclaims it after another task's stack becomes active.
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

    reap_dead_tasks();

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
