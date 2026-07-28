#include "scheduler.h"
#include "app_process.h"
#include "heap.h"
#include "syslog.h"
#include "gdt.h"
#include "kstdio.h"
#include "paging.h"

static Task* g_current_task = NULL;
static Task* g_head = NULL;
static uint64_t g_next_pid = 1;

#define STACK_SIZE 16384
#define INITIAL_TASK_RFLAGS 0x202ull
#define CR0_TASK_SWITCHED (1ull << 3)

extern _Noreturn void enter_user_mode(uint64_t entry_point,
                                      uint64_t user_stack_top);

static _Noreturn void user_task_bootstrap(void);

static void disable_unmanaged_fpu_state(void) {
    /*
     * Apps v1 does not yet save x87/MMX/SIMD state during a context switch.
     * Keep CR0.TS set so an app attempting to use that state receives #NM,
     * which the ring-3 exception path contains instead of leaking another
     * process's register state.
     */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_TASK_SWITCHED;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

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
    paging_address_space_destroy(task->address_space);
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
    kmain_task->kernel_stack_top = (uint64_t)g_kernel_stack_top;
    kmain_task->kernel_stack = NULL;
    kmain_task->user_stack = NULL;
    kmain_task->address_space = NULL;
    kmain_task->user_entry = 0;
    kmain_task->user_stack_top = 0;
    kmain_task->app_process_id = 0;
    kmain_task->app_capabilities = 0;
    kmain_task->quantum_ticks = 0;
    kmain_task->next = kmain_task; // Circular list

    g_head = kmain_task;
    g_current_task = kmain_task;
    disable_unmanaged_fpu_state();
    
    syslog_write(
        "Scheduler: cooperative kernel/preemptive user tasks; FPU trapped");
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
    new_task->address_space = NULL;
    new_task->user_entry = 0;
    new_task->user_stack_top = 0;
    new_task->app_process_id = 0;
    new_task->app_capabilities = 0;
    new_task->quantum_ticks = 0;
    
    uint64_t* sp = (uint64_t*)(stack + STACK_SIZE);
    
    // Synthetic return address used when the task entry function returns.
    *(--sp) = (uint64_t)task_return_trampoline;

    // Return address for context_switch
    *(--sp) = (uint64_t)entry_point;

    // RFLAGS restored by context_switch before RET.
    *(--sp) = INITIAL_TASK_RFLAGS;
    
    // Callee-saved registers, in reverse of context_switch's pop order.
    *(--sp) = 0; // RBX
    *(--sp) = 0; // RBP
    *(--sp) = 0; // R12
    *(--sp) = 0; // R13
    *(--sp) = 0; // R14
    *(--sp) = 0; // R15
    
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
    syslog_write("Scheduler: raw user entry points are unsupported");
    return false;
}

bool scheduler_spawn_user_process(
    const char* name,
    struct paging_address_space* address_space,
    uint64_t entry_point,
    uint64_t user_stack_top,
    uint64_t app_process_id,
    uint64_t app_capabilities) {
    if (g_head == NULL || address_space == NULL ||
        app_process_id == 0 ||
        (app_capabilities & ~APP_CAPABILITY_ALL) != 0 ||
        entry_point < PAGING_USER_BASE ||
        entry_point >= PAGING_USER_LIMIT ||
        user_stack_top <= PAGING_USER_BASE ||
        user_stack_top > PAGING_USER_LIMIT ||
        !paging_user_range_mapped(address_space, entry_point, 1, false) ||
        !paging_user_range_mapped(address_space,
                                  user_stack_top - sizeof(uint64_t),
                                  sizeof(uint64_t), true)) {
        return false;
    }

    Task* new_task = (Task*)kmalloc(sizeof(Task));
    if (new_task == NULL) {
        syslog_write("Scheduler: user task allocation failed");
        return false;
    }
    uint8_t* kernel_stack = (uint8_t*)kmalloc(STACK_SIZE);
    if (kernel_stack == NULL) {
        kfree(new_task);
        syslog_write("Scheduler: user kernel-stack allocation failed");
        return false;
    }

    if (!allocate_task_id(&new_task->id)) {
        kfree(kernel_stack);
        kfree(new_task);
        syslog_write("Scheduler: PID allocation failed");
        return false;
    }

    copy_task_name(new_task->name,
                   name != NULL && name[0] != '\0'
                       ? name
                       : "app/user");
    new_task->is_user = true;
    new_task->state = TASK_READY;
    new_task->kernel_stack = kernel_stack;
    new_task->user_stack = NULL;
    new_task->address_space = address_space;
    new_task->user_entry = entry_point;
    new_task->user_stack_top = user_stack_top;
    new_task->app_process_id = app_process_id;
    new_task->app_capabilities = app_capabilities;
    new_task->quantum_ticks = 0;
    new_task->kernel_stack_top =
        (uint64_t)(kernel_stack + STACK_SIZE);

    uint64_t* sp = (uint64_t*)(kernel_stack + STACK_SIZE);
    *(--sp) = (uint64_t)task_return_trampoline;
    *(--sp) = (uint64_t)user_task_bootstrap;
    *(--sp) = INITIAL_TASK_RFLAGS;
    *(--sp) = 0; // RBX
    *(--sp) = 0; // RBP
    *(--sp) = 0; // R12
    *(--sp) = 0; // R13
    *(--sp) = 0; // R14
    *(--sp) = 0; // R15
    new_task->rsp = (uint64_t)sp;

    new_task->next = g_head->next;
    g_head->next = new_task;
    return true;
}

