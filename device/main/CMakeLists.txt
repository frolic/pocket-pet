idf_component_register(
    SRCS
        "main.c"
        "device_wifi.c"
        "device_ota.c"
        "device_power.c"
        "device_state.c"
        "device_touch_raw.c"
        "device_flush_gate.c"
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
        "../../app/sprites/raichu_sprites.c"
        "../../app/sprites/field_bg.c"
        "../../app/sprites/hud_box.c"
        "../../app/sprites/pixel_font.c"
    INCLUDE_DIRS "." "../../app"
    REQUIRES esp32_s3_touch_amoled_2_06 driver esp_timer esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_https_ota app_update nvs_flash lwip
)

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
