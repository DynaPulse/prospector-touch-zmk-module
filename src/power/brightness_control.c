/*
 * Prospector Touch - backlight brightness control.
 *
 * Provides the real implementations of the prospector_brightness_* hooks used
 * by the hybrid state manager and the UI (overriding their weak defaults).
 * PWM backlight via the pwm-leds "disp_bl" channel; optional auto-brightness
 * from the APDS9960 ambient light sensor. Manual level + auto flag persist via
 * display_settings (NVS). Clean-room.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif

#include "../settings/display_settings.h"

LOG_MODULE_REGISTER(prospector_bl, CONFIG_ZMK_LOG_LEVEL);

#define DISP_BL_IDX DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static const struct device *const pwmleds = DEVICE_DT_GET_ONE(pwm_leds);

static uint8_t cur = 60;
static bool auto_mode;

static void set_pct(uint8_t pct) {
    pct = CLAMP(pct, 1, 100);
    cur = pct;
    if (device_is_ready(pwmleds)) {
        (void)led_set_brightness(pwmleds, DISP_BL_IDX, pct);
    }
}

void prospector_brightness_resume(void) {
    auto_mode = display_settings_get_auto_brightness();
    set_pct(display_settings_get_manual_brightness());
}

void prospector_brightness_dim(uint8_t pct) {
    auto_mode = false; /* explicit dim wins until next resume */
    set_pct(pct);
}

void prospector_brightness_step(int delta) {
    auto_mode = false;
    display_settings_set_auto_brightness(false);
    set_pct((uint8_t)CLAMP((int)cur + delta, 1, 100));
    display_settings_set_manual_brightness(cur);
}

uint8_t prospector_brightness_get(void) { return cur; }

bool prospector_brightness_is_auto(void) { return auto_mode; }

void prospector_brightness_set_auto(bool en) {
    auto_mode = en;
    display_settings_set_auto_brightness(en);
    if (!en) {
        set_pct(display_settings_get_manual_brightness());
    }
}

/* ---- idle backlight management ---- */
#define IDLE_MS ((int64_t)CONFIG_PROSPECTOR_DISPLAY_IDLE_S * 1000)
static int64_t last_active;
static bool asleep;

void prospector_activity_ping(void) {
    last_active = k_uptime_get();
    if (asleep) {
        asleep = false;
        set_pct(cur);
    }
}

static void idle_work(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(idle_dwork, idle_work);
static void idle_work(struct k_work *w) {
    ARG_UNUSED(w);
    if (IDLE_MS > 0 && !asleep && (k_uptime_get() - last_active) > IDLE_MS) {
        asleep = true;
        if (device_is_ready(pwmleds)) {
            (void)led_set_brightness(pwmleds, DISP_BL_IDX, 0);
        }
    }
    k_work_reschedule(&idle_dwork, K_SECONDS(2));
}

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
static const struct device *const als = DEVICE_DT_GET_ONE(avago_apds9960);

static uint8_t map_light(int32_t v) {
    v = CLAMP(v, 0, 100);
    return (uint8_t)(1 + (v * 99) / 100);
}

static void als_work(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(als_dwork, als_work);
static void als_work(struct k_work *w) {
    ARG_UNUSED(w);
    if (auto_mode && !asleep && device_is_ready(als)) {
        struct sensor_value v;
        if (sensor_sample_fetch(als) == 0 &&
            sensor_channel_get(als, SENSOR_CHAN_LIGHT, &v) == 0) {
            uint8_t target = map_light(v.val1);
            /* single-step glide to avoid abrupt jumps */
            if (target > cur) {
                set_pct(cur + 1);
            } else if (target < cur) {
                set_pct(cur - 1);
            }
        }
    }
    k_work_reschedule(&als_dwork, K_MSEC(200));
}
#endif

static int brightness_init(void) {
    auto_mode = display_settings_get_auto_brightness();
    set_pct(display_settings_get_manual_brightness());
    last_active = k_uptime_get();
    k_work_reschedule(&idle_dwork, K_SECONDS(2));
#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
    k_work_reschedule(&als_dwork, K_MSEC(500));
#endif
    return 0;
}

/* After display_settings (priority 90) so persisted values are available. */
SYS_INIT(brightness_init, APPLICATION, 91);
