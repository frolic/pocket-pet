#include "lvgl.h"
#include "debug_panel.h"
#include "fake_step_source.h"
#include "fake_battery_source.h"
#include "../app/button_source.h"
#include "../app/power_button.h"
#include "../app/pet.h"

static bool record_held;

static void record_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) record_held = true;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) record_held = false;
}

static void steps_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    fake_step_source_add(2500);
}

static bool power_pressed;

static void power_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    power_pressed = true;
}

bool power_button_pressed(void)
{
    bool pressed = power_pressed;
    power_pressed = false;
    return pressed;
}

static void battery_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    fake_battery_source_cycle();
}

static void celebrate_clicked(lv_event_t *event)
{
    pet_celebrate((pet_celebration_t)(uintptr_t)lv_event_get_user_data(event));
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

/* The sim's stand-in for the physical BOOT button. */
bool button_source_held(void)
{
    return record_held;
}

void debug_panel_create(lv_obj_t *parent)
{
    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 150, 502);
    lv_obj_set_pos(rail, 410, 0);
    lv_obj_set_style_bg_color(rail, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rail, 12, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rail, 12, 0);

    lv_obj_t *title = lv_label_create(rail);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8A8A8E), 0);
    lv_label_set_text(title, "DEBUG");

    lv_obj_t *record_button = make_button(rail, "REC\n(hold)");
    lv_obj_add_event_cb(record_button, record_event, LV_EVENT_ALL, NULL);

    lv_obj_t *power_button = make_button(rail, "PWR");
    lv_obj_add_event_cb(power_button, power_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *steps_button = make_button(rail, "+2500 steps");
    lv_obj_add_event_cb(steps_button, steps_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *battery_button = make_button(rail, "Battery");
    lv_obj_add_event_cb(battery_button, battery_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *shock_button = make_button(rail, "Shock");
    lv_obj_add_event_cb(shock_button, celebrate_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)PET_CELEBRATION_SHOCK);
    lv_obj_t *hop_button = make_button(rail, "Hop");
    lv_obj_add_event_cb(hop_button, celebrate_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)PET_CELEBRATION_HOP);
    lv_obj_t *breath_button = make_button(rail, "Breath");
    lv_obj_add_event_cb(breath_button, celebrate_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)PET_CELEBRATION_BREATH);
}
