#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <SDL.h>
#include "lvgl.h"
#include "../app/frolic_app.h"
#include "debug_panel.h"
#include "fake_step_source.h"
#include "screenshot.h"

/* Matches the Waveshare ESP32-S3-Touch-AMOLED-2.06 panel. */
#define SCREEN_WIDTH 410
#define SCREEN_HEIGHT 502
/* Sim-only rail to the right of the watch panel. */
#define DEBUG_RAIL_WIDTH 150

static uint32_t tick_cb(void)
{
    return SDL_GetTicks();
}

int main(void)
{
    srand((unsigned)time(NULL));

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *display = lv_sdl_window_create(SCREEN_WIDTH + DEBUG_RAIL_WIDTH, SCREEN_HEIGHT);
    lv_sdl_window_set_title(display, "frolic (410x502 + debug)");
    lv_sdl_mouse_create();

    /* The watch panel proper — pixel-exact, clips like the real bezel. */
    lv_obj_t *panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(panel, 0, 0);

    fake_step_source_start();
    frolic_app_init(panel);

    /* Rounded-glass preview: black mask whose inner window uses the corner
       radius eyeballed against the real panel (the drawing's R4.5mm is the
       case corner; the visible glass rounds much deeper — ~100px at this
       0.081mm pixel pitch). Sim-only. */
    lv_obj_t *bezel = lv_obj_create(panel);
    lv_obj_remove_style_all(bezel);
    lv_obj_set_size(bezel, SCREEN_WIDTH + 200, SCREEN_HEIGHT + 200);
    lv_obj_set_pos(bezel, -100, -100);
    lv_obj_set_style_radius(bezel, 200, 0);
    lv_obj_set_style_border_width(bezel, 100, 0);
    lv_obj_set_style_border_color(bezel, lv_color_black(), 0);
    lv_obj_remove_flag(bezel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);

    debug_panel_create(lv_screen_active());

    /*
     * FROLIC_SMOKE=1 exits cleanly after ~3s so scripts can verify the build runs.
     * FROLIC_SHOT=path.bmp writes a screenshot at ~2.5s (headless visual check).
     */
    bool smoke = getenv("FROLIC_SMOKE") != NULL;
    const char *shot_path = getenv("FROLIC_SHOT");
    uint32_t start = SDL_GetTicks();
    while (true) {
        lv_timer_handler();
        SDL_Delay(5);
        if (shot_path != NULL && SDL_GetTicks() - start > 2500) {
            screenshot_write_bmp(shot_path);
            shot_path = NULL;
        }
        if (smoke && SDL_GetTicks() - start > 3000) return 0;
    }
}
