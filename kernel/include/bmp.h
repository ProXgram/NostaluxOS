#ifndef BMP_H
#define BMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bmp_image {
    const uint8_t* data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_offset;
    uint32_t row_stride;
    bool top_down;
};

bool bmp_open(const void* data, size_t size, struct bmp_image* image);
bool bmp_get_pixel(const struct bmp_image* image, uint32_t x, uint32_t y,
                   uint32_t* color);

#endif /* BMP_H */
