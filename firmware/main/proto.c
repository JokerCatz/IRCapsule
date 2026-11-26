/* Command interpreter. One JSON object per line in, one reply line out.
 * Request: {"q":<n>,"cmd":"<name>", ...}   Reply: {"q":<n>,"ok":true,...} or {"q":<n>,"ok":false,"err":"..."}
 * See docs/protocol.md. */
#include "proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "engine.h"
#include "ir_rmt.h"
#include "remote.h"
#include "secure.h"
#include "jsonl.h"
#include "store.h"
#include "ui_store.h"
#include "mbedtls/base64.h"

#define FW_VERSION "0.2.0"
#define LINE_MAX_LEN 12288

volatile int g_ble_mtu = 23;

typedef struct { char *line; uint8_t tr; } msg_t;
static QueueHandle_t s_q;
static char *s_buf[TR_COUNT];
static size_t s_len[TR_COUNT];
static uint16_t s_raw[MAX_RAW];

/* ---------- reply helpers ---------- */
static void reply_err(tr_id_t tr, long q, const char *err)
{
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":false,\"err\":", q);
    out_jstr(&o, err);
    out_puts(&o, "}");
    tr_end(tr, &o);
}

static void reply_simple(tr_id_t tr, long q, const char *extra)
{
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true%s}", q, extra ? extra : "");
    tr_end(tr, &o);
}

static void put_raw(out_t *o, const uint16_t *raw, size_t n)
{
    out_puts(o, "\"raw\":[");
    for (size_t i = 0; i < n; i++) out_printf(o, i ? ",%u" : "%u", raw[i]);
    out_puts(o, "]");
}

static void copy_name(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }

/* ---------- commands ---------- */
static void cmd_info(tr_id_t tr, long q)
{
    int ns = 0, nr = 0, nd = 0, ndr = 0;
    for (int i = 0; i < MAX_SIGNALS; i++) { ns += g_meta.sig[i].id != 0; ndr += g_meta.sig[i].id && g_meta.sig[i].state == ST_DRAFT; }
    for (int i = 0; i < MAX_RULES; i++) nr += g_meta.rule[i].id != 0;
    for (int i = 0; i < MAX_DEVICES; i++) nd += g_meta.dev[i].id != 0;
    nvs_stats_t st = {0};
    nvs_get_stats(NULL, &st);
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"fw\":\"%s\",\"name\":", q, FW_VERSION);
    out_jstr(&o, g_meta.device_name[0] ? g_meta.device_name : "IRCapsule");
    out_printf(&o, ",\"heap\":%u,\"minheap\":%u,\"up\":%lld,\"layer\":%u,\"rec\":%d,\"ns\":%d,\"nr\":%d,\"nd\":%d,\"drafts\":%d,"
                   "\"access\":%d,\"dedupe\":%u,\"mtu\":%d,\"nvs\":{\"used\":%u,\"free\":%u,\"total\":%u},"
                   "\"max\":{\"sig\":%d,\"rule\":%d,\"dev\":%d,\"raw\":%d,\"act\":%d}}",
               (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
               (long long)(esp_timer_get_time() / 1000000), engine_layer(), engine_recording(), ns, nr, nd, ndr,
               secure_enabled(), g_meta.dedupe_ms, g_ble_mtu, (unsigned)st.used_entries, (unsigned)st.free_entries, (unsigned)st.total_entries,
               MAX_SIGNALS, MAX_RULES, MAX_DEVICES, MAX_RAW, MAX_ACTIONS);
    tr_end(tr, &o);
}

