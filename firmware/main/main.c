#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "ble.h"
#include "engine.h"
#include "ir_rmt.h"
#include "proto.h"
#include "store.h"
#include "transport.h"

#define LED_GPIO   8   /* Super Mini on-board LED, active low */
#define IR_TX_GPIO 5   /* ULN2003 IN1-IN4 */
#define IR_RX_GPIO 4   /* VS1838B OUT, idle high, low = IR present */
#define MIN_ENTRIES 4
#define MAX_ENTRIES 1200

static volatile int64_t s_pulse_until_us;

static void led_set(bool on) { gpio_set_level(LED_GPIO, on ? 0 : 1); }

/* LED: fast blink while recording, steady while a BLE client is connected, short flash per frame. */
static void led_task(void *p)
{
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool on;
        if (engine_recording()) on = (now / 125000) % 2;
        else if (now < s_pulse_until_us) on = true;
        else on = ble_connected();
        led_set(on);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void capture_task(void *p)
{
    static uint16_t buf[MAX_ENTRIES];
    for (;;) {
        size_t n = 0;
        esp_err_t e = ir_receive(buf, MAX_ENTRIES, &n, 1000);
        if (e == ESP_ERR_TIMEOUT) continue;
        if (e != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (n < MIN_ENTRIES) continue;
        int64_t now = ir_last_rx_time_us();
        if (now < ir_tx_block_until_us()) continue;           /* we heard ourselves */
        s_pulse_until_us = esp_timer_get_time() + 80000;
        engine_on_frame(buf, n, now);
    }
}

static void usb_rx_task(void *p)
{
    uint8_t chunk[128];
    for (;;) {
        int got = usb_serial_jtag_read_bytes(chunk, sizeof chunk, pdMS_TO_TICKS(100));
        if (got > 0) proto_feed(TR_USB, chunk, got);
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_ERROR);   /* keep the serial stream clean JSON */
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_config_t io = { .pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    led_set(false);

    transport_init();
    ESP_ERROR_CHECK(store_init());
    engine_init();
    proto_init();
    ESP_ERROR_CHECK(ir_init(IR_TX_GPIO, IR_RX_GPIO, false, 0));
    ble_start();

    xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 4, NULL);
    xTaskCreate(capture_task, "capture", 6144, NULL, 5, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    ilog("boot ok");
}
