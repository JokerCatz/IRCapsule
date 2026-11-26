/* Access header over Bluetooth. With a header set, a link must prove it knows it and everything after that
 * is AES-GCM encrypted; the header itself is never sent again. Without one, links are plain and open.
 * This sits under the line protocol: the command handlers see ordinary lines. UART is not involved.
 * Wire format: docs/protocol.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURE_HEADER_MAX 64

void secure_init(void);                                   /* after NVS is ready, before BLE starts */
bool secure_enabled(void);
void secure_get(char *out, size_t cap);                   /* the header, "" when none */
/* "" clears. Closes the other web links (all of them when the request did not come over BLE).
 * NULL on success, else why not. */
const char *secure_set(const char *header, bool from_ble);

/* from the BLE host task */
void secure_link_open(uint16_t conn);                     /* the link subscribed to replies */
void secure_link_close(uint16_t conn);                    /* unsubscribed or disconnected */
void secure_rx(uint16_t conn, const uint8_t *data, size_t len);
void secure_mtu(uint16_t conn, int mtu);

/* the accepted link, if any */
bool secure_ready(void);
bool secure_is_web(uint16_t conn);
void secure_tx(const char *data, size_t len);             /* a piece of the outgoing line stream */
