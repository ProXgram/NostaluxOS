#include "bmp.h"

#include <limits.h>

#define BMP_FILE_HEADER_SIZE 14u
#define BMP_INFO_HEADER_SIZE 40u
#define BMP_MINIMUM_SIZE (BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE)
#define BMP_PIXEL_FORMAT_24 24u
#define BMP_COMPRESSION_RGB 0u

static uint16_t read_u16_le(const uint8_t* bytes) {
    return (uint16_t)bytes[0] |
           (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int32_t read_i32_le(const uint8_t* bytes) {
    return (int32_t)read_u32_le(bytes);
}

static void write_u16_le(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void clear_image(struct bmp_image* image) {
    image->data = NULL;
    image->data_size = 0;
    image->width = 0;
    image->height = 0;
    image->pixel_offset = 0;
    image->row_stride = 0;
    image->top_down = false;
}

bool bmp_open(const void* data, size_t size, struct bmp_image* image) {
    if (image == NULL) return false;
    clear_image(image);
    if (data == NULL || size < BMP_MINIMUM_SIZE) return false;

    const uint8_t* bytes = (const uint8_t*)data;
    if (bytes[0] != 'B' || bytes[1] != 'M') return false;

    uint32_t declared_size = read_u32_le(bytes + 2);
    uint32_t pixel_offset = read_u32_le(bytes + 10);
    uint32_t dib_size = read_u32_le(bytes + 14);
    if (declared_size < BMP_MINIMUM_SIZE ||
        (uint64_t)declared_size > (uint64_t)size ||
        dib_size < BMP_INFO_HEADER_SIZE) {
        return false;
    }

    uint64_t header_end = (uint64_t)BMP_FILE_HEADER_SIZE + dib_size;
    if (header_end > declared_size || pixel_offset < header_end ||
        pixel_offset > declared_size) {
        return false;
    }

    int32_t signed_width = read_i32_le(bytes + 18);
    int32_t signed_height = read_i32_le(bytes + 22);
    uint16_t planes = read_u16_le(bytes + 26);
    uint16_t bits_per_pixel = read_u16_le(bytes + 28);
    uint32_t compression = read_u32_le(bytes + 30);

    if (signed_width <= 0 || signed_height == 0 ||
        signed_height == INT32_MIN || planes != 1u ||
        bits_per_pixel != BMP_PIXEL_FORMAT_24 ||
        compression != BMP_COMPRESSION_RGB) {
        return false;
    }

    uint32_t width = (uint32_t)signed_width;
    uint32_t height = signed_height < 0
        ? (uint32_t)(-(int64_t)signed_height)
        : (uint32_t)signed_height;
    uint64_t packed_row_bytes = (uint64_t)width * 3u;
    uint64_t row_stride = (packed_row_bytes + 3u) & ~(uint64_t)3u;
    if (row_stride > UINT32_MAX) return false;

    uint64_t pixel_bytes = row_stride * height;
    uint64_t required_size = (uint64_t)pixel_offset + pixel_bytes;
    if (required_size > declared_size || required_size > size) return false;

    image->data = bytes;
    image->data_size = declared_size;
    image->width = width;
    image->height = height;
    image->pixel_offset = pixel_offset;
    image->row_stride = (uint32_t)row_stride;
    image->top_down = signed_height < 0;
    return true;
}

bool bmp_get_pixel(const struct bmp_image* image, uint32_t x, uint32_t y,
                   uint32_t* color) {
    if (image == NULL || color == NULL || image->data == NULL ||
        x >= image->width || y >= image->height) {
        return false;
    }

    uint32_t source_row = image->top_down
        ? y
        : image->height - 1u - y;
    uint64_t offset = (uint64_t)image->pixel_offset +
                      (uint64_t)source_row * image->row_stride +
                      (uint64_t)x * 3u;
    if (offset + 3u > image->data_size) return false;

    const uint8_t* pixel = image->data + (size_t)offset;
    uint32_t blue = pixel[0];
    uint32_t green = pixel[1];
    uint32_t red = pixel[2];
    *color = 0xFF000000u | (red << 16) | (green << 8) | blue;
    return true;
}

bool bmp_encode_rgb24(const uint32_t* pixels, uint32_t width, uint32_t height,
                      size_t source_stride_pixels, void* output,
                      size_t output_capacity, size_t* encoded_size) {
    if (encoded_size == NULL) return false;
    *encoded_size = 0;

    if (pixels == NULL || output == NULL || width == 0u || height == 0u ||
        width > (uint32_t)INT32_MAX || height > (uint32_t)INT32_MAX ||
        source_stride_pixels < (size_t)width) {
        return false;
    }

    /*
     * Validate the complete source span before using row pointer arithmetic.
     * This cannot prove the caller's allocation size, but it prevents every
     * representational overflow implied by the supplied dimensions/stride.
     */
    size_t source_pixel_count = (size_t)width;
    if (height > 1u) {
        size_t preceding_rows = (size_t)height - 1u;
        if (source_stride_pixels > (SIZE_MAX - source_pixel_count) /
                                    preceding_rows) {
            return false;
        }
        source_pixel_count += preceding_rows * source_stride_pixels;
    }
    if (source_pixel_count > SIZE_MAX / sizeof(*pixels)) return false;

    uint64_t packed_row_bytes = (uint64_t)width * 3u;
    uint64_t row_stride = (packed_row_bytes + 3u) & ~(uint64_t)3u;
    uint64_t pixel_bytes = row_stride * (uint64_t)height;
    uint64_t total_size = (uint64_t)BMP_MINIMUM_SIZE + pixel_bytes;
    if (row_stride > UINT32_MAX || pixel_bytes > UINT32_MAX ||
        total_size > UINT32_MAX || total_size > (uint64_t)SIZE_MAX ||
        total_size > (uint64_t)output_capacity) {
        return false;
    }

    size_t size = (size_t)total_size;
    uint8_t* bytes = (uint8_t*)output;
    for (size_t i = 0; i < size; i++) bytes[i] = 0;

    bytes[0] = 'B';
    bytes[1] = 'M';
    write_u32_le(bytes + 2, (uint32_t)total_size);
    write_u32_le(bytes + 10, BMP_MINIMUM_SIZE);
    write_u32_le(bytes + 14, BMP_INFO_HEADER_SIZE);
    write_u32_le(bytes + 18, width);
    write_u32_le(bytes + 22, height);
    write_u16_le(bytes + 26, 1u);
    write_u16_le(bytes + 28, BMP_PIXEL_FORMAT_24);
    write_u32_le(bytes + 30, BMP_COMPRESSION_RGB);
    write_u32_le(bytes + 34, (uint32_t)pixel_bytes);

    for (uint32_t file_row = 0; file_row < height; file_row++) {
        uint32_t source_y = height - 1u - file_row;
        const uint32_t* source_row =
            pixels + (size_t)source_y * source_stride_pixels;
        uint8_t* destination_row =
            bytes + BMP_MINIMUM_SIZE + (size_t)file_row * (size_t)row_stride;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t color = source_row[x];
            destination_row[(size_t)x * 3u] = (uint8_t)color;
            destination_row[(size_t)x * 3u + 1u] = (uint8_t)(color >> 8);
            destination_row[(size_t)x * 3u + 2u] = (uint8_t)(color >> 16);
        }
    }

    *encoded_size = size;
    return true;
}
