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

/*
 * Encode top-down rows of 0xAARRGGBB/0x00RRGGBB pixels as an uncompressed,
 * bottom-up 24-bit BMP. Alpha is ignored. source_stride_pixels is the number
 * of uint32_t pixels between successive source rows and may exceed width.
 *
 * output and pixels must not overlap. On success, encoded_size receives the
 * exact number of bytes written. On failure, encoded_size is set to zero and
 * output is left unchanged.
 */
bool bmp_encode_rgb24(const uint32_t* pixels, uint32_t width, uint32_t height,
                      size_t source_stride_pixels, void* output,
                      size_t output_capacity, size_t* encoded_size);

#endif /* BMP_H */
