#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

// Initialize sound subsystem (if needed)
void sound_init(void);

// Play a sound at a specific frequency (Hz)
void sound_play(uint32_t frequency);

// Stop the current sound
void sound_stop(void);

// Beep for a specific duration (in timer ticks)
// Note: This is blocking
void sound_beep(uint32_t frequency, int duration_ticks);

#endif
