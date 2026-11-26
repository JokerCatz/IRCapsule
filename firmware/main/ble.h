#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ble_start(void);
bool ble_connected(void);          /* a web page is connected and accepted */

/* used by secure.c */
bool ble_send(uint16_t conn, const char *data, size_t len);   /* notify, in pieces; false if not all went out */
void ble_drop(uint16_t conn);
int  ble_mtu(uint16_t conn);
