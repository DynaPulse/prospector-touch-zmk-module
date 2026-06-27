/*
 * Prospector Touch - Hybrid USB-dongle / BLE-scanner mode manager (design A).
 *
 * The image always compiles split-central + USB HID (dongle) AND a BT observer
 * (scanner). The active role is chosen from USB power at boot:
 *
 *   Dongle mode  (USB power): standard ZMK split central + USB HID. The
 *                             observer is left stopped.
 *   Scanner mode (battery):   passive BLE observer parses status adverts; the
 *                             central / HID path is not used, the LCD is
 *                             dimmed, and status advertising is throttled.
 *
 * A split central and a passive observer cannot share the radio role cleanly
 * at runtime, so a USB-power transition triggers a cold reboot to bring the
 * BLE stack up in the correct role (see HYBRID_PROSPECTOR_SPEC.md, section 16).
 * Clean-room implementation.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/status_scanner.h>
#include <zmk/prospector_state.h>

LOG_MODULE_REGISTER(prospector_hybrid, CONFIG_ZMK_LOG_LEVEL);

/* Hooks implemented by the brightness / settings units; weak defaults keep
 * this translation unit linkable on its own. */
__weak void prospector_brightness_resume(void) {}
__weak void prospector_brightness_dim(uint8_t pct) { ARG_UNUSED(pct); }
__weak void prospector_settings_flush(void) {}

static atomic_t g_mode = ATOMIC_INIT(PROSPECTOR_MODE_SCANNER);
static bool boot_done;

enum prospector_mode prospector_mode_get(void) {
    return (enum prospector_mode)atomic_get(&g_mode);
}

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void enter_dongle(void) {
    atomic_set(&g_mode, PROSPECTOR_MODE_DONGLE);
#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)
    zmk_status_scanner_stop();
#endif
    prospector_brightness_resume();
    LOG_INF("Dongle mode: USB HID + split central");
}

static void enter_scanner(void) {
    atomic_set(&g_mode, PROSPECTOR_MODE_SCANNER);
#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)
    zmk_status_scanner_init();
    zmk_status_scanner_start();
#endif
    prospector_brightness_dim(CONFIG_PROSPECTOR_HYBRID_BATTERY_DIM);
    LOG_INF("Scanner mode: passive BLE observer, central disabled");
}

static int usb_listener(const zmk_event_t *eh) {
    if (!as_zmk_usb_conn_state_changed(eh) || !boot_done) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    enum prospector_mode want =
        usb_powered() ? PROSPECTOR_MODE_DONGLE : PROSPECTOR_MODE_SCANNER;
    if (want != prospector_mode_get()) {
        LOG_INF("USB power change -> cold reboot to switch role");
        prospector_settings_flush();
        sys_reboot(SYS_REBOOT_COLD);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prospector_hybrid, usb_listener);
ZMK_SUBSCRIPTION(prospector_hybrid, zmk_usb_conn_state_changed);

static int prospector_hybrid_init(void) {
    if (usb_powered()) {
        enter_dongle();
    } else {
        enter_scanner();
    }
    boot_done = true;
    return 0;
}

SYS_INIT(prospector_hybrid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
