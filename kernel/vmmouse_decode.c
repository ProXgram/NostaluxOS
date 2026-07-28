#include "vmmouse_decode.h"

#define VMMOUSE_COORDINATE_MAX 65535u
#define VMMOUSE_BUTTON_LEFT    0x20u
#define VMMOUSE_BUTTON_RIGHT   0x10u

static int scale_coordinate(uint32_t coordinate, int extent) {
    if (extent <= 1) return 0;
    if (coordinate > VMMOUSE_COORDINATE_MAX) {
        coordinate = VMMOUSE_COORDINATE_MAX;
    }

    return (int)(((uint64_t)coordinate * (uint64_t)(extent - 1) +
                  VMMOUSE_COORDINATE_MAX / 2u) /
                 VMMOUSE_COORDINATE_MAX);
}

MouseState vmmouse_decode_event(uint32_t buttons,
                                uint32_t absolute_x,
                                uint32_t absolute_y,
                                int framebuffer_width,
                                int framebuffer_height) {
    MouseState state = {
        .x = scale_coordinate(absolute_x, framebuffer_width),
        .y = scale_coordinate(absolute_y, framebuffer_height),
        .left_button = (buttons & VMMOUSE_BUTTON_LEFT) != 0,
        .right_button = (buttons & VMMOUSE_BUTTON_RIGHT) != 0,
    };

    return state;
}
