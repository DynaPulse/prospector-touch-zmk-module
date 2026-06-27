/*
 * Prospector Touch - settings persistence (Zephyr settings subsystem / NVS).
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "display_settings.h"

LOG_MODULE_REGISTER(prospector_settings, CONFIG_ZMK_LOG_LEVEL);

#define CFG_KEY "prospector/cfg"

struct prospector_cfg {
    uint8_t auto_brightness;
    uint8_t manual_brightness;
    uint8_t layout;
    uint8_t channel;
    uint8_t max_layers;
};

static struct prospector_cfg cfg = {
    .auto_brightness = IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR),
    .manual_brightness = 60,
    .layout = CONFIG_PROSPECTOR_DEFAULT_LAYOUT,
    .channel = 0,
    .max_layers = 7,
};

static bool dirty;

static int cfg_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "cfg", &next) && !next) {
        if (len != sizeof(cfg)) {
            return -EINVAL;
        }
        ssize_t rc = read_cb(cb_arg, &cfg, sizeof(cfg));
        return rc >= 0 ? 0 : (int)rc;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(prospector, "prospector", NULL, cfg_set, NULL, NULL);

void prospector_settings_flush(void) {
    if (dirty) {
        int rc = settings_save_one(CFG_KEY, &cfg, sizeof(cfg));
        if (rc) {
            LOG_WRN("settings_save_one: %d", rc);
        } else {
            dirty = false;
        }
    }
}

void display_settings_init(void) { /* loaded by SYS_INIT below */ }

bool display_settings_get_auto_brightness(void) { return cfg.auto_brightness; }
void display_settings_set_auto_brightness(bool en) { cfg.auto_brightness = en; dirty = true; }

uint8_t display_settings_get_manual_brightness(void) { return cfg.manual_brightness; }
void display_settings_set_manual_brightness(uint8_t l) { cfg.manual_brightness = l; dirty = true; }

uint8_t display_settings_get_layout(void) { return cfg.layout; }
void display_settings_set_layout(uint8_t l) { cfg.layout = l; dirty = true; }

uint8_t display_settings_get_channel(void) { return cfg.channel; }
void display_settings_set_channel(uint8_t c) { cfg.channel = c; dirty = true; }

uint8_t display_settings_get_max_layers(void) { return cfg.max_layers; }
void display_settings_set_max_layers(uint8_t m) { cfg.max_layers = m; dirty = true; }

static void flush_work(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(flush_dwork, flush_work);
static void flush_work(struct k_work *w) {
    ARG_UNUSED(w);
    prospector_settings_flush();
    k_work_reschedule(&flush_dwork, K_SECONDS(5));
}

static int prospector_settings_load(void) {
    int rc = settings_subsys_init();
    if (rc) {
        LOG_WRN("settings_subsys_init: %d", rc);
        return 0;
    }
    settings_load_subtree("prospector");
    k_work_reschedule(&flush_dwork, K_SECONDS(5));
    return 0;
}

SYS_INIT(prospector_settings_load, APPLICATION, 90);
