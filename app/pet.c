#include <math.h>
#include <stdlib.h>
#include "pet.h"
#include "pixel_scale.h"
#include "sprites/raichu_sprites.h"

/* How long after the last step the pet keeps walking before settling down. */
#define WALK_LINGER_MS 2500
/* Continued quiet after settling before he sits down. */
#define SIT_AFTER_MS 20000
/* Rectangle the pet roams, as translate offsets from his home position.
   X reaches the panel edges: up to half his body can leave the frame. */
#define ROAM_MIN_X (-200)
#define ROAM_MAX_X 200
#define ROAM_MIN_Y (-80)
#define ROAM_MAX_Y 80
#define MIN_LEG_PX 60
#define WALK_SPEED_PX_S 70
#define MOVE_TICK_MS 33
/* Extra container height above the sprite so the hop never leaves its bounds. */
#define HOP_HEADROOM 40
/* Turn-in-place cadence: one 45-degree facing step per beat. */
#define TURN_STEP_MS 120
#define FACING_SOUTH 0
#define FACING_NORTH 4
/* Feet anchor on the panel: fixed so animation canvas sizes never move him. */
#define PET_FEET_Y 334
/* The Shock burst plays this many times when celebrating. */
#define CELEBRATE_LOOPS 2

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
    PET_STATE_TURN_LISTEN,    /* record button held: turn to face the viewer... */
    PET_STATE_LISTENING,      /* ...and nod along while they speak */
    PET_STATE_ACK_NOD,        /* recording done: two deliberate nods... */
    PET_STATE_CELEBRATE,      /* ...then the happy flourish... */
    PET_STATE_STAND_WAIT,     /* ...then stand quietly a moment before moving on */
    PET_STATE_TURN_CELEBRATE, /* milestone hit: turn to face the viewer... */
    PET_STATE_CELEBRATE_PLAY, /* ...and play the celebration animation */
    PET_STATE_LIE_DOWN,       /* long quiet after sitting: settle down to sleep... */
    PET_STATE_SLEEPING,       /* ...curled breathing loop; only steps or a tap wake him */
    PET_STATE_WAKING,         /* the stretch-and-stand Wake sequence */
} pet_state_t;

/* Seated this long (permanent sit) before lying down to sleep. */
#define SLEEP_AFTER_MS 30000

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
static anim_set_t nod_set;
static anim_set_t pose_set;
static anim_set_t shock_set;
static anim_set_t hop_set;
static anim_set_t breath_set;
static anim_set_t laying_set;
static anim_set_t sleep_set;
static anim_set_t wake_set;
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
static bool listen_requested;   /* record button held while mid-stand-up */
static bool wake_to_wander;     /* steps woke him — walk after the stretch */
static uint32_t seated_since_tick;
static bool called_over;        /* walking to a tapped spot — don't settle mid-leg */
static pet_celebration_t celebration_kind;
static uint32_t celebrate_loops_left;
static uint32_t ack_nods_left;

static void hop(int32_t height);

/* Frames ship native-res and are upscaled on demand into ping-pong scratch
   buffers (alternating src pointers so LVGL sees every change). */
static lv_image_dsc_t frame_scratch[2];
static uint8_t *frame_buffer[2];
static int frame_scratch_next;

