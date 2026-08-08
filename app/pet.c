#include <math.h>
#include <stdlib.h>
#include "pet.h"
#include "sprites/raichu_sprites.h"

/* How long after the last step the pet keeps walking before settling down. */
#define WALK_LINGER_MS 2500
/* Ellipse the pet patrols — wider than tall so it reads as a ground circle. */
#define ORBIT_RADIUS_X 110
#define ORBIT_RADIUS_Y 55
#define WALK_SPEED_PX_S 70
#define MOVE_TICK_MS 33
/* Extra container height above the sprite so the hop never leaves its bounds. */
#define HOP_HEADROOM 40

typedef struct {
    const lv_image_dsc_t *const *frames;
    const uint32_t *durations_ms;
    uint32_t count;
} anim_set_t;

static anim_set_t idle_set;
/* Indexed by sheet-row order: S, SE, E, NE, N, NW, W, SW. */
static anim_set_t walk_sets[8];

static lv_obj_t *pet_root;
static lv_obj_t *sprite;
static lv_timer_t *frame_timer;
static lv_timer_t *move_timer;
static const anim_set_t *current_set;
static bool walking;
static uint32_t frame_index;
static uint32_t last_step_tick;
static float orbit_angle = (float)M_PI / 2; /* start front-center, closest to viewer */
static int orbit_direction = 1;

static void start_idle_bob(void);
static void stop_idle_bob(void);

static void show_frame(void)
{
    lv_image_set_src(sprite, current_set->frames[frame_index]);
    lv_timer_set_period(frame_timer, current_set->durations_ms[frame_index]);
}

static void set_anim(const anim_set_t *set)
{
    if (current_set == set) return;
    current_set = set;
    /* Keep cadence position so direction changes don't restart the stride. */
    frame_index %= set->count;
    if (set == &idle_set) frame_index = 0;
    show_frame();
}

/* Maps a movement heading (screen coords, y down) onto the 8 sheet rows. */
static const anim_set_t *walk_set_for_heading(float velocity_x, float velocity_y)
{
    static const int row_for_octant[8] = {2, 1, 0, 7, 6, 5, 4, 3};
    float degrees = atan2f(velocity_y, velocity_x) * 180.0f / (float)M_PI;
    int octant = ((int)lroundf(degrees / 45.0f) + 8) % 8;
    return &walk_sets[row_for_octant[octant]];
}

static void orbit_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    float average_radius = (ORBIT_RADIUS_X + ORBIT_RADIUS_Y) / 2.0f;
    float angular_speed = WALK_SPEED_PX_S / average_radius;
    orbit_angle += orbit_direction * angular_speed * MOVE_TICK_MS / 1000.0f;

    lv_obj_set_style_translate_x(pet_root, (int32_t)(ORBIT_RADIUS_X * cosf(orbit_angle)), 0);
    lv_obj_set_style_translate_y(pet_root, (int32_t)(ORBIT_RADIUS_Y * sinf(orbit_angle)), 0);

    float velocity_x = -ORBIT_RADIUS_X * sinf(orbit_angle) * orbit_direction;
    float velocity_y = ORBIT_RADIUS_Y * cosf(orbit_angle) * orbit_direction;
    set_anim(walk_set_for_heading(velocity_x, velocity_y));
}

static void settle_to_idle(void)
{
    walking = false;
    lv_timer_pause(move_timer);
    set_anim(&idle_set);
    start_idle_bob();
}

static void advance_frame(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (walking && lv_tick_elaps(last_step_tick) > WALK_LINGER_MS) {
        settle_to_idle();
        return;
    }
    /* Idle is the static rest pose; the breathe is a whole-sprite translate. */
    if (current_set == &idle_set) return;
    frame_index = (frame_index + 1) % current_set->count;
    show_frame();
}

static void translate_y_exec(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y(obj, value, 0);
}

/* Full-body levitate bob: the authored bounce frames redraw the pose
   (feet flicker), so idle breathes by moving the rest frame as one unit. */