static void cmd_list(tr_id_t tr, long q)
{
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"layer\":%u,\"dev\":[", q, engine_layer());
    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        dev_meta_t *d = &g_meta.dev[i];
        if (!d->id) continue;
        out_printf(&o, "%s{\"id\":%u,\"name\":", first ? "" : ",", d->id); out_jstr(&o, d->name); out_puts(&o, "}");
        first = false;
    }
    out_puts(&o, "],\"sig\":[");
    first = true;
    for (int i = 0; i < MAX_SIGNALS; i++) {
        sig_meta_t *s = &g_meta.sig[i];
        if (!s->id) continue;
        out_printf(&o, "%s{\"id\":%u,\"kind\":%u,\"st\":%u,\"hits\":%u,\"dev\":%u,\"khz\":%u,\"n\":%u,\"name\":", first ? "" : ",", s->id, s->kind, s->state, s->hits, s->device, s->carrier_khz, s->n);
        out_jstr(&o, s->name); out_puts(&o, ",\"p\":"); out_jstr(&o, s->proto); out_puts(&o, "}");
        first = false;
    }
    out_puts(&o, "],\"rule\":[");
    first = true;
    for (int i = 0; i < MAX_RULES; i++) {
        rule_meta_t *r = &g_meta.rule[i];
        if (!r->id) continue;
        out_printf(&o, "%s{\"id\":%u,\"layer\":%u,\"on\":%u,\"trig\":%u,\"name\":", first ? "" : ",", r->id, r->layer, r->enabled, r->trigger);
        out_jstr(&o, r->name); out_puts(&o, ",\"act\":[");
        for (int a = 0; a < r->nact; a++) out_printf(&o, "%s%u,%u", a ? "," : "", r->act[a].type, r->act[a].val);
        out_puts(&o, "]}");
        first = false;
    }
    out_puts(&o, "]}");
    tr_end(tr, &o);
}

static void cmd_get(tr_id_t tr, long q, const char *line)
{
    long id = 0; js_int(line, "id", &id);
    sig_meta_t *s = store_sig((uint16_t)id);
    size_t n;
    if (!s) return reply_err(tr, q, "no such signal");
    if (store_sig_read_raw(s->id, s_raw, MAX_RAW, &n) != ESP_OK) return reply_err(tr, q, "no data");
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"id\":%u,\"kind\":%u,\"st\":%u,\"hits\":%u,\"dev\":%u,\"khz\":%u,\"name\":", q, s->id, s->kind, s->state, s->hits, s->device, s->carrier_khz);
    out_jstr(&o, s->name); out_puts(&o, ",\"p\":"); out_jstr(&o, s->proto);
    out_printf(&o, ",\"n\":%u,", (unsigned)n); put_raw(&o, s_raw, n);
    out_puts(&o, "}");
    tr_end(tr, &o);
}

static void cmd_frame(tr_id_t tr, long q, const char *line)
{
    long seq = 0; js_int(line, "seq", &seq);
    frame_t f;
    if (!engine_frame_copy((uint32_t)seq, &f, s_raw, MAX_RAW)) return reply_err(tr, q, "no such frame");
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"seq\":%u,\"t\":%lld,\"gap\":%d,\"m\":%u,\"p\":", q, (unsigned)f.seq, (long long)f.t_ms, (int)f.gap_ms, f.match);
    out_jstr(&o, f.proto); out_printf(&o, ",\"n\":%u,", f.n); put_raw(&o, f.raw, f.n);
    out_puts(&o, "}");
    tr_end(tr, &o);
}

static void cmd_sig_put(tr_id_t tr, long q, const char *line)
{
    long id = 0, v;
    js_int(line, "id", &id);
    size_t n = 0;
    char proto[24] = "";
    bool have_raw = false;
    int cnt = js_arr_u16(line, "raw", s_raw, MAX_RAW);
    if (cnt >= 4) { n = cnt; have_raw = true; engine_normalise(s_raw, n); }
    else if (js_int(line, "frame", &v)) {
        frame_t f;
        if (!engine_frame_copy((uint32_t)v, &f, s_raw, MAX_RAW)) return reply_err(tr, q, "no such frame");
        n = f.n; snprintf(proto, sizeof proto, "%s", f.proto); have_raw = true;
    }
    sig_meta_t *s;
    if (id) {
        s = store_sig((uint16_t)id);
        if (!s) return reply_err(tr, q, "no such signal");
    } else {
        if (!have_raw) return reply_err(tr, q, "raw or frame required");
        s = store_sig_alloc();
        if (!s) return reply_err(tr, q, "signal table full");
        s->carrier_khz = 38;
        s->hits = 1;
    }
    char name[NAME_LEN];
    bool named = js_str(line, "name", name, sizeof name) && name[0];
    if (named) copy_name(s->name, sizeof s->name, name);
    if (js_int(line, "state", &v) && v >= 0 && v <= 2) {
        if (v != ST_DRAFT && !s->name[0]) return reply_err(tr, q, "a signal needs a name before it can be used");
        s->state = (uint8_t)v;
    } else if (!id) s->state = named ? ST_ACTIVE : ST_DRAFT;
    if (js_str(line, "proto", s->proto, sizeof s->proto)) {}
    else if (proto[0]) snprintf(s->proto, sizeof s->proto, "%s", proto);
    if (js_int(line, "kind", &v)) s->kind = v ? SIG_TRIGGER : SIG_REAL;
    if (js_int(line, "dev", &v)) s->device = (uint8_t)v;
    if (js_int(line, "khz", &v) && v >= 20 && v <= 60) s->carrier_khz = (uint16_t)v;
    if (have_raw) {
        if (store_sig_write_raw(s->id, s_raw, n) != ESP_OK) return reply_err(tr, q, "flash write failed");
        s->n = (uint16_t)n;
    }
    if (store_save_meta() != ESP_OK) return reply_err(tr, q, "flash write failed");
    ilog("saved signal \"%s\" (%u entries)", s->name[0] ? s->name : "(draft)", s->n);
    char extra[40]; snprintf(extra, sizeof extra, ",\"id\":%u", s->id);
    reply_simple(tr, q, extra);
}

