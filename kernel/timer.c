#include "timer.h"
#include "io.h"
#include "syslog.h"
#include "interrupts.h"
#include <stddef.h> 

#define PIT_FREQUENCY 1193180

static volatile uint64_t g_ticks = 0;
static int g_freq_hz = 100;
static timer_callback_t g_callback = NULL;

void timer_phase(int hz) {
    if (hz <= 0) hz = 100;
    if (hz > PIT_FREQUENCY) hz = PIT_FREQUENCY;
    uint32_t divisor = (uint32_t)(PIT_FREQUENCY / hz);
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;
    g_freq_hz = (int)(PIT_FREQUENCY / divisor);
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_set_callback(timer_callback_t callback) {
    g_callback = callback;
}

void timer_handler(void) {
    g_ticks++;
    
    if (g_callback != NULL && (g_ticks % 4 == 0)) {
        g_callback();
    }

}

void timer_wait(int ticks) {
    if (ticks <= 0) return;
    uint64_t end = g_ticks + (uint64_t)ticks;
    while (g_ticks < end) {
        __asm__ volatile("pause");
    }
}

uint64_t timer_get_ticks(void) { return g_ticks; }
uint64_t timer_get_uptime(void) {
    return g_freq_hz > 0 ? g_ticks / (uint64_t)g_freq_hz : 0;
}

void timer_init(void) {
    timer_phase(100);
    g_callback = NULL;
    interrupts_enable_irq(0);
    syslog_write("PIT: System timer initialized");
}
