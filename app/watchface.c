#include <stdio.h>
#include <time.h>
#include "lvgl.h"
#include "watchface.h"
#include "pet.h"
#include "pixel_text.h"
#include "sprites/field_bg.h"
#include "sprites/hud_box.h"

#define STEP_GOAL 10000

/* Box/slot/border colors live in the generated chrome (tools/make_hud.py). */
#define EXP_FILL_COLOR lv_color_hex(0x48C0E8)
#define RECORD_COLOR lv_color_hex(0xD93A2F)

/* Sizes match the generated pixel-art chrome (tools/make_hud.py). */
#define BOX_WIDTH 384
#define BOX_HEIGHT 96
#define BOX_MARGIN 12
#define BOX_PAD 18
#define TEXT_Y 20
#define EXP_BAR_WIDTH 348
#define EXP_BAR_HEIGHT 24
#define EXP_BAR_Y (BOX_HEIGHT - EXP_BAR_HEIGHT - 12)
#define EXP_FILL_MAX (EXP_BAR_WIDTH - 12)

static lv_obj_t *panel;
static lv_obj_t *time_shadow;
static lv_obj_t *time_text;
static lv_obj_t *name_text;
static lv_obj_t *right_text;
static lv_obj_t *exp_fill;
static lv_obj_t *record_dot;
static lv_timer_t *daily_revert_timer;
static uint32_t steps_total;
static bool showing_daily;

/* Level is display-only until the EXP system lands. */
#define LEVEL_TEXT "Lv1"

static void show_level_view(void)
{
    showing_daily = false;
    pixel_text_set(name_text, "RAICHU");
    pixel_text_set(right_text, LEVEL_TEXT);
    lv_obj_align(right_text, LV_ALIGN_TOP_RIGHT, -BOX_PAD, TEXT_Y);
}

static void show_daily_view(void)
{
    showing_daily = true;
    pixel_text_set(name_text, "Daily");
    char text[16];
    snprintf(text, sizeof(text), "%u/10k", (unsigned)steps_total);
    pixel_text_set(right_text, text);
    lv_obj_align(right_text, LV_ALIGN_TOP_RIGHT, -BOX_PAD, TEXT_Y);
}

static void revert_to_level_view(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    show_level_view();
    lv_timer_pause(daily_revert_timer);
}

static void box_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    if (showing_daily) {
        show_level_view();
        lv_timer_pause(daily_revert_timer);
        return;
    }
    show_daily_view();
    lv_timer_reset(daily_revert_timer);
    lv_timer_resume(daily_revert_timer);
}

static void refresh_time(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char text[8];
    snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    pixel_text_set(time_shadow, text);
    lv_obj_align(time_shadow, LV_ALIGN_TOP_MID, 3, 17);
    pixel_text_set(time_text, text);
    lv_obj_align(time_text, LV_ALIGN_TOP_MID, 0, 14);
}

static void field_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(panel, &area);
    pet_call_to(point.x - area.x1, point.y - area.y1);
}

void watchface_create(lv_obj_t *parent)
{
    panel = parent;
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, field_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *screen = panel;

    /* GBA-route grass field. (Costs AMOLED pixels-off battery; it's a pet, worth it.) */
    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, field_bg);

    /* Overworld-sign style clock: white with a one-font-pixel black shadow. */
    time_shadow = pixel_text_create(screen);
    pixel_text_set_color(time_shadow, lv_color_black());
    time_text = pixel_text_create(screen);
    pixel_text_set_color(time_text, lv_color_white());
    lv_timer_create(refresh_time, 1000, NULL);
    refresh_time(NULL);

    /* The pet roams the field; the info box overlays the bottom. */
    lv_obj_t *pet = pet_create(screen);
    lv_obj_align(pet, LV_ALIGN_CENTER, 0, -30);

    /* Pixel-art chrome images; a plain wrapper keeps child coords honest. */
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BOX_WIDTH, BOX_HEIGHT);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -BOX_MARGIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box_frame = lv_image_create(box);
    lv_image_set_src(box_frame, hud_dialog_box);

    name_text = pixel_text_create(box);
    lv_obj_set_pos(name_text, BOX_PAD, TEXT_Y);

    right_text = pixel_text_create(box);
    daily_revert_timer = lv_timer_create(revert_to_level_view, 3500, NULL);
    lv_timer_pause(daily_revert_timer);
    show_level_view();
    lv_obj_add_event_cb(box, box_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exp_bar = lv_image_create(box);
    lv_image_set_src(exp_bar, hud_exp_frame);
    lv_obj_set_pos(exp_bar, (BOX_WIDTH - EXP_BAR_WIDTH) / 2, EXP_BAR_Y);

    exp_fill = lv_obj_create(box);
    lv_obj_remove_style_all(exp_fill);
    lv_obj_set_size(exp_fill, 0, EXP_BAR_HEIGHT - 12);
    lv_obj_set_pos(exp_fill, (BOX_WIDTH - EXP_BAR_WIDTH) / 2 + 6, EXP_BAR_Y + 6);
    lv_obj_set_style_bg_color(exp_fill, EXP_FILL_COLOR, 0);
    lv_obj_set_style_bg_opa(exp_fill, LV_OPA_COVER, 0);

    record_dot = lv_obj_create(screen);
    lv_obj_remove_style_all(record_dot);
    lv_obj_set_size(record_dot, 18, 18);
    lv_obj_set_style_bg_color(record_dot, RECORD_COLOR, 0);
    lv_obj_set_style_bg_opa(record_dot, LV_OPA_COVER, 0);
    lv_obj_align(record_dot, LV_ALIGN_TOP_RIGHT, -14, 14);
    lv_obj_add_flag(record_dot, LV_OBJ_FLAG_HIDDEN);
}

void watchface_set_steps(uint32_t total)
{
    steps_total = total;
    if (showing_daily) show_daily_view();

    /* Fill like the games: left to right, quantized to the 6px pixel grid. */
    uint32_t fill = (uint64_t)total * EXP_FILL_MAX / STEP_GOAL;
    if (fill > EXP_FILL_MAX) fill = EXP_FILL_MAX;
    lv_obj_set_width(exp_fill, (int32_t)(fill / 6 * 6));
}

void watchface_set_recording(bool recording)
{
    if (recording) {
        lv_obj_remove_flag(record_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(record_dot, LV_OBJ_FLAG_HIDDEN);
    }
}
