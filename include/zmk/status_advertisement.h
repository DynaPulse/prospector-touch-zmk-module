/*
 * Prospector status-advertisement wire format (decode side).
 *
 * 26-byte manufacturer-data payload broadcast by ZMK keyboards that run the
 * Prospector status-advertisement feature. The layout is reproduced here only
 * for binary interoperability with existing keyboards; no implementation code
 * was copied.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zmk_status_adv_data {
    uint8_t manufacturer_id[2];    /* 0xFF 0xFF (reserved company id) */
    uint8_t service_uuid[2];       /* 0xAB 0xCD (Prospector) */
    uint8_t version;               /* [7:4]=major, [3:0]=minor */
    uint8_t battery_level;         /* central/standalone, 0-100 */
    uint8_t active_layer;          /* 0-15 */
    uint8_t profile_slot;          /* [6]=dev, [5:3]=patch, [2:0]=profile */
    uint8_t connection_count;      /* 0-5 */
    uint8_t status_flags;          /* see ZMK_STATUS_FLAG_* */
    uint8_t device_role;           /* 0=standalone, 1=central, 2=peripheral */
    uint8_t device_index;
    uint8_t peripheral_battery[3]; /* left, right/aux, third (0=N/A) */
    char    layer_name[4];         /* null-terminated, up to 4 chars */
    uint8_t keyboard_id[4];        /* HWINFO-derived unique id */
    uint8_t modifier_flags;        /* see ZMK_MOD_FLAG_* */
    uint8_t wpm_value;             /* 0-255 (0 = unknown) */
    uint8_t channel;               /* 0=all, 1-255 specific */
} __packed; /* 26 bytes */

#define ZMK_STATUS_FLAG_CAPS_WORD     (1 << 0)
#define ZMK_STATUS_FLAG_CHARGING      (1 << 1)
#define ZMK_STATUS_FLAG_USB_CONNECTED (1 << 2)
#define ZMK_STATUS_FLAG_USB_HID_READY (1 << 3)
#define ZMK_STATUS_FLAG_BLE_CONNECTED (1 << 4)
#define ZMK_STATUS_FLAG_BLE_BONDED    (1 << 5)

#define ZMK_MOD_FLAG_LCTL (1 << 0)
#define ZMK_MOD_FLAG_LSFT (1 << 1)
#define ZMK_MOD_FLAG_LALT (1 << 2)
#define ZMK_MOD_FLAG_LGUI (1 << 3)
#define ZMK_MOD_FLAG_RCTL (1 << 4)
#define ZMK_MOD_FLAG_RSFT (1 << 5)
#define ZMK_MOD_FLAG_RALT (1 << 6)
#define ZMK_MOD_FLAG_RGUI (1 << 7)

#define ZMK_DEVICE_ROLE_STANDALONE 0
#define ZMK_DEVICE_ROLE_CENTRAL    1
#define ZMK_DEVICE_ROLE_PERIPHERAL 2

#define ZMK_STATUS_ADV_SERVICE_UUID 0xABCD

#define PROSPECTOR_DECODE_VERSION_MAJOR(v) ((v) >> 4)
#define PROSPECTOR_DECODE_VERSION_MINOR(v) ((v) & 0x0F)
#define PROSPECTOR_DECODE_PATCH(s)         (((s) >> 3) & 0x07)
#define PROSPECTOR_DECODE_DEV(s)           (((s) >> 6) & 0x01)
#define PROSPECTOR_DECODE_PROFILE(s)       ((s) & 0x07)

#ifdef __cplusplus
}
#endif
