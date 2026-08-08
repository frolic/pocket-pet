#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "screenshot.h"

static void write_u16(uint8_t *out, uint16_t value)
{
    out[0] = value & 0xFF;
    out[1] = value >> 8;
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = value & 0xFF;
    out[1] = (value >> 8) & 0xFF;
    out[2] = (value >> 16) & 0xFF;
    out[3] = (value >> 24) & 0xFF;
}

int screenshot_write_bmp(const char *path)
{
    lv_draw_buf_t *snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (snapshot == NULL) {
        fprintf(stderr, "screenshot: lv_snapshot_take failed (LV_MEM_SIZE too small?)\n");
        return -1;
    }

    uint32_t width = snapshot->header.w;
    uint32_t height = snapshot->header.h;
    uint32_t image_size = width * height * 4;

    /* 14-byte file header + 40-byte BITMAPINFOHEADER, 32bpp, rows bottom-up. */
    uint8_t header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    write_u32(header + 2, 54 + image_size);
    write_u32(header + 10, 54);
    write_u32(header + 14, 40);
    write_u32(header + 18, width);
    write_u32(header + 22, height);
    write_u16(header + 26, 1);
    write_u16(header + 28, 32);
    write_u32(header + 34, image_size);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        lv_draw_buf_destroy(snapshot);
        return -1;
    }
    fwrite(header, 1, sizeof(header), file);
    for (int32_t row = (int32_t)height - 1; row >= 0; row--) {
        fwrite(snapshot->data + (uint32_t)row * snapshot->header.stride, 1, width * 4, file);
    }
    fclose(file);
    lv_draw_buf_destroy(snapshot);
    return 0;
}
