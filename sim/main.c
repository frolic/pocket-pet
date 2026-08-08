#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <SDL.h>
#include "lvgl.h"
#include "../app/frolic_app.h"
#include "fake_step_source.h"
#include "screenshot.h"

/* Matches the Waveshare ESP32-S3-Touch-AMOLED-2.06 panel. */
#define SCREEN_WIDTH 410
#define SCREEN_HEIGHT 502

static uint32_t tick_cb(void)
{
    return SDL_GetTicks();
}

int main(void)
{
    srand((unsigned)time(NULL));

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *display = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_sdl_window_set_title(display, "frolic (410x502)");
    lv_sdl_mouse_create();

    fake_step_source_start();
    frolic_app_init();

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