static _Noreturn void user_task_bootstrap(void) {
    Task* task = g_current_task;
    if (task == NULL || !task->is_user ||
        task->address_space == NULL ||
        app_process_mark_running(task->app_process_id) != APP_PROCESS_OK) {
        syslog_write("Scheduler: user task bootstrap rejected");
        exit_current_task();
        for (;;) __asm__ volatile("cli; hlt");
    }

    enter_user_mode(task->user_entry, task->user_stack_top);
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
            if (task->is_user &&
                app_process_mark_exited(task->app_process_id, -1) !=
                    APP_PROCESS_OK) {
                syslog_write(
                    "Scheduler: terminated app metadata update failed");
            }
            task->state = TASK_DEAD;
            reap_dead_tasks();
            return true;
        }
        task = (Task*)task->next;
    } while (task != NULL && task != g_head);

    return false;
}

bool scheduler_terminate_app_process(uint64_t app_process_id) {
    if (app_process_id == 0 || g_head == NULL) return false;

    Task* task = g_head;
    do {
        if (task->is_user &&
            task->app_process_id == app_process_id) {
            if (task == g_current_task || task->state == TASK_DEAD) {
                return false;
            }
            if (app_process_mark_exited(task->app_process_id, -1) !=
                APP_PROCESS_OK) {
                syslog_write(
                    "Scheduler: stopped app metadata update failed");
            }
            task->state = TASK_DEAD;
            reap_dead_tasks();
            return true;
        }
        task = (Task*)task->next;
    } while (task != NULL && task != g_head);

    return false;
}

bool scheduler_current_is_user(void) {
    return g_current_task != NULL &&
           g_current_task->is_user &&
           g_current_task->state == TASK_READY;
}

struct paging_address_space* scheduler_current_address_space(void) {
    return scheduler_current_is_user()
        ? g_current_task->address_space
        : NULL;
}

uint64_t scheduler_current_app_capabilities(void) {
    return scheduler_current_is_user()
        ? g_current_task->app_capabilities
        : 0;
}

uint64_t scheduler_current_app_process_id(void) {
    return scheduler_current_is_user()
        ? g_current_task->app_process_id
        : 0;
}

void scheduler_timer_tick(void) {
    if (!scheduler_current_is_user()) return;

    if (g_current_task->quantum_ticks <
        SCHEDULER_USER_QUANTUM_TICKS) {
        g_current_task->quantum_ticks++;
    }
    if (g_current_task->quantum_ticks <
        SCHEDULER_USER_QUANTUM_TICKS) {
        return;
    }

    /*
     * IRQ0 has already received its EOI before this function is called. The
     * suspended interrupt handler remains on this task's private kernel stack
     * and finishes its IRET when the task is selected again.
     */
    g_current_task->quantum_ticks = 0;
    schedule();
}

_Noreturn void scheduler_exit_current_user(int64_t exit_code) {
    if (!scheduler_current_is_user()) {
        syslog_write("Scheduler: rejected user exit outside a user task");
        for (;;) __asm__ volatile("cli; hlt");
    }

    if (app_process_mark_exited(g_current_task->app_process_id,
                                exit_code) != APP_PROCESS_OK) {
        syslog_write("Scheduler: app exit metadata update failed");
    }
    g_current_task->state = TASK_DEAD;
    schedule();
    for (;;) __asm__ volatile("cli; hlt");
}

_Noreturn void scheduler_fault_current_user(uint8_t vector,
                                            bool has_error_code,
                                            uint64_t error_code,
                                            uint64_t instruction_pointer,
                                            uint64_t stack_pointer,
                                            uint64_t fault_address) {
    if (!scheduler_current_is_user()) {
        syslog_write("Scheduler: rejected user fault outside a user task");
        for (;;) __asm__ volatile("cli; hlt");
    }

    const struct app_fault_record fault = {
        .vector = vector,
        .has_error_code = has_error_code,
        .error_code = error_code,
        .instruction_pointer = instruction_pointer,
        .stack_pointer = stack_pointer,
        .fault_address = fault_address,
    };
    if (app_process_record_fault(g_current_task->app_process_id,
                                 &fault) != APP_PROCESS_OK) {
        syslog_write("Scheduler: app fault metadata update failed");
    }
    syslog_write("Scheduler: contained and terminated a user exception");
    g_current_task->state = TASK_DEAD;
    schedule();
    for (;;) __asm__ volatile("cli; hlt");
}

void exit_current_task(void) {
    // We cannot free the stack we are currently using. Mark it dead; the
    // scheduler reclaims it after another task's stack becomes active.
    if (g_current_task) {
        if (g_current_task->is_user &&
            app_process_mark_exited(g_current_task->app_process_id, -1) !=
                APP_PROCESS_OK) {
            syslog_write("Scheduler: app bootstrap exit update failed");
        }
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
    uint64_t saved_rflags;
    __asm__ volatile("pushfq; popq %0; cli"
                     : "=r"(saved_rflags)
                     :
                     : "memory");
    g_current_task = next;
    prev->quantum_ticks = 0;
    next->quantum_ticks = 0;
    
    if (next->kernel_stack_top != 0) {
        gdt_set_kernel_stack(next->kernel_stack_top);
    }

    paging_activate(next->address_space);
    context_switch(&prev->rsp, next->rsp, saved_rflags);
    reap_dead_tasks();
}
