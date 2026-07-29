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

#define STACK_SIZE 16384u
#define STACK_GUARD_SIZE ((size_t)PAGING_PAGE_SIZE)
#define STACK_ALLOCATION_SIZE \
    ((size_t)STACK_SIZE + (STACK_GUARD_SIZE * 2u))
#define STACK_CANARY_WORDS 8u
#define STACK_CANARY_SEED 0xD14FC0DEF00DBAADull
#define INITIAL_TASK_RFLAGS 0x202ull
#define CR0_MONITOR_COPROCESSOR (1ull << 1)
#define CR0_EMULATION (1ull << 2)
#define CR0_TASK_SWITCHED (1ull << 3)
#define CR0_NUMERIC_ERROR (1ull << 5)
#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE (1ull << 18)
#define CPUID_FEATURE_FPU (1u << 0)
#define CPUID_FEATURE_FXSR (1u << 24)
#define CPUID_FEATURE_SSE (1u << 25)
#define CPUID_FEATURE_SSE2 (1u << 26)

static bool g_fpu_context_enabled = false;
static uint8_t
    g_initial_fpu_state[SCHEDULER_FPU_STATE_SIZE]
        __attribute__((aligned(16)));

_Static_assert(
    offsetof(Task, fpu_state) % 16u == 0,
    "Task FPU save area must be 16-byte aligned");
_Static_assert(
    _Alignof(Task) >= 16u,
    "Task allocations must preserve FXSAVE alignment");

extern _Noreturn void enter_user_mode(uint64_t entry_point,
                                      uint64_t user_stack_top);

static _Noreturn void user_task_bootstrap(void);

static void copy_bytes(void* destination, const void* source, size_t count) {
    uint8_t* out = (uint8_t*)destination;
    const uint8_t* in = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        out[index] = in[index];
    }
}

static void disable_unmanaged_fpu_state(void) {
    /*
     * This is a compatibility fallback for an unusually limited CPU. Keep
     * CR0.TS set so an attempted x87/MMX/SIMD instruction receives #NM rather
     * than allowing unmanaged register state to cross task boundaries.
     */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_TASK_SWITCHED;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static bool initialize_fpu_contexts(void) {
    uint32_t feature_a;
    uint32_t feature_b;
    uint32_t feature_c;
    uint32_t feature_d;
    __asm__ volatile(
        "cpuid"
        : "=a"(feature_a), "=b"(feature_b),
          "=c"(feature_c), "=d"(feature_d)
        : "a"(1u), "c"(0u));
    (void)feature_a;
    (void)feature_b;
    (void)feature_c;

    const uint32_t required =
        CPUID_FEATURE_FPU | CPUID_FEATURE_FXSR |
        CPUID_FEATURE_SSE | CPUID_FEATURE_SSE2;
    if ((feature_d & required) != required) {
        disable_unmanaged_fpu_state();
        return false;
    }

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EMULATION | CR0_TASK_SWITCHED);
    cr0 |= CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    /*
     * FXSAVE does not preserve AVX YMM upper halves. Keep OSXSAVE disabled so
     * applications cannot enter an extended state this scheduler would omit.
     */
    cr4 &= ~CR4_OSXSAVE;
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    /*
     * Build the architectural reset image explicitly. FNINIT does not erase
     * the physical x87/MMX payload, and LDMXCSR does not clear XMM registers;
     * saving immediately after those instructions would copy privileged
     * residue into every new process.
     *
     * FXSAVE layout: FCW at 0, MXCSR at 24, x87/MMX at 32, and XMM at 160.
     * Zero means all x87 tags are empty and every data register starts clean.
     */
    for (size_t index = 0;
         index < sizeof(g_initial_fpu_state); index++) {
        g_initial_fpu_state[index] = 0;
    }
    g_initial_fpu_state[0] = 0x7fu;
    g_initial_fpu_state[1] = 0x03u; /* x87 control word 0x037F */
    g_initial_fpu_state[24] = 0x80u;
    g_initial_fpu_state[25] = 0x1fu; /* MXCSR 0x00001F80 */
    __asm__ volatile(
        "fxrstor64 %0"
        :
        : "m"(g_initial_fpu_state)
        : "memory");
    return true;
}

