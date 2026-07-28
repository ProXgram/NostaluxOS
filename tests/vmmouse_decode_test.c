#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vmmouse_decode.h"

static void require(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void) {
    MouseState origin = vmmouse_decode_event(0, 0, 0, 800, 600);
    require(origin.x == 0 && origin.y == 0,
            "absolute origin should map to framebuffer origin");
    require(!origin.left_button && !origin.right_button,
            "empty button mask should publish released buttons");

    MouseState corner =
        vmmouse_decode_event(0x20u | 0x10u, 65535u, 65535u, 800, 600);
    require(corner.x == 799 && corner.y == 599,
            "absolute maximum should map inside the last framebuffer pixel");
    require(corner.left_button && corner.right_button,
            "VMMouse left/right masks should publish pressed buttons");

    MouseState quarter =
        vmmouse_decode_event(0x20u, 32768u, 16384u, 800, 600);
    require(quarter.x == 400 && quarter.y == 150,
            "fractional absolute coordinates should round to nearest pixel");
    require(quarter.left_button && !quarter.right_button,
            "left-only packets should not publish a right click");

    MouseState clamped =
        vmmouse_decode_event(0x10u | 0x08u, UINT32_MAX, UINT32_MAX, 1, 0);
    require(clamped.x == 0 && clamped.y == 0,
            "empty or one-pixel dimensions should stay at coordinate zero");
    require(!clamped.left_button && clamped.right_button,
            "middle/unknown bits must not alias the left/right buttons");

    puts("VMMouse coordinate/button decode tests passed.");
    return 0;
}
