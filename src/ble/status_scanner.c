/*
 * Prospector status scanner - passive BLE observer.
 *
 * Runs only in scanner (battery) mode. Parses ZMK status advertisements, keeps
 * a small table of recently seen keyboards keyed by BLE address, and hands the
 * primary keyboard's state to the UI. The bt scan callback runs in the BT RX
 * thread, so it only enqueues raw adverts; all table mutation happens in a
 * system-workqueue handler to avoid stalling the RX path. Clean-room.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zmk/status_scanner.h>

LOG_MODULE_REGISTER(prospector_scanner, CONFIG_ZMK_LOG_LEVEL);

/* Weak fallback so this unit links even when the display is not built. */
__weak void prospector_ui_on_scanner_update(const struct zmk_keyboard_status *ks) {
    ARG_UNUSED(ks);
}

#define SCAN_TIMEOUT_MS CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS

struct incoming {
    struct zmk_status_adv_data data;
    int8_t rssi;
    bt_addr_le_t addr;
    char name[32];
};

K_MSGQ_DEFINE(scan_msgq, sizeof(struct incoming), 16, 4);

static struct zmk_keyboard_status keyboards[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
static zmk_status_scanner_callback_t user_cb;
static struct k_work drain_work;
static atomic_t scanning = ATOMIC_INIT(0);
static uint8_t runtime_channel;
static int selected_idx = -1;

struct parse_ctx {
    struct incoming *in;
    bool got_data;
};

static bool ad_parse(struct bt_data *ad, void *user) {
    struct parse_ctx *ctx = user;
    switch (ad->type) {
    case BT_DATA_NAME_COMPLETE:
    case BT_DATA_NAME_SHORTENED: {
        size_t n = MIN(ad->data_len, sizeof(ctx->in->name) - 1);
        memcpy(ctx->in->name, ad->data, n);
        ctx->in->name[n] = '\0';
        break;
    }
    case BT_DATA_MANUFACTURER_DATA:
        if (ad->data_len >= sizeof(struct zmk_status_adv_data)) {
            memcpy(&ctx->in->data, ad->data, sizeof(struct zmk_status_adv_data));
            ctx->got_data = true;
        }
        break;
    default:
        break;
    }
    return true;
}

static bool is_prospector(const struct zmk_status_adv_data *d) {
    return d->manufacturer_id[0] == 0xFF && d->manufacturer_id[1] == 0xFF &&
           d->service_uuid[0] == 0xAB && d->service_uuid[1] == 0xCD;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *buf) {
    ARG_UNUSED(adv_type);
    struct incoming in = {0};
    struct parse_ctx ctx = {.in = &in, .got_data = false};

    bt_data_parse(buf, ad_parse, &ctx);
    if (!ctx.got_data || !is_prospector(&in.data)) {
        return;
    }
    /* Channel filter: 0 on either side = accept all. */
    if (runtime_channel != 0 && in.data.channel != 0 &&
        runtime_channel != in.data.channel) {
        return;
    }
    in.rssi = rssi;
    bt_addr_le_copy(&in.addr, addr);

    if (k_msgq_put(&scan_msgq, &in, K_NO_WAIT) == 0) {
        k_work_submit(&drain_work);
    }
}

static int find_or_alloc(const bt_addr_le_t *addr) {
    int free_idx = -1;
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && memcmp(keyboards[i].ble_addr, addr->a.val, 6) == 0) {
            return i;
        }
        if (!keyboards[i].active && free_idx < 0) {
            free_idx = i;
        }
    }
    return free_idx;
}

static void notify_display(void) {
    int idx = selected_idx;
    if (idx < 0 || idx >= ZMK_STATUS_SCANNER_MAX_KEYBOARDS || !keyboards[idx].active) {
        idx = zmk_status_scanner_get_primary_keyboard();
    }
    if (idx >= 0) {
        prospector_ui_on_scanner_update(&keyboards[idx]);
    }
}

static void drain(struct k_work *w) {
    ARG_UNUSED(w);
    struct incoming in;

    while (k_msgq_get(&scan_msgq, &in, K_NO_WAIT) == 0) {
        int idx = find_or_alloc(&in.addr);
        if (idx < 0) {
            continue;
        }
        struct zmk_keyboard_status *ks = &keyboards[idx];
        bool is_new = !ks->active;

        ks->active = true;
        ks->last_seen = k_uptime_get_32();
        ks->data = in.data;
        ks->rssi = in.rssi;
        memcpy(ks->ble_addr, in.addr.a.val, 6);
        ks->ble_addr_type = in.addr.type;
        if (in.name[0]) {
            strncpy(ks->ble_name, in.name, sizeof(ks->ble_name) - 1);
        }

        if (user_cb) {
            struct zmk_status_scanner_event_data ev = {
                .event = is_new ? ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND
                                : ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED,
                .keyboard_index = idx,
                .status = ks,
            };
            user_cb(&ev);
        }
    }
    notify_display();
}

static void timeout_work(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(timeout_dwork, timeout_work);

static void timeout_work(struct k_work *w) {
    ARG_UNUSED(w);
#if SCAN_TIMEOUT_MS > 0
    uint32_t now = k_uptime_get_32();
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && (now - keyboards[i].last_seen) > SCAN_TIMEOUT_MS) {
            keyboards[i].active = false;
            if (user_cb) {
                struct zmk_status_scanner_event_data ev = {
                    .event = ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST,
                    .keyboard_index = i,
                    .status = &keyboards[i],
                };
                user_cb(&ev);
            }
        }
    }
#endif
    k_work_reschedule(&timeout_dwork, K_SECONDS(5));
}

int zmk_status_scanner_init(void) {
    k_work_init(&drain_work, drain);
    return 0;
}

static const struct bt_le_scan_param scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = BT_GAP_SCAN_FAST_INTERVAL,
    .window = BT_GAP_SCAN_FAST_WINDOW,
};

int zmk_status_scanner_start(void) {
    if (atomic_set(&scanning, 1)) {
        return 0;
    }
    int err = bt_le_scan_start(&scan_param, scan_cb);
    if (err) {
        atomic_set(&scanning, 0);
        LOG_ERR("bt_le_scan_start failed: %d", err);
        return err;
    }
    k_work_reschedule(&timeout_dwork, K_SECONDS(5));
    LOG_INF("observer scanning for status adverts");
    return 0;
}

int zmk_status_scanner_stop(void) {
    if (!atomic_set(&scanning, 0)) {
        return 0;
    }
    k_work_cancel_delayable(&timeout_dwork);
    return bt_le_scan_stop();
}

int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t cb) {
    user_cb = cb;
    return 0;
}

void zmk_status_scanner_set_channel(uint8_t channel) {
    runtime_channel = channel;
}

uint8_t zmk_status_scanner_get_channel(void) {
    return runtime_channel;
}

void zmk_status_scanner_set_selected(int index) {
    selected_idx = index;
    notify_display();
}

int zmk_status_scanner_get_selected(void) {
    return selected_idx;
}

void zmk_status_scanner_refresh_display(void) {
    notify_display();
}

struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index) {
    if (index < 0 || index >= ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
        return NULL;
    }
    return &keyboards[index];
}

int zmk_status_scanner_get_active_count(void) {
    int c = 0;
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            c++;
        }
    }
    return c;
}

int zmk_status_scanner_get_primary_keyboard(void) {
    int best = -1;
    uint32_t newest = 0;
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && (best < 0 || keyboards[i].last_seen > newest)) {
            best = i;
            newest = keyboards[i].last_seen;
        }
    }
    return best;
}
