#include "pet.h"
#include "sprites/raichu_sprites.h"

/* How long after the last step the pet keeps walking before settling down. */
#define WALK_LINGER_MS 2500

typedef enum {
    PET_STATE_IDLE,
    PET_STATE_WALKING,
} pet_state_t;

static lv_obj_t *pet_root;
static lv_obj_t *sprite;
static lv_timer_t *frame_timer;
static pet_state_t state = PET_STATE_IDLE;
static uint32_t frame_index;
static uint32_t last_step_tick;

static const lv_image_dsc_t *const *state_frames(void)
{
    return state == PET_STATE_WALKING ? raichu_walk_frames : raichu_idle_frames;
}

static uint32_t state_frame_count(void)
{
    return state == PET_STATE_WALKING ? raichu_walk_frame_count : raichu_idle_frame_count;
}

static const uint32_t *state_durations(void)
{
    return state == PET_STATE_WALKING ? raichu_walk_durations_ms : raichu_idle_durations_ms;
}

static void show_frame(void)
{
    lv_image_set_src(sprite, state_frames()[frame_index]);
    lv_timer_set_period(frame_timer, state_durations()[frame_index]);
}

static void advance_frame(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (state == PET_STATE_WALKING && lv_tick_elaps(last_step_tick) > WALK_LINGER_MS) {
        state = PET_STATE_IDLE;
        frame_index = 0;
    } else {
        frame_index = (frame_index + 1) % state_frame_count();
    }
    show_frame();
}

static void translate_y_exec(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y(obj, value, 0);
}

static void hop(int32_t height)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, sprite);
    lv_anim_set_exec_cb(&anim, translate_y_exec);
    lv_anim_set_values(&anim, 0, -height);
    lv_anim_set_duration(&anim, 160);
    lv_anim_set_playback_duration(&anim, 200);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void sprite_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    hop(34);
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    pet_root = lv_obj_create(parent);
    lv_obj_remove_style_all(pet_root);
    /* Fixed to the tallest animation so feet stay planted when frames swap size. */
    lv_obj_set_size(pet_root, 160, 224);
    lv_obj_remove_flag(pet_root, LV_OBJ_FLAG_SCROLLABLE);

    sprite = lv_image_create(pet_root);
    lv_obj_align(sprite, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(sprite, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sprite, sprite_clicked, LV_EVENT_CLICKED, NULL);

    frame_timer = lv_timer_create(advance_frame, 100, NULL);
    show_frame();
    return pet_root;
}

void pet_notice_steps(uint32_t delta)
{
    if (delta == 0) return;
    last_step_tick = lv_tick_get();
    if (state == PET_STATE_IDLE) {
        state = PET_STATE_WALKING;
        frame_index = 0;
        show_frame();
    }
}