static void initialize_task_fpu(Task* task) {
    if (task == NULL || !g_fpu_context_enabled) return;
    copy_bytes(task->fpu_state, g_initial_fpu_state,
               sizeof(task->fpu_state));
}

static void save_task_fpu(Task* task) {
    if (task == NULL || !g_fpu_context_enabled) return;
    __asm__ volatile(
        "fxsave64 %0"
        : "=m"(task->fpu_state)
        :
        : "memory");
}

static void restore_task_fpu(const Task* task) {
    if (task == NULL || !g_fpu_context_enabled) return;
    __asm__ volatile(
        "fxrstor64 %0"
        :
        : "m"(task->fpu_state)
        : "memory");
}

static uint64_t stack_canary_value(const Task* task, size_t index) {
    return STACK_CANARY_SEED ^
           (task != NULL ? task->id * 0x9E3779B97F4A7C15ull : 0) ^
           (uint64_t)index;
}

static void initialize_stack_canary(Task* task) {
    if (task == NULL || task->kernel_stack_canary == NULL) return;
    for (size_t index = 0; index < STACK_CANARY_WORDS; index++) {
        task->kernel_stack_canary[index] =
            stack_canary_value(task, index);
    }
}

static bool stack_canary_intact(const Task* task) {
    if (task == NULL || task->kernel_stack_canary == NULL) return false;
    for (size_t index = 0; index < STACK_CANARY_WORDS; index++) {
        if (task->kernel_stack_canary[index] !=
            stack_canary_value(task, index)) {
            return false;
        }
    }
    return true;
}

