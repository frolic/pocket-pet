#include "pet.h"

#define PET_BODY_COLOR lv_color_hex(0xFFD93B)
#define PET_CHEEK_COLOR lv_color_hex(0xE8563A)
#define PET_DARK_COLOR lv_color_hex(0x2B2118)

static lv_obj_t *pet_root;
static lv_obj_t *body;
static uint32_t last_hop_tick;

static void translate_y_exec(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y(obj, value, 0);
}

static void start_idle_bob(void)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, pet_root);
    lv_anim_set_exec_cb(&anim, translate_y_exec);
    lv_anim_set_values(&anim, 0, -8);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_playback_duration(&anim, 900);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void hop(int32_t height)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_exec_cb(&anim, translate_y_exec);
    lv_anim_set_values(&anim, 0, -height);
    lv_anim_set_duration(&anim, 160);
    lv_anim_set_playback_duration(&anim, 200);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void body_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    hop(30);
}

static lv_obj_t *make_circle(lv_obj_t *parent, int32_t size, lv_color_t color)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, size, size);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, color, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    return circle;
}

static lv_obj_t *make_ear(lv_obj_t *parent, int32_t rotation_tenths_deg)
{
    lv_obj_t *ear = lv_obj_create(parent);
    lv_obj_remove_style_all(ear);
    lv_obj_set_size(ear, 26, 74);
    lv_obj_set_style_radius(ear, 13, 0);
    lv_obj_set_style_bg_color(ear, PET_BODY_COLOR, 0);
    lv_obj_set_style_bg_opa(ear, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(ear, rotation_tenths_deg, 0);
    lv_obj_set_style_transform_pivot_x(ear, 13, 0);
    lv_obj_set_style_transform_pivot_y(ear, 70, 0);

    lv_obj_t *tip = lv_obj_create(ear);
    lv_obj_remove_style_all(tip);
    lv_obj_set_size(tip, 26, 26);
    lv_obj_set_style_radius(tip, 13, 0);
    lv_obj_set_style_bg_color(tip, PET_DARK_COLOR, 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 0);
    return ear;
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    pet_root = lv_obj_create(parent);
    lv_obj_remove_style_all(pet_root);
    lv_obj_set_size(pet_root, 220, 230);
    lv_obj_remove_flag(pet_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_ear = make_ear(pet_root, -280);
    lv_obj_align(left_ear, LV_ALIGN_TOP_MID, -52, 0);
    lv_obj_t *right_ear = make_ear(pet_root, 280);
    lv_obj_align(right_ear, LV_ALIGN_TOP_MID, 52, 0);

    body = make_circle(pet_root, 156, PET_BODY_COLOR);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, body_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *left_eye = make_circle(body, 18, PET_DARK_COLOR);
    lv_obj_align(left_eye, LV_ALIGN_CENTER, -34, -18);
    lv_obj_t *right_eye = make_circle(body, 18, PET_DARK_COLOR);
    lv_obj_align(right_eye, LV_ALIGN_CENTER, 34, -18);

    lv_obj_t *left_cheek = make_circle(body, 26, PET_CHEEK_COLOR);
    lv_obj_align(left_cheek, LV_ALIGN_CENTER, -54, 16);
    lv_obj_t *right_cheek = make_circle(body, 26, PET_CHEEK_COLOR);
    lv_obj_align(right_cheek, LV_ALIGN_CENTER, 54, 16);

    lv_obj_t *mouth = lv_obj_create(body);
    lv_obj_remove_style_all(mouth);
    lv_obj_set_size(mouth, 20, 8);
    lv_obj_set_style_radius(mouth, 4, 0);
    lv_obj_set_style_bg_color(mouth, PET_DARK_COLOR, 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 14);

    start_idle_bob();
    return pet_root;
}

void pet_notice_steps(uint32_t delta)
{
    if (delta == 0) return;
    /* Cooldown so continuous walking reads as an occasional happy hop, not vibration. */
    uint32_t now = lv_tick_get();
    if (now - last_hop_tick < 2000) return;
    last_hop_tick = now;
    hop(16);
}
