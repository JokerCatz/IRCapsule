#include "ui_store.h"
#include <stdio.h>
#include "nvs.h"

/* Same namespace as the signals, so a factory reset erases the remotes too. */
static esp_err_t open_ns(nvs_handle_t *h) { return nvs_open("ircap", NVS_READWRITE, h); }
static void key_of(char *k, size_t cap, int slot) { snprintf(k, cap, "ui%d", slot); }

size_t ui_size(int slot)
{
    nvs_handle_t h; char k[8]; size_t n = 0;
    if (open_ns(&h) != ESP_OK) return 0;
    key_of(k, sizeof k, slot);
    if (nvs_get_blob(h, k, NULL, &n) != ESP_OK) n = 0;
    nvs_close(h);
    return n;
}

esp_err_t ui_get(int slot, uint8_t *out, size_t cap, size_t *n)
{
    nvs_handle_t h; char k[8];
    esp_err_t e = open_ns(&h);
    if (e != ESP_OK) return e;
    key_of(k, sizeof k, slot);
    *n = cap;
    e = nvs_get_blob(h, k, out, n);
    nvs_close(h);
    return e;
}

esp_err_t ui_put(int slot, const uint8_t *data, size_t n)
{
    nvs_handle_t h; char k[8];
    esp_err_t e = open_ns(&h);
    if (e != ESP_OK) return e;
    key_of(k, sizeof k, slot);
    e = nvs_set_blob(h, k, data, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t ui_del(int slot)
{
    nvs_handle_t h; char k[8];
    esp_err_t e = open_ns(&h);
    if (e != ESP_OK) return e;
    key_of(k, sizeof k, slot);
    e = nvs_erase_key(h, k);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
