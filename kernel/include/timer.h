#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

struct timer_cpu_counters {
    uint64_t total_cycles;
    uint64_t idle_cycles;
};

void timer_init(void);
void timer_phase(int hz);
void timer_handler(void);
/* Runs deferred timer callback work in foreground context. */
void timer_poll(void);
void timer_wait(int ticks);
void timer_idle_wait(void);
void timer_interrupt_entry(void);
void timer_read_cpu_counters(struct timer_cpu_counters* counters);
uint64_t timer_get_ticks(void);
uint64_t timer_get_uptime(void);
uint64_t timer_get_milliseconds(void);

// Callback typedef
typedef void (*timer_callback_t)(void);
void timer_set_callback(timer_callback_t callback);

#endif
