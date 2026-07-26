#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define TASK_NAME_MAX 32

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
bool spawn_task(void (*entry_point)(void));
/* User tasks remain disabled until separate user mappings are implemented. */
bool spawn_user_task(void (*entry_point)(void));
void schedule(void);
void exit_current_task(void);
size_t scheduler_task_count(void);
size_t scheduler_snapshot_tasks(struct scheduler_task_info* tasks, size_t capacity);
size_t scheduler_snapshot_tasks_from(size_t offset,
                                     struct scheduler_task_info* tasks,
                                     size_t capacity);
void scheduler_set_current_name(const char* name);

// Assembly helper
extern void context_switch(uint64_t* old_sp_ptr, uint64_t new_sp);

#endif
