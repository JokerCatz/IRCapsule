#include "remote.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nvs.h"
#include "engine.h"

#define PAIR_WINDOW_S 120
/* Pairing is always accepted: because of the HID service, Linux hosts (and possibly others) ask for it on
 * every connection and drop the whole link, config page included, when it is refused. A bond alone gets
 * nothing: key presses go only to the TV, and a peer becomes the TV only by a NEW pairing made while the
 * pairing window is open. */
#define MAX_PEERS 8

static const uint8_t REPORT_MAP[] = {
    /* report 1: keyboard: 8 modifier bits, 1 reserved byte, 6 key codes */
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xFF,0x00, 0x05,0x07, 0x19,0x00, 0x2A,0xFF,0x00, 0x81,0x00,
    0xC0,
    /* report 2: consumer control: one 16-bit usage */
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x02,
    0x15,0x00, 0x26,0xFF,0x03, 0x19,0x00, 0x2A,0xFF,0x03, 0x75,0x10, 0x95,0x01, 0x81,0x00,
    0xC0,
};
#define KBD_LEN 8
#define CC_LEN  2

#define KB 1                 /* report 1: keyboard page */
#define CC 2                 /* report 2: consumer page */
typedef struct { const char *name; uint8_t report; uint16_t code; } key_def_t;
/* Which of these a TV acts on depends on its Android key layout, which Android picks by vendor/product id. */
static const key_def_t KEYS[] = {
    { "up",     KB, 0x52 }, { "down",   KB, 0x51 }, { "left",   KB, 0x50 }, { "right",  KB, 0x4F },
    { "up2",    CC, 0x42 }, { "down2",  CC, 0x43 }, { "left2",  CC, 0x44 }, { "right2", CC, 0x45 },
    { "ok",     KB, 0x28 },       /* keyboard Enter */
    { "ok2",    CC, 0x0041 },     /* Menu Pick */
    { "back",   CC, 0x0224 },     /* AC Back */
    { "back2",  KB, 0x29 },       /* keyboard Escape */
    { "home",   CC, 0x0223 },     /* AC Home */
    { "input",  CC, 0x01BB },     /* TV_INPUT in the layout of Google's remote: needs compatibility mode */
    { "input2", CC, 0x0089 },     /* Media Select TV */
    { "settings",  CC, 0x0096 },  /* SETTINGS in the layout of Google's remote */
    { "settings2", CC, 0x019F },  /* SETTINGS in Android's generic layout */
    { "menu",   CC, 0x0040 },     { "menu2",  KB, 0x65 },
    { "search", CC, 0x0221 },     { "guide",  CC, 0x008D },   { "info",  CC, 0x0060 },
    { "chup",   CC, 0x009C },     { "chdown", CC, 0x009D },
    { "volup",  CC, 0x00E9 },     { "voldown", CC, 0x00EA },  { "mute",  CC, 0x00E2 },
    { "playpause", CC, 0x00CD },  { "stop",   CC, 0x00B7 },
    { "rew",    CC, 0x00B4 },     { "ffwd",   CC, 0x00B3 },
    { "prev",   CC, 0x00B6 },     { "next",   CC, 0x00B5 },
    { "power",  CC, 0x0030 },
    { "youtube", CC, 0x0077 },    { "netflix", CC, 0x0078 },  /* Google remote layout */
};

/* PnP ID of the Device Information service: source 2 (USB), vendor, product, version, little endian.
 * Own: the shared V-USB ids. Compatibility mode (off by default) reports the ids of Google's reference
 * remote, so that Android applies that remote's key layout. It does not make this a Google device. */
static const uint8_t PNP_OWN[]    = { 0x02, 0xC0, 0x16, 0xDF, 0x05, 0x00, 0x01 };
static const uint8_t PNP_COMPAT[] = { 0x02, 0x57, 0x09, 0x01, 0x00, 0x01, 0x00 };
static volatile bool s_compat;

static ble_addr_t s_tv;              /* identity address of the paired TV */
static bool s_has_tv;
static volatile uint16_t s_tv_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile int64_t s_window_until_us;
static esp_timer_handle_t s_window_timer;
static uint16_t s_kbd_handle, s_cc_handle;

