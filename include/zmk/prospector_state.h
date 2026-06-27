/*
 * Prospector Touch runtime mode (shared between the state manager and the UI).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum prospector_mode {
    PROSPECTOR_MODE_DONGLE = 0,  /* USB power present: split central + USB HID */
    PROSPECTOR_MODE_SCANNER = 1, /* battery: passive BLE observer */
};

/* Current mode, resolved at boot from USB power and on USB-power transitions. */
enum prospector_mode prospector_mode_get(void);

#ifdef __cplusplus
}
#endif
