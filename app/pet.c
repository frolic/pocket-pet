#include <math.h>
#include <stdlib.h>
#include "pet.h"
#include "sprites/raichu_sprites.h"

/* How long after the last step the pet keeps walking before settling down. */
#define WALK_LINGER_MS 2500
/* Continued quiet after settling before he sits down. */
#define SIT_AFTER_MS 20000
/* Rectangle the pet roams, as translate offsets from his home position. */
#define ROAM_MIN_X (-115)
#define ROAM_MAX_X 115
#define ROAM_MIN_Y (-60)
#define ROAM_MAX_Y 70
#define MIN_LEG_PX 60
#define WALK_SPEED_PX_S 70
#define MOVE_TICK_MS 33
/* Extra container height above the sprite so the hop never leaves its bounds. */
#define HOP_HEADROOM 40
/* Turn-in-place cadence: one 45-degree facing step per beat. */
#define TURN_STEP_MS 120
#define FACING_SOUTH 0
#define FACING_NORTH 4

/*
 * The Sit artwork is single-row and faces away from the camera, so sitting
 * means facing north: he turns to north before sitting down, and after
 * standing up he is facing north and turns from there — no snapping.
 */
typedef enum {
    PET_STATE_WANDER,
    PET_STATE_TURN_SOUTH,
    PET_STATE_IDLE,
    PET_STATE_TURN_NORTH,
    PET_STATE_SIT_DOWN,
    PET_STATE_SEATED,
    PET_STATE_STAND_UP,
} pet_state_t;

/* Idle texture: long rests between bounces, occasional brief sit instead. */
#define IDLE_REST_MIN_MS 2500
#define IDLE_REST_MAX_MS 6000
#define SIT_BREAK_CHANCE_PCT 30
#define SEATED_HOLD_MIN_MS 2500
#define SEATED_HOLD_MAX_MS 6000

typedef enum {
    WANDER_TURNING,
    WANDER_WALKING,
    WANDER_PAUSING,
} wander_phase_t;

typedef struct {
    const lv_image_dsc_t *const *frames;
    const uint32_t *durations_ms;
    uint32_t count;
} anim_set_t;

static anim_set_t idle_set;
static anim_set_t sit_set;
/* Indexed by sheet-row order: S, SE, E, NE, N, NW, W, SW. */
static anim_set_t walk_sets[8];

static lv_obj_t *pet_root;
static lv_obj_t *sprite;
static lv_timer_t *frame_timer;
static lv_timer_t *move_timer;
static const anim_set_t *current_set;
static pet_state_t state = PET_STATE_IDLE;
static wander_phase_t wander_phase = WANDER_PAUSING;
static int facing = FACING_SOUTH;
static int target_facing = FACING_SOUTH;
static uint32_t frame_index;
static uint32_t last_step_tick;
static float pos_x, pos_y = 30.0f;
static float target_x, target_y;
static int32_t pause_left_ms;
static uint32_t turn_accum_ms;
static bool seated_permanent;   /* long-quiet sit vs a brief idle sit-break */
static bool stand_up_to_wander; /* steps arrived while seated — walk after standing */

static void show_frame(void)
{
    lv_image_set_src(sprite, current_set->frames[frame_index]);
    lv_timer_set_period(frame_timer, current_set->durations_ms[frame_index]);
}

/*
 * Idle rests and seated holds park the frame timer on multi-second periods.
 * When a state change interrupts one, the next frame decision must happen
 * now — otherwise movement resumes under a frozen sprite (the glide bug).
 */
static void kick_frame_timer(void)
{
    lv_timer_set_period(frame_timer, 1);
    lv_timer_reset(frame_timer);
}

static void set_anim(const anim_set_t *set)
{
    if (current_set == set) return;
    current_set = set;
    if (set == &idle_set || set == &sit_set) {
        frame_index = 0;
    } else {
        /* Keep cadence position so direction changes don't restart the stride. */
        frame_index %= set->count;
    }
    show_frame();
}

