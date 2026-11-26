#include "ir_rmt.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define RMT_RES_HZ     1000000
#define RX_MARK_LEVEL  0           /* the VS1838B output is low while it sees infrared */
#define RX_SYMBOLS     640         /* 1280 mark/space entries, enough for long AC frames */
#define NEC_TOL(x, ref) ((x) > (ref) * 7 / 10 && (x) < (ref) * 13 / 10)

static rmt_channel_handle_t s_tx, s_rx;
static rmt_encoder_handle_t s_copy;
static QueueHandle_t s_rxq;
static rmt_symbol_word_t *s_rxbuf;
static size_t s_rx_cap;
static volatile bool s_armed;
static volatile int64_t s_last_rx_us;
static volatile int64_t s_tx_block_until;

typedef struct { size_t n; } rx_evt_t;
#define RX_IDLE_NS   30000000   /* frame ends after 30 ms of silence (RMT max is ~32.7 ms at 1 MHz) */
#define RX_GLITCH_NS 3000       /* hardware glitch filter maximum is ~3187 ns */

static bool IRAM_ATTR rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *d, void *ud)
{
    BaseType_t woken = pdFALSE;
    rx_evt_t e = { .n = d->num_symbols };
    s_last_rx_us = esp_timer_get_time();
    xQueueSendFromISR(s_rxq, &e, &woken);
    return woken == pdTRUE;
}

esp_err_t ir_init(int tx_gpio, int rx_gpio)
{
    rmt_tx_channel_config_t tc = {
        .gpio_num = tx_gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 48, .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tc, &s_tx));
    rmt_copy_encoder_config_t cc = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&cc, &s_copy));

    rmt_rx_channel_config_t rc = {
        .gpio_num = rx_gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 48,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rc, &s_rx));
    s_rx_cap = RX_SYMBOLS;
    s_rxbuf = malloc(s_rx_cap * sizeof(rmt_symbol_word_t));
    s_rxq = xQueueCreate(4, sizeof(rx_evt_t));
    rmt_rx_event_callbacks_t cb = { .on_recv_done = rx_done };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx, &cb, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_tx));
    ESP_ERROR_CHECK(rmt_enable(s_rx));
    return ESP_OK;
}

int64_t ir_tx_block_until_us(void) { return s_tx_block_until; }

/* RMT durations are 15 bits (max 32767 us). Longer marks/spaces are split into same-level pieces. */
#define RMT_MAX_PIECE 32000

esp_err_t ir_send_raw(const uint16_t *us, size_t n, uint32_t carrier_khz)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    size_t cap = n * 3 + 2, np = 0;
    uint16_t *dur = malloc(cap * sizeof(uint16_t));
    uint8_t *lvl = malloc(cap);
    if (!dur || !lvl) { free(dur); free(lvl); return ESP_ERR_NO_MEM; }
    int64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t left = us[i];
        total += left;
        do {
            uint32_t piece = left > RMT_MAX_PIECE ? RMT_MAX_PIECE : left;
            if (piece == 0) break;
            dur[np] = (uint16_t)piece; lvl[np] = (i % 2 == 0); np++;
            left -= piece;
        } while (left);
    }
    size_t ns = (np + 1) / 2;
    rmt_symbol_word_t *sym = calloc(ns, sizeof(*sym));
    if (!sym) { free(dur); free(lvl); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < ns; i++) {
        sym[i].level0 = lvl[2 * i]; sym[i].duration0 = dur[2 * i];
        if (2 * i + 1 < np) { sym[i].level1 = lvl[2 * i + 1]; sym[i].duration1 = dur[2 * i + 1]; }
        else { sym[i].level1 = 0; sym[i].duration1 = 0; }
    }
    free(dur); free(lvl);
    /* Ignore our own light: block RX from now until the frame ends plus a guard time. */
    s_tx_block_until = esp_timer_get_time() + total + IR_RX_GUARD_US;
    rmt_carrier_config_t car = { .frequency_hz = carrier_khz * 1000, .duty_cycle = 0.33f };
    esp_err_t err = rmt_apply_carrier(s_tx, carrier_khz ? &car : NULL);
    if (err == ESP_OK) {
        rmt_transmit_config_t t = { .loop_count = 0 };
        err = rmt_transmit(s_tx, s_copy, sym, ns * sizeof(*sym), &t);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(s_tx, 2000);
    }
    s_tx_block_until = esp_timer_get_time() + IR_RX_GUARD_US;
    free(sym);
    return err;
}

