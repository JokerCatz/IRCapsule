#include "engine.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ir_rmt.h"
#include "remote.h"
#include "transport.h"

#define RING 6
#define NEC_QUIET_US 150000
#define LOGN 48

static frame_t s_ring[RING];
static uint32_t s_seq;
static int64_t s_prev_us;
static SemaphoreHandle_t s_lock;
static log_t s_log[LOGN];
static uint32_t s_logseq;
static QueueHandle_t s_jobq;
static volatile int64_t s_learn_until_us;
static uint8_t s_layer;
static int64_t s_last_match_us[MAX_RULES];   /* last frame that matched the rule's trigger */
static volatile int64_t s_ledtest_until_us;
static volatile bool s_ledtest_job;   /* a blink job is queued or running */
static SemaphoreHandle_t s_frame_lock;   /* engine_on_frame uses static buffers; real and injected frames take turns */

typedef struct { uint8_t type; uint16_t id; uint16_t khz; uint16_t n; uint16_t *raw; } job_t;

/* ---------- log ---------- */
static void esc(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c < 0x20) n += snprintf(dst + n, cap - n, "\\u%04x", c);
        else dst[n++] = c;
    }
    dst[n] = 0;
}

void ilog(const char *fmt, ...)
{
    char m[96];
    va_list ap; va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    int64_t t = esp_timer_get_time() / 1000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_t *l = &s_log[s_logseq % LOGN];
    l->seq = ++s_logseq; l->t_ms = t;
    memcpy(l->m, m, sizeof l->m);
    xSemaphoreGive(s_lock);
    char e[260], q[200];
    esc(q, sizeof q, m);
    snprintf(e, sizeof e, "{\"ev\":\"log\",\"t\":%lld,\"m\":\"%s\"}", (long long)t, q);
    ev_send(e);
}

int engine_logs(log_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int have = s_logseq < LOGN ? (int)s_logseq : LOGN, n = 0;
    for (int i = have; i > 0 && n < max; i--) out[n++] = s_log[(s_logseq - i) % LOGN];
    xSemaphoreGive(s_lock);
    return n;
}

/* ---------- normalise / match ---------- */
static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

void engine_normalise(uint16_t *r, size_t n)
{
    uint16_t head[20]; size_t nh = 0, total = 0;
    for (size_t k = 2; k + 1 < n; k += 2) if (r[k] < 1500) { total++; if (nh < 20) head[nh++] = r[k]; }
    if (total < 24) return;
    qsort(head, nh, sizeof head[0], cmp_u16);
    uint32_t canon = (head[nh / 2 - 1] + head[nh / 2]) / 2;
    size_t tailn = total / 4 > 8 ? total / 4 : 8, cnt = 0;
    uint32_t sum = 0;
    for (size_t k = ((n - 1) / 2) * 2; k >= 2 && cnt < tailn; k -= 2)
        if (k < n && r[k] < 1500) { sum += r[k]; cnt++; }
    if (!cnt || sum / cnt > canon * 85 / 100) return;
    for (size_t k = 2; k + 1 < n; k += 2) {
        if (r[k] >= 1500) continue;
        int32_t s = (int32_t)r[k + 1] + (int32_t)r[k] - (int32_t)canon;
        r[k] = (uint16_t)canon;
        r[k + 1] = (uint16_t)(s < 100 ? 100 : (s > 65535 ? 65535 : s));
    }
    if (n % 2 && r[n - 1] < 1500) r[n - 1] = (uint16_t)canon;
}

static bool raw_match(const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
    size_t L = na < nb ? na : nb;
    if (L < 4 || (na != nb && L < 20)) return false;
    for (size_t i = 0; i < L; i++) {
        int d = (int)a[i] - (int)b[i]; if (d < 0) d = -d;
        int tol = b[i] / 4 > 150 ? b[i] / 4 : 150;
        if (d > tol) return false;
    }
    return true;
}

