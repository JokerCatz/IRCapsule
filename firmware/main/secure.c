#include "secure.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "ble.h"
#include "engine.h"
#include "jsonl.h"
#include "proto.h"

#define ITERATIONS      20000
#define SALT_LEN        16
#define NONCE_LEN       16
#define KEY_LEN         32
#define TAG_LEN         16
#define PIECE           600                       /* most plaintext in one record */
#define RECORD_MAX      (1 + 4 * ((PIECE + TAG_LEN + 2) / 3))
#define LINE_CAP        (RECORD_MAX + 64)
#define LINKS           CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define UNACCEPTED_US   30000000LL
#define LOCKOUT_US      8000000LL                 /* after a wrong header, for everybody; longer than a reconnect */
static const char PROOF[] = "ircapsule";

typedef struct {
    bool used, accepted, encrypted, dead, have_nonce;
    uint16_t conn;
    int64_t opened_us, drop_at_us;
    uint8_t nonce[NONCE_LEN];                     /* ours, from the last hello; used once */
    psa_key_id_t k_rx, k_tx;
    uint64_t n_rx, n_tx;
    size_t len;
    char line[LINE_CAP];
} link_t;

static link_t s_links[LINKS];
static SemaphoreHandle_t s_m;                     /* links, keys and the header */
static volatile uint16_t s_web = BLE_HS_CONN_HANDLE_NONE;   /* the one accepted link */
static char s_header[SECURE_HEADER_MAX + 1];
static uint8_t s_salt[SALT_LEN], s_master[KEY_LEN];
static int64_t s_lockout_until_us;
static esp_timer_handle_t s_timer;

#define LOCK()   xSemaphoreTake(s_m, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_m)

/* ---------- small crypto helpers (PSA) ---------- */
static bool hmac_key(const uint8_t *key, size_t n, psa_key_id_t *id)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    return psa_import_key(&a, key, n, id) == PSA_SUCCESS;
}

static bool hmac(psa_key_id_t id, const uint8_t *in, size_t n, uint8_t out[KEY_LEN])
{
    size_t len = 0;
    return psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), in, n, out, KEY_LEN, &len) == PSA_SUCCESS && len == KEY_LEN;
}

/* PBKDF2-HMAC-SHA256, first (and only) 32-byte block */
static bool derive_master(const char *header, const uint8_t salt[SALT_LEN], uint8_t out[KEY_LEN])
{
    psa_key_id_t k;
    if (!hmac_key((const uint8_t *)header, strlen(header), &k)) return false;
    uint8_t first[SALT_LEN + 4] = { 0 }, u[KEY_LEN], t[KEY_LEN];
    memcpy(first, salt, SALT_LEN);
    first[SALT_LEN + 3] = 1;
    bool ok = hmac(k, first, sizeof first, u);
    memcpy(t, u, KEY_LEN);
    for (int i = 1; ok && i < ITERATIONS; i++) {
        ok = hmac(k, u, KEY_LEN, u);
        for (int j = 0; j < KEY_LEN; j++) t[j] ^= u[j];
        if (!(i % 2000)) vTaskDelay(1);           /* let the other tasks and the watchdog run */
    }
    psa_destroy_key(k);
    if (ok) memcpy(out, t, KEY_LEN);
    return ok;
}

static bool session_key(const char *label, const uint8_t *client_nonce, const uint8_t *our_nonce, psa_key_usage_t use, psa_key_id_t *id)
{
    uint8_t msg[3 + 2 * NONCE_LEN], key[KEY_LEN];
    memcpy(msg, label, 3);
    memcpy(msg + 3, client_nonce, NONCE_LEN);
    memcpy(msg + 3 + NONCE_LEN, our_nonce, NONCE_LEN);
    psa_key_id_t m;
    if (!hmac_key(s_master, KEY_LEN, &m)) return false;
    bool ok = hmac(m, msg, sizeof msg, key);
    psa_destroy_key(m);
    if (!ok) return false;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_usage_flags(&a, use);
    ok = psa_import_key(&a, key, KEY_LEN, id) == PSA_SUCCESS;
    memset(key, 0, sizeof key);
    return ok;
}

static void iv_of(uint8_t iv[12], uint64_t counter)
{
    memset(iv, 0, 12);
    for (int i = 0; i < 8; i++) iv[11 - i] = (uint8_t)(counter >> (8 * i));
}

