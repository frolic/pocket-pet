#include <stdlib.h>
#include "pet.h"
#include "sprites/raichu_sprites.h"

/* How long after the last step the pet keeps walking before settling down. */
#define WALK_LINGER_MS 2500
/* Horizontal wander range either side of center, and walking speed. */
#define WANDER_RANGE 110
#define WALK_SPEED_PX_S 70
#define MIN_LEG_PX 60

typedef struct {
    const lv_image_dsc_t *const *frames;
    const uint32_t *durations_ms;
    uint32_t count;
} anim_set_t;

static anim_set_t idle_set;
static anim_set_t walk_east_set;
static anim_set_t walk_west_set;

static lv_obj_t *pet_root;
static lv_obj_t *sprite;
static lv_timer_t *frame_timer;
static const anim_set_t *current_set;
static bool walking;
static uint32_t frame_index;
static uint32_t last_step_tick;

static void show_frame(void)
{
    lv_image_set_src(sprite, current_set->frames[frame_index]);
    lv_timer_set_period(frame_timer, current_set->durations_ms[frame_index]);
}

static void set_anim(const anim_set_t *set)
{
    current_set = set;
    frame_index = 0;
    show_frame();
}

static void translate_x_exec(void *obj, int32_t value)
{
    lv_obj_set_style_translate_x(obj, value, 0);
}

static void translate_y_exec(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y(obj, value, 0);
}

static void wander_leg_done(lv_anim_t *anim);

/* Picks a new horizontal target and walks there with the matching side-facing frames. */
static void start_wander_leg(void)
{
    int32_t current = lv_obj_get_style_translate_x(pet_root, LV_PART_MAIN);
    int32_t target;
    do {
        target = (rand() % (2 * WANDER_RANGE + 1)) - WANDER_RANGE;
    } while (LV_ABS(target - current) < MIN_LEG_PX);

    set_anim(target > current ? &walk_east_set : &walk_west_set);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, pet_root);
    lv_anim_set_exec_cb(&anim, translate_x_exec);
    lv_anim_set_values(&anim, current, target);
    lv_anim_set_duration(&anim, LV_ABS(target - current) * 1000 / WALK_SPEED_PX_S);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_completed_cb(&anim, wander_leg_done);
    lv_anim_start(&anim);
}

static void wander_leg_done(lv_anim_t *anim)
{
    LV_UNUSED(anim);
    if (walking) start_wander_leg();
}

static void settle_to_idle(void)
{
    walking = false;
    lv_anim_delete(pet_root, translate_x_exec);
    set_anim(&idle_set);
}

static void advance_frame(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (walking && lv_tick_elaps(last_step_tick) > WALK_LINGER_MS) {
        settle_to_idle();
        return;
    }
    frame_index = (frame_index + 1) % current_set->count;
    show_frame();
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

static int32_t max_frame_width(void)
{
    int32_t width = raichu_idle_frames[0]->header.w;
    width = LV_MAX(width, raichu_walk_east_frames[0]->header.w);
    return LV_MAX(width, raichu_walk_west_frames[0]->header.w);
}

static int32_t max_frame_height(void)
{
    int32_t height = raichu_idle_frames[0]->header.h;
    height = LV_MAX(height, raichu_walk_east_frames[0]->header.h);
    return LV_MAX(height, raichu_walk_west_frames[0]->header.h);
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    idle_set = (anim_set_t){raichu_idle_frames, raichu_idle_durations_ms, raichu_idle_frame_count};
    walk_east_set = (anim_set_t){raichu_walk_east_frames, raichu_walk_east_durations_ms, raichu_walk_east_frame_count};
    walk_west_set = (anim_set_t){raichu_walk_west_frames, raichu_walk_west_durations_ms, raichu_walk_west_frame_count};

    pet_root = lv_obj_create(parent);
    lv_obj_remove_style_all(pet_root);
    /* Sized to the largest animation so feet stay planted when frames swap size. */
    lv_obj_set_size(pet_root, max_frame_width(), max_frame_height());
    lv_obj_remove_flag(pet_root, LV_OBJ_FLAG_SCROLLABLE);

    sprite = lv_image_create(pet_root);
    lv_obj_align(sprite, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(sprite, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sprite, sprite_clicked, LV_EVENT_CLICKED, NULL);

    frame_timer = lv_timer_create(advance_frame, 100, NULL);
    set_anim(&idle_set);
    return pet_root;
}

void pet_notice_steps(uint32_t delta)
{
    if (delta == 0) return;
    last_step_tick = lv_tick_get();
    if (!walking) {
        walking = true;
        start_wander_leg();
    }
}
