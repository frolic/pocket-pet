cmake_minimum_required(VERSION 3.20)
project(frolic C)

# --- Generated assets (sprites/HUD/font). Only sources live in git; the C
# arrays regenerate here at configure time. Swap POKEMON to change character. ---
set(POKEMON "raichu" CACHE STRING "Pet sprite set under assets/")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_SOURCE_DIR}/tools/gen_assets.py
        ${CMAKE_SOURCE_DIR}/app/sprites
        ${CMAKE_SOURCE_DIR}/assets/${POKEMON}
    RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "asset generation failed")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# LVGL is fetched at configure time so the repo stays dependency-light.
# The same app/ sources will later be built by the PlatformIO device target.
include(FetchContent)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
set(LV_CONF_BUILD_DISABLE_DEMOS ON)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

find_package(SDL2 REQUIRED CONFIG)

# LVGL reads lv_conf.h from this directory (minimal file, defaults elsewhere).
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

add_executable(frolic_sim
    sim/main.c
    sim/debug_panel.c
    sim/fake_step_source.c
    sim/fake_battery_source.c
    sim/screenshot.c
    app/frolic_app.c
    app/watchface.c
    app/pet.c
    app/display_sleep.c
    app/pixel_scale.c
    app/pixel_text.c
    app/sprites/pixel_font.c
    app/sprites/hud_box.c
    app/sprites/pet_sprites.c
    app/sprites/field_bg.c
)
target_include_directories(frolic_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frolic_sim PRIVATE lvgl SDL2::SDL2)
