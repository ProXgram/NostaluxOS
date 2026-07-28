#include "timer.h"
#include "io.h"
#include "syslog.h"
#include "interrupts.h"
#include <stdbool.h>
#include <stddef.h>

#define PIT_FREQUENCY 1193180

static volatile uint64_t g_ticks = 0;
static int g_freq_hz = 100;
static timer_callback_t g_callback = NULL;
static volatile bool g_callback_due = false;
static uint64_t g_cpu_start_cycles = 0;
static volatile uint64_t g_cpu_idle_cycles = 0;
static volatile uint64_t g_cpu_idle_start = 0;
static volatile bool g_cpu_idle_waiting = false;

static uint64_t timer_save_and_disable_interrupts(void) {
    uint64_t saved_rflags;
    __asm__ volatile("pushfq; popq %0; cli"
                     : "=r"(saved_rflags)
                     :
                     : "memory");
    return saved_rflags;
}

static void timer_restore_interrupts(uint64_t saved_rflags) {
    if ((saved_rflags & (1ULL << 9)) != 0) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static uint64_t read_tsc_serialized(void) {
    uint32_t low;
    uint32_t high;

    /*
     * NostaluxOS is x86_64-only. LFENCE orders RDTSC against the surrounding
     * work so snapshots can be compared without needing a TSC frequency.
     */
    __asm__ volatile(
        "lfence\n\t"
        "rdtsc\n\t"
        "lfence"
        : "=a"(low), "=d"(high)
        :
        : "memory"
    );

    return ((uint64_t)high << 32) | low;
}

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
    uint64_t saved_rflags = timer_save_and_disable_interrupts();
    g_callback = callback;
    /*
     * Do not deliver a callback request accumulated for a previous owner.
     * The newly installed callback starts on the next four-tick boundary.
     */
    g_callback_due = false;
    timer_restore_interrupts(saved_rflags);
}

void timer_handler(void) {
    g_ticks++;

    /*
     * IRQ0 only publishes work. Framebuffer drawing and any future callback
     * work run from timer_poll() after the interrupt handler has returned.
     */
    if (g_ticks % 4 == 0) {
        g_callback_due = true;
    }
}

void timer_poll(void) {
    timer_callback_t callback = NULL;
    uint64_t saved_rflags = timer_save_and_disable_interrupts();

    if (g_callback_due) {
        g_callback_due = false;
        callback = g_callback;
    }

    timer_restore_interrupts(saved_rflags);

    if (callback != NULL) {
        callback();
    }
}

void timer_wait(int ticks) {
    if (ticks <= 0) return;
    uint64_t end = g_ticks + (uint64_t)ticks;
    uint64_t observed_tick = g_ticks;

    while (g_ticks < end) {
        uint64_t current_tick = g_ticks;
        if (current_tick != observed_tick) {
            observed_tick = current_tick;
            timer_poll();
        }
        __asm__ volatile("pause");
    }
    timer_poll();
}

void timer_idle_wait(void) {
    /*
     * Publish the idle interval with maskable interrupts disabled, then use
     * STI's one-instruction interrupt shadow to make the following HLT atomic
     * with respect to IRQ delivery. Without this sequence, an IRQ could close
     * the interval just before HLT and make the next sleep look like busy time.
     */
    __asm__ volatile("cli" : : : "memory");
    g_cpu_idle_start = read_tsc_serialized();
    g_cpu_idle_waiting = true;
    __asm__ volatile("sti\n\thlt" : : : "memory");

    /*
     * Normal IRQ handlers close the idle interval at entry so their work is
     * counted as busy. This fallback covers an unusual wake source that does
     * not pass through the PIC handlers. Mask interrupts while testing and
     * closing the interval so a late PIC IRQ cannot close it between those
     * operations and double-count the same idle time.
     */
    uint64_t saved_rflags = timer_save_and_disable_interrupts();
    if (g_cpu_idle_waiting) {
        uint64_t end = read_tsc_serialized();
        uint64_t start = g_cpu_idle_start;
        if (end >= start) g_cpu_idle_cycles += end - start;
        g_cpu_idle_waiting = false;
    }
    timer_restore_interrupts(saved_rflags);

    timer_poll();
}

void timer_interrupt_entry(void) {
    if (!g_cpu_idle_waiting) return;

    uint64_t end = read_tsc_serialized();
    uint64_t start = g_cpu_idle_start;
    if (end >= start) g_cpu_idle_cycles += end - start;
    g_cpu_idle_waiting = false;
}

void timer_read_cpu_counters(struct timer_cpu_counters* counters) {
    if (counters == NULL) return;

    uint64_t now = read_tsc_serialized();
    counters->total_cycles = now - g_cpu_start_cycles;
    counters->idle_cycles = g_cpu_idle_cycles;
}

uint64_t timer_get_ticks(void) { return g_ticks; }
uint64_t timer_get_uptime(void) {
    return g_freq_hz > 0 ? g_ticks / (uint64_t)g_freq_hz : 0;
}

uint64_t timer_get_milliseconds(void) {
    const uint64_t ticks = g_ticks;
    const uint64_t frequency =
        g_freq_hz > 0 ? (uint64_t)g_freq_hz : 1u;
    const uint64_t seconds = ticks / frequency;
    const uint64_t remainder = ticks % frequency;
    if (seconds > UINT64_MAX / 1000u) return UINT64_MAX;
    return seconds * 1000u + (remainder * 1000u) / frequency;
}

void timer_init(void) {
    /*
     * IRQ0 remains masked by interrupts_init() until all timer-visible state
     * and the PIT divisor are ready. Reset BIOS-era ticks before unmasking it.
     */
    g_ticks = 0;
    g_callback = NULL;
    g_callback_due = false;
    g_cpu_idle_cycles = 0;
    g_cpu_idle_start = 0;
    g_cpu_idle_waiting = false;
    g_cpu_start_cycles = read_tsc_serialized();
    timer_phase(100);
    interrupts_enable_irq(0);
    syslog_write("PIT: System timer initialized");
}