static void show_frame(void)
{
    int slot = frame_scratch_next;
    frame_scratch_next ^= 1;
    pixel_scale_into(current_set->frames[frame_index], RAICHU_SPRITE_SCALE,
                     &frame_scratch[slot], frame_buffer[slot]);
    lv_image_set_src(sprite, &frame_scratch[slot]);
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
    bool is_walk = set >= &walk_sets[0] && set <= &walk_sets[7];
    if (is_walk) {
        /* Keep cadence position so direction changes don't restart the stride. */
        frame_index %= set->count;
    } else {
        frame_index = 0;
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
    show_frame();
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
            /* Arrived at a tapped spot: normal settle rules apply again. */
            called_over = false;
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
        if (!called_over && lv_tick_elaps(last_step_tick) > WALK_LINGER_MS) {
            lv_timer_pause(move_timer);
            state = PET_STATE_TURN_SOUTH;
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
            return;
        }
        if (wander_phase != WANDER_WALKING) {
            /* Standing while turning or pausing — hold the standing pose. */
            if (frame_index != 0) {
                frame_index = 0;
                show_frame();
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
            seated_since_tick = lv_tick_get();
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
            return;
        }
        if (lv_tick_elaps(seated_since_tick) > SLEEP_AFTER_MS) {
            state = PET_STATE_LIE_DOWN;
            set_anim(&laying_set);
            lv_timer_set_period(frame_timer, 700);
        }
        return;
    case PET_STATE_LIE_DOWN:
        state = PET_STATE_SLEEPING;
        set_anim(&sleep_set);
        return;
    case PET_STATE_SLEEPING:
        /* Curled breathing loop; nothing else happens while he sleeps. */
        frame_index = (frame_index + 1) % current_set->count;
        show_frame();
        return;
    case PET_STATE_WAKING:
        if (frame_index < current_set->count - 1) {
            frame_index++;
            show_frame();
        } else if (wake_to_wander) {
            state = PET_STATE_WANDER;
            pick_new_target();
            lv_timer_resume(move_timer);
            kick_frame_timer();
        } else {
            state = PET_STATE_TURN_SOUTH;
            facing = FACING_NORTH; /* woke from lying; turn back around to face front */
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
        }
        return;
    case PET_STATE_STAND_UP:
        if (frame_index > 0) {
            frame_index--;
            show_frame();
        } else if (listen_requested) {
            listen_requested = false;
            state = PET_STATE_TURN_LISTEN;
            lv_timer_set_period(frame_timer, TURN_STEP_MS);
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
    case PET_STATE_TURN_LISTEN:
        if (facing == FACING_SOUTH) {
            state = PET_STATE_LISTENING;
            set_anim(&nod_set);
            return;
        }
        face_step_toward(FACING_SOUTH);
        lv_timer_set_period(frame_timer, TURN_STEP_MS);
        return;
    case PET_STATE_LISTENING:
        /* Nods along while the user speaks: nod, a beat of stillness, nod. */
        frame_index = (frame_index + 1) % current_set->count;
        show_frame();
        if (frame_index == 0) {
            lv_timer_set_period(frame_timer, 500 + (uint32_t)(rand() % 800));
        }
        return;
    case PET_STATE_ACK_NOD:
        frame_index++;
        if (frame_index >= current_set->count) {
            if (--ack_nods_left == 0) {
                state = PET_STATE_CELEBRATE;
                set_anim(&pose_set);
                lv_timer_reset(frame_timer);
                hop(24);
                return;
            }
            frame_index = 0;
        }
        show_frame();
        return;
    case PET_STATE_CELEBRATE:
        if (frame_index < current_set->count - 1) {
            frame_index++;
            show_frame();
        } else {
            /* Stand quietly for a beat before going back to his day. */
            state = PET_STATE_STAND_WAIT;
            set_anim(&idle_set);
            lv_timer_set_period(frame_timer, 1700);
        }
        return;
    case PET_STATE_STAND_WAIT:
        state = PET_STATE_IDLE;
        lv_timer_set_period(frame_timer, 100);
        return;
    case PET_STATE_TURN_CELEBRATE:
        if (facing == FACING_SOUTH) {
            state = PET_STATE_CELEBRATE_PLAY;
            switch (celebration_kind) {
            case PET_CELEBRATION_SHOCK:
                celebrate_loops_left = CELEBRATE_LOOPS;
                set_anim(&shock_set);
                hop(30);
                break;
            case PET_CELEBRATION_HOP:
                celebrate_loops_left = CELEBRATE_LOOPS;
                set_anim(&hop_set);
                break;
            case PET_CELEBRATION_BREATH:
                celebrate_loops_left = 1;
                set_anim(&breath_set);
                break;
            }
            return;
        }
        face_step_toward(FACING_SOUTH);
        lv_timer_set_period(frame_timer, TURN_STEP_MS);
        return;
    case PET_STATE_CELEBRATE_PLAY:
        frame_index++;
        if (frame_index >= current_set->count) {
            if (--celebrate_loops_left == 0) {
                state = PET_STATE_IDLE;
                set_anim(&idle_set);
                lv_timer_reset(frame_timer);
                return;
            }
            frame_index = 0;
        }
        show_frame();
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
    switch (state) {
    case PET_STATE_SIT_DOWN:
    case PET_STATE_SEATED:
        /* A tap while sitting stands him up. */
        stand_up_to_wander = false;
        state = PET_STATE_STAND_UP;
        kick_frame_timer();
        return;
    case PET_STATE_LIE_DOWN:
    case PET_STATE_SLEEPING:
        /* A tap wakes him: stretch, stand, turn to face you. */
        wake_to_wander = false;
        state = PET_STATE_WAKING;
        set_anim(&wake_set);
        kick_frame_timer();
        return;
    case PET_STATE_TURN_LISTEN:
    case PET_STATE_LISTENING:
    case PET_STATE_ACK_NOD:
    case PET_STATE_WAKING:
        return;
    default:
        hop(34);
    }
}

/* Largest native frame across every animation set. */
static void measure_frames(int32_t *width, int32_t *height)
{
    const anim_set_t *sets[] = {&idle_set, &sit_set, &nod_set, &pose_set, &shock_set,
                                &hop_set, &breath_set, &laying_set, &sleep_set, &wake_set,
                                &walk_sets[0], &walk_sets[1], &walk_sets[2], &walk_sets[3],
                                &walk_sets[4], &walk_sets[5], &walk_sets[6], &walk_sets[7]};
    *width = 0;
    *height = 0;
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        *width = LV_MAX(*width, sets[i]->frames[0]->header.w);
        *height = LV_MAX(*height, sets[i]->frames[0]->header.h);
    }
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    idle_set = (anim_set_t){raichu_idle_frames, raichu_idle_durations_ms, raichu_idle_frame_count};
    sit_set = (anim_set_t){raichu_sit_frames, raichu_sit_durations_ms, raichu_sit_frame_count};
    nod_set = (anim_set_t){raichu_nod_frames, raichu_nod_durations_ms, raichu_nod_frame_count};
    pose_set = (anim_set_t){raichu_pose_frames, raichu_pose_durations_ms, raichu_pose_frame_count};
    shock_set = (anim_set_t){raichu_shock_frames, raichu_shock_durations_ms, raichu_shock_frame_count};
    hop_set = (anim_set_t){raichu_hop_frames, raichu_hop_durations_ms, raichu_hop_frame_count};
    breath_set = (anim_set_t){raichu_breath_frames, raichu_breath_durations_ms, raichu_breath_frame_count};
    laying_set = (anim_set_t){raichu_laying_frames, raichu_laying_durations_ms, raichu_laying_frame_count};
    sleep_set = (anim_set_t){raichu_sleep_frames, raichu_sleep_durations_ms, raichu_sleep_frame_count};
    wake_set = (anim_set_t){raichu_wake_frames, raichu_wake_durations_ms, raichu_wake_frame_count};
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
    int32_t native_width, native_height;
    measure_frames(&native_width, &native_height);
    int32_t width = native_width * RAICHU_SPRITE_SCALE;
    int32_t height = native_height * RAICHU_SPRITE_SCALE;
    /* Sized to the largest animation (plus hop headroom) so nothing ever clips,
       anchored by the feet so canvas growth (e.g. Shock's bolts) never moves him. */
    lv_obj_set_size(pet_root, width, height + HOP_HEADROOM);
    lv_obj_align(pet_root, LV_ALIGN_TOP_MID, 0, PET_FEET_Y - (height + HOP_HEADROOM));
    lv_obj_remove_flag(pet_root, LV_OBJ_FLAG_SCROLLABLE);
    apply_position();

    size_t scratch_size = (size_t)native_width * native_height *
                          RAICHU_SPRITE_SCALE * RAICHU_SPRITE_SCALE * 4;
    frame_buffer[0] = malloc(scratch_size);
    frame_buffer[1] = malloc(scratch_size);
    LV_ASSERT_NULL(frame_buffer[0]);
    LV_ASSERT_NULL(frame_buffer[1]);

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

void pet_freeze(bool frozen)
{
    if (frozen) {
        lv_timer_pause(frame_timer);
        lv_timer_pause(move_timer);
    } else {
        lv_timer_resume(frame_timer);
        if (state == PET_STATE_WANDER) lv_timer_resume(move_timer);
    }
}

void pet_set_paused(bool paused)
{
    if (paused) {
        lv_timer_pause(move_timer);
        state = PET_STATE_IDLE;
        set_anim(&idle_set);
        lv_timer_pause(frame_timer);
    } else {
        lv_timer_resume(frame_timer);
    }
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
    case PET_STATE_LIE_DOWN:
    case PET_STATE_SLEEPING:
        /* Steps wake him: stretch, then off we go. */
        wake_to_wander = true;
        state = PET_STATE_WAKING;
        set_anim(&wake_set);
        kick_frame_timer();
        break;
    case PET_STATE_WAKING:
        wake_to_wander = true;
        break;
    case PET_STATE_STAND_UP:
        stand_up_to_wander = true;
        break;
    case PET_STATE_TURN_LISTEN:
    case PET_STATE_LISTENING:
    case PET_STATE_ACK_NOD:
    case PET_STATE_CELEBRATE:
    case PET_STATE_STAND_WAIT:
    case PET_STATE_TURN_CELEBRATE:
    case PET_STATE_CELEBRATE_PLAY:
        /* Attention interactions outrank walking; steps refresh the timestamp. */
        break;
    default:
        state = PET_STATE_WANDER;
        pick_new_target();
        lv_timer_resume(move_timer);
        kick_frame_timer();
    }
}

void pet_celebrate(pet_celebration_t kind)
{
    switch (state) {
    case PET_STATE_TURN_LISTEN:
    case PET_STATE_LISTENING:
    case PET_STATE_TURN_CELEBRATE:
    case PET_STATE_CELEBRATE_PLAY:
        /* Listening outranks celebrating; already celebrating repeats nothing. */
        return;
    default:
        break;
    }
    celebration_kind = kind;
    lv_timer_pause(move_timer);
    called_over = false;
    state = PET_STATE_TURN_CELEBRATE;
    kick_frame_timer();
}

void pet_listen_start(void)
{
    lv_timer_pause(move_timer);
    switch (state) {
    case PET_STATE_SIT_DOWN:
    case PET_STATE_SEATED:
        /* Stand up first, then turn to listen. */
        listen_requested = true;
        state = PET_STATE_STAND_UP;
        kick_frame_timer();
        break;
    case PET_STATE_STAND_UP:
        listen_requested = true;
        break;
    case PET_STATE_TURN_LISTEN:
    case PET_STATE_LISTENING:
    case PET_STATE_ACK_NOD:
    case PET_STATE_CELEBRATE:
        break;
    default:
        state = PET_STATE_TURN_LISTEN;
        kick_frame_timer();
    }
}

void pet_listen_end(void)
{
    listen_requested = false;
    if (state == PET_STATE_TURN_LISTEN) {
        /* Released mid-turn: finish turning to face front, then idle. */
        state = PET_STATE_TURN_SOUTH;
        lv_timer_set_period(frame_timer, TURN_STEP_MS);
    } else if (state == PET_STATE_LISTENING) {
        /* Outro: two deliberate nods, then the flourish (see ACK_NOD chain). */
        state = PET_STATE_ACK_NOD;
        ack_nods_left = 2;
        set_anim(&nod_set);
        lv_timer_reset(frame_timer);
    }
}

void pet_call_to(int32_t x, int32_t y)
{
    switch (state) {
    case PET_STATE_WANDER:
    case PET_STATE_TURN_SOUTH:
    case PET_STATE_IDLE:
        break;
    default:
        /* Seated or listening: he's busy; ignore the tap. */
        return;
    }

    lv_obj_t *parent = lv_obj_get_parent(pet_root);
    /* Feet sit at the fixed anchor; aim them at the tapped spot. */
    float home_x = lv_obj_get_width(parent) / 2.0f;
    target_x = LV_CLAMP(ROAM_MIN_X, x - home_x, ROAM_MAX_X);
    target_y = LV_CLAMP(ROAM_MIN_Y, y - PET_FEET_Y, ROAM_MAX_Y);
    if (hypotf(target_x - pos_x, target_y - pos_y) < 10.0f) return;

    target_facing = heading_row(target_x - pos_x, target_y - pos_y);
    turn_accum_ms = 0;
    wander_phase = facing == target_facing ? WANDER_WALKING : WANDER_TURNING;
    called_over = true;
    state = PET_STATE_WANDER;
    lv_timer_resume(move_timer);
    kick_frame_timer();
}