static void cmd_sig_del(tr_id_t tr, long q, const char *line)
{
    long id = 0; js_int(line, "id", &id);
    if (store_sig_delete((uint16_t)id) != ESP_OK) return reply_err(tr, q, "no such signal");
    ilog("deleted signal %ld", id);
    reply_simple(tr, q, NULL);
}

static void cmd_dev_put(tr_id_t tr, long q, const char *line)
{
    long id = 0; js_int(line, "id", &id);
    dev_meta_t *d = id ? store_dev((uint8_t)id) : store_dev_alloc();
    if (!d) return reply_err(tr, q, id ? "no such device" : "device table full");
    char name[NAME_LEN];
    if (js_str(line, "name", name, sizeof name) && name[0]) copy_name(d->name, sizeof d->name, name);
    else if (!d->name[0]) snprintf(d->name, sizeof d->name, "Device %u", d->id);
    store_save_meta();
    char extra[40]; snprintf(extra, sizeof extra, ",\"id\":%u", d->id);
    reply_simple(tr, q, extra);
}

static void cmd_dev_del(tr_id_t tr, long q, const char *line)
{
    long id = 0; js_int(line, "id", &id);
    dev_meta_t *d = store_dev((uint8_t)id);
    if (!d) return reply_err(tr, q, "no such device");
    for (int i = 0; i < MAX_SIGNALS; i++) if (g_meta.sig[i].device == id) g_meta.sig[i].device = 0;
    memset(d, 0, sizeof *d);
    store_save_meta();
    reply_simple(tr, q, NULL);
}

static bool tv_key_valid(uint16_t v)
{
    unsigned code = v & KEY_CODE_MASK;
    if (v & ~(KEY_KB_PAGE | KEY_LONG | KEY_CODE_MASK)) return false;
    return code >= 1 && (!(v & KEY_KB_PAGE) || code <= 0xFF);
}

static void cmd_rule_put(tr_id_t tr, long q, const char *line)
{
    long id = 0, v, trig = 0; js_int(line, "id", &id);
    if (id && !store_rule((uint16_t)id)) return reply_err(tr, q, "no such rule");
    /* everything is checked before anything changes: a refused request leaves no trace */
    bool have_trig = js_int(line, "trig", &trig);
    if (have_trig && trig) {
        sig_meta_t *ts = store_sig((uint16_t)trig);
        if (!ts) return reply_err(tr, q, "trigger signal not found");
        if (ts->state == ST_DRAFT) return reply_err(tr, q, "name and save the draft first");
    }
    uint16_t act[MAX_ACTIONS * 2];
    int cnt = js_arr_u16(line, "act", act, MAX_ACTIONS * 2);
    if (cnt > MAX_ACTIONS * 2) cnt = MAX_ACTIONS * 2;
    for (int i = 0; i + 1 < cnt; i += 2) {
        if (act[i] > ACT_KEY) return reply_err(tr, q, "bad action type");
        if (act[i] == ACT_KEY && !tv_key_valid(act[i + 1])) return reply_err(tr, q, "bad tv key");
        if (act[i] == ACT_FIRE) {
            sig_meta_t *as = store_sig(act[i + 1]);
            if (!as) return reply_err(tr, q, "action signal not found");
            if (as->state == ST_DRAFT) return reply_err(tr, q, "name and save the draft first");
        }
    }
    rule_meta_t *r = id ? store_rule((uint16_t)id) : store_rule_alloc();
    if (!r) return reply_err(tr, q, "rule table full");
    char name[NAME_LEN];
    if (js_str(line, "name", name, sizeof name) && name[0]) copy_name(r->name, sizeof r->name, name);
    else if (!r->name[0]) snprintf(r->name, sizeof r->name, "Rule %u", r->id);
    if (js_int(line, "layer", &v)) r->layer = (uint8_t)v;
    if (js_int(line, "on", &v)) r->enabled = v ? 1 : 0;
    if (have_trig) r->trigger = (uint16_t)trig;
    if (cnt >= 0) {
        r->nact = 0;
        for (int i = 0; i + 1 < cnt; i += 2) {
            r->act[r->nact].type = (uint8_t)act[i];
            r->act[r->nact].val = act[i + 1];
            r->nact++;
        }
    }
    store_save_meta();
    char extra[40]; snprintf(extra, sizeof extra, ",\"id\":%u", r->id);
    reply_simple(tr, q, extra);
}

