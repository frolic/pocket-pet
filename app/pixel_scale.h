#pragma once

#include "lvgl.h"

/*
 * Integer nearest-neighbor upscaling for native-res ARGB8888 assets.
 * Generated art ships native (small binaries); these expand it in RAM.
 */

/* Heap-allocates and returns a scaled copy (for small assets, done once). */
const lv_image_dsc_t *pixel_scale_image(const lv_image_dsc_t *source, int32_t scale);

/* Scales into caller-owned dsc+buffer (reusable scratch for sprite frames).
   buffer must hold source w*h*scale*scale*4 bytes. */
void pixel_scale_into(const lv_image_dsc_t *source, int32_t scale,
                      lv_image_dsc_t *destination, uint8_t *buffer);
