#pragma once

#include "lvgl.h"

/* A text row composed of pixel-font glyph sprites (proportional advances). */
lv_obj_t *pixel_text_create(lv_obj_t *parent);

/* Replaces the text (unknown characters are skipped). */
void pixel_text_set(lv_obj_t *text_row, const char *text);

/* Recolors the glyphs (default is the font's baked ink). */
void pixel_text_set_color(lv_obj_t *text_row, lv_color_t color);
