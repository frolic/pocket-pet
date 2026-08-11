#pragma once

/* Entry point of the render-characterization firmware (built with
   FROLIC_RENDER_TEST=1): runs the esp_lcd draw-path test battery instead of
   the app and never returns. See render_test_main.c for the phase list. */
void render_test_main(void);
