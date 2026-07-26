#include "sound.h"
#include "io.h"
#include "timer.h"

#define PIT_INPUT_HZ    1193180u
#define PIT_DIVISOR_MAX 0xFFFFu

void sound_init(void) {
    // PC speaker doesn't strictly require init, but we ensure it's off
    sound_stop();
}

void sound_play(uint32_t frequency) {
    if (frequency == 0) {
        sound_stop();
        return;
    }

    uint32_t divisor = PIT_INPUT_HZ / frequency;
    if (divisor == 0) divisor = 1;
    if (divisor > PIT_DIVISOR_MAX) divisor = PIT_DIVISOR_MAX;

    // Configure PIT Channel 2
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    // Enable PC Speaker via Port 0x61
    // Bit 0: Gate 2 (Enable PIT 2)
    // Bit 1: Speaker Data (Connect output to speaker)
    uint8_t temp = inb(0x61);
    if (temp != (temp | 3)) {
        outb(0x61, temp | 3);
    }
}

void sound_stop(void) {
    uint8_t temp = inb(0x61) & 0xFC;
    outb(0x61, temp);
}

void sound_beep(uint32_t frequency, int duration_ticks) {
    sound_play(frequency);
    timer_wait(duration_ticks);
    sound_stop();
}
