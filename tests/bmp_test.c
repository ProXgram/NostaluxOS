#include "bmp.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_BMP_SIZE 70u

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

static void make_bottom_up_bmp(uint8_t bmp[TEST_BMP_SIZE]) {
    memset(bmp, 0, TEST_BMP_SIZE);
    bmp[0] = 'B';
    bmp[1] = 'M';
    write_u32_le(bmp + 2, TEST_BMP_SIZE);
    write_u32_le(bmp + 10, 54u);
    write_u32_le(bmp + 14, 40u);
    write_u32_le(bmp + 18, 2u);
    write_u32_le(bmp + 22, 2u);
    write_u16_le(bmp + 26, 1u);
    write_u16_le(bmp + 28, 24u);
    write_u32_le(bmp + 34, 16u);

    /* Bottom row: blue, white. */
    bmp[54] = 0xFF;
    bmp[55] = 0x00;
    bmp[56] = 0x00;
    bmp[57] = 0xFF;
    bmp[58] = 0xFF;
    bmp[59] = 0xFF;

    /* Top row: red, green. */
    bmp[62] = 0x00;
    bmp[63] = 0x00;
    bmp[64] = 0xFF;
    bmp[65] = 0x00;
    bmp[66] = 0xFF;
    bmp[67] = 0x00;
}

static void expect_pixel(const struct bmp_image* image, uint32_t x, uint32_t y,
                         uint32_t expected) {
    uint32_t actual = 0;
    assert(bmp_get_pixel(image, x, y, &actual));
    assert(actual == expected);
}

static void test_bottom_up(void) {
    uint8_t bmp[TEST_BMP_SIZE];
    struct bmp_image image;
    make_bottom_up_bmp(bmp);

    assert(bmp_open(bmp, sizeof(bmp), &image));
    assert(image.width == 2u);
    assert(image.height == 2u);
    assert(!image.top_down);
    expect_pixel(&image, 0u, 0u, 0xFFFF0000u);
    expect_pixel(&image, 1u, 0u, 0xFF00FF00u);
    expect_pixel(&image, 0u, 1u, 0xFF0000FFu);
    expect_pixel(&image, 1u, 1u, 0xFFFFFFFFu);
    assert(!bmp_get_pixel(&image, 2u, 0u, NULL));
    assert(!bmp_get_pixel(&image, 2u, 0u, &(uint32_t){0}));
}

static void test_top_down(void) {
    uint8_t bmp[TEST_BMP_SIZE];
    uint8_t first_row[8];
    struct bmp_image image;
    make_bottom_up_bmp(bmp);

    memcpy(first_row, bmp + 54, sizeof(first_row));
    memcpy(bmp + 54, bmp + 62, sizeof(first_row));
    memcpy(bmp + 62, first_row, sizeof(first_row));
    write_u32_le(bmp + 22, 0xFFFFFFFEu);

    assert(bmp_open(bmp, sizeof(bmp), &image));
    assert(image.top_down);
    expect_pixel(&image, 0u, 0u, 0xFFFF0000u);
    expect_pixel(&image, 1u, 1u, 0xFFFFFFFFu);
}

static void expect_rejected(uint8_t bmp[TEST_BMP_SIZE]) {
    struct bmp_image image = {
        .data = (const uint8_t*)1,
        .data_size = 1,
        .width = 1,
        .height = 1,
        .pixel_offset = 1,
        .row_stride = 1,
        .top_down = true,
    };
    assert(!bmp_open(bmp, TEST_BMP_SIZE, &image));
    assert(image.data == NULL);
    assert(image.width == 0u);
    assert(image.height == 0u);
}

