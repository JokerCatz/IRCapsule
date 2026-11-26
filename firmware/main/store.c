#include "store.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define MAGIC 0x49524332u   /* "IRC2" */

meta_t g_meta;
static nvs_handle_t s_h;

static void defaults(void)
{
    memset(&g_meta, 0, sizeof g_meta);
    g_meta.magic = MAGIC;
    g_meta.next_id = 1;
    g_meta.dedupe_ms = 400;
}

esp_err_t store_init(void)
{
    esp_err_t e = nvs_open("ircap", NVS_READWRITE, &s_h);
    if (e != ESP_OK) return e;
    size_t sz = sizeof g_meta;
    e = nvs_get_blob(s_h, "meta", &g_meta, &sz);
    if (e != ESP_OK || sz != sizeof g_meta || g_meta.magic != MAGIC) {
        defaults();
        return store_save_meta();
    }
    return ESP_OK;
}

esp_err_t store_save_meta(void)
{
    esp_err_t e = nvs_set_blob(s_h, "meta", &g_meta, sizeof g_meta);
    if (e == ESP_OK) e = nvs_commit(s_h);
    return e;
}

esp_err_t store_factory_reset(void)
{
    esp_err_t e = nvs_erase_all(s_h);
    if (e == ESP_OK) e = nvs_commit(s_h);
    defaults();
    if (e == ESP_OK) e = store_save_meta();
    return e;
}

uint16_t store_next_id(void)
{
    uint16_t id = g_meta.next_id++;
    if (g_meta.next_id == 0) g_meta.next_id = 1;
    return id;
}

sig_meta_t *store_sig(uint16_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < MAX_SIGNALS; i++) if (g_meta.sig[i].id == id) return &g_meta.sig[i];
    return NULL;
}

sig_meta_t *store_sig_alloc(void)
{
    for (int i = 0; i < MAX_SIGNALS; i++) if (!g_meta.sig[i].id) {
        memset(&g_meta.sig[i], 0, sizeof g_meta.sig[i]);
        g_meta.sig[i].id = store_next_id();
        return &g_meta.sig[i];
    }
    return NULL;
}

static void raw_key(char *k, uint16_t id) { snprintf(k, 12, "s%u", id); }

esp_err_t store_sig_write_raw(uint16_t id, const uint16_t *raw, size_t n)
{
    char k[12]; raw_key(k, id);
    esp_err_t e = nvs_set_blob(s_h, k, raw, n * sizeof(uint16_t));
    if (e == ESP_OK) e = nvs_commit(s_h);
    return e;
}

esp_err_t store_sig_read_raw(uint16_t id, uint16_t *raw, size_t max, size_t *n)
{
    char k[12]; raw_key(k, id);
    size_t sz = max * sizeof(uint16_t);
    esp_err_t e = nvs_get_blob(s_h, k, raw, &sz);
    if (e == ESP_OK) *n = sz / sizeof(uint16_t);
    return e;
}

esp_err_t store_sig_delete(uint16_t id)
{
    sig_meta_t *s = store_sig(id);
    if (!s) return ESP_ERR_NOT_FOUND;
    char k[12]; raw_key(k, id);
    nvs_erase_key(s_h, k);
    /* rules that used this signal lose the reference */
    for (int i = 0; i < MAX_RULES; i++) {
        rule_meta_t *r = &g_meta.rule[i];
        if (!r->id) continue;
        if (r->trigger == id) r->trigger = 0;
        uint8_t w = 0;
        for (uint8_t a = 0; a < r->nact; a++)
            if (!(r->act[a].type == ACT_FIRE && r->act[a].val == id)) r->act[w++] = r->act[a];
        r->nact = w;
    }
    memset(s, 0, sizeof *s);
    return store_save_meta();
}

rule_meta_t *store_rule(uint16_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < MAX_RULES; i++) if (g_meta.rule[i].id == id) return &g_meta.rule[i];
    return NULL;
}

rule_meta_t *store_rule_alloc(void)
{
    for (int i = 0; i < MAX_RULES; i++) if (!g_meta.rule[i].id) {
        memset(&g_meta.rule[i], 0, sizeof g_meta.rule[i]);
        g_meta.rule[i].id = store_next_id();
        g_meta.rule[i].enabled = 1;
        return &g_meta.rule[i];
    }
    return NULL;
}

dev_meta_t *store_dev(uint8_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < MAX_DEVICES; i++) if (g_meta.dev[i].id == id) return &g_meta.dev[i];
    return NULL;
}

dev_meta_t *store_dev_alloc(void)
{
    for (uint8_t id = 1; id < 255; id++) {
        if (store_dev(id)) continue;
        for (int i = 0; i < MAX_DEVICES; i++) if (!g_meta.dev[i].id) {
            memset(&g_meta.dev[i], 0, sizeof g_meta.dev[i]);
            g_meta.dev[i].id = id;
            return &g_meta.dev[i];
        }
        return NULL;
    }
    return NULL;
}

uint8_t store_get_layer(void)
{
    uint8_t v = 0;
    nvs_get_u8(s_h, "layer", &v);
    return v;
}

void store_set_layer(uint8_t layer)
{
    nvs_set_u8(s_h, "layer", layer);
    nvs_commit(s_h);
}
