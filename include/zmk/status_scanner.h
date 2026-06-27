/*
 * Prospector status scanner (BLE observer) public API.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zmk/status_advertisement.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMK_STATUS_SCANNER_MAX_KEYBOARDS CONFIG_PROSPECTOR_MAX_KEYBOARDS

struct zmk_keyboard_status {
    bool active;
    uint32_t last_seen;
    struct zmk_status_adv_data data;
    int8_t rssi;
    char ble_name[32];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
};

enum zmk_status_scanner_event {
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND,
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED,
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST,
};

struct zmk_status_scanner_event_data {
    enum zmk_status_scanner_event event;
    int keyboard_index;
    struct zmk_keyboard_status *status;
};

typedef void (*zmk_status_scanner_callback_t)(struct zmk_status_scanner_event_data *event_data);

int zmk_status_scanner_init(void);
int zmk_status_scanner_start(void);
int zmk_status_scanner_stop(void);
int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t callback);
struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index);
int zmk_status_scanner_get_active_count(void);
int zmk_status_scanner_get_primary_keyboard(void);

/* Channel filter (0 = accept all). */
void zmk_status_scanner_set_channel(uint8_t channel);
uint8_t zmk_status_scanner_get_channel(void);

/* Selected keyboard for display (-1 = auto / most-recent). */
void zmk_status_scanner_set_selected(int index);
int zmk_status_scanner_get_selected(void);

/* Re-push the currently displayed keyboard to the UI. */
void zmk_status_scanner_refresh_display(void);

/* Implemented by the display unit; called for the primary keyboard on update. */
void prospector_ui_on_scanner_update(const struct zmk_keyboard_status *ks);

#ifdef __cplusplus
}
#endif