static void test_rejections(void) {
    uint8_t bmp[TEST_BMP_SIZE];
    struct bmp_image image;

    make_bottom_up_bmp(bmp);
    assert(!bmp_open(NULL, sizeof(bmp), &image));
    assert(!bmp_open(bmp, sizeof(bmp), NULL));
    assert(!bmp_open(bmp, 53u, &image));

    make_bottom_up_bmp(bmp);
    bmp[0] = 'X';
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 2, TEST_BMP_SIZE + 1u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 14, 39u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 10, 53u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 18, 0u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 22, 0x80000000u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u16_le(bmp + 26, 2u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u16_le(bmp + 28, 32u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 30, 1u);
    expect_rejected(bmp);

    make_bottom_up_bmp(bmp);
    write_u32_le(bmp + 22, 3u);
    expect_rejected(bmp);
}

static void test_encode_round_trip(void) {
    const uint32_t pixels[6] = {
        0x80FF0000u, 0x0000FF00u, 0xDEADBEEFu,
        0xFF0000FFu, 0x00FFFFFFu, 0xCAFEBABEu,
    };
    uint8_t bmp[TEST_BMP_SIZE];
    struct bmp_image image;
    size_t encoded_size = 123u;

    memset(bmp, 0xA5, sizeof(bmp));
    assert(bmp_encode_rgb24(pixels, 2u, 2u, 3u, bmp, sizeof(bmp),
                            &encoded_size));
    assert(encoded_size == TEST_BMP_SIZE);
    assert(bmp[0] == 'B' && bmp[1] == 'M');
    assert(bmp[54] == 0xFFu && bmp[55] == 0x00u && bmp[56] == 0x00u);
    assert(bmp[57] == 0xFFu && bmp[58] == 0xFFu && bmp[59] == 0xFFu);
    assert(bmp[60] == 0u && bmp[61] == 0u);
    assert(bmp[62] == 0x00u && bmp[63] == 0x00u && bmp[64] == 0xFFu);
    assert(bmp[65] == 0x00u && bmp[66] == 0xFFu && bmp[67] == 0x00u);
    assert(bmp[68] == 0u && bmp[69] == 0u);

    assert(bmp_open(bmp, encoded_size, &image));
    assert(!image.top_down);
    expect_pixel(&image, 0u, 0u, 0xFFFF0000u);
    expect_pixel(&image, 1u, 0u, 0xFF00FF00u);
    expect_pixel(&image, 0u, 1u, 0xFF0000FFu);
    expect_pixel(&image, 1u, 1u, 0xFFFFFFFFu);
}

static void test_encode_17_by_17(void) {
    uint32_t pixels[17u * 17u];
    uint8_t bmp[938u];
    struct bmp_image image;
    size_t encoded_size = 0;

    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        pixels[i] = (uint32_t)i * 0x010101u;
    }
    assert(bmp_encode_rgb24(pixels, 17u, 17u, 17u, bmp, sizeof(bmp),
                            &encoded_size));
    assert(encoded_size == 938u);
    assert(encoded_size < 1024u);
    assert(bmp_open(bmp, encoded_size, &image));
    assert(image.width == 17u && image.height == 17u);
    expect_pixel(&image, 0u, 0u, 0xFF000000u);
}

static void test_encode_rejections(void) {
    uint32_t pixels[4] = {0};
    uint8_t bmp[TEST_BMP_SIZE];
    uint8_t original[TEST_BMP_SIZE];
    size_t encoded_size = 99u;

    memset(bmp, 0x5Au, sizeof(bmp));
    memcpy(original, bmp, sizeof(bmp));
    assert(!bmp_encode_rgb24(pixels, 2u, 2u, 2u, bmp,
                             TEST_BMP_SIZE - 1u, &encoded_size));
    assert(encoded_size == 0u);
    assert(memcmp(bmp, original, sizeof(bmp)) == 0);

    encoded_size = 99u;
    assert(!bmp_encode_rgb24(NULL, 2u, 2u, 2u, bmp, sizeof(bmp),
                             &encoded_size));
    assert(encoded_size == 0u);
    assert(!bmp_encode_rgb24(pixels, 2u, 2u, 2u, NULL, sizeof(bmp),
                             &encoded_size));
    assert(!bmp_encode_rgb24(pixels, 0u, 2u, 2u, bmp, sizeof(bmp),
                             &encoded_size));
    assert(!bmp_encode_rgb24(pixels, 2u, 0u, 2u, bmp, sizeof(bmp),
                             &encoded_size));
    assert(!bmp_encode_rgb24(pixels, 2u, 2u, 1u, bmp, sizeof(bmp),
                             &encoded_size));
    assert(!bmp_encode_rgb24(pixels, 2u, 2u, 2u, bmp, sizeof(bmp), NULL));
    assert(!bmp_encode_rgb24(pixels, UINT32_MAX, 1u, UINT32_MAX, bmp,
                             sizeof(bmp), &encoded_size));
    assert(!bmp_encode_rgb24(pixels, 1u, UINT32_MAX, SIZE_MAX, bmp,
                             sizeof(bmp), &encoded_size));
}

int main(void) {
    test_bottom_up();
    test_top_down();
    test_rejections();
    test_encode_round_trip();
    test_encode_17_by_17();
    test_encode_rejections();
    puts("bmp tests passed");
    return 0;
}
