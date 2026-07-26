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

int main(void) {
    test_bottom_up();
    test_top_down();
    test_rejections();
    puts("bmp tests passed");
    return 0;
}
