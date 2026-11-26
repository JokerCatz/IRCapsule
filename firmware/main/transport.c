#include "transport.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"

static SemaphoreHandle_t s_lock[TR_COUNT];

/* Write through the USB driver (not printf): the stdio path drops bytes when the host reads slowly. */
static void usb_sink(void *ctx, const char *d, size_t n)
{
    size_t off = 0;
    while (off < n && usb_serial_jtag_is_connected()) {
        int w = usb_serial_jtag_write_bytes(d + off, n - off, pdMS_TO_TICKS(500));
        if (w <= 0) break;                  /* host stopped reading: drop the rest */
        off += (size_t)w;
    }
}
static void ble_sink(void *ctx, const char *d, size_t n) { ble_tx_write(d, n); }

void transport_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 12288;   /* long command lines are several KB; the default 256 B overflows */
    cfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    for (int i = 0; i < TR_COUNT; i++) s_lock[i] = xSemaphoreCreateMutex();
}

bool tr_ready(tr_id_t t) { return t == TR_USB ? usb_serial_jtag_is_connected() : ble_tx_ready(); }

void tr_begin(tr_id_t t, out_t *o)
{
    xSemaphoreTake(s_lock[t], portMAX_DELAY);
    out_init(o, t == TR_USB ? usb_sink : ble_sink, NULL);
}

void tr_end(tr_id_t t, out_t *o)
{
    out_puts(o, "\n");
    out_flush(o);
    xSemaphoreGive(s_lock[t]);
}

static void send_to(tr_id_t t, const char *json)
{
    if (!tr_ready(t)) return;
    out_t o;
    tr_begin(t, &o);
    out_puts(&o, json);
    tr_end(t, &o);
}

void ev_send(const char *json) { for (int t = 0; t < TR_COUNT; t++) send_to((tr_id_t)t, json); }
void ev_send_usb(const char *json) { send_to(TR_USB, json); }
