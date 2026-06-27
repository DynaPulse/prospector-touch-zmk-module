/*
 * Prospector Touch - merged LVGL UI (YADS-style widgets + scanner data).
 *
 * Thread-safety contract:
 *   - ZMK event listeners (event thread) and the touch input callback (input
 *     thread) only publish into a mutex-guarded model / atomic gesture slot.
 *   - All LVGL object mutation happens on the display thread, drained by an
 *     lv_timer. No LVGL call is ever made from a BLE RX / input context.
 *
 * Clean-room implementation: behaviour reproduced from the analysed modules.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>
#endif
#include <zmk/battery.h>
#include <zmk/endpoints.h>
#include <zmk/prospector_state.h>
#include <zmk/status_scanner.h>
#include <zmk/status_advertisement.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/events/peripheral_battery_state_changed.h>
#endif
#include "../settings/display_settings.h"
#include <zephyr/sys/reboot.h>
#include <zmk/settings.h>
#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

#define PROSPECTOR_TOUCH_VERSION "0.1.0"

LOG_MODULE_REGISTER(prospector_ui, CONFIG_ZMK_LOG_LEVEL);

/* Brightness + activity hooks (strong impls in power/brightness_control.c). */
extern void prospector_brightness_resume(void);
extern void prospector_brightness_dim(uint8_t pct);
extern uint8_t prospector_brightness_get(void);
extern bool prospector_brightness_is_auto(void);
extern void prospector_brightness_set_auto(bool enabled);
extern void prospector_activity_ping(void);
__weak void prospector_brightness_step(int delta) { ARG_UNUSED(delta); }

#define ABS_I16(x) ((x) < 0 ? -(x) : (x))
#define SWIPE_MIN_PX 30
#define TAP_MAX_PX   12
#define TAP_MAX_MS   300

enum gesture { GEST_UP = 0, GEST_DOWN, GEST_LEFT, GEST_RIGHT, GEST_TAP };

#define UI_PAGE_COUNT 5
enum ui_page {
    UI_PAGE_MAIN = 0,
    UI_PAGE_KEYBOARDS,
    UI_PAGE_ACTIONS,
    UI_PAGE_SETTINGS,
    UI_PAGE_INFO,
};

#define ACTION_COUNT 3
#define LAYOUT_COUNT 3

struct ui_model {
    bool scanner;          /* true = scanner mode, false = dongle */
    uint8_t layer;
    char layer_name[16];
    uint8_t wpm;
    uint8_t battery;       /* central / self */
    uint8_t periph[3];     /* peripheral batteries, 0 = N/A */
    uint8_t mods;          /* HID modifier bitmask */
    uint8_t profile;       /* BLE profile index */
    bool usb;              /* USB transport / connected */
    bool ble_conn;         /* BLE profile connected */
    bool charging;
    bool caps_word;
    int8_t rssi;
};

static struct ui_model model;
static struct k_mutex model_lock;
static atomic_t model_dirty = ATOMIC_INIT(0);
static atomic_t pending_gesture = ATOMIC_INIT(-1);

/* Peripheral battery cache (dongle mode), updated by the split battery event. */
static uint8_t periph_cache[3];

