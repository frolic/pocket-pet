#pragma once

#include "lvgl.h"

/* A text row composed of 5x7 pixel-font glyph sprites at scene scale. */
lv_obj_t *pixel_text_create(lv_obj_t *parent);

/* Replaces the text (uppercased; unknown characters are skipped). */
void pixel_text_set(lv_obj_t *text_row, const char *text);