/* Maps a movement heading (screen coords, y down) onto the 8 sheet rows. */
static int heading_row(float velocity_x, float velocity_y)
{
    static const int row_for_octant[8] = {2, 1, 0, 7, 6, 5, 4, 3};
    float degrees = atan2f(velocity_y, velocity_x) * 180.0f / (float)M_PI;
    int octant = ((int)lroundf(degrees / 45.0f) + 8) % 8;
    return row_for_octant[octant];
}

/* One 45-degree facing step toward `toward`, along the shorter rotation. */
static void face_step_toward(int toward)
{
    int plus_steps = (toward - facing + 8) % 8;
    int minus_steps = (facing - toward + 8) % 8;
    facing = (facing + (plus_steps <= minus_steps ? 1 : 7)) % 8;
    current_set = &walk_sets[facing];
    frame_index = 0;
    lv_image_set_src(sprite, current_set->frames[0]);
}

static void apply_position(void)
{
    lv_obj_set_style_translate_x(pet_root, (int32_t)pos_x, 0);
    lv_obj_set_style_translate_y(pet_root, (int32_t)pos_y, 0);
}

static float random_in(float low, float high)
{
    return low + (high - low) * ((float)rand() / (float)RAND_MAX);
}

static void pick_new_target(void)
{
    do {
        target_x = random_in(ROAM_MIN_X, ROAM_MAX_X);
        target_y = random_in(ROAM_MIN_Y, ROAM_MAX_Y);
    } while (hypotf(target_x - pos_x, target_y - pos_y) < MIN_LEG_PX);
    target_facing = heading_row(target_x - pos_x, target_y - pos_y);
    turn_accum_ms = 0;
    wander_phase = facing == target_facing ? WANDER_WALKING : WANDER_TURNING;
}

/* Aimless explore: turn toward a random point, amble there, pause, repeat. */
static void wander_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    switch (wander_phase) {
    case WANDER_TURNING:
        turn_accum_ms += MOVE_TICK_MS;
        if (turn_accum_ms >= TURN_STEP_MS) {
            turn_accum_ms = 0;
            face_step_toward(target_facing);
            if (facing == target_facing) wander_phase = WANDER_WALKING;
        }
        break;
    case WANDER_WALKING: {
        float distance_x = target_x - pos_x;
        float distance_y = target_y - pos_y;
        float distance = hypotf(distance_x, distance_y);
        float step = WALK_SPEED_PX_S * MOVE_TICK_MS / 1000.0f;
        if (distance <= step) {
            pos_x = target_x;
            pos_y = target_y;
            apply_position();
            pause_left_ms = (int32_t)random_in(250, 1500);
            wander_phase = WANDER_PAUSING;
            break;
        }
        pos_x += distance_x / distance * step;
        pos_y += distance_y / distance * step;
        apply_position();
        set_anim(&walk_sets[facing]);
        break;
    }
    case WANDER_PAUSING:
        pause_left_ms -= MOVE_TICK_MS;
        if (pause_left_ms <= 0) pick_new_target();
        break;
    }
}

