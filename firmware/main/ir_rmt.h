#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw IR timing: alternating mark/space durations in microseconds, starting with a mark. */
typedef struct {
    uint16_t *us;
    size_t    n;
} ir_raw_t;

/* tx_gpio/rx_gpio may be the same pin with loopback=true (bench test without hardware). */
/* rx_mark_level: pin level that means 'IR present' (0 for VS1838B, 1 for loopback test). */
esp_err_t ir_init(int tx_gpio, int rx_gpio, bool loopback, int rx_mark_level);

/* Send raw mark/space list. carrier_khz == 0 disables the hardware carrier. */
esp_err_t ir_send_raw(const uint16_t *us, size_t n, uint32_t carrier_khz);

/* Build and send an NEC frame (32-bit, address/command with inverted check bytes). */
esp_err_t ir_send_nec(uint8_t addr, uint8_t cmd, uint32_t carrier_khz);

/* Blocking receive of one frame. Returns ESP_OK and fills us[] (up to max) with n entries. */
esp_err_t ir_receive(uint16_t *us, size_t max, size_t *n, uint32_t timeout_ms);

#define IR_RX_GUARD_US 100000  /* keep ignoring RX this long after our own transmission */

/* Frames that finish before this esp_timer time (us) were caused by our own transmitter: drop them. */
int64_t ir_tx_block_until_us(void);

/* esp_timer time (us) at which the last received frame finished. */
int64_t ir_last_rx_time_us(void);

/* Human-readable guess of the protocol, e.g. "NEC a=04 c=10", "SONY12 0xA90", "RAW". */
void ir_describe(const uint16_t *us, size_t n, char *out, size_t outlen);

/* Decode a raw capture as NEC. Returns true and fills addr/cmd on success. */
bool ir_decode_nec(const uint16_t *us, size_t n, uint8_t *addr, uint8_t *cmd);

#ifdef __cplusplus
}
#endif
