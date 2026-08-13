#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "device_leash.h"
#include "step_source.h"
#include "battery_source.h"

/*
 * Leash P1: the watch as a BLE peripheral speaking the framed channel from
 * docs/ble-gateway-design.md. Bench scope — always advertising, one
 * central, no bonding yet (bring-up runs open; encryption arrives before
 * real tokens do):
 *
 *   INFO (read)   {"device_id":"pikachu-01","proto":1}
 *   TX   (notify) frames watch -> central
 *   RX   (write)  frames central -> watch
 *
 * Channel behavior for validation: every reassembled JSON message with an
 * "id" is echoed back as {"id":N,"status":599,"body":{"echo":<msg>}} — a
 * generic central (nRF Connect, the Mac, the app's transport test) can
 * prove framing, MTU handling, and duplex flow without any server. While
 * subscribed, a real telemetry event ({"ev":"http",...}) goes out every
 * 10s, so the P2 app can relay something true from day one.
 *
 * Frame layout (must match leash/src/protocol):
 *   [len:u16 LE][flags:u8][stream:u8][payload]
 *   flags bit0-1 kind (0 json, 1 binary), bit2-3 position
 *   (0 only, 1 first, 2 cont, 3 last)
 */

#define LEASH_DEVICE_ID "pikachu-01"
#define LEASH_PROTO 1
#define TELEMETRY_PERIOD_MS 10000
#define REASSEMBLY_MAX 2048

/* 128-bit UUIDs in BLE wire order (little-endian).
   Human-readable: f0c0f00d-1eaf-4c02-8a5c-0000000000NN */
static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x8a,
                     0x02, 0x4c, 0xaf, 0x1e, 0x0d, 0xf0, 0xc0, 0xf0);
static const ble_uuid128_t info_uuid =
    BLE_UUID128_INIT(0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x8a,
                     0x02, 0x4c, 0xaf, 0x1e, 0x0d, 0xf0, 0xc0, 0xf0);
static const ble_uuid128_t tx_uuid =
    BLE_UUID128_INIT(0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x8a,
                     0x02, 0x4c, 0xaf, 0x1e, 0x0d, 0xf0, 0xc0, 0xf0);
static const ble_uuid128_t rx_uuid =
    BLE_UUID128_INIT(0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x8a,
                     0x02, 0x4c, 0xaf, 0x1e, 0x0d, 0xf0, 0xc0, 0xf0);

static uint16_t tx_value_handle;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool tx_subscribed;

static uint8_t reassembly[REASSEMBLY_MAX];
static size_t reassembly_length;
static bool reassembly_open;

static void start_advertising(void);

/* ---------- outbound: fragment and notify ---------- */

static int send_frame(const uint8_t *payload, size_t length, uint8_t flags)
{
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE || !tx_subscribed) {
        return -1;
    }
    uint8_t header[4] = {length & 0xFF, (length >> 8) & 0xFF, flags, 0};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(header, sizeof(header));
    if (om == NULL) return -1;
    if (os_mbuf_append(om, payload, length) != 0) {
        os_mbuf_free_chain(om);
        return -1;
    }
    return ble_gatts_notify_custom(connection_handle, tx_value_handle, om);
}

static void send_json(const char *json)
{
    size_t total = strlen(json);
    uint16_t mtu = ble_att_mtu(connection_handle);
    size_t chunk_max = (mtu > 3 + 4 + 1) ? (size_t)(mtu - 3 - 4) : 20;
    if (total <= chunk_max) {
        send_frame((const uint8_t *)json, total, 0x00); /* json, only */
        return;
    }
    size_t offset = 0;
    while (offset < total) {
        size_t chunk = total - offset;
        if (chunk > chunk_max) chunk = chunk_max;
        uint8_t position = offset == 0 ? 1 : (offset + chunk >= total ? 3 : 2);
        if (send_frame((const uint8_t *)json + offset, chunk,
                       (uint8_t)(position << 2)) != 0) {
            return;
        }
        offset += chunk;
    }
}

/* ---------- inbound: reassemble and echo ---------- */

