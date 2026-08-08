#include <time.h>
#include "lvgl.h"
#include "watchface.h"
#include "pet.h"
#include "sprites/field_bg.h"

#define STEP_GOAL 8000
#define ACCENT_COLOR lv_color_hex(0xFFD93B)
/* Dark inks that read against the grass field. */
#define INK_COLOR lv_color_hex(0x24382A)
#define MUTED_COLOR lv_color_hex(0x3E5844)
#define RING_TRACK_COLOR lv_color_hex(0x69A862)

static lv_obj_t *time_label;
static lv_obj_t *steps_label;
static lv_obj_t *goal_arc;

static void refresh_time(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    lv_label_set_text_fmt(time_label, "%02d:%02d", local.tm_hour, local.tm_min);
}

void watchface_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* GBA-route grass field. (Costs AMOLED pixels-off battery; it's a pet, worth it.) */
    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, field_bg);

    time_label = lv_label_create(screen);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(time_label, INK_COLOR, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(time_label, "--:--");
    lv_timer_create(refresh_time, 1000, NULL);
    refresh_time(NULL);

    goal_arc = lv_arc_create(screen);
    lv_obj_set_size(goal_arc, 404, 404);
    lv_obj_align(goal_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(goal_arc, 270);
    lv_arc_set_bg_angles(goal_arc, 0, 360);
    lv_arc_set_range(goal_arc, 0, 100);
    lv_arc_set_value(goal_arc, 0);
    lv_obj_set_style_arc_width(goal_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(goal_arc, RING_TRACK_COLOR, LV_PART_MAIN);
    lv_obj_set_style_arc_width(goal_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(goal_arc, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(goal_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(goal_arc, LV_OBJ_FLAG_CLICKABLE);

    /* The pet nearly fills the panel; the step readout overlays it, watch-face style. */
    lv_obj_t *pet = pet_create(screen);
    lv_obj_align(pet, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *steps_row = lv_obj_create(screen);
    lv_obj_remove_style_all(steps_row);
    lv_obj_set_size(steps_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(steps_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(steps_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(steps_row, 8, 0);
    lv_obj_align(steps_row, LV_ALIGN_BOTTOM_MID, 0, -10);
    /* Overlays the pet's feet — don't steal its clicks. */
    lv_obj_remove_flag(steps_row, LV_OBJ_FLAG_CLICKABLE);

    steps_label = lv_label_create(steps_row);
    lv_obj_set_style_text_font(steps_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(steps_label, INK_COLOR, 0);
    lv_label_set_text(steps_label, "0");

    lv_obj_t *caption = lv_label_create(steps_row);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(caption, MUTED_COLOR, 0);
    lv_obj_set_style_pad_bottom(caption, 6, 0);
    lv_label_set_text(caption, "steps");
}

void watchface_set_steps(uint32_t total)
{
    lv_label_set_text_fmt(steps_label, "%u", (unsigned)total);
    uint32_t percent = total * 100 / STEP_GOAL;
    if (percent > 100) percent = 100;
    lv_arc_set_value(goal_arc, (int32_t)percent);
}
