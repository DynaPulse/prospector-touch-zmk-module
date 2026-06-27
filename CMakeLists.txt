# Prospector Touch ZMK module — Zephyr 4.1 / ZMK main
# The ST7789V (MIPI DBI), CST816S input and APDS9960 sensor drivers come from
# upstream Zephyr. This module ships the hybrid mode manager, the passive BLE
# status scanner, the merged LVGL UI, brightness control and NVS settings.

zephyr_include_directories(include)
zephyr_library()

if(CONFIG_SHIELD_PROSPECTOR_TOUCH)
    # Mode manager provides prospector_mode_get(); always built.
    zephyr_library_sources(src/hybrid_state_manager.c)

    if(CONFIG_PROSPECTOR_MODE_SCANNER)
        zephyr_library_sources(src/ble/status_scanner.c)
    endif()

    if(CONFIG_ZMK_DISPLAY)
        zephyr_library_sources(src/display/lvgl_ui.c)
        zephyr_library_sources(src/power/brightness_control.c)
        zephyr_library_sources(src/settings/display_settings.c)
    endif()
endif()
