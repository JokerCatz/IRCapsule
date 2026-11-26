/* Two transports speak the same line-based JSON protocol: USB serial and BLE.
 * Replies go back to the transport a command came from; events go to every ready transport. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "jsonl.h"

typedef enum { TR_USB = 0, TR_BLE = 1, TR_COUNT } tr_id_t;

void transport_init(void);
bool tr_ready(tr_id_t t);
/* Lock a transport and start a message. Finish with tr_end(), which appends the newline. */
void tr_begin(tr_id_t t, out_t *o);
void tr_end(tr_id_t t, out_t *o);

/* Broadcast one complete JSON object (no trailing newline) to every ready transport. */
void ev_send(const char *json);
/* Same, but only USB: used for frame events that carry the full raw timings. */
void ev_send_usb(const char *json);

/* Implemented by ble.c */
void ble_tx_write(const char *d, size_t n);
bool ble_tx_ready(void);
bool ble_in_host_task(void);