/* "~base64(ciphertext||tag)\n" into out; returns its length, 0 on failure. Called with the lock held. */
static size_t seal(link_t *l, const char *plain, size_t n, char *out, size_t cap)
{
    static uint8_t box[PIECE + TAG_LEN];
    uint8_t iv[12];
    size_t blen = 0, olen = 0;
    iv_of(iv, l->n_tx);
    if (psa_aead_encrypt(l->k_tx, PSA_ALG_GCM, iv, sizeof iv, NULL, 0, (const uint8_t *)plain, n, box, sizeof box, &blen) != PSA_SUCCESS) return 0;
    out[0] = '~';
    if (mbedtls_base64_encode((unsigned char *)out + 1, cap - 2, &olen, box, blen)) return 0;
    out[1 + olen] = '\n';
    l->n_tx++;
    return olen + 2;
}

/* opens base64 text; returns the plaintext length or -1. Called with the lock held. */
static int open_record(link_t *l, const char *b64, uint8_t *out, size_t cap)
{
    static uint8_t box[PIECE + TAG_LEN];
    uint8_t iv[12];
    size_t blen = 0, plen = 0;
    if (mbedtls_base64_decode(box, sizeof box, &blen, (const unsigned char *)b64, strlen(b64)) || blen < TAG_LEN) return -1;
    iv_of(iv, l->n_rx);
    if (psa_aead_decrypt(l->k_rx, PSA_ALG_GCM, iv, sizeof iv, NULL, 0, box, blen, out, cap, &plen) != PSA_SUCCESS) return -1;
    l->n_rx++;
    return (int)plen;
}

static bool unhex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < 2 * n; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] | v : v << 4);
    }
    return true;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
}

/* ---------- links ---------- */
static link_t *find(uint16_t conn)
{
    for (int i = 0; i < LINKS; i++) if (s_links[i].used && s_links[i].conn == conn) return &s_links[i];
    return NULL;
}

static void release(link_t *l)
{
    if (l->k_rx) psa_destroy_key(l->k_rx);
    if (l->k_tx) psa_destroy_key(l->k_tx);
    memset(l, 0, sizeof *l);
}

static void kill(link_t *l)                        /* lock held; stops using the link and disconnects it */
{
    l->dead = true;
    if (s_web == l->conn) s_web = BLE_HS_CONN_HANDLE_NONE;
    ble_drop(l->conn);
}

/* This link is now the web page: it takes over and the previous one is disconnected. Lock held. */
static void accept(link_t *l)
{
    uint16_t old = s_web;
    l->accepted = true;
    proto_reset(TR_BLE);                           /* never glue a half line of the old page to the new one */
    s_web = l->conn;
    g_ble_mtu = ble_mtu(l->conn);
    if (old != BLE_HS_CONN_HANDLE_NONE && old != l->conn) {
        link_t *o = find(old);
        if (o) kill(o);
    }
}

void secure_link_open(uint16_t conn)
{
    LOCK();
    link_t *l = find(conn);
    if (l) release(l);
    if (s_web == conn) s_web = BLE_HS_CONN_HANDLE_NONE;   /* subscribed again: it has to be accepted again */
    for (int i = 0; !l && i < LINKS; i++) if (!s_links[i].used) l = &s_links[i];
    if (l) {
        memset(l, 0, sizeof *l);
        l->used = true;
        l->conn = conn;
        l->opened_us = esp_timer_get_time();
        l->encrypted = s_header[0] != 0;
        if (!l->encrypted) accept(l);
    }
    UNLOCK();
}

void secure_link_close(uint16_t conn)
{
    LOCK();
    link_t *l = find(conn);
    if (l) release(l);
    if (s_web == conn) s_web = BLE_HS_CONN_HANDLE_NONE;
    UNLOCK();
}

void secure_mtu(uint16_t conn, int mtu) { if (conn == s_web) g_ble_mtu = mtu; }

/* A new page may reuse the Bluetooth connection (no new subscribe event): its hello starts the link over.
 * Lock held. */
static void restart(link_t *l)
{
    if (s_web == l->conn) s_web = BLE_HS_CONN_HANDLE_NONE;
    if (l->k_rx) psa_destroy_key(l->k_rx);
    if (l->k_tx) psa_destroy_key(l->k_tx);
    l->k_rx = l->k_tx = 0;
    l->n_rx = l->n_tx = 0;
    l->accepted = l->dead = l->have_nonce = false;
    l->encrypted = s_header[0] != 0;
    l->drop_at_us = 0;
    l->opened_us = esp_timer_get_time();
}

static bool is_hello(const char *line)
{
    char cmd[8] = "";
    return line[0] == '{' && js_str(line, "cmd", cmd, sizeof cmd) && !strcmp(cmd, "hello");
}

static void say(uint16_t conn, const char *s) { ble_send(conn, s, strlen(s)); }

/* A plaintext line from a link that has not proved anything yet: only hello and auth mean something.
 * Takes the lock itself for the parts that need it; replies are sent without it. */