static void handle_json_message(const uint8_t *bytes, size_t length)
{
    printf("leash: rx json (%u bytes)\n", (unsigned)length);
    /* Echo envelope: find "id":N with a dumb scan (no JSON lib on this
       path yet; the P2 watch client will own real parsing). */
    const char *id_key = NULL;
    for (size_t i = 0; i + 4 < length; i++) {
        if (memcmp(bytes + i, "\"id\"", 4) == 0) {
            id_key = (const char *)bytes + i;
            break;
        }
    }
    if (id_key == NULL) return; /* event or malformed: nothing owed */
    long id = strtol(strchr(id_key, ':') + 1, NULL, 10);
    char reply[REASSEMBLY_MAX + 64];
    int reply_length = snprintf(reply, sizeof(reply),
                                "{\"id\":%ld,\"status\":599,\"body\":{\"echo\":%.*s}}",
                                id, (int)length, (const char *)bytes);
    if (reply_length > 0 && (size_t)reply_length < sizeof(reply)) {
        send_json(reply);
    }
}

static void feed_frame(const uint8_t *frame, size_t frame_length)
{
    if (frame_length < 4) return;
    size_t payload_length = frame[0] | (frame[1] << 8);
    if (frame_length != 4 + payload_length) return;
    uint8_t kind = frame[2] & 0x03;
    uint8_t position = (frame[2] >> 2) & 0x03;
    const uint8_t *payload = frame + 4;
    if (kind != 0) return; /* binary streams arrive with the P2 client */

    if (position == 0) { /* only */
        handle_json_message(payload, payload_length);
        reassembly_open = false;
        return;
    }
    if (position == 1) { /* first */
        reassembly_length = 0;
        reassembly_open = true;
    }
    if (!reassembly_open) return;
    if (reassembly_length + payload_length > sizeof(reassembly)) {
        reassembly_open = false;
        return;
    }
    memcpy(reassembly + reassembly_length, payload, payload_length);
    reassembly_length += payload_length;
    if (position == 3) { /* last */
        reassembly_open = false;
        handle_json_message(reassembly, reassembly_length);
    }
}

/* ---------- GATT ---------- */

static int info_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    char info[96];
    int length = snprintf(info, sizeof(info),
                          "{\"device_id\":\"%s\",\"proto\":%d}",
                          LEASH_DEVICE_ID, LEASH_PROTO);
    return os_mbuf_append(ctxt->om, info, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    uint8_t frame[512];
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length > sizeof(frame)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &length);
    feed_frame(frame, length);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0; /* notify-only; reads return empty */
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &info_uuid.u, .access_cb = info_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &tx_uuid.u, .access_cb = tx_access,
              .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_value_handle },
            { .uuid = &rx_uuid.u, .access_cb = rx_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 },
        },
    },
    { 0 },
};

/* ---------- GAP ---------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection_handle = event->connect.conn_handle;
            printf("leash: central connected\n");
        } else {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("leash: central disconnected (reason=%d)\n", event->disconnect.reason);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        tx_subscribed = false;
        reassembly_open = false;
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_value_handle) {
            tx_subscribed = event->subscribe.cur_notify;
            printf("leash: tx %ssubscribed\n", tx_subscribed ? "" : "un");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        printf("leash: mtu=%d\n", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)"leash-pika";
    fields.name_len = 10;
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    /* Service UUID goes in the scan response (128-bit doesn't share a
       31-byte packet with a name). */
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = (ble_uuid128_t *)&service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&response);
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        printf("leash: advertise failed rc=%d\n", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    printf("leash: advertising as leash-pika\n");
    start_advertising();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Real telemetry while subscribed: the P2 relay forwards these verbatim. */
static void telemetry_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
        if (!tx_subscribed) continue;
        char event[192];
        /* Bring-up target: a public echo endpoint, so the first end-to-end
           relay proves itself with a real 200. Swaps to the pet API when
           that exists. */
        snprintf(event, sizeof(event),
                 "{\"ev\":\"http\",\"p\":{\"method\":\"POST\","
                 "\"url\":\"https://httpbin.org/post\","
                 "\"body\":{\"steps\":%lu,\"bat\":%d}}}",
                 (unsigned long)step_source_total(), battery_source_percent());
        send_json(event);
    }
}

void device_leash_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        printf("leash: nimble init failed (%s)\n", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("leash-pika");
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    nimble_port_freertos_init(host_task);
    xTaskCreate(telemetry_task, "leashtel", 3072, NULL, 3, NULL);
    printf("leash: BLE peripheral up\n");
}
