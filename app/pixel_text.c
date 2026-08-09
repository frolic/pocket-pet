#include "pixel_scale.h"
#include "pixel_text.h"
#include "sprites/pixel_font.h"

/* Glyphs ship native; each is upscaled once on first use and cached. */
static const lv_image_dsc_t *scaled_glyph(char c)
{
    static const lv_image_dsc_t *cache[128];
    unsigned char index = (unsigned char)c;
    if (index >= 128) return NULL;
    if (cache[index] == NULL) {
        const lv_image_dsc_t *native = pixel_font_glyph(c);
        if (native == NULL) return NULL;
        cache[index] = pixel_scale_image(native, PIXEL_FONT_GLYPH_SCALE);
    }
    return cache[index];
}

static void apply_color(lv_obj_t *text_row, lv_obj_t *image)
{
    uint32_t stored = (uint32_t)(uintptr_t)lv_obj_get_user_data(text_row);
    if (stored == 0) return;
    lv_obj_set_style_image_recolor(image, lv_color_hex(stored - 1), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

lv_obj_t *pixel_text_create(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 0, PIXEL_FONT_HEIGHT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Text never swallows taps — clicks fall through to whatever it sits on. */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

void pixel_text_set_color(lv_obj_t *text_row, lv_color_t color)
{
    /* 24-bit RGB stored +1 so black (0x000000) is distinguishable from "no recolor". */
    lv_obj_set_user_data(text_row, (void *)(uintptr_t)((lv_color_to_u32(color) & 0xFFFFFF) + 1));
    uint32_t count = lv_obj_get_child_count(text_row);
    for (uint32_t i = 0; i < count; i++) {
        apply_color(text_row, lv_obj_get_child(text_row, (int32_t)i));
    }
}

void pixel_text_set(lv_obj_t *text_row, const char *text)
{
    lv_obj_clean(text_row);
    int32_t x = 0;
    for (const char *c = text; *c != '\0'; c++) {
        const lv_image_dsc_t *glyph = scaled_glyph(*c);
        if (glyph != NULL) {
            lv_obj_t *image = lv_image_create(text_row);
            lv_image_set_src(image, glyph);
            lv_obj_set_pos(image, x, 0);
            apply_color(text_row, image);
        }
        x += pixel_font_advance(*c);
    }
    lv_obj_set_size(text_row, x, PIXEL_FONT_HEIGHT);
}
