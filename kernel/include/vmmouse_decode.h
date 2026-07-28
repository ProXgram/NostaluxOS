#ifndef VMMOUSE_DECODE_H
#define VMMOUSE_DECODE_H

#include <stdint.h>

#include "mouse.h"

/*
 * Converts one QEMU VMMouse absolute packet into framebuffer coordinates.
 * Kept free of port I/O so the protocol mapping is host-testable.
 */
MouseState vmmouse_decode_event(uint32_t buttons,
                                uint32_t absolute_x,
                                uint32_t absolute_y,
                                int framebuffer_width,
                                int framebuffer_height);

#endif /* VMMOUSE_DECODE_H */
