idf_component_register(
    SRCS
        "main.c"
        "device_time.c"
        "device_button_source.c"
        "device_step_source.c"
        "../../app/frolic_app.c"
        "../../app/watchface.c"
        "../../app/pet.c"
        "../../app/pixel_scale.c"
        "../../app/pixel_text.c"
        "../../app/sprites/raichu_sprites.c"
        "../../app/sprites/field_bg.c"
        "../../app/sprites/hud_box.c"
        "../../app/sprites/pixel_font.c"
    INCLUDE_DIRS "." "../../app"
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip
)
