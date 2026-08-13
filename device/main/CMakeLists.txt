execute_process(
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_assets.py
        ${CMAKE_CURRENT_LIST_DIR}/../../app/sprites
        ${CMAKE_CURRENT_LIST_DIR}/../../assets/raichu
    RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "asset generation failed")
endif()

idf_component_register(
    SRCS
        "main.c"
        "render_test_main.c"
        "device_leash.c"
        "device_lv_malloc.c"
        "device_wifi.c"
        "device_ota.c"
        "device_power.c"
        "device_rtc.c"
        "device_sleep.c"
        "device_state.c"
        "device_touch_raw.c"
        "device_flush_gate.c"
        "device_debug_console.c"
        "device_debug.c"
        "device_button_source.c"
        "device_axp2101.c"
        "device_step_source.c"
        "../../app/frolic_app.c"
        "../../app/watchface.c"
        "../../app/pet.c"
        "../../app/pixel_scale.c"
        "../../app/display_sleep.c"
        "../../app/pixel_text.c"
        "../../app/sprites/pet_sprites.c"
        "../../app/sprites/field_bg.c"
        "../../app/sprites/hud_box.c"
        "../../app/sprites/pixel_font.c"
    INCLUDE_DIRS "." "../../app"
    REQUIRES esp32_s3_touch_amoled_2_06 driver esp_timer mbedtls esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_https_ota app_update nvs_flash lwip bt
)

# Force the custom LVGL allocator objects into the link: lvgl's archive
# needs them, but nothing in main references them directly.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

if(DEFINED ENV{FROLIC_RENDER_TEST})
    # 1 = raw esp_lcd battery, 2 = LVGL-stack battery with instrumented flush
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FROLIC_RENDER_TEST=$ENV{FROLIC_RENDER_TEST})
endif()

if(DEFINED ENV{FROLIC_DISABLE_WIFI})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FROLIC_DISABLE_WIFI=1)
endif()

if(DEFINED ENV{FROLIC_DEBUG})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FROLIC_DEBUG=1)
endif()

if(DEFINED ENV{FROLIC_OTA_URL})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FROLIC_OTA_URL="$ENV{FROLIC_OTA_URL}")
endif()

if(DEFINED ENV{FROLIC_OTA_FAST})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FROLIC_OTA_FAST=1)
endif()
