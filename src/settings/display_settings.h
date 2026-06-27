/*
 * Prospector Touch - on-device settings persisted via the Zephyr settings
 * subsystem (NVS backend, shared with ZMK). One packed record under the
 * "prospector/cfg" key. Clean-room.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_settings_init(void);
void prospector_settings_flush(void); /* persist if dirty */

bool display_settings_get_auto_brightness(void);
void display_settings_set_auto_brightness(bool enabled);

uint8_t display_settings_get_manual_brightness(void);
void display_settings_set_manual_brightness(uint8_t level);

uint8_t display_settings_get_layout(void);
void display_settings_set_layout(uint8_t layout);

uint8_t display_settings_get_max_layers(void);
void display_settings_set_max_layers(uint8_t max);

#ifdef __cplusplus
}
#endif
