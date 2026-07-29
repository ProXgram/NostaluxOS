#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ui.h"

#define IMAGE_CAPACITY 8192u

struct app_bmp {
    const uint8_t* bytes;
    size_t size;
    uint32_t data_offset;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride;
    bool top_down;
};

static uint16_t image_u16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] |
           ((uint16_t)bytes[1] << 8);
}

static uint32_t image_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool image_parse_bmp(const uint8_t* bytes, size_t size,
                            struct app_bmp* image) {
    if (bytes == NULL || image == NULL || size < 54u ||
        bytes[0] != 'B' || bytes[1] != 'M') {
        return false;
    }

    uint32_t declared_size = image_u32(bytes + 2);
    uint32_t offset = image_u32(bytes + 10);
    uint32_t dib_size = image_u32(bytes + 14);
    if (declared_size < 54u || declared_size > size ||
        dib_size < 40u) {
        return false;
    }
    uint64_t header_end = 14u + (uint64_t)dib_size;
    if (header_end > declared_size ||
        offset < header_end || offset > declared_size) {
        return false;
    }

    int32_t signed_width = (int32_t)image_u32(bytes + 18);
    int32_t signed_height = (int32_t)image_u32(bytes + 22);
    if (signed_width <= 0 ||
        signed_height == 0 || signed_height == INT32_MIN ||
        image_u16(bytes + 26) != 1u ||
        image_u16(bytes + 28) != 24u ||
        image_u32(bytes + 30) != 0u) {
        return false;
    }
    uint32_t width = (uint32_t)signed_width;
    uint32_t height =
        signed_height < 0
            ? (uint32_t)(-signed_height)
            : (uint32_t)signed_height;
    uint64_t row_bytes = (uint64_t)width * 3u;
    uint64_t row_stride = (row_bytes + 3u) & ~3ull;
    uint64_t required = (uint64_t)offset + row_stride * height;
    if (row_stride > UINT32_MAX ||
        required > declared_size || required > size) {
        return false;
    }

    image->bytes = bytes;
    image->size = declared_size;
    image->data_offset = offset;
    image->width = width;
    image->height = height;
    image->row_stride = (uint32_t)row_stride;
    image->top_down = signed_height < 0;
    return true;
}

static uint32_t image_pixel(const struct app_bmp* image,
                            uint32_t x, uint32_t y) {
    uint32_t source_y =
        image->top_down ? y : image->height - 1u - y;
    size_t offset =
        image->data_offset +
        (size_t)source_y * image->row_stride +
        (size_t)x * 3u;
    const uint8_t* pixel = image->bytes + offset;
    return 0xff000000u |
           ((uint32_t)pixel[2] << 16) |
           ((uint32_t)pixel[1] << 8) |
           pixel[0];
}

static void image_render(struct app_ui* ui,
                         const struct app_bmp* image,
                         const char* path) {
    app_ui_clear(ui, 0xff202631u);
    app_ui_fill(ui, 0, 0, (int32_t)ui->width, 32, 0xff18324bu);
    app_ui_text(ui, 12, 9, "IMAGE VIEWER -", 0xffffffffu, 1);
    app_ui_text(ui, 102, 9, path, 0xffffffffu, 1);

    const int32_t area_x = 12;
    const int32_t area_y = 43;
    const uint32_t area_width = ui->width - 24u;
    const uint32_t area_height = ui->height - 76u;
    for (uint32_t y = 0; y < area_height; y++) {
        for (uint32_t x = 0; x < area_width; x++) {
            uint32_t checker =
                (((x / 10u) + (y / 10u)) & 1u) != 0
                    ? 0xffb8bec7u : 0xffd9dde3u;
            app_ui_pixel(ui, area_x + (int32_t)x,
                         area_y + (int32_t)y, checker);
        }
    }

    if (image == NULL) {
        app_ui_text(ui, 34, 118, "INVALID OR MISSING 24-BIT BMP",
                    0xffb3261eu, 2);
        app_ui_text(ui, 12, (int32_t)ui->height - 22,
                    "FILE BYTES WERE REJECTED", 0xfff3b7b3u, 1);
        (void)app_ui_present(ui);
        return;
    }

    uint32_t destination_width = area_width;
    uint32_t destination_height =
        (uint32_t)(((uint64_t)image->height * destination_width) /
                   image->width);
    if (destination_height > area_height) {
        destination_height = area_height;
        destination_width =
            (uint32_t)(((uint64_t)image->width * destination_height) /
                       image->height);
    }
    if (destination_width == 0) destination_width = 1;
    if (destination_height == 0) destination_height = 1;

    int32_t destination_x =
        area_x + (int32_t)(area_width - destination_width) / 2;
    int32_t destination_y =
        area_y + (int32_t)(area_height - destination_height) / 2;
    for (uint32_t y = 0; y < destination_height; y++) {
        uint32_t source_y =
            (uint32_t)(((uint64_t)y * image->height) /
                       destination_height);
        for (uint32_t x = 0; x < destination_width; x++) {
            uint32_t source_x =
                (uint32_t)(((uint64_t)x * image->width) /
                           destination_width);
            app_ui_pixel(ui, destination_x + (int32_t)x,
                         destination_y + (int32_t)y,
                         image_pixel(image, source_x, source_y));
        }
    }
    app_ui_text(ui, 12, (int32_t)ui->height - 22,
                "REAL FILE DATA - 24-BIT BMP", 0xffd9e7f4u, 1);
    (void)app_ui_present(ui);
}

__attribute__((noreturn)) void nostalux_app_entry(void) {
    char path[APP_STARTUP_ARGUMENT_MAX + 1u];
    uint64_t argument_length =
        app_argument_get(path, sizeof(path));
    if (!app_result_ok(argument_length) ||
        argument_length > APP_STARTUP_ARGUMENT_MAX) {
        app_log("Image Viewer: invalid startup filename.");
        app_exit(1);
    }
    if (argument_length == 0) {
        app_text_copy(path, sizeof(path), "nostalux.bmp");
    }
    uint8_t bytes[IMAGE_CAPACITY];
    size_t size = 0;

    uint64_t handle =
        app_file_open(
            path, app_text_length(path), APP_FILE_OPEN_READ);
    if (app_result_ok(handle)) {
        while (size < sizeof(bytes)) {
            size_t request = sizeof(bytes) - size;
            if (request > APP_FILE_TRANSFER_MAX) {
                request = APP_FILE_TRANSFER_MAX;
            }
            uint64_t read =
                app_file_read(handle, bytes + size, request, size);
            if (!app_result_ok(read) || read == 0) break;
            size += (size_t)read;
            if (read < request) break;
        }
        (void)app_file_close(handle);
    }

    struct app_bmp image;
    bool valid = image_parse_bmp(bytes, size, &image);
    struct app_ui ui;
    if (!app_ui_open(&ui, 440, 320, "Image Viewer (isolated)")) {
        app_log("Image Viewer: open it from the graphical desktop.");
        app_exit(1);
    }
    image_render(&ui, valid ? &image : NULL, path);

    for (;;) {
        struct app_input_event event;
        if (!app_ui_poll(&ui, &event)) {
            app_yield();
            continue;
        }
        if (event.type == APP_INPUT_WINDOW_CLOSE) break;
    }
    app_ui_close(&ui);
    app_exit(valid ? 0 : 1);
}
