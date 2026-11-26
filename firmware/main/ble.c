/* BLE transport: one GATT service with a write characteristic (commands in) and a notify
 * characteristic (replies and events out). Both carry the same newline-terminated JSON lines
 * as the USB serial port, split into chunks of at most MTU-3 bytes. No pairing: any central
 * in range can connect. */
#include "ble.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "proto.h"
#include "store.h"
#include "transport.h"

/* Service 9f1d0001-6b7a-4c5e-8a2b-1c4a9e5d7f10; RX ...0002 (write); TX ...0003 (notify) */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0x10,0x7f,0x5d,0x9e,0x4a,0x1c,0x2b,0x8a,0x5e,0x4c,0x7a,0x6b,0x01,0x00,0x1d,0x9f);
static const ble_uuid128_t RX_UUID  = BLE_UUID128_INIT(0x10,0x7f,0x5d,0x9e,0x4a,0x1c,0x2b,0x8a,0x5e,0x4c,0x7a,0x6b,0x02,0x00,0x1d,0x9f);
static const ble_uuid128_t TX_UUID  = BLE_UUID128_INIT(0x10,0x7f,0x5d,0x9e,0x4a,0x1c,0x2b,0x8a,0x5e,0x4c,0x7a,0x6b,0x03,0x00,0x1d,0x9f);

static uint16_t s_tx_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify;
static uint8_t s_addr_type;
static char s_name[28];

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t tmp[512];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof tmp) len = sizeof tmp;
    os_mbuf_copydata(ctxt->om, 0, len, tmp);
    proto_feed(TR_BLE, tmp, len);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC_UUID.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &RX_UUID.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
          { .uuid = &TX_UUID.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY },
          { 0 } } },
    { 0 }
};

bool ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_tx_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_notify; }

void ble_tx_write(const char *d, size_t n)
{
    size_t chunk = (size_t)g_ble_mtu - 3;
    if (chunk > 200) chunk = 200;
    if (chunk < 20) chunk = 20;
    while (n && ble_tx_ready()) {
        size_t k = n < chunk ? n : chunk;
        int rc = BLE_HS_ENOMEM;
        for (int tries = 0; tries < 200 && rc == BLE_HS_ENOMEM && ble_tx_ready(); tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(d, k);
            if (!om) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);   /* consumes om */
            if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (rc != 0) return;
        d += k; n -= k;
        vTaskDelay(1);   /* let the controller drain; keeps the mbuf pool from running dry */
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void adv_start(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name; f.name_len = strlen(s_name); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&SVC_UUID; rsp.num_uuids128 = 1; rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle; s_notify = false; g_ble_mtu = 23;
        } else adv_start();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_notify = false; g_ble_mtu = 23;
        adv_start();
        break;
    case BLE_GAP_EVENT_MTU:
        g_ble_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) s_notify = ev->subscribe.cur_notify;
        break;
    default: break;
    }
    return 0;
}

static void on_sync(void) { ble_hs_id_infer_auto(0, &s_addr_type); adv_start(); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_start(void)
{
    if (g_meta.device_name[0]) snprintf(s_name, sizeof s_name, "%s", g_meta.device_name);
    else {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
        snprintf(s_name, sizeof s_name, "IRCapsule-%02X%02X", mac[4], mac[5]);
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    ble_gatts_count_cfg(GATT_SVCS);
    ble_gatts_add_svcs(GATT_SVCS);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
