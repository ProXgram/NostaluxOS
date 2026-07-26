#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    TASK_READY,
    TASK_DEAD
} TaskState;

typedef struct {
    uint64_t id;
    uint64_t rsp;
    uint64_t kernel_stack_top; // For Ring 3 -> 0 transitions
    void* kernel_stack;
    void* user_stack;
    bool is_user;
    TaskState state;
    void* next;
} Task;

void scheduler_init(void);
bool spawn_task(void (*entry_point)(void));
/* User tasks remain disabled until separate user mappings are implemented. */
bool spawn_user_task(void (*entry_point)(void));
void schedule(void);
void exit_current_task(void);

// Assembly helper
extern void context_switch(uint64_t* old_sp_ptr, uint64_t new_sp);

#endif