static _Noreturn void halt_on_stack_corruption(void) {
    syslog_write(
        "Scheduler: KERNEL STACK CANARY CORRUPTED; system halted");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static bool allocate_guarded_kernel_stack(Task* task) {
    if (task == NULL) return false;

    void* raw = kmalloc(STACK_ALLOCATION_SIZE);
    if (raw == NULL) return false;

    const uintptr_t guard =
        ((uintptr_t)raw + STACK_GUARD_SIZE - 1u) &
        ~(uintptr_t)(STACK_GUARD_SIZE - 1u);
    const uintptr_t stack_base = guard + STACK_GUARD_SIZE;
    const uintptr_t stack_top = stack_base + STACK_SIZE;
    const uintptr_t allocation_end =
        (uintptr_t)raw + STACK_ALLOCATION_SIZE;
    if (stack_top > allocation_end ||
        !paging_kernel_guard_page((void*)guard)) {
        kfree(raw);
        return false;
    }

    task->kernel_stack = raw;
    task->kernel_stack_guard = (void*)guard;
    task->kernel_stack_canary = (uint64_t*)stack_base;
    task->kernel_stack_top = (uint64_t)stack_top;
    task->owns_kernel_stack = true;
    return true;
}

static void release_kernel_stack(Task* task) {
    if (task == NULL || !task->owns_kernel_stack) return;

    if (!paging_kernel_unguard_page(task->kernel_stack_guard)) {
        /*
         * Never return a still-unmapped page to the general heap. Leaking one
         * failed cleanup allocation is safer than poisoning future kmallocs.
         */
        syslog_write("Scheduler: failed to remove kernel stack guard");
        return;
    }
    kfree(task->kernel_stack);
    task->kernel_stack = NULL;
    task->kernel_stack_guard = NULL;
    task->kernel_stack_canary = NULL;
    task->owns_kernel_stack = false;
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
    if (!stack_canary_intact(task)) {
        halt_on_stack_corruption();
    }
    release_kernel_stack(task);
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
    *kmain_task = (Task){0};

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
    kmain_task->kernel_stack = g_kernel_stack;
    kmain_task->kernel_stack_guard = g_kernel_stack_guard_page;
    kmain_task->kernel_stack_canary = (uint64_t*)g_kernel_stack;
    kmain_task->owns_kernel_stack = false;
    kmain_task->user_stack = NULL;
    kmain_task->address_space = NULL;
    kmain_task->user_entry = 0;
    kmain_task->user_stack_top = 0;
    kmain_task->app_process_id = 0;
    kmain_task->app_capabilities = 0;
    kmain_task->quantum_ticks = 0;
    kmain_task->next = kmain_task; // Circular list

    if (!paging_kernel_guard_page(g_kernel_stack_guard_page)) {
        kfree(kmain_task);
        syslog_write(
            "Scheduler: bootstrap stack guard installation failed");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    initialize_stack_canary(kmain_task);
    g_fpu_context_enabled = initialize_fpu_contexts();
    if (g_fpu_context_enabled) {
        save_task_fpu(kmain_task);
    }

    g_head = kmain_task;
    g_current_task = kmain_task;
    
    syslog_write(g_fpu_context_enabled
        ? "Scheduler: guarded stacks; per-task x87/MMX/SSE state"
        : "Scheduler: guarded stacks; unsupported FPU state trapped");
}

extern void task_return_trampoline(void);

bool spawn_named_task(const char* name, void (*entry_point)(void)) {
    if (entry_point == NULL || g_head == NULL) return false;

    Task* new_task = (Task*)kmalloc(sizeof(Task));
    if (new_task == NULL) {
        syslog_write("Scheduler: task allocation failed");
        return false;
    }
    *new_task = (Task){0};

    if (!allocate_guarded_kernel_stack(new_task)) {
        kfree(new_task);
        syslog_write("Scheduler: guarded stack allocation failed");
        return false;
    }
    
    if (!allocate_task_id(&new_task->id)) {
        release_kernel_stack(new_task);
        kfree(new_task);
        syslog_write("Scheduler: PID allocation failed");
        return false;
    }
    copy_task_name(new_task->name,
                   name != NULL && name[0] != '\0' ? name : "kernel/task");
    new_task->is_user = false;
    new_task->state = TASK_READY;
    new_task->user_stack = NULL;
    new_task->address_space = NULL;
    new_task->user_entry = 0;
    new_task->user_stack_top = 0;
    new_task->app_process_id = 0;
    new_task->app_capabilities = 0;
    new_task->quantum_ticks = 0;
    initialize_stack_canary(new_task);
    initialize_task_fpu(new_task);
    
    uint64_t* sp =
        (uint64_t*)(uintptr_t)new_task->kernel_stack_top;
    
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
        (user_stack_top & 0xfull) != 0 ||
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
    *new_task = (Task){0};
    if (!allocate_guarded_kernel_stack(new_task)) {
        kfree(new_task);
        syslog_write(
            "Scheduler: user guarded-stack allocation failed");
        return false;
    }

    if (!allocate_task_id(&new_task->id)) {
        release_kernel_stack(new_task);
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
    new_task->user_stack = NULL;
    new_task->address_space = address_space;
    new_task->user_entry = entry_point;
    new_task->user_stack_top = user_stack_top;
    new_task->app_process_id = app_process_id;
    new_task->app_capabilities = app_capabilities;
    new_task->quantum_ticks = 0;
    initialize_stack_canary(new_task);
    initialize_task_fpu(new_task);

    uint64_t* sp =
        (uint64_t*)(uintptr_t)new_task->kernel_stack_top;
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
    if (!stack_canary_intact(g_current_task)) {
        halt_on_stack_corruption();
    }

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
    if (!stack_canary_intact(prev) ||
        !stack_canary_intact(next)) {
        halt_on_stack_corruption();
    }
    save_task_fpu(prev);
    restore_task_fpu(next);
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
