#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_phase(int hz);
void timer_handler(void);
void timer_wait(int ticks);
uint64_t timer_get_ticks(void);
uint64_t timer_get_uptime(void);

// Callback typedef
typedef void (*timer_callback_t)(void);
void timer_set_callback(timer_callback_t callback);

#endif