static void handshake(uint16_t conn, const char *line)
{
    long q = 0;
    js_int(line, "q", &q);
    char cmd[8] = "", reply[200];
    js_str(line, "cmd", cmd, sizeof cmd);
    LOCK();
    link_t *l = find(conn);
    if (!l || l->dead || l->accepted) { UNLOCK(); return; }
    if (!strcmp(cmd, "hello")) {
        esp_fill_random(l->nonce, NONCE_LEN);
        l->have_nonce = true;
        char s[2 * SALT_LEN + 1], n[2 * NONCE_LEN + 1];
        hex(s_salt, SALT_LEN, s);
        hex(l->nonce, NONCE_LEN, n);
        snprintf(reply, sizeof reply, "{\"q\":%ld,\"ok\":true,\"enc\":1,\"s\":\"%s\",\"i\":%d,\"n\":\"%s\"}\n", q, s, ITERATIONS, n);
        UNLOCK();
        say(conn, reply);
        return;
    }
    if (strcmp(cmd, "auth")) {
        UNLOCK();
        snprintf(reply, sizeof reply, "{\"q\":%ld,\"ok\":false,\"err\":\"locked\"}\n", q);
        say(conn, reply);
        return;
    }
    if (esp_timer_get_time() < s_lockout_until_us) {
        UNLOCK();
        snprintf(reply, sizeof reply, "{\"q\":%ld,\"ok\":false,\"err\":\"wait\"}\n", q);
        say(conn, reply);
        return;
    }
    char chex[2 * NONCE_LEN + 2] = "", proof[64] = "";
    uint8_t cn[NONCE_LEN], plain[32];
    bool ok = l->have_nonce && js_str(line, "c", chex, sizeof chex) && unhex(chex, cn, NONCE_LEN) && js_str(line, "p", proof, sizeof proof)
              && session_key("c2d", cn, l->nonce, PSA_KEY_USAGE_DECRYPT, &l->k_rx)
              && session_key("d2c", cn, l->nonce, PSA_KEY_USAGE_ENCRYPT, &l->k_tx);
    l->have_nonce = false;                         /* one try per hello: a recorded auth is worth nothing */
    ok = ok && open_record(l, proof, plain, sizeof plain) == (int)strlen(PROOF) && !memcmp(plain, PROOF, strlen(PROOF));
    if (!ok) {
        s_lockout_until_us = esp_timer_get_time() + LOCKOUT_US;
        l->dead = true;
        l->drop_at_us = esp_timer_get_time() + 500000;   /* give the answer time to leave */
        UNLOCK();
        snprintf(reply, sizeof reply, "{\"q\":%ld,\"ok\":false,\"err\":\"bad header\"}\n", q);
        say(conn, reply);
        ilog("access: wrong header");
        return;
    }
    char answer[48], record[96];
    int an = snprintf(answer, sizeof answer, "{\"q\":%ld,\"ok\":true}\n", q);
    size_t rn = seal(l, answer, an, record, sizeof record);
    UNLOCK();
    if (!rn || !ble_send(conn, record, rn)) { ble_drop(conn); return; }
    LOCK();                                        /* events may only start after the answer has left */
    if ((l = find(conn)) && !l->dead) accept(l);
    UNLOCK();
}

void secure_rx(uint16_t conn, const uint8_t *data, size_t len)
{
    LOCK();
    link_t *l = find(conn);
    bool dead = !l || l->dead, plain_pass = l && l->accepted && !l->encrypted;
    UNLOCK();
    if (dead) return;
    if (plain_pass) {                              /* open mode: the bytes go straight to the line parser */
        if (conn == s_web) proto_feed(TR_BLE, data, len);
        return;
    }
    static char line[LINE_CAP];                    /* only the BLE host task runs this */
    static uint8_t plain[PIECE];
    for (size_t i = 0; i < len; i++) {
        LOCK();
        l = find(conn);
        if (!l || l->dead) { UNLOCK(); return; }
        if (data[i] != '\n') {
            if (l->len < LINE_CAP - 1) l->line[l->len++] = (char)data[i];
            else kill(l);
            UNLOCK();
            continue;
        }
        l->line[l->len] = 0;
        memcpy(line, l->line, l->len + 1);
        l->len = 0;
        if (l->accepted && s_header[0] && is_hello(line)) restart(l);   /* e.g. a reloaded page */
        if (!l->accepted) {
            UNLOCK();
            handshake(conn, line);
            continue;
        }
        int n = line[0] == '~' ? open_record(l, line + 1, plain, sizeof plain) : -1;
        if (n < 0) {                               /* altered, repeated, out of order or not encrypted */
            kill(l);
            UNLOCK();
            ilog("access: bad record, link closed");
            return;
        }
        UNLOCK();
        if (conn == s_web) proto_feed(TR_BLE, plain, n);
    }
}

