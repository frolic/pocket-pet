#include <stdio.h>
#include <time.h>
#include "lvgl.h"
#include "watchface.h"
#include "display_sleep.h"
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
static lv_obj_t *battery_percent_text;
static lv_obj_t *wifi_icon;
static lv_obj_t *wifi_bars[3];
static watchface_wifi_state_t wifi_state;
static lv_timer_t *wifi_blink_timer;
static lv_obj_t *setup_modal;
static lv_obj_t *setup_modal_content;
static void (*wifi_tap_cb)(void);

#define MODAL_DIALOG_TOP 110
#define MODAL_DIALOG_HEIGHT 300
#define MODAL_CANCEL_TOP (MODAL_DIALOG_TOP + MODAL_DIALOG_HEIGHT - 88)

/* Seeking reads as activity: the fan pulses while connecting. */
static void wifi_blink_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (wifi_state != WATCHFACE_WIFI_CONNECTING) return;
    static bool dim;
    dim = !dim;
    lv_obj_set_style_opa(wifi_icon, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static void wifi_icon_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    if (wifi_tap_cb != NULL) wifi_tap_cb();
}

#define WIFI_SLASH_COLOR lv_color_hex(0xD84030)
#define WIFI_SEEKING_COLOR lv_color_hex(0xF8D030)

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

#define NIGHT_START_HOUR 22
#define NIGHT_END_HOUR 7
#define DAY_BRIGHTNESS 30
#define NIGHT_BRIGHTNESS 15

static void refresh_time(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    /* Clock-scheduled dimming: gentle on night eyes (and the battery). */
    bool night = local.tm_hour >= NIGHT_START_HOUR ||
                 local.tm_hour < NIGHT_END_HOUR;
    display_sleep_set_awake_brightness(night ? NIGHT_BRIGHTNESS
                                             : DAY_BRIGHTNESS);
    char text[8];
    /* No redraws under the blanket: invalidations while dark still flush. */
    if (display_sleep_is_asleep()) return;
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
    lv_obj_align(record_dot, LV_ALIGN_TOP_RIGHT, -128, 27);
    lv_obj_add_flag(record_dot, LV_OBJ_FLAG_HIDDEN);

    /* Battery: outlined body with a nub, level bar inside. Flat siblings in
       a borderless container — LVGL offsets children by parent border width,
       so the border lives on a leaf object instead. */
    battery_root = lv_obj_create(screen);
    lv_obj_remove_style_all(battery_root);
    lv_obj_set_size(battery_root, 10 * BATTERY_PX, 5 * BATTERY_PX);
    lv_obj_align(battery_root, LV_ALIGN_TOP_RIGHT, -40, 26);
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

    battery_percent_text = pixel_text_create_mini(screen);
    lv_obj_add_flag(battery_percent_text, LV_OBJ_FLAG_HIDDEN);

    /* Radio status: pixel wifi fan, tinted by state. Tap = wifi setup. */
    wifi_icon = lv_obj_create(screen);
    lv_obj_remove_style_all(wifi_icon);
    lv_obj_set_size(wifi_icon, 8 * BATTERY_PX, 6 * BATTERY_PX);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -84, 24);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(wifi_icon, 36);
    lv_obj_add_event_cb(wifi_icon, wifi_icon_clicked, LV_EVENT_CLICKED, NULL);
    /* A signal fan: dot at the bottom, arcs widening upward. */
    static const struct { int8_t x, y, w; } wifi_pixels[] = {
        {1, 0, 6}, {2, 2, 4}, {3, 4, 2},
    };
    for (size_t i = 0; i < sizeof(wifi_pixels) / sizeof(wifi_pixels[0]); i++) {
        lv_obj_t *px = lv_obj_create(wifi_icon);
        lv_obj_remove_style_all(px);
        lv_obj_set_size(px, wifi_pixels[i].w * BATTERY_PX, BATTERY_PX);
        lv_obj_set_pos(px, wifi_pixels[i].x * BATTERY_PX, wifi_pixels[i].y * BATTERY_PX);
        lv_obj_set_style_bg_color(px, WIFI_SLASH_COLOR, 0);
        lv_obj_set_style_bg_opa(px, LV_OPA_COVER, 0);
        wifi_bars[i] = px;
    }
    wifi_blink_timer = lv_timer_create(wifi_blink_tick, 400, NULL);
}

