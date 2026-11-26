/* Custom web remotes: up to UI_SLOTS opaque blobs. The web page owns their format. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define UI_SLOTS 8
#define UI_CAP   4096

/* slot is 1..UI_SLOTS; the caller checks it */
size_t    ui_size(int slot);                                     /* 0 = empty */
esp_err_t ui_get(int slot, uint8_t *out, size_t cap, size_t *n);
esp_err_t ui_put(int slot, const uint8_t *data, size_t n);
esp_err_t ui_del(int slot);