/* ---------- ring ---------- */
static void ring_add(const uint16_t *raw, size_t n, int64_t t_us, const char *proto, uint32_t *seq_out, int32_t *gap_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    frame_t *f = &s_ring[s_seq % RING];
    free(f->raw);
    f->seq = ++s_seq;
    f->t_ms = t_us / 1000;
    f->gap_ms = s_prev_us ? (int32_t)((t_us - s_prev_us) / 1000) : -1;
    f->n = n;
    snprintf(f->proto, sizeof f->proto, "%s", proto);
    f->match = 0;
    f->raw = malloc(n * sizeof(uint16_t));
    if (f->raw) memcpy(f->raw, raw, n * sizeof(uint16_t)); else f->n = 0;
    s_prev_us = t_us;
    *seq_out = f->seq;
    *gap_out = f->gap_ms;
    xSemaphoreGive(s_lock);
}

bool engine_frame_copy(uint32_t seq, frame_t *out, uint16_t *rawbuf, size_t max)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!seq) seq = s_seq;
    for (int i = 0; i < RING && seq; i++) {
        frame_t *f = &s_ring[i];
        if (f->seq == seq && f->raw) {
            *out = *f;
            size_t n = f->n < max ? f->n : max;
            memcpy(rawbuf, f->raw, n * sizeof(uint16_t));
            out->raw = rawbuf; out->n = n;
            ok = true; break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* ---------- frames heard ---------- */
#define RECORD_IDLE_US 300000000LL   /* a recording session ends after 5 minutes without a frame */
void engine_record(bool on)
{
    bool was = engine_recording();
    s_learn_until_us = on ? esp_timer_get_time() + RECORD_IDLE_US : 0;
    if (was && !on) store_save_meta();   /* keep the press counters */
}
bool engine_recording(void) { return s_learn_until_us > esp_timer_get_time(); }
uint8_t engine_layer(void) { return s_layer; }
void engine_set_layer(uint8_t l) { s_layer = l; store_set_layer(l); }

void engine_on_frame(const uint16_t *raw_in, size_t n, int64_t t_us)
{
    static uint16_t raw[MAX_RAW], tbuf[MAX_RAW];
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    if (n > MAX_RAW) n = MAX_RAW;
    memcpy(raw, raw_in, n * sizeof(uint16_t));
    engine_normalise(raw, n);
    char proto[24];
    ir_describe(raw, n, proto, sizeof proto);
    uint32_t seq;
    int32_t gap;
    ring_add(raw, n, t_us, proto, &seq, &gap);

    /* Is this a signal we already have (draft, active or disabled)? */
    uint16_t sig_hit = 0, draft = 0;
    if (n >= 12) {
        for (int i = 0; i < MAX_SIGNALS && !sig_hit; i++) {
            sig_meta_t *sg = &g_meta.sig[i];
            size_t tn;
            if (!sg->id || !sg->n) continue;
            if (store_sig_read_raw(sg->id, tbuf, MAX_RAW, &tn) == ESP_OK && raw_match(raw, n, tbuf, tn)) sig_hit = sg->id;
        }
    }

    uint16_t matched = 0;
    if (engine_recording()) {
        s_learn_until_us = t_us + RECORD_IDLE_US;
        if (n >= 12) {
            sig_meta_t *hit = store_sig(sig_hit);
            if (hit && hit->state == ST_DRAFT) { if (hit->hits < 255) hit->hits++; draft = hit->id; }
            else if (!hit) {
                int drafts = 0;
                for (int i = 0; i < MAX_SIGNALS; i++) drafts += g_meta.sig[i].id && g_meta.sig[i].state == ST_DRAFT;
                sig_meta_t *d = drafts < MAX_DRAFTS ? store_sig_alloc() : NULL;
                if (!d) ilog("draft limit reached (%d)", MAX_DRAFTS);
                else {
                    d->state = ST_DRAFT; d->hits = 1; d->carrier_khz = 38; d->n = (uint16_t)n;
                    snprintf(d->proto, sizeof d->proto, "%s", proto);
                    if (store_sig_write_raw(d->id, raw, n) == ESP_OK && store_save_meta() == ESP_OK) {
                        draft = sig_hit = d->id;
                        ilog("recorded draft #%u (%s)", d->id, proto);
                    } else { ilog("flash write failed"); memset(d, 0, sizeof *d); }
                }
            }
        }
    } else if (n >= 4 && !s_ledtest_job) {   /* a rule queued now would only run after the LED test */
        for (int i = 0; i < MAX_RULES && !matched; i++) {
            rule_meta_t *r = &g_meta.rule[i];
            sig_meta_t *trig = store_sig(r->trigger);
            if (!r->id || !r->enabled || !r->nact || r->layer != s_layer || !trig || trig->state != ST_ACTIVE) continue;
            size_t tn;
            if (store_sig_read_raw(r->trigger, tbuf, MAX_RAW, &tn) != ESP_OK) continue;
            if (!raw_match(raw, n, tbuf, tn)) continue;
            matched = r->id;
            /* One press, one run: a new press needs a quiet gap since the last matching frame, so holding never
             * re-triggers. A held NEC button normally sends short repeat frames, which never get here, so NEC
             * only needs a gap just over its 108 ms frame period (for remotes that resend the whole frame). */
            int64_t gap = t_us - s_last_match_us[i];
            s_last_match_us[i] = t_us;
            int64_t quiet = strncmp(proto, "NEC ", 4) ? (int64_t)g_meta.dedupe_ms * 1000 : NEC_QUIET_US;
            if (gap < quiet) break;
            ilog("trigger \"%s\" -> rule \"%s\"", trig->name, r->name);
            engine_enqueue(JOB_RULE, r->id);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RING; i++) if (s_ring[i].seq == seq) s_ring[i].match = matched;
    xSemaphoreGive(s_lock);

    char head[200];
    snprintf(head, sizeof head, "{\"ev\":\"frame\",\"seq\":%u,\"t\":%lld,\"gap\":%d,\"n\":%u,\"p\":\"%s\",\"m\":%u,\"sig\":%u,\"draft\":%u,\"rec\":%d",
             (unsigned)seq, (long long)(t_us / 1000), (int)gap, (unsigned)n, proto, matched, sig_hit, draft, engine_recording());
    char e[260];
    snprintf(e, sizeof e, "%s}", head);
    ev_send(e);
    /* USB gets the raw timings too, for the capture tool */
    size_t cap = n * 6 + 300;
    char *big = tr_ready(TR_USB) ? malloc(cap) : NULL;
    if (big) {
        size_t k = snprintf(big, cap, "%s,\"raw\":[", head);
        for (size_t i = 0; i < n && k + 8 < cap; i++) k += snprintf(big + k, cap - k, i ? ",%u" : "%u", raw[i]);
        snprintf(big + k, cap - k, "]}");
        ev_send_usb(big);
        free(big);
    }
    xSemaphoreGive(s_frame_lock);
}

/* ---------- executor ---------- */
bool engine_enqueue(int type, uint16_t id)
{
    job_t j = { .type = type, .id = id };
    return xQueueSend(s_jobq, &j, 0) == pdTRUE;
}

bool engine_enqueue_raw(const uint16_t *raw, size_t n, uint16_t khz)
{
    job_t j = { .type = JOB_RAW, .khz = khz, .n = n, .raw = malloc(n * sizeof(uint16_t)) };
    if (!j.raw) return false;
    memcpy(j.raw, raw, n * sizeof(uint16_t));
    if (xQueueSend(s_jobq, &j, 0) != pdTRUE) { free(j.raw); return false; }
    return true;
}

static void fire_signal(uint16_t id, uint16_t *buf)
{
    sig_meta_t *s = store_sig(id);
    size_t n;
    if (!s) { ilog("fire: signal %u not found", id); return; }
    if (s->state != ST_ACTIVE) { ilog("fire: \"%s\" is %s", s->name[0] ? s->name : "draft", s->state == ST_DRAFT ? "a draft" : "disabled"); return; }
    if (store_sig_read_raw(id, buf, MAX_RAW, &n) != ESP_OK) { ilog("fire: no data for \"%s\"", s->name); return; }
    esp_err_t e = ir_send_raw(buf, n, s->carrier_khz ? s->carrier_khz : 38);
    ilog("sent \"%s\" (%u entries) %s", s->name, (unsigned)n, e == ESP_OK ? "ok" : "ERROR");
}

#define LEDTEST_US       60000000LL
#define LEDTEST_HALF_MS  500

bool engine_ledtest(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ledtest_until_us = on ? esp_timer_get_time() + LEDTEST_US : 0;
    bool start = on && !s_ledtest_job;
    if (start) s_ledtest_job = true;
    xSemaphoreGive(s_lock);
    if (start && !engine_enqueue(JOB_LEDTEST, 0)) { s_ledtest_job = false; return false; }
    return true;
}

/* Blink all emitters so they can be checked with a camera. No carrier: brighter on camera. */
static void run_ledtest(void)
{
    /* ten 50 ms marks; zero-length spaces are skipped by ir_send_raw, so they join into 500 ms */
    static const uint16_t pulse[20] = { 50000, 0, 50000, 0, 50000, 0, 50000, 0, 50000, 0,
                                        50000, 0, 50000, 0, 50000, 0, 50000, 0, 50000, 0 };
    ilog("led test on");
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool more = esp_timer_get_time() < s_ledtest_until_us;
        if (!more) s_ledtest_job = false;   /* same lock as engine_ledtest: no lost restart */
        xSemaphoreGive(s_lock);
        if (!more) break;
        if (ir_send_raw(pulse, 20, 0) != ESP_OK) { ilog("led test: send ERROR"); s_ledtest_until_us = 0; }
        vTaskDelay(pdMS_TO_TICKS(LEDTEST_HALF_MS));
    }
    ilog("led test off");
}

static void exec_task(void *p)
{
    static uint16_t buf[MAX_RAW];
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobq, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.type == JOB_SIGNAL) fire_signal(j.id, buf);
        else if (j.type == JOB_LEDTEST) run_ledtest();
        else if (j.type == JOB_RAW) {
            esp_err_t e = ir_send_raw(j.raw, j.n, j.khz ? j.khz : 38);
            ilog("sent raw (%u entries) %s", j.n, e == ESP_OK ? "ok" : "ERROR");
            free(j.raw);
        } else if (j.type == JOB_RULE) {
            rule_meta_t *r = store_rule(j.id);
            if (!r) { ilog("run: rule %u not found", j.id); continue; }
            for (uint8_t a = 0; a < r->nact; a++) {
                action_t ac = r->act[a];
                if (ac.type == ACT_FIRE) fire_signal(ac.val, buf);
                else if (ac.type == ACT_DELAY_MS) vTaskDelay(pdMS_TO_TICKS(ac.val));
                else if (ac.type == ACT_LAYER) { engine_set_layer((uint8_t)ac.val); ilog("layer %u", ac.val); }
                else if (ac.type == ACT_KEY) {
                    const char *err = remote_raw(ac.val & KEY_KB_PAGE ? "kb" : "cc", ac.val & KEY_CODE_MASK,
                                                 ac.val & KEY_LONG ? 1000 : REMOTE_KEY_MS);
                    if (err) ilog("rule: tv key 0x%X skipped (%s)", ac.val, err);
                }
            }
        }
    }
}

void engine_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_frame_lock = xSemaphoreCreateMutex();
    s_jobq = xQueueCreate(8, sizeof(job_t));
    s_layer = store_get_layer();
    xTaskCreate(exec_task, "exec", 6144, NULL, 4, NULL);
}