int64_t ir_last_rx_time_us(void) { return s_last_rx_us; }

esp_err_t ir_receive(uint16_t *us, size_t max, size_t *n, uint32_t timeout_ms)
{
    rmt_receive_config_t rcfg = { .signal_range_min_ns = RX_GLITCH_NS, .signal_range_max_ns = RX_IDLE_NS };
    if (!s_armed) {
        xQueueReset(s_rxq);
        esp_err_t err = rmt_receive(s_rx, s_rxbuf, s_rx_cap * sizeof(rmt_symbol_word_t), &rcfg);
        if (err != ESP_OK) return err;
        s_armed = true;
    }
    rx_evt_t e;
    /* On timeout stay armed: do not cut a frame that may be arriving. */
    if (xQueueReceive(s_rxq, &e, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_armed = false;
    size_t k = 0;
    bool started = false;
    for (size_t i = 0; i < e.n; i++) {
        int lv[2] = { s_rxbuf[i].level0, s_rxbuf[i].level1 };
        uint16_t du[2] = { s_rxbuf[i].duration0, s_rxbuf[i].duration1 };
        for (int j = 0; j < 2; j++) {
            if (du[j] == 0) continue;
            if (!started) { if (lv[j] != RX_MARK_LEVEL) continue; started = true; }
            if (k < max) us[k++] = du[j];
        }
    }
    *n = k;
    return ESP_OK;
}

static bool near(uint32_t v, uint32_t ref) { return v > ref * 7 / 10 && v < ref * 13 / 10; }

void ir_describe(const uint16_t *us, size_t n, char *out, size_t outlen)
{
    uint8_t a, c;
    if (n >= 66 && ir_decode_nec(us, n, &a, &c)) { snprintf(out, outlen, "NEC a=%02X c=%02X", a, c); return; }
    if (n >= 2 && n <= 4 && near(us[0], 9000) && near(us[1], 2250)) { snprintf(out, outlen, "NEC-REPEAT"); return; }
    /* Sony SIRC: 2400 header, marks 600 (0) / 1200 (1), spaces 600; 12/15/20 bits */
    if (n >= 25 && near(us[0], 2400) && near(us[1], 600)) {
        size_t bits = (n - 2 + 1) / 2;
        if (bits == 12 || bits == 15 || bits == 20) {
            uint32_t v = 0; bool ok = true;
            for (size_t i = 0; i < bits; i++) {
                uint16_t m = us[2 + 2 * i];
                if (near(m, 1200)) v |= 1u << i; else if (!near(m, 600)) ok = false;
            }
            if (ok) { snprintf(out, outlen, "SONY%u 0x%X", (unsigned)bits, (unsigned)v); return; }
        }
    }
    /* Samsung: 4500/4500 header, 32 bits, mark 560, space 1690/560 */
    if (n >= 66 && near(us[0], 4500) && near(us[1], 4500)) {
        uint32_t v = 0; bool ok = true;
        for (int i = 0; i < 32; i++) {
            if (!near(us[2 + 2 * i], 560)) { ok = false; break; }
            uint16_t s = us[3 + 2 * i];
            if (near(s, 1690)) v |= 1u << i; else if (!near(s, 560)) { ok = false; break; }
        }
        if (ok) { snprintf(out, outlen, "SAMSUNG32 0x%08X", (unsigned)v); return; }
    }
    snprintf(out, outlen, n >= 100 ? "RAW-LONG(AC?) n=%u" : "RAW n=%u", (unsigned)n);
}

bool ir_decode_nec(const uint16_t *us, size_t n, uint8_t *addr, uint8_t *cmd)
{
    if (n < 66 || !NEC_TOL(us[0], 9000) || !NEC_TOL(us[1], 4500)) return false;
    uint32_t w = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t mark = us[2 + 2 * i], space = us[3 + 2 * i];
        if (!NEC_TOL(mark, 560)) return false;
        if (NEC_TOL(space, 1690)) w |= 1u << i;
        else if (!NEC_TOL(space, 560)) return false;
    }
    uint8_t a = w & 0xff, na = (w >> 8) & 0xff, c = (w >> 16) & 0xff, nc = (w >> 24) & 0xff;
    if ((uint8_t)~a != na || (uint8_t)~c != nc) return false;
    *addr = a; *cmd = c;
    return true;
}