static lv_obj_t *pages[UI_PAGE_COUNT];
/* main page widgets */
static lv_obj_t *lbl_layer, *lbl_mods, *lbl_profile, *lbl_endpoint, *lbl_rssi, *lbl_caps;
static lv_obj_t *lbl_wpm, *lbl_batt, *bar_batt, *bar_pl, *bar_pr;
/* settings page widgets */
static lv_obj_t *bar_bright, *lbl_bright, *lbl_auto;
/* keyboards page widgets */
static lv_obj_t *lbl_kb[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
static lv_obj_t *lbl_channel;
/* actions page widgets */
static lv_obj_t *lbl_action[ACTION_COUNT];
static lv_obj_t *lbl_version;
static int action_cursor;
static uint8_t layout_style;
/* info page widget */
static lv_obj_t *lbl_info;
static enum ui_page active_page = UI_PAGE_MAIN;

/* ---------- producer side (event / input threads) ---------- */

static void publish_from_zmk(void) {
    struct ui_model m = {0};
    m.scanner = false;
    uint8_t idx = zmk_keymap_highest_layer_active();
    m.layer = idx;
    const char *name = zmk_keymap_layer_name(idx);
    if (name && name[0]) {
        strncpy(m.layer_name, name, sizeof(m.layer_name) - 1);
    } else {
        snprintf(m.layer_name, sizeof(m.layer_name), "%u", idx);
    }
#if IS_ENABLED(CONFIG_ZMK_WPM)
    m.wpm = zmk_wpm_get_state();
#endif
    m.battery = zmk_battery_state_of_charge();
    m.mods = zmk_hid_get_keyboard_report()->body.modifiers;
    m.periph[0] = periph_cache[0];
    m.periph[1] = periph_cache[1];
    m.periph[2] = periph_cache[2];

    struct zmk_endpoint_instance ep = zmk_endpoints_selected();
    m.usb = (ep.transport == ZMK_TRANSPORT_USB);
#if IS_ENABLED(CONFIG_ZMK_BLE)
    m.profile = zmk_ble_active_profile_index();
    m.ble_conn = zmk_ble_active_profile_is_connected();
#endif

    k_mutex_lock(&model_lock, K_FOREVER);
    model = m;
    k_mutex_unlock(&model_lock);
    atomic_set(&model_dirty, 1);
}

/* Scanner-mode data path: called by the BLE observer for the primary keyboard. */
void prospector_ui_on_scanner_update(const struct zmk_keyboard_status *ks) {
    if (!ks) {
        return;
    }
    const struct zmk_status_adv_data *d = &ks->data;
    struct ui_model m = {0};
    m.scanner = true;
    m.layer = d->active_layer;
    size_t len = 0;
    while (len < sizeof(d->layer_name) && d->layer_name[len]) {
        len++;
    }
    size_t cn = MIN(len, sizeof(m.layer_name) - 1);
    memcpy(m.layer_name, d->layer_name, cn);
    m.layer_name[cn] = '\0';
    if (cn == 0) {
        snprintf(m.layer_name, sizeof(m.layer_name), "%u", m.layer);
    }
    m.wpm = d->wpm_value;
    m.battery = d->battery_level;
    m.periph[0] = d->peripheral_battery[0];
    m.periph[1] = d->peripheral_battery[1];
    m.periph[2] = d->peripheral_battery[2];
    m.mods = d->modifier_flags;
    m.profile = PROSPECTOR_DECODE_PROFILE(d->profile_slot);
    m.usb = d->status_flags & ZMK_STATUS_FLAG_USB_CONNECTED;
    m.ble_conn = d->status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED;
    m.charging = d->status_flags & ZMK_STATUS_FLAG_CHARGING;
    m.caps_word = d->status_flags & ZMK_STATUS_FLAG_CAPS_WORD;
    m.rssi = ks->rssi;

    k_mutex_lock(&model_lock, K_FOREVER);
    model = m;
    k_mutex_unlock(&model_lock);
    atomic_set(&model_dirty, 1);
    prospector_activity_ping();
}

static int ui_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *pb =
        as_zmk_peripheral_battery_state_changed(eh);
    if (pb && pb->source < 3) {
        periph_cache[pb->source] = pb->state_of_charge;
    }
#else
    ARG_UNUSED(eh);
#endif
    prospector_activity_ping();
    /* Local ZMK state is authoritative only in dongle mode; scanner mode is
     * driven by prospector_ui_on_scanner_update() from received adverts. */
    if (prospector_mode_get() == PROSPECTOR_MODE_DONGLE) {
        publish_from_zmk();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prospector_ui, ui_event_listener);
ZMK_SUBSCRIPTION(prospector_ui, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(prospector_ui, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_WPM)
ZMK_SUBSCRIPTION(prospector_ui, zmk_wpm_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(prospector_ui, zmk_peripheral_battery_state_changed);
#endif

static void touch_input_cb(struct input_event *evt, void *user) {
    ARG_UNUSED(user);
    static int16_t cx, cy, sx, sy;
    static int64_t t_down;
    static bool down;

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            cx = evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            cy = evt->value;
        }
        break;
    case INPUT_EV_KEY:
        if (evt->code != INPUT_BTN_TOUCH) {
            break;
        }
        prospector_activity_ping();
        if (evt->value) {
            down = true;
            sx = cx;
            sy = cy;
            t_down = k_uptime_get();
        } else if (down) {
            down = false;
            int16_t dx = cx - sx, dy = cy - sy;
            int64_t dt = k_uptime_get() - t_down;
            int dir = -1;
            if (ABS_I16(dx) > ABS_I16(dy) && ABS_I16(dx) > SWIPE_MIN_PX) {
                dir = dx > 0 ? GEST_RIGHT : GEST_LEFT;
            } else if (ABS_I16(dy) > SWIPE_MIN_PX) {
                dir = dy > 0 ? GEST_DOWN : GEST_UP;
            } else if (ABS_I16(dx) <= TAP_MAX_PX && ABS_I16(dy) <= TAP_MAX_PX &&
                       dt <= TAP_MAX_MS) {
                dir = GEST_TAP;
            }
            if (dir >= 0) {
                atomic_set(&pending_gesture, dir);
            }
        }
        break;
    default:
        break;
    }
}
INPUT_CALLBACK_DEFINE(NULL, touch_input_cb, NULL);

/* ---------- consumer side (display thread, via lv_timer) ---------- */

static void show_page(enum ui_page p) {
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        if (pages[i]) {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    active_page = p;
    if (pages[p]) {
        lv_obj_clear_flag(pages[p], LV_OBJ_FLAG_HIDDEN);
    }
}

static void do_reset(void) {
    prospector_settings_flush();
#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
    bootmode_set(BOOT_MODE_TYPE_NORMAL);
    sys_reboot(SYS_REBOOT_WARM);
#else
    sys_reboot(SYS_REBOOT_COLD);
#endif
}

static void do_bootloader(void) {
#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
    bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    sys_reboot(SYS_REBOOT_WARM);
#else
    sys_reboot(0x57); /* RST_UF2 - Adafruit nRF52 bootloader magic */
#endif
}

static void do_reset_settings(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    zmk_settings_erase();
#endif
    do_reset();
}

static void select_kb(int dir) {
    int actives[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
    int cnt = 0;
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *k = zmk_status_scanner_get_keyboard(i);
        if (k && k->active) {
            actives[cnt++] = i;
        }
    }
    if (cnt == 0) {
        return;
    }
    int cur = zmk_status_scanner_get_selected();
    int pos = 0;
    for (int i = 0; i < cnt; i++) {
        if (actives[i] == cur) {
            pos = i;
            break;
        }
    }
    pos = (cur < 0) ? 0 : (pos + (dir > 0 ? 1 : cnt - 1)) % cnt;
    zmk_status_scanner_set_selected(actives[pos]);
}

static void cycle_channel(void) {
    uint8_t ch = (zmk_status_scanner_get_channel() + 1) % 10; /* 0..9 */
    zmk_status_scanner_set_channel(ch);
    display_settings_set_channel(ch);
    zmk_status_scanner_refresh_display();
}

static void exec_action(void) {
    switch (action_cursor) {
    case 0:
        do_reset();
        break;
    case 1:
        do_bootloader();
        break;
    case 2:
        do_reset_settings();
        break;
    }
}

static void apply_gesture(enum gesture g) {
    switch (g) {
    case GEST_RIGHT:
        show_page((active_page + 1) % UI_PAGE_COUNT);
        break;
    case GEST_LEFT:
        show_page((active_page + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT);
        break;
    case GEST_UP:
        if (active_page == UI_PAGE_KEYBOARDS) {
            select_kb(-1);
        } else if (active_page == UI_PAGE_ACTIONS) {
            action_cursor = (action_cursor + ACTION_COUNT - 1) % ACTION_COUNT;
        } else {
            prospector_brightness_step(+10);
        }
        break;
    case GEST_DOWN:
        if (active_page == UI_PAGE_KEYBOARDS) {
            select_kb(+1);
        } else if (active_page == UI_PAGE_ACTIONS) {
            action_cursor = (action_cursor + 1) % ACTION_COUNT;
        } else {
            prospector_brightness_step(-10);
        }
        break;
    case GEST_TAP:
        if (active_page == UI_PAGE_SETTINGS) {
            prospector_brightness_set_auto(!prospector_brightness_is_auto());
        } else if (active_page == UI_PAGE_KEYBOARDS) {
            cycle_channel();
        } else if (active_page == UI_PAGE_ACTIONS) {
            exec_action();
        } else if (active_page == UI_PAGE_MAIN) {
            layout_style = (layout_style + 1) % LAYOUT_COUNT;
            display_settings_set_layout(layout_style);
        }
        break;
    }
    atomic_set(&model_dirty, 1);
}

static lv_color_t batt_color(uint8_t lvl) {
    if (lvl >= 60) {
        return lv_color_hex(0x39d353);
    }
    if (lvl >= 30) {
        return lv_color_hex(0xe3b341);
    }
    return lv_color_hex(0xf85149);
}

static const char *batt_sym(uint8_t lvl) {
    if (lvl >= 80) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (lvl >= 60) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (lvl >= 40) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (lvl >= 20) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void set_batt_bar(lv_obj_t *bar, uint8_t lvl) {
    if (!bar) {
        return;
    }
    lv_bar_set_value(bar, lvl, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, batt_color(lvl), LV_PART_INDICATOR);
}

static void set_vis(lv_obj_t *o, bool show) {
    if (!o) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_model(void) {
    struct ui_model m;
    k_mutex_lock(&model_lock, K_FOREVER);
    m = model;
    k_mutex_unlock(&model_lock);

    /* ---- main ---- */
    if (lbl_layer) {
        lv_label_set_text(lbl_layer, m.layer_name);
    }
    if (lbl_mods) {
        lv_label_set_text_fmt(lbl_mods, "%c %c %c %c",
                              (m.mods & 0x11) ? 'C' : '-',
                              (m.mods & 0x22) ? 'S' : '-',
                              (m.mods & 0x44) ? 'A' : '-',
                              (m.mods & 0x88) ? 'G' : '-');
    }
    if (lbl_profile) {
        lv_label_set_text_fmt(lbl_profile, "%u", (unsigned)(m.profile + 1));
    }
    if (lbl_endpoint) {
        lv_label_set_text(lbl_endpoint, m.usb ? LV_SYMBOL_USB : LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(lbl_endpoint,
            (m.usb || m.ble_conn) ? lv_color_hex(0x39d353) : lv_color_hex(0x8b949e),
            LV_PART_MAIN);
    }
    if (lbl_rssi) {
        lv_label_set_text_fmt(lbl_rssi, "%ddBm", m.rssi);
    }
    if (lbl_caps) {
        lv_label_set_text(lbl_caps, m.caps_word ? "CAPS" : "");
    }
    if (lbl_wpm) {
        lv_label_set_text_fmt(lbl_wpm, "%u wpm", m.wpm);
    }
    if (lbl_batt) {
        lv_label_set_text_fmt(lbl_batt, "%s%s %u%%", batt_sym(m.battery),
                              m.charging ? " " LV_SYMBOL_CHARGE : "", m.battery);
    }
    set_batt_bar(bar_batt, m.battery);
    if (bar_pl && m.periph[0]) {
        set_batt_bar(bar_pl, m.periph[0]);
    }
    if (bar_pr && m.periph[1]) {
        set_batt_bar(bar_pr, m.periph[1]);
    }
    {
        /* layout-style visibility: 0=Dashboard, 1=Focus, 2=Minimal */
        bool extra = (layout_style == 0);
        bool show_mods = (layout_style != 1);
        bool show_cbatt = (layout_style != 2);
        set_vis(lbl_profile, extra);
        set_vis(lbl_endpoint, extra);
        set_vis(lbl_rssi, extra && m.scanner);
        set_vis(lbl_wpm, extra);
        set_vis(lbl_batt, extra);
        set_vis(bar_batt, show_cbatt);
        set_vis(bar_pl, extra && m.periph[0]);
        set_vis(bar_pr, extra && m.periph[1]);
        set_vis(lbl_mods, show_mods);
        set_vis(lbl_caps, (layout_style != 2) && m.caps_word);
    }

    /* ---- settings ---- */
    if (bar_bright) {
        lv_bar_set_value(bar_bright, prospector_brightness_get(), LV_ANIM_ON);
    }
    if (lbl_bright) {
        lv_label_set_text_fmt(lbl_bright, "Brightness %u%%", prospector_brightness_get());
    }
    if (lbl_auto) {
        lv_label_set_text_fmt(lbl_auto, "Auto: %s",
                              prospector_brightness_is_auto() ? "ON" : "OFF");
    }

    /* ---- info ---- */
    if (lbl_info) {
        lv_label_set_text_fmt(lbl_info, "%s\nWPM %u\nBatt %u%%\nRSSI %ddBm",
                              m.scanner ? "SCANNER" : "DONGLE",
                              m.wpm, m.battery, m.rssi);
    }

    /* ---- keyboards ---- */
    if (lbl_channel) {
        uint8_t ch = zmk_status_scanner_get_channel();
        if (ch == 0) {
            lv_label_set_text(lbl_channel, "Ch: ALL   (tap)");
        } else {
            lv_label_set_text_fmt(lbl_channel, "Ch: %u   (tap)", ch);
        }
    }
    int disp = zmk_status_scanner_get_selected();
    if (disp < 0) {
        disp = zmk_status_scanner_get_primary_keyboard();
    }
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (!lbl_kb[i]) {
            continue;
        }
        struct zmk_keyboard_status *k = zmk_status_scanner_get_keyboard(i);
        if (k && k->active) {
            const char *nm = (k->ble_name[0]) ? k->ble_name : "keyboard";
            lv_label_set_text_fmt(lbl_kb[i], "%c %.10s L%u %u%%",
                                  (i == disp) ? '>' : ' ',
                                  nm, k->data.active_layer, k->data.battery_level);
            lv_obj_clear_flag(lbl_kb[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_kb[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ---- actions ---- */
    static const char *const action_names[ACTION_COUNT] = {
        "Reboot", "Bootloader", "Reset Settings"};
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (lbl_action[i]) {
            lv_label_set_text_fmt(lbl_action[i], "%c %s",
                                  (i == action_cursor) ? '>' : ' ', action_names[i]);
        }
    }
}

static void ui_tick(lv_timer_t *t) {
    ARG_UNUSED(t);
    int g = atomic_set(&pending_gesture, -1);
    if (g >= 0) {
        apply_gesture((enum gesture)g);
    }
    if (atomic_set(&model_dirty, 0)) {
        render_model();
    }
}

static lv_obj_t *make_page(lv_obj_t *root) {
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ZMK custom status screen entry point. Runs on the display thread. */
lv_obj_t *zmk_display_status_screen(void) {
    k_mutex_init(&model_lock);

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- Page MAIN: dashboard ---- */
    pages[UI_PAGE_MAIN] = make_page(root);

    lbl_profile = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_profile, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_label_set_text(lbl_profile, "1");

    lbl_endpoint = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_endpoint, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_label_set_text(lbl_endpoint, "BLE");

    lbl_rssi = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_rssi, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(lbl_rssi, "");
    lv_obj_add_flag(lbl_rssi, LV_OBJ_FLAG_HIDDEN);

    lbl_caps = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_caps, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(lbl_caps, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_label_set_text(lbl_caps, "");
    lv_obj_add_flag(lbl_caps, LV_OBJ_FLAG_HIDDEN);

    lbl_layer = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_set_style_text_font(lbl_layer, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(lbl_layer, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(lbl_layer, "Base");

    lbl_mods = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_mods, LV_ALIGN_CENTER, 0, 18);
    lv_label_set_text(lbl_mods, "- - - -");

    lbl_wpm = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_wpm, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_label_set_text(lbl_wpm, "0 wpm");

    lbl_batt = lv_label_create(pages[UI_PAGE_MAIN]);
    lv_obj_align(lbl_batt, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_label_set_text(lbl_batt, "0%");

    bar_pl = lv_bar_create(pages[UI_PAGE_MAIN]);
    lv_obj_set_size(bar_pl, LV_PCT(38), 6);
    lv_obj_align(bar_pl, LV_ALIGN_BOTTOM_LEFT, 6, -28);
    lv_bar_set_range(bar_pl, 0, 100);
    lv_obj_add_flag(bar_pl, LV_OBJ_FLAG_HIDDEN);

    bar_pr = lv_bar_create(pages[UI_PAGE_MAIN]);
    lv_obj_set_size(bar_pr, LV_PCT(38), 6);
    lv_obj_align(bar_pr, LV_ALIGN_BOTTOM_RIGHT, -6, -28);
    lv_bar_set_range(bar_pr, 0, 100);
    lv_obj_add_flag(bar_pr, LV_OBJ_FLAG_HIDDEN);

    bar_batt = lv_bar_create(pages[UI_PAGE_MAIN]);
    lv_obj_set_size(bar_batt, LV_PCT(90), 8);
    lv_obj_align(bar_batt, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_bar_set_range(bar_batt, 0, 100);

    /* ---- Page SETTINGS ---- */
    pages[UI_PAGE_SETTINGS] = make_page(root);
    lv_obj_t *s_title = lv_label_create(pages[UI_PAGE_SETTINGS]);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(s_title, "Settings");

    lbl_bright = lv_label_create(pages[UI_PAGE_SETTINGS]);
    lv_obj_align(lbl_bright, LV_ALIGN_CENTER, 0, -16);
    lv_label_set_text(lbl_bright, "Brightness");

    bar_bright = lv_bar_create(pages[UI_PAGE_SETTINGS]);
    lv_obj_set_size(bar_bright, LV_PCT(80), 10);
    lv_obj_align(bar_bright, LV_ALIGN_CENTER, 0, 4);
    lv_bar_set_range(bar_bright, 1, 100);

    lbl_auto = lv_label_create(pages[UI_PAGE_SETTINGS]);
    lv_obj_align(lbl_auto, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(lbl_auto, "Auto: OFF");

    lv_obj_t *s_hint = lv_label_create(pages[UI_PAGE_SETTINGS]);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_hint, "swipe U/D   tap=auto");

    /* ---- Page INFO ---- */
    pages[UI_PAGE_INFO] = make_page(root);
    lbl_info = lv_label_create(pages[UI_PAGE_INFO]);
    lv_obj_align(lbl_info, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_info, "INFO");

    /* ---- Page KEYBOARDS ---- */
    pages[UI_PAGE_KEYBOARDS] = make_page(root);
    lv_obj_t *k_title = lv_label_create(pages[UI_PAGE_KEYBOARDS]);
    lv_obj_align(k_title, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(k_title, "Keyboards");
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        lbl_kb[i] = lv_label_create(pages[UI_PAGE_KEYBOARDS]);
        lv_obj_align(lbl_kb[i], LV_ALIGN_TOP_LEFT, 6, 30 + i * 18);
        lv_label_set_text(lbl_kb[i], "");
        lv_obj_add_flag(lbl_kb[i], LV_OBJ_FLAG_HIDDEN);
    }
    lbl_channel = lv_label_create(pages[UI_PAGE_KEYBOARDS]);
    lv_obj_align(lbl_channel, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(lbl_channel, "Ch: ALL   (tap)");

    /* ---- Page ACTIONS ---- */
    pages[UI_PAGE_ACTIONS] = make_page(root);
    lv_obj_t *a_title = lv_label_create(pages[UI_PAGE_ACTIONS]);
    lv_obj_align(a_title, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(a_title, "Actions");
    for (int i = 0; i < ACTION_COUNT; i++) {
        lbl_action[i] = lv_label_create(pages[UI_PAGE_ACTIONS]);
        lv_obj_align(lbl_action[i], LV_ALIGN_TOP_LEFT, 10, 32 + i * 22);
        lv_label_set_text(lbl_action[i], "");
    }
    lbl_version = lv_label_create(pages[UI_PAGE_ACTIONS]);
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(lbl_version, "Touch v" PROSPECTOR_TOUCH_VERSION);

    zmk_status_scanner_set_channel(display_settings_get_channel());
    layout_style = display_settings_get_layout();
    if (layout_style >= LAYOUT_COUNT) {
        layout_style = 0;
    }

    show_page(UI_PAGE_MAIN);
    if (prospector_mode_get() == PROSPECTOR_MODE_DONGLE) {
        publish_from_zmk();
    }
    render_model();

    lv_timer_create(ui_tick, 50, NULL);
    return root;
}

#endif /* CONFIG_ZMK_DISPLAY */
