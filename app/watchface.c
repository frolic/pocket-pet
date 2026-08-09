#include <stdio.h>
#include <time.h>
#include "lvgl.h"
#include "watchface.h"
#include "pet.h"
#include "game_config.h"
#include "pixel_scale.h"
#include "pixel_text.h"
#include "sprites/field_bg.h"
#include "sprites/hud_box.h"

/* Box/slot/border colors live in the generated chrome (tools/make_hud.py). */
#define EXP_FILL_COLOR lv_color_hex(0x48C0E8)
#define RECORD_COLOR lv_color_hex(0xD93A2F)

/* Sizes match the generated pixel-art chrome (tools/make_hud.py). */
#define BOX_WIDTH 304
#define BOX_HEIGHT 72
#define BOX_MARGIN 24
#define BOX_PAD 14
#define TEXT_Y 14
#define EXP_BAR_WIDTH 272
#define EXP_BAR_HEIGHT 16
#define EXP_BAR_Y (BOX_HEIGHT - EXP_BAR_HEIGHT - 8)
#define EXP_FILL_MAX (EXP_BAR_WIDTH - 8)

static lv_obj_t *panel;
static lv_obj_t *time_shadow;
static lv_obj_t *time_text;
static lv_obj_t *name_text;
static lv_obj_t *right_text;
static lv_obj_t *exp_fill;
static lv_obj_t *record_dot;
static lv_obj_t *battery_root;
static lv_obj_t *battery_fill;

/* Battery icon in HUD art-pixels (PX=4 screen px each): 9x5 body + 1x3 nub. */
#define BATTERY_PX 4
#define BATTERY_FILL_MAX_PX 7
#define BATTERY_OUTLINE_COLOR lv_color_hex(0x202020)
#define BATTERY_OK_COLOR lv_color_hex(0xF8F8F8)
#define BATTERY_LOW_COLOR lv_color_hex(0xE04838)
#define BATTERY_CHARGE_COLOR lv_color_hex(0x58C858)
static lv_obj_t *banner_shadow;
static lv_obj_t *banner_text;
static lv_timer_t *daily_revert_timer;
static uint32_t steps_total;
static uint32_t level = 1;
static bool showing_daily;