static void start_idle_bob(void)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, sprite);
    lv_anim_set_exec_cb(&anim, translate_y_exec);
    lv_anim_set_values(&anim, 0, -6);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_playback_duration(&anim, 900);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void stop_idle_bob(void)
{
    lv_anim_delete(sprite, translate_y_exec);
    lv_obj_set_style_translate_y(sprite, 0, 0);
}

static void hop_done(lv_anim_t *anim)
{
    LV_UNUSED(anim);
    /* The hop takes over the sprite's translate; resume the breathe after. */
    if (!walking) start_idle_bob();
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
    lv_anim_set_completed_cb(&anim, hop_done);
    lv_anim_start(&anim);
}

static void sprite_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    hop(34);
}

static void measure_frames(int32_t *width, int32_t *height)
{
    *width = idle_set.frames[0]->header.w;
    *height = idle_set.frames[0]->header.h;
    for (int row = 0; row < 8; row++) {
        *width = LV_MAX(*width, walk_sets[row].frames[0]->header.w);
        *height = LV_MAX(*height, walk_sets[row].frames[0]->header.h);
    }
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    idle_set = (anim_set_t){raichu_idle_frames, raichu_idle_durations_ms, raichu_idle_frame_count};
    walk_sets[0] = (anim_set_t){raichu_walk_s_frames, raichu_walk_s_durations_ms, raichu_walk_s_frame_count};
    walk_sets[1] = (anim_set_t){raichu_walk_se_frames, raichu_walk_se_durations_ms, raichu_walk_se_frame_count};
    walk_sets[2] = (anim_set_t){raichu_walk_e_frames, raichu_walk_e_durations_ms, raichu_walk_e_frame_count};
    walk_sets[3] = (anim_set_t){raichu_walk_ne_frames, raichu_walk_ne_durations_ms, raichu_walk_ne_frame_count};
    walk_sets[4] = (anim_set_t){raichu_walk_n_frames, raichu_walk_n_durations_ms, raichu_walk_n_frame_count};
    walk_sets[5] = (anim_set_t){raichu_walk_nw_frames, raichu_walk_nw_durations_ms, raichu_walk_nw_frame_count};
    walk_sets[6] = (anim_set_t){raichu_walk_w_frames, raichu_walk_w_durations_ms, raichu_walk_w_frame_count};
    walk_sets[7] = (anim_set_t){raichu_walk_sw_frames, raichu_walk_sw_durations_ms, raichu_walk_sw_frame_count};

    pet_root = lv_obj_create(parent);
    lv_obj_remove_style_all(pet_root);
    int32_t width, height;
    measure_frames(&width, &height);
    /* Sized to the largest animation (plus hop headroom) so nothing ever clips. */
    lv_obj_set_size(pet_root, width, height + HOP_HEADROOM);
    lv_obj_remove_flag(pet_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Start on the orbit so the first walk doesn't teleport. */
    lv_obj_set_style_translate_x(pet_root, (int32_t)(ORBIT_RADIUS_X * cosf(orbit_angle)), 0);
    lv_obj_set_style_translate_y(pet_root, (int32_t)(ORBIT_RADIUS_Y * sinf(orbit_angle)), 0);

    sprite = lv_image_create(pet_root);
    lv_obj_align(sprite, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(sprite, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sprite, sprite_clicked, LV_EVENT_CLICKED, NULL);

    frame_timer = lv_timer_create(advance_frame, 100, NULL);
    move_timer = lv_timer_create(orbit_tick, MOVE_TICK_MS, NULL);
    lv_timer_pause(move_timer);
    current_set = &idle_set;
    show_frame();
    start_idle_bob();
    return pet_root;
}

void pet_notice_steps(uint32_t delta)
{
    if (delta == 0) return;
    last_step_tick = lv_tick_get();
    if (!walking) {
        walking = true;
        stop_idle_bob();
        orbit_direction = rand() % 2 == 0 ? 1 : -1;
        lv_timer_resume(move_timer);
    }
}