static void cmd_rule_del(tr_id_t tr, long q, const char *line)
{
    long id = 0; js_int(line, "id", &id);
    rule_meta_t *r = store_rule((uint16_t)id);
    if (!r) return reply_err(tr, q, "no such rule");
    memset(r, 0, sizeof *r);
    store_save_meta();
    reply_simple(tr, q, NULL);
}

static void cmd_log(tr_id_t tr, long q)
{
    static log_t logs[48];
    int n = engine_logs(logs, 48);
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"log\":[", q);
    for (int i = 0; i < n; i++) {
        out_printf(&o, "%s{\"t\":%lld,\"m\":", i ? "," : "", (long long)logs[i].t_ms);
        out_jstr(&o, logs[i].m); out_puts(&o, "}");
    }
    out_puts(&o, "]}");
    tr_end(tr, &o);
}

/* The paired TV (BLE remote). op: status (default), pair, cancel, forget. */
static void cmd_tv(tr_id_t tr, long q, const char *line)
{
    char op[12] = "status";
    js_str(line, "op", op, sizeof op);
    if (!strcmp(op, "pair")) remote_pair(true);
    else if (!strcmp(op, "cancel")) remote_pair(false);
    else if (!strcmp(op, "forget")) remote_forget();
    else if (strcmp(op, "status")) return reply_err(tr, q, "unknown op");
    bool paired, connected; int window;
    remote_status(&paired, &connected, &window);
    char extra[64]; snprintf(extra, sizeof extra, ",\"paired\":%d,\"connected\":%d,\"window\":%d", paired, connected, window);
    reply_simple(tr, q, extra);
}

/* Custom remotes. The bytes travel as base64, so the flat line parser never has to look inside them. */
#define UI_B64_MAX (4 * ((UI_CAP + 2) / 3))
static void cmd_ui(tr_id_t tr, long q, const char *cmd, const char *line)
{
    static uint8_t blob[UI_CAP];
    static char b64[UI_B64_MAX + 8];                 /* one spare group, so that too long input is seen */
    if (!strcmp(cmd, "ui_list")) {
        out_t o; tr_begin(tr, &o);
        out_printf(&o, "{\"q\":%ld,\"ok\":true,\"max\":%d,\"cap\":%d,\"slots\":[", q, UI_SLOTS, UI_CAP);
        bool first = true;
        for (int s = 1; s <= UI_SLOTS; s++) {
            size_t n = ui_size(s);
            if (!n) continue;
            out_printf(&o, "%s{\"slot\":%d,\"size\":%u}", first ? "" : ",", s, (unsigned)n);
            first = false;
        }
        out_puts(&o, "]}");
        tr_end(tr, &o);
        return;
    }
    long slot = 0;
    js_int(line, "slot", &slot);
    if (slot < 1 || slot > UI_SLOTS) return reply_err(tr, q, "bad slot");
    if (!strcmp(cmd, "ui_del")) {
        if (ui_del((int)slot) != ESP_OK) return reply_err(tr, q, "flash write failed");
        return reply_simple(tr, q, NULL);
    }
    if (!strcmp(cmd, "ui_put")) {
        if (!js_str(line, "data", b64, sizeof b64)) return reply_err(tr, q, "bad data");
        size_t len = strlen(b64), n = 0;
        if (len > UI_B64_MAX) return reply_err(tr, q, "too big");      /* js_str cuts long input: never store that */
        int rc = mbedtls_base64_decode(blob, sizeof blob, &n, (const unsigned char *)b64, len);
        if (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return reply_err(tr, q, "too big");
        if (rc || !n || len % 4) return reply_err(tr, q, "bad data");
        if (ui_put((int)slot, blob, n) != ESP_OK) return reply_err(tr, q, "flash write failed");
        return reply_simple(tr, q, NULL);
    }
    size_t n = 0, olen = 0;
    if (!ui_size((int)slot) || ui_get((int)slot, blob, sizeof blob, &n) != ESP_OK) return reply_err(tr, q, "no such remote");
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &olen, blob, n)) return reply_err(tr, q, "no data");
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"slot\":%ld,\"data\":\"", q, slot);
    out_puts(&o, b64);
    out_puts(&o, "\"}");
    tr_end(tr, &o);
}

