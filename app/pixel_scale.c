#include <stdlib.h>
#include <string.h>
#include "pixel_scale.h"

static void scale_pixels(const lv_image_dsc_t *source, int32_t scale, uint8_t *out)
{
    const uint32_t *in = (const uint32_t *)source->data;
    uint32_t *dest = (uint32_t *)out;
    int32_t out_width = source->header.w * scale;
    for (int32_t y = 0; y < source->header.h; y++) {
        uint32_t *row_start = dest;
        for (int32_t x = 0; x < source->header.w; x++) {
            uint32_t pixel = in[y * source->header.w + x];
            for (int32_t i = 0; i < scale; i++) *dest++ = pixel;
        }
        for (int32_t i = 1; i < scale; i++) {
            memcpy(dest, row_start, (size_t)out_width * 4);
            dest += out_width;
        }
    }
}

void pixel_scale_into(const lv_image_dsc_t *source, int32_t scale,
                      lv_image_dsc_t *destination, uint8_t *buffer)
{
    scale_pixels(source, scale, buffer);
    destination->header.magic = LV_IMAGE_HEADER_MAGIC;
    destination->header.cf = LV_COLOR_FORMAT_ARGB8888;
    destination->header.w = source->header.w * scale;
    destination->header.h = source->header.h * scale;
    destination->header.stride = source->header.w * scale * 4;
    destination->data_size = (uint32_t)(source->header.w * scale) * (uint32_t)(source->header.h * scale) * 4;
    destination->data = buffer;
}

const lv_image_dsc_t *pixel_scale_image(const lv_image_dsc_t *source, int32_t scale)
{
    uint8_t *buffer = malloc((size_t)source->header.w * source->header.h * scale * scale * 4);
    lv_image_dsc_t *destination = malloc(sizeof(lv_image_dsc_t));
    LV_ASSERT_NULL(buffer);
    LV_ASSERT_NULL(destination);
    pixel_scale_into(source, scale, destination, buffer);
    return destination;
}
