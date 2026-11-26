/* BLE HID remote ("HID over GATT") so a TV can pair with the capsule and receive key presses. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "host/ble_gatt.h"

void remote_init(void);                        /* load the paired TV, set up pairing */
const struct ble_gatt_svc_def *remote_gatt_svcs(void);

/* link events from ble.c */
void remote_on_encrypted(uint16_t conn, bool new_pairing, bool is_web);
bool remote_is_tv(uint16_t conn);
void remote_on_disconnect(uint16_t conn);

bool remote_pairing_open(void);
void remote_pair(bool open);                   /* open: accept one new TV for 120 s */
void remote_forget(void);
void remote_status(bool *paired, bool *connected, int *window_s);
/* NULL when sent, else why not */
const char *remote_key(const char *name);