static void advance_frame(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    switch (state) {
    case PET_STATE_WANDER:
        if (lv_tick_elaps(last_step_tick) > WALK_LINGER_MS) {
            lv_timer_pause(move_timer);
            state = PET_STATE_TURN_SOUTH;
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
            return;
        }
        if (wander_phase != WANDER_WALKING) {
            /* Standing while turning or pausing — hold the standing pose. */
            if (frame_index != 0) {
                frame_index = 0;
                lv_image_set_src(sprite, current_set->frames[0]);
            }
            lv_timer_set_period(frame_timer, 100);
            return;
        }
        break;
    case PET_STATE_TURN_SOUTH:
        if (facing == FACING_SOUTH) {
            state = PET_STATE_IDLE;
            set_anim(&idle_set);
            return;
        }
        face_step_toward(FACING_SOUTH);
        lv_timer_set_period(frame_timer, TURN_STEP_MS);
        return;
    case PET_STATE_IDLE:
        if (lv_tick_elaps(last_step_tick) > WALK_LINGER_MS + SIT_AFTER_MS) {
            seated_permanent = true;
            state = PET_STATE_TURN_NORTH;
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
            return;
        }
        if (frame_index == 0) {
            /* A rest hold just ended: usually bounce, sometimes sit a moment. */
            if (rand() % 100 < SIT_BREAK_CHANCE_PCT) {
                seated_permanent = false;
                state = PET_STATE_TURN_NORTH;
                lv_timer_set_period(frame_timer, TURN_STEP_MS);
                return;
            }
            frame_index = 1;
            show_frame();
            return;
        }
        frame_index = (frame_index + 1) % current_set->count;
        show_frame();
        if (frame_index == 0) {
            lv_timer_set_period(frame_timer,
                (uint32_t)(IDLE_REST_MIN_MS + rand() % (IDLE_REST_MAX_MS - IDLE_REST_MIN_MS)));
        }
        return;
    case PET_STATE_TURN_NORTH:
        if (facing == FACING_NORTH) {
            state = PET_STATE_SIT_DOWN;
            set_anim(&sit_set);
            return;
        }
        face_step_toward(FACING_NORTH);
        lv_timer_set_period(frame_timer, TURN_STEP_MS);
        return;
    case PET_STATE_SIT_DOWN:
        if (frame_index < current_set->count - 1) {
            frame_index++;
            show_frame();
        } else {
            state = PET_STATE_SEATED;
            lv_timer_set_period(frame_timer, seated_permanent
                ? 500
                : (uint32_t)(SEATED_HOLD_MIN_MS + rand() % (SEATED_HOLD_MAX_MS - SEATED_HOLD_MIN_MS)));
        }
        return;
    case PET_STATE_SEATED:
        if (!seated_permanent) {
            stand_up_to_wander = false;
            state = PET_STATE_STAND_UP;
            lv_timer_set_period(frame_timer, 1);
        }
        return;
    case PET_STATE_STAND_UP:
        if (frame_index > 0) {
            frame_index--;
            show_frame();
        } else if (stand_up_to_wander) {
            state = PET_STATE_WANDER;
            pick_new_target();
            lv_timer_resume(move_timer);
            kick_frame_timer();
        } else {
            state = PET_STATE_TURN_SOUTH;
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
        }
        return;
    }
    frame_index = (frame_index + 1) % current_set->count;
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

static void measure_frames(int32_t *width, int32_t *height)
{
    *width = LV_MAX(idle_set.frames[0]->header.w, sit_set.frames[0]->header.w);
    *height = LV_MAX(idle_set.frames[0]->header.h, sit_set.frames[0]->header.h);
    for (int row = 0; row < 8; row++) {
        *width = LV_MAX(*width, walk_sets[row].frames[0]->header.w);
        *height = LV_MAX(*height, walk_sets[row].frames[0]->header.h);
    }
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    idle_set = (anim_set_t){raichu_idle_frames, raichu_idle_durations_ms, raichu_idle_frame_count};
    sit_set = (anim_set_t){raichu_sit_frames, raichu_sit_durations_ms, raichu_sit_frame_count};
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
    apply_position();

    sprite = lv_image_create(pet_root);
    lv_obj_align(sprite, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(sprite, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sprite, sprite_clicked, LV_EVENT_CLICKED, NULL);

    frame_timer = lv_timer_create(advance_frame, 100, NULL);
    move_timer = lv_timer_create(wander_tick, MOVE_TICK_MS, NULL);
    lv_timer_pause(move_timer);
    current_set = &idle_set;
    show_frame();
    return pet_root;
}

void pet_notice_steps(uint32_t delta)
{
    if (delta == 0) return;
    last_step_tick = lv_tick_get();
    switch (state) {
    case PET_STATE_WANDER:
        break;
    case PET_STATE_SIT_DOWN:
    case PET_STATE_SEATED:
        /* Stand up first (reverse sit), then walk from the north facing. */
        stand_up_to_wander = true;
        state = PET_STATE_STAND_UP;
        kick_frame_timer();
        break;
    case PET_STATE_STAND_UP:
        stand_up_to_wander = true;
        break;
    default:
        state = PET_STATE_WANDER;
        pick_new_target();
        lv_timer_resume(move_timer);
        kick_frame_timer();
    }
}