bool secure_hello_again(const char *line)
{
    uint16_t conn = s_web;
    LOCK();
    link_t *l = s_header[0] && conn != BLE_HS_CONN_HANDLE_NONE ? find(conn) : NULL;
    if (l) restart(l);
    UNLOCK();
    if (l) handshake(conn, line);
    return l != NULL;
}

bool secure_ready(void) { return s_web != BLE_HS_CONN_HANDLE_NONE; }
bool secure_is_web(uint16_t conn) { return conn == s_web; }

/* The transport lock makes callers take turns, so records leave in counter order. */
void secure_tx(const char *data, size_t len)
{
    static char record[RECORD_MAX + 4];
    while (len) {
        LOCK();
        link_t *l = s_web == BLE_HS_CONN_HANDLE_NONE ? NULL : find(s_web);
        if (!l) { UNLOCK(); return; }
        uint16_t conn = l->conn;
        bool enc = l->encrypted;
        size_t piece = enc && len > PIECE ? PIECE : len, out = piece;
        if (enc) out = seal(l, data, piece, record, sizeof record);
        UNLOCK();
        if (!out || !ble_send(conn, enc ? record : data, out)) {
            if (enc) ble_drop(conn);               /* a lost record would leave both sides out of step */
            return;
        }
        data += piece;
        len -= piece;
    }
}

/* ---------- the header ---------- */
bool secure_enabled(void) { return s_header[0] != 0; }

void secure_get(char *out, size_t cap)
{
    LOCK();
    snprintf(out, cap, "%s", s_header);
    UNLOCK();
}

const char *secure_set(const char *h, bool from_ble)
{
    size_t n = strlen(h);
    if (n > SECURE_HEADER_MAX || strspn(h, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~") != n) return "bad header";
    uint8_t salt[SALT_LEN] = { 0 }, master[KEY_LEN] = { 0 };
    if (n) {
        esp_fill_random(salt, sizeof salt);
        if (!derive_master(h, salt, master)) return "crypto failed";
    }
    nvs_handle_t nv;                               /* namespace of the signals: factory reset erases it */
    if (nvs_open("ircap", NVS_READWRITE, &nv) != ESP_OK) return "flash write failed";
    esp_err_t e = ESP_OK;
    if (n) {
        e = nvs_set_str(nv, "acc_h", h);
        if (e == ESP_OK) e = nvs_set_blob(nv, "acc_salt", salt, sizeof salt);
        if (e == ESP_OK) e = nvs_set_blob(nv, "acc_mk", master, sizeof master);
    } else {
        nvs_erase_key(nv, "acc_h");
        nvs_erase_key(nv, "acc_salt");
        nvs_erase_key(nv, "acc_mk");
    }
    if (e == ESP_OK) e = nvs_commit(nv);
    nvs_close(nv);
    if (e != ESP_OK) return "flash write failed";
    LOCK();
    snprintf(s_header, sizeof s_header, "%s", h);
    memcpy(s_salt, salt, sizeof salt);
    memcpy(s_master, master, sizeof master);
    for (int i = 0; i < LINKS; i++)
        if (s_links[i].used && !(from_ble && s_links[i].conn == s_web)) kill(&s_links[i]);
    UNLOCK();
    ilog(n ? "access: header set" : "access: header cleared");
    return NULL;
}

static void tick(void *arg)
{
    int64_t now = esp_timer_get_time();
    LOCK();
    for (int i = 0; i < LINKS; i++) {
        link_t *l = &s_links[i];
        if (!l->used || l->accepted) continue;
        if (l->drop_at_us && now >= l->drop_at_us) { l->drop_at_us = 0; kill(l); }
        else if (!l->dead && now - l->opened_us > UNACCEPTED_US) kill(l);
    }
    UNLOCK();
}

void secure_init(void)
{
    s_m = xSemaphoreCreateMutex();
    if (psa_crypto_init() != PSA_SUCCESS) ilog("access: crypto init failed");
    nvs_handle_t nv;
    if (nvs_open("ircap", NVS_READONLY, &nv) == ESP_OK) {
        size_t a = sizeof s_header, b = sizeof s_salt, c = sizeof s_master;
        if (nvs_get_str(nv, "acc_h", s_header, &a) != ESP_OK || nvs_get_blob(nv, "acc_salt", s_salt, &b) != ESP_OK
            || nvs_get_blob(nv, "acc_mk", s_master, &c) != ESP_OK) s_header[0] = 0;
        nvs_close(nv);
    }
    const esp_timer_create_args_t t = { .callback = tick, .name = "access" };
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, 250000));
}