/* ---------- GATT ---------- */
enum { A_INFO, A_MAP, A_CTRL, A_PROTO, A_KBD, A_CC, A_KBD_REF, A_CC_REF, A_BATT, A_PNP, A_MANUF };

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *c, void *arg)
{
    static const uint8_t info[] = { 0x11, 0x01, 0x00, 0x03 };       /* HID 1.11, remote wake, connectable */
    static const uint8_t proto = 1;                                   /* report protocol */
    static const uint8_t ref_kbd[] = { 1, 1 }, ref_cc[] = { 2, 1 };   /* report id, input */
    static const uint8_t zero[KBD_LEN] = { 0 };
    static const uint8_t battery = 100;
    static const char manuf[] = "IRCapsule";
    if (c->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;               /* control point, protocol mode */
    const void *p = NULL; size_t n = 0;
    switch ((int)(intptr_t)arg) {
    case A_INFO:    p = info; n = sizeof info; break;
    case A_MAP:     p = REPORT_MAP; n = sizeof REPORT_MAP; break;
    case A_PROTO:   p = &proto; n = 1; break;
    case A_KBD:     p = zero; n = KBD_LEN; break;
    case A_CC:      p = zero; n = CC_LEN; break;
    case A_KBD_REF: p = ref_kbd; n = 2; break;
    case A_CC_REF:  p = ref_cc; n = 2; break;
    case A_BATT:    p = &battery; n = 1; break;
    case A_PNP:     p = s_compat ? PNP_COMPAT : PNP_OWN; n = sizeof PNP_OWN; break;
    case A_MANUF:   p = manuf; n = sizeof manuf - 1; break;
    default: return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(c->om, p, n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

#define U16(x) BLE_UUID16_DECLARE(x)
#define ARG(x) .access_cb = hid_access, .arg = (void *)(intptr_t)(x)
#define REPORT_FLAGS (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC)

static const struct ble_gatt_svc_def SVCS[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x1812), .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = U16(0x2A4A), ARG(A_INFO), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
        { .uuid = U16(0x2A4B), ARG(A_MAP),  .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
        { .uuid = U16(0x2A4C), ARG(A_CTRL), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
        { .uuid = U16(0x2A4E), ARG(A_PROTO), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
        { .uuid = U16(0x2A4D), ARG(A_KBD), .flags = REPORT_FLAGS, .val_handle = &s_kbd_handle,
          .descriptors = (struct ble_gatt_dsc_def[]) {
              { .uuid = U16(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, ARG(A_KBD_REF) }, { 0 } } },
        { .uuid = U16(0x2A4D), ARG(A_CC), .flags = REPORT_FLAGS, .val_handle = &s_cc_handle,
          .descriptors = (struct ble_gatt_dsc_def[]) {
              { .uuid = U16(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, ARG(A_CC_REF) }, { 0 } } },
        { 0 } } },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180F), .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = U16(0x2A19), ARG(A_BATT), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY }, { 0 } } },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180A), .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = U16(0x2A50), ARG(A_PNP), .flags = BLE_GATT_CHR_F_READ },
        { .uuid = U16(0x2A29), ARG(A_MANUF), .flags = BLE_GATT_CHR_F_READ }, { 0 } } },
    { 0 }
};

const struct ble_gatt_svc_def *remote_gatt_svcs(void) { return SVCS; }

/* ---------- the paired TV ---------- */
static void save_tv(void)
{
    nvs_handle_t h;
    if (nvs_open("tvremote", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_has_tv) nvs_set_blob(h, "peer", &s_tv, sizeof s_tv); else nvs_erase_key(h, "peer");
    nvs_commit(h);
    nvs_close(h);
}

static void close_window(void) { s_window_until_us = 0; }
static void window_expired(void *arg) { close_window(); ilog("tv: pairing window closed (time is up)"); }

/* Bond storage is full: drop some other peer, never the TV. */
static int store_status(struct ble_store_status_event *ev, void *arg)
{
    if (ev->event_code == BLE_STORE_EVENT_OVERFLOW && s_has_tv) {
        ble_addr_t peers[MAX_PEERS];
        int n = 0;
        if (!ble_store_util_bonded_peers(peers, &n, MAX_PEERS))
            for (int i = 0; i < n; i++)
                if (ble_addr_cmp(&peers[i], &s_tv)) return ble_store_util_delete_peer(&peers[i]);
    }
    return ble_store_util_status_rr(ev, arg);
}

void remote_init(void)
{
    ble_hs_cfg.store_status_cb = store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    close_window();
    const esp_timer_create_args_t t = { .callback = window_expired, .name = "pairwin" };
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_window_timer));
    nvs_handle_t h;
    if (nvs_open("tvremote", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof s_tv;
        s_has_tv = nvs_get_blob(h, "peer", &s_tv, &n) == ESP_OK && n == sizeof s_tv;
        uint8_t compat = 0;
        nvs_get_u8(h, "compat", &compat);
        s_compat = compat != 0;
        nvs_close(h);
    }
}

bool remote_compat(void) { return s_compat; }

void remote_set_compat(bool on)
{
    if (on == s_compat) return;
    s_compat = on;
    nvs_handle_t h;
    if (nvs_open("tvremote", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "compat", on); nvs_commit(h); nvs_close(h); }
    ilog("tv: compatibility mode %s", on ? "on" : "off");
    remote_forget();          /* a TV reads the ids when it pairs, so it has to pair again */
}