void watchface_set_wifi(watchface_wifi_state_t state)
{
    if (state == wifi_state) return;
    wifi_state = state;
    if (state == WATCHFACE_WIFI_HIDDEN) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_color_t tint;
    switch (state) {
    case WATCHFACE_WIFI_CONNECTING: tint = WIFI_SEEKING_COLOR; break;
    case WATCHFACE_WIFI_CONNECTED: tint = lv_color_white(); break;
    default: tint = WIFI_SLASH_COLOR; break; /* offline / stranded: red */
    }
    for (size_t i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(wifi_bars[i], tint, 0);
    }
    lv_obj_set_style_opa(wifi_icon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
}

void watchface_set_wifi_tap_cb(void (*tap_cb)(void))
{
    wifi_tap_cb = tap_cb;
}

/* Built lazily: setup mode is rare and the modal is static once shown. */
static void build_setup_modal(void)
{
    /* Full-screen cover: flat field green so nothing else is part of the
       frame, with the dialog centered on top. */
    setup_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(setup_modal);
    lv_obj_set_size(setup_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(setup_modal, lv_color_hex(0x88C878), 0);
    lv_obj_set_style_bg_opa(setup_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(setup_modal, LV_OBJ_FLAG_CLICKABLE); /* swallow taps */

    lv_obj_t *dialog = lv_obj_create(setup_modal);
    lv_obj_remove_style_all(dialog);
    lv_obj_set_size(dialog, 340, MODAL_DIALOG_HEIGHT);
    lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, MODAL_DIALOG_TOP);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0xF8F8F0), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_width(dialog, 6, 0);
    lv_obj_t *setup_modal_dialog = dialog;
    setup_modal_content = setup_modal_dialog;

    lv_obj_t *title = pixel_text_create(setup_modal_content);
    pixel_text_set_color(title, lv_color_hex(0x202020));
    pixel_text_set(title, "WIFI SETUP");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *line1 = pixel_text_create(setup_modal_content);
    pixel_text_set_color(line1, lv_color_hex(0x505050));
    pixel_text_set(line1, "JOIN");
    lv_obj_align(line1, LV_ALIGN_TOP_MID, 0, 84);

    lv_obj_t *line2 = pixel_text_create(setup_modal_content);
    pixel_text_set_color(line2, lv_color_hex(0x505050));
    pixel_text_set(line2, "POCKET PET");
    lv_obj_align(line2, LV_ALIGN_TOP_MID, 0, 118);

    lv_obj_t *line3 = pixel_text_create(setup_modal_content);
    pixel_text_set_color(line3, lv_color_hex(0x505050));
    pixel_text_set(line3, "ON YOUR PHONE");
    lv_obj_align(line3, LV_ALIGN_TOP_MID, 0, 152);

    lv_obj_t *cancel = lv_obj_create(setup_modal_content);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, 260, 56);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);

    lv_obj_t *cancel_text = pixel_text_create(cancel);
    pixel_text_set_color(cancel_text, lv_color_white());
    pixel_text_set(cancel_text, "CANCEL");
    lv_obj_center(cancel_text);
}

void watchface_show_setup_modal(bool show)
{
    if (setup_modal == NULL) {
        if (!show) return;
        build_setup_modal();
    }
    if (show) lv_obj_remove_flag(setup_modal, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(setup_modal, LV_OBJ_FLAG_HIDDEN);
}

int watchface_setup_modal_cancel_min_y(void)
{
    return MODAL_CANCEL_TOP;
}

void watchface_set_battery(int percent, bool charging)
{
    if (percent < 0) {
        lv_obj_add_flag(battery_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_percent_text, LV_OBJ_FLAG_HIDDEN);
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

    /* Tiny percent digits under the bar, right edges aligned, tinted to
       match. Re-rendered only when the reading changes. */
    static int shown_percent = -1;
    static bool shown_charging;
    if (percent != shown_percent || charging != shown_charging) {
        shown_percent = percent;
        shown_charging = charging;
        char text[16];
        snprintf(text, sizeof(text), "%d", percent);
        pixel_text_set(battery_percent_text, text);
        pixel_text_set_color(battery_percent_text, color);
        lv_obj_align(battery_percent_text, LV_ALIGN_TOP_RIGHT, -40,
                     26 + 5 * BATTERY_PX + 3);
    }
    lv_obj_remove_flag(battery_percent_text, LV_OBJ_FLAG_HIDDEN);
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