static void show_level_view(void)
{
    showing_daily = false;
    pixel_text_set(name_text, "RAICHU");
    char text[12];
    snprintf(text, sizeof(text), "LV%u", (unsigned)level);
    pixel_text_set(right_text, text);
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
    lv_obj_align(time_shadow, LV_ALIGN_TOP_MID, 3, 27);
    pixel_text_set(time_text, text);
    lv_obj_align(time_text, LV_ALIGN_TOP_MID, 0, 24);
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

    /* GBA-route grass field. (Costs AMOLED pixels-off battery; it's a pet, worth it.)
       Assets ship native and are upscaled once into RAM at startup. */
    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, pixel_scale_image(field_bg, FIELD_BG_SCALE));

    /* Overworld-sign style clock: white with a one-font-pixel black shadow. */
    time_shadow = pixel_text_create(screen);
    pixel_text_set_color(time_shadow, lv_color_black());
    time_text = pixel_text_create(screen);
    pixel_text_set_color(time_text, lv_color_white());
    lv_timer_create(refresh_time, 1000, NULL);
    refresh_time(NULL);

    /* The pet roams the field (it anchors its own feet position). */
    pet_create(screen);

    /* Pixel-art chrome images; a plain wrapper keeps child coords honest. */
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BOX_WIDTH, BOX_HEIGHT);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -BOX_MARGIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box_frame = lv_image_create(box);
    lv_image_set_src(box_frame, pixel_scale_image(hud_dialog_box, HUD_SCALE));

    name_text = pixel_text_create(box);
    lv_obj_set_pos(name_text, BOX_PAD, TEXT_Y);

    right_text = pixel_text_create(box);
    daily_revert_timer = lv_timer_create(revert_to_level_view, 3500, NULL);
    lv_timer_pause(daily_revert_timer);
    show_level_view();
    lv_obj_add_event_cb(box, box_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exp_bar = lv_image_create(box);
    lv_image_set_src(exp_bar, pixel_scale_image(hud_exp_frame, HUD_SCALE));
    lv_obj_set_pos(exp_bar, (BOX_WIDTH - EXP_BAR_WIDTH) / 2, EXP_BAR_Y);

    exp_fill = lv_obj_create(box);
    lv_obj_remove_style_all(exp_fill);
    lv_obj_remove_flag(exp_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(exp_fill, 0, EXP_BAR_HEIGHT - 8);
    lv_obj_set_pos(exp_fill, (BOX_WIDTH - EXP_BAR_WIDTH) / 2 + 4, EXP_BAR_Y + 4);
    lv_obj_set_style_bg_color(exp_fill, EXP_FILL_COLOR, 0);
    lv_obj_set_style_bg_opa(exp_fill, LV_OPA_COVER, 0);

    banner_shadow = pixel_text_create(screen);
    pixel_text_set_color(banner_shadow, lv_color_black());
    banner_text = pixel_text_create(screen);
    pixel_text_set_color(banner_text, lv_color_white());

    record_dot = lv_obj_create(screen);
    lv_obj_remove_style_all(record_dot);
    lv_obj_set_size(record_dot, 18, 18);
    lv_obj_set_style_bg_color(record_dot, RECORD_COLOR, 0);
    lv_obj_set_style_bg_opa(record_dot, LV_OPA_COVER, 0);
    lv_obj_align(record_dot, LV_ALIGN_TOP_RIGHT, -72, 19);
    lv_obj_add_flag(record_dot, LV_OBJ_FLAG_HIDDEN);

    /* Battery: outlined body with a nub, level bar inside. Flat siblings in
       a borderless container — LVGL offsets children by parent border width,
       so the border lives on a leaf object instead. */
    battery_root = lv_obj_create(screen);
    lv_obj_remove_style_all(battery_root);
    lv_obj_set_size(battery_root, 10 * BATTERY_PX, 5 * BATTERY_PX);
    lv_obj_align(battery_root, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_add_flag(battery_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *battery_body = lv_obj_create(battery_root);
    lv_obj_remove_style_all(battery_body);
    lv_obj_set_size(battery_body, 9 * BATTERY_PX, 5 * BATTERY_PX);
    lv_obj_set_style_border_color(battery_body, BATTERY_OUTLINE_COLOR, 0);
    lv_obj_set_style_border_width(battery_body, BATTERY_PX, 0);
    lv_obj_set_pos(battery_body, 0, 0);

    lv_obj_t *battery_nub = lv_obj_create(battery_root);
    lv_obj_remove_style_all(battery_nub);
    lv_obj_set_size(battery_nub, BATTERY_PX, 3 * BATTERY_PX);
    lv_obj_set_style_bg_color(battery_nub, BATTERY_OUTLINE_COLOR, 0);
    lv_obj_set_style_bg_opa(battery_nub, LV_OPA_COVER, 0);
    lv_obj_set_pos(battery_nub, 9 * BATTERY_PX, BATTERY_PX);

    battery_fill = lv_obj_create(battery_root);
    lv_obj_remove_style_all(battery_fill);
    lv_obj_set_size(battery_fill, BATTERY_PX, 3 * BATTERY_PX);
    lv_obj_set_style_bg_color(battery_fill, BATTERY_OK_COLOR, 0);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_pos(battery_fill, BATTERY_PX, BATTERY_PX);
}

void watchface_set_battery(int percent, bool charging)
{
    if (percent < 0) {
        lv_obj_add_flag(battery_root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(battery_root, LV_OBJ_FLAG_HIDDEN);
    int fill_px = (percent * BATTERY_FILL_MAX_PX + 50) / 100;
    if (fill_px < 1) fill_px = 1;
    lv_obj_set_width(battery_fill, fill_px * BATTERY_PX);
    lv_color_t color = charging ? BATTERY_CHARGE_COLOR
                     : percent <= 20 ? BATTERY_LOW_COLOR
                     : BATTERY_OK_COLOR;
    lv_obj_set_style_bg_color(battery_fill, color, 0);
}

void watchface_set_steps(uint32_t total)
{
    steps_total = total;
    if (showing_daily) show_daily_view();

    /* Each STEP_GOAL fills the bar once: it drains and the level ticks up,
       like the games. Fill is quantized to the 6px pixel grid. */
    uint32_t new_level = 1 + total / STEP_GOAL;
    if (new_level != level) {
        level = new_level;
        if (!showing_daily) show_level_view();
    }
    uint32_t fill = (uint64_t)(total % STEP_GOAL) * EXP_FILL_MAX / STEP_GOAL;
    lv_obj_set_width(exp_fill, (int32_t)(fill / 4 * 4));
}

void watchface_set_banner(const char *text)
{
    if (text == NULL) {
        pixel_text_set(banner_shadow, "");
        pixel_text_set(banner_text, "");
        return;
    }
    pixel_text_set(banner_shadow, text);
    lv_obj_align(banner_shadow, LV_ALIGN_CENTER, 3, -132);
    pixel_text_set(banner_text, text);
    lv_obj_align(banner_text, LV_ALIGN_CENTER, 0, -135);
}

void watchface_set_recording(bool recording)
{
    if (recording) {
        lv_obj_remove_flag(record_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(record_dot, LV_OBJ_FLAG_HIDDEN);
    }
}
