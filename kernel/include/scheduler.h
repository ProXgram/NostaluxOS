#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define TASK_NAME_MAX 32
#define SCHEDULER_USER_QUANTUM_TICKS 5u

struct paging_address_space;

typedef enum {
    TASK_READY,
    TASK_DEAD
} TaskState;

typedef struct {
    uint64_t id;
    char name[TASK_NAME_MAX];
    uint64_t rsp;
    uint64_t kernel_stack_top; // For Ring 3 -> 0 transitions
    void* kernel_stack;
    void* user_stack;
    struct paging_address_space* address_space;
    uint64_t user_entry;
    uint64_t user_stack_top;
    uint64_t app_process_id;
    uint64_t app_capabilities;
    uint32_t quantum_ticks;
    bool is_user;
    TaskState state;
    void* next;
} Task;

struct scheduler_task_info {
    uint64_t id;
    char name[TASK_NAME_MAX];
    TaskState state;
    bool is_user;
    bool is_current;
};

void scheduler_init(void);
bool spawn_named_task(const char* name, void (*entry_point)(void));
bool spawn_task(void (*entry_point)(void));
/*
 * Legacy pointer-entry spawning remains disabled. User code must arrive as a
 * validated ELF image in an owned address space.
 */
bool spawn_user_task(void (*entry_point)(void));
bool scheduler_spawn_user_process(
    const char* name,
    struct paging_address_space* address_space,
    uint64_t entry_point,
    uint64_t user_stack_top,
    uint64_t app_process_id,
    uint64_t app_capabilities);
void schedule(void);
void exit_current_task(void);
_Noreturn void scheduler_exit_current_user(int64_t exit_code);
_Noreturn void scheduler_fault_current_user(uint8_t vector,
                                            bool has_error_code,
                                            uint64_t error_code,
                                            uint64_t instruction_pointer,
                                            uint64_t stack_pointer,
                                            uint64_t fault_address);
bool scheduler_current_is_user(void);
struct paging_address_space* scheduler_current_address_space(void);
uint64_t scheduler_current_app_capabilities(void);
uint64_t scheduler_current_app_process_id(void);
/*
 * Marks a non-current task dead and safely reclaims it. The currently
 * executing task must use exit_current_task() so its live stack is not freed.
 */
bool scheduler_terminate_task(uint64_t id);
/*
 * Called only after IRQ0 has been acknowledged. It may suspend the current
 * ring-3 interrupt frame and resume another task when a user quantum expires.
 */
void scheduler_timer_tick(void);
bool scheduler_terminate_app_process(uint64_t app_process_id);
size_t scheduler_task_count(void);
size_t scheduler_snapshot_tasks(struct scheduler_task_info* tasks, size_t capacity);
size_t scheduler_snapshot_tasks_from(size_t offset,
                                     struct scheduler_task_info* tasks,
                                     size_t capacity);
void scheduler_set_current_name(const char* name);

// Assembly helper
extern void context_switch(uint64_t* old_sp_ptr, uint64_t new_sp,
                           uint64_t saved_rflags);

#endif
