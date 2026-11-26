#pragma once
#include <stddef.h>
#include "transport.h"

void proto_init(void);
/* Feed raw bytes from a transport; complete lines are queued for the worker task. */
void proto_feed(tr_id_t tr, const uint8_t *data, size_t len);
extern volatile int g_ble_mtu;