/* The access header (secure.c). Without "h": read it. With "h": set it, "" clears. Only UART and an accepted
 * Bluetooth link get here. */
static void cmd_access(tr_id_t tr, long q, const char *line)
{
    char h[SECURE_HEADER_MAX + 8], cur[SECURE_HEADER_MAX + 1];
    bool set = js_str(line, "h", h, sizeof h);
    if (set) {
        const char *err = secure_set(h, tr == TR_BLE);
        if (err) return reply_err(tr, q, err);
    }
    secure_get(cur, sizeof cur);
    out_t o; tr_begin(tr, &o);
    out_printf(&o, "{\"q\":%ld,\"ok\":true,\"set\":%d,\"h\":", q, cur[0] != 0);
    out_jstr(&o, cur);
    out_puts(&o, "}");
    tr_end(tr, &o);
}

static void handle(const char *line, tr_id_t tr)
{
    long q = 0, v;
    js_int(line, "q", &q);
    char cmd[16] = "";
    if (!js_str(line, "cmd", cmd, sizeof cmd)) return reply_err(tr, q, "missing cmd");

    if (!strcmp(cmd, "ping")) reply_simple(tr, q, NULL);
    else if (!strcmp(cmd, "hello")) reply_simple(tr, q, ",\"enc\":0");    /* an encrypted link never gets this far unproven */
    else if (!strcmp(cmd, "access")) cmd_access(tr, q, line);
    else if (!strcmp(cmd, "info")) cmd_info(tr, q);
    else if (!strcmp(cmd, "list")) cmd_list(tr, q);
    else if (!strcmp(cmd, "get")) cmd_get(tr, q, line);
    else if (!strcmp(cmd, "frame")) cmd_frame(tr, q, line);
    else if (!strcmp(cmd, "sig_put")) cmd_sig_put(tr, q, line);
    else if (!strcmp(cmd, "sig_del")) cmd_sig_del(tr, q, line);
    else if (!strcmp(cmd, "dev_put")) cmd_dev_put(tr, q, line);
    else if (!strcmp(cmd, "dev_del")) cmd_dev_del(tr, q, line);
    else if (!strcmp(cmd, "rule_put")) cmd_rule_put(tr, q, line);
    else if (!strcmp(cmd, "rule_del")) cmd_rule_del(tr, q, line);
    else if (!strcmp(cmd, "log")) cmd_log(tr, q);
    else if (!strcmp(cmd, "fire")) {
        js_int(line, "id", &v);
        sig_meta_t *fs = store_sig((uint16_t)v);
        if (!fs) return reply_err(tr, q, "no such signal");
        if (fs->state != ST_ACTIVE) return reply_err(tr, q, fs->state == ST_DRAFT ? "name and save the draft first" : "signal is disabled");
        engine_enqueue(JOB_SIGNAL, (uint16_t)v) ? reply_simple(tr, q, NULL) : reply_err(tr, q, "busy");
    } else if (!strcmp(cmd, "run")) {
        js_int(line, "id", &v);
        if (!store_rule((uint16_t)v)) return reply_err(tr, q, "no such rule");
        engine_enqueue(JOB_RULE, (uint16_t)v) ? reply_simple(tr, q, NULL) : reply_err(tr, q, "busy");
    } else if (!strcmp(cmd, "play")) {
        long khz = 38; js_int(line, "khz", &khz);
        int n = js_arr_u16(line, "raw", s_raw, MAX_RAW);
        if (n < 2) return reply_err(tr, q, "raw required");
        engine_enqueue_raw(s_raw, n, (uint16_t)khz) ? reply_simple(tr, q, NULL) : reply_err(tr, q, "busy");
    } else if (!strcmp(cmd, "ui_list") || !strcmp(cmd, "ui_get") || !strcmp(cmd, "ui_put") || !strcmp(cmd, "ui_del")) {
        cmd_ui(tr, q, cmd, line);
    } else if (!strcmp(cmd, "tv")) {
        cmd_tv(tr, q, line);
    } else if (!strcmp(cmd, "key")) {
        /* {k} sends a named key, {page,code} any code; ms is how long it is held down */
        char k[12] = "", page[4] = "";
        long code = 0, ms = REMOTE_KEY_MS;
        js_int(line, "ms", &ms);
        const char *err;
        if (js_str(line, "k", k, sizeof k)) err = remote_key(k, (int)ms);
        else {
            js_str(line, "page", page, sizeof page);
            js_int(line, "code", &code);
            err = remote_raw(page, code, (int)ms);
        }
        err ? reply_err(tr, q, err) : reply_simple(tr, q, NULL);
    } else if (!strcmp(cmd, "ledtest")) {
        v = 1; js_int(line, "on", &v);
        engine_ledtest(v) ? reply_simple(tr, q, NULL) : reply_err(tr, q, "busy");
    } else if (!strcmp(cmd, "inject")) {
        /* test helper: treat the given timings as if the receiver had just heard them */
        int n = js_arr_u16(line, "raw", s_raw, MAX_RAW);
        if (n < 4) return reply_err(tr, q, "raw required");
        reply_simple(tr, q, NULL);
        engine_on_frame(s_raw, n, esp_timer_get_time());
    } else if (!strcmp(cmd, "record") || !strcmp(cmd, "learn")) {
        v = 1; js_int(line, "on", &v);
        engine_record(v);
        ilog(v ? "recording on" : "recording off");
        reply_simple(tr, q, NULL);
    } else if (!strcmp(cmd, "layer")) {
        if (js_int(line, "n", &v)) { engine_set_layer((uint8_t)v); ilog("layer %ld", v); }
        char extra[24]; snprintf(extra, sizeof extra, ",\"layer\":%u", engine_layer());
        reply_simple(tr, q, extra);
    } else if (!strcmp(cmd, "set")) {
        char name[24];
        if (js_str(line, "name", name, sizeof name) && name[0]) copy_name(g_meta.device_name, sizeof g_meta.device_name, name);
        if (js_int(line, "dedupe", &v) && v >= 0 && v <= 5000) g_meta.dedupe_ms = (uint16_t)v;
        store_save_meta();
        reply_simple(tr, q, NULL);
    } else if (!strcmp(cmd, "reset")) {
        if (!js_int(line, "confirm", &v) || !v) return reply_err(tr, q, "confirm required");
        remote_forget();
        store_factory_reset();
        ilog("factory reset");
        reply_simple(tr, q, NULL);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (!strcmp(cmd, "reboot")) {
        reply_simple(tr, q, NULL);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else reply_err(tr, q, "unknown cmd");
}

static void worker(void *p)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        handle(m.line, (tr_id_t)m.tr);
        free(m.line);
    }
}

void proto_feed(tr_id_t tr, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (s_len[tr]) {
                s_buf[tr][s_len[tr]] = 0;
                msg_t m = { .line = strdup(s_buf[tr]), .tr = tr };
                if (!m.line || xQueueSend(s_q, &m, 0) != pdTRUE) free(m.line);
            }
            s_len[tr] = 0;
            continue;
        }
        if (s_len[tr] < LINE_MAX_LEN - 1) s_buf[tr][s_len[tr]++] = (char)c;
        else s_len[tr] = 0;                 /* overlong line: drop it */
    }
}

void proto_reset(tr_id_t tr) { s_len[tr] = 0; }

void proto_init(void)
{
    for (int i = 0; i < TR_COUNT; i++) s_buf[i] = malloc(LINE_MAX_LEN);
    s_q = xQueueCreate(4, sizeof(msg_t));
    xTaskCreate(worker, "proto", 8192, NULL, 4, NULL);
}