bool remote_is_tv(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    return s_has_tv && !ble_gap_conn_find(conn, &d) && !ble_addr_cmp(&s_tv, &d.peer_id_addr);
}

bool remote_pairing_open(void) { return s_window_until_us > esp_timer_get_time(); }

void remote_pair(bool open)
{
    esp_timer_stop(s_window_timer);          /* fails harmlessly when not running */
    if (!open) {
        if (remote_pairing_open()) ilog("tv: pairing window closed");
        close_window();
        return;
    }
    s_window_until_us = esp_timer_get_time() + PAIR_WINDOW_S * 1000000LL;
    esp_timer_start_once(s_window_timer, PAIR_WINDOW_S * 1000000ULL);
    ilog("tv: pairing window open for %d s", PAIR_WINDOW_S);
}

void remote_forget(void)
{
    if (!s_has_tv) return;
    uint16_t conn = s_tv_conn;
    s_tv_conn = BLE_HS_CONN_HANDLE_NONE;
    s_has_tv = false;
    ble_store_util_delete_peer(&s_tv);
    save_tv();
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    ilog("tv: forgotten");
}

void remote_on_encrypted(uint16_t conn, bool new_pairing, bool is_web)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d)) return;
    if (new_pairing && remote_pairing_open() && d.sec_state.bonded && !is_web) {
        if (s_has_tv && ble_addr_cmp(&s_tv, &d.peer_id_addr)) ble_store_util_delete_peer(&s_tv);
        s_tv = d.peer_id_addr;
        s_has_tv = true;
        save_tv();
        s_tv_conn = conn;
        esp_timer_stop(s_window_timer);
        close_window();
        ilog("tv: paired and connected");
    } else if (s_has_tv && !ble_addr_cmp(&s_tv, &d.peer_id_addr)) {
        s_tv_conn = conn;
        ilog("tv: reconnected");
    } else if (new_pairing && !is_web) {
        /* the usual mistake: pairing from the TV before opening the window */
        ilog("tv: a host paired while the pairing window was closed; not used as the TV");
    }
}

void remote_on_disconnect(uint16_t conn)
{
    if (conn != s_tv_conn) return;
    s_tv_conn = BLE_HS_CONN_HANDLE_NONE;
    ilog("tv: disconnected");
}

void remote_status(bool *paired, bool *connected, int *window_s)
{
    *paired = s_has_tv;
    *connected = s_tv_conn != BLE_HS_CONN_HANDLE_NONE;
    int64_t left = s_window_until_us - esp_timer_get_time();
    *window_s = left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

static int notify(uint16_t conn, uint16_t handle, const uint8_t *buf, size_t n)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
    return om ? ble_gatts_notify_custom(conn, handle, om) : BLE_HS_ENOMEM;
}

static const char *press(uint8_t report, uint16_t code, const char *label, int hold_ms)
{
    if (hold_ms < 1 || hold_ms > REMOTE_MAX_HOLD_MS) return "bad hold time";
    uint16_t conn = s_tv_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return "TV not connected";
    uint8_t down[KBD_LEN] = { 0 }, up[KBD_LEN] = { 0 };
    size_t n = report == KB ? KBD_LEN : CC_LEN;
    uint16_t handle = report == KB ? s_kbd_handle : s_cc_handle;
    if (report == KB) down[2] = (uint8_t)code;
    else { down[0] = code & 0xff; down[1] = code >> 8; }
    int a = notify(conn, handle, down, n);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    int b = notify(conn, handle, up, n);
    if (a || b) { vTaskDelay(pdMS_TO_TICKS(40)); b = notify(conn, handle, up, n); }   /* never leave a key down */
    /* the code is logged so that a user's report tells exactly what was tried */
    ilog("tv: key %s (%s 0x%X%s%s) %s", label, report == KB ? "kb" : "cc", code, hold_ms >= 500 ? " held" : "",
         s_compat ? " compat" : "", !a && !b ? "sent" : "ERROR");
    return !a && !b ? NULL : "send failed";
}

const char *remote_key(const char *name, int hold_ms)
{
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (!strcmp(KEYS[i].name, name)) return press(KEYS[i].report, KEYS[i].code, name, hold_ms);
    return "unknown key";
}

const char *remote_raw(const char *page, long code, int hold_ms)
{
    bool kb = !strcmp(page, "kb");
    if ((!kb && strcmp(page, "cc")) || code < 1 || code > (kb ? 0xFF : 0x3FF)) return "bad key code";   /* report map limits */
    return press(kb ? KB : CC, (uint16_t)code, "raw", hold_ms);
}
