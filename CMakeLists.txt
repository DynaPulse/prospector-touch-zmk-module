# Prospector Touch ZMK module — Zephyr 4.1 / ZMK main
# The ST7789V (MIPI DBI), CST816S input and APDS9960 sensor drivers come from
# upstream Zephyr. This module ships the hybrid mode manager, the passive BLE
# status scanner, the merged LVGL UI, brightness control and NVS settings.

zephyr_include_directories(include)
# Expose ZMK's public headers (zmk/event_manager.h, zmk/keymap.h, zmk/endpoints.h,
# zmk/events/*.h, ...) to this module's sources. APPLICATION_SOURCE_DIR is the ZMK
# app directory (zmk/app), whose include/ holds the public zmk/ headers.
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library()

if(CONFIG_SHIELD_PROSPECTOR_TOUCH)
    if(CONFIG_ZMK_DISPLAY)
        zephyr_library_sources(src/display/lvgl_ui.c)
        zephyr_library_sources(src/power/brightness_control.c)
        zephyr_library_sources(src/settings/display_settings.c)
    endif()
endif()
