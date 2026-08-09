#include <ctype.h>
#include "pixel_text.h"
#include "sprites/pixel_font.h"

/* 5px glyph + 1px gap. Font pixels are 4px — finer than the 6px scene
   grid, matching how GBA text is finer than its tile art. */
#define GLYPH_ADVANCE (6 * 4)
#define GLYPH_HEIGHT (7 * 4)

lv_obj_t *pixel_text_create(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 0, GLYPH_HEIGHT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void pixel_text_set(lv_obj_t *text_row, const char *text)
{
    lv_obj_clean(text_row);
    int32_t x = 0;
    for (const char *c = text; *c != '\0'; c++) {
        char upper = (char)toupper((unsigned char)*c);
        if (upper != ' ') {
            const lv_image_dsc_t *glyph = pixel_font_glyph(upper);
            if (glyph == NULL) continue;
            lv_obj_t *image = lv_image_create(text_row);
            lv_image_set_src(image, glyph);
            lv_obj_set_pos(image, x, 0);
        }
        x += GLYPH_ADVANCE;
    }
    lv_obj_set_size(text_row, x > 0 ? x - 4 : 0, GLYPH_HEIGHT);
}
