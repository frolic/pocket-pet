#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_err.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "device_familiar.h"
#include "device_state.h"
#include "device_axp2101.h"
#include "step_source.h"
#include "battery_source.h"

/*
 * Familiar client: the watch as a BLE peripheral speaking the framed channel from
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
 * Frame layout (must match the Familiar app protocol — frolic/familiar docs/design.md):
 *   [len:u16 LE][flags:u8][stream:u8][payload]
 *   flags bit0-1 kind (0 json, 1 binary), bit2-3 position
 *   (0 only, 1 first, 2 cont, 3 last)
 */

#define FAMILIAR_DEVICE_ID "pikachu-01"
#define FAMILIAR_PROTO 1
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
static volatile bool stack_running;

static uint8_t reassembly[REASSEMBLY_MAX];
static size_t reassembly_length;
static bool reassembly_open;

static void start_advertising(void);
static void ble_stack_start(void);
static void ble_stack_stop(void);

/*
 * Latency rig (famping console command): the device sends binary frames
 * carrying their own send-timestamp; the central echoes every binary frame
 * back verbatim; RTT is computed here against the same microsecond clock,
 * so the phone's state (foreground / background / locked / relaunched) is
 * the thing under test and never part of the instrument.
 *
 * Ping payload (12 bytes, fits a single frame even at MTU 23):
 *   'P' 'G'  [seq:u16 LE]  [t_us:i64 LE]
 */
static int64_t *ping_rtts;          /* PSRAM, one slot per expected echo */
static volatile uint32_t ping_received;
static uint32_t ping_target;

static void ping_note_echo(const uint8_t *payload, size_t length)
{
    if (length < 12 || payload[0] != 'P' || payload[1] != 'G') return;
    int64_t sent_us;
    memcpy(&sent_us, payload + 4, sizeof(sent_us));
    int64_t rtt = esp_timer_get_time() - sent_us;
    if (ping_rtts != NULL && ping_received < ping_target) {
        ping_rtts[ping_received] = rtt;
    }
    ping_received++;
}

static int compare_int64(const void *a, const void *b)
{
    int64_t left = *(const int64_t *)a;
    int64_t right = *(const int64_t *)b;
    return left < right ? -1 : left > right ? 1 : 0;
}

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

/* Runs on the familiar worker task ONLY (statics are safe): bounded,
   NUL-terminated parsing — arbitrary central input must never walk off
   a buffer (pocket-pet#1). */
static volatile long weather_outstanding_id;
static volatile int64_t weather_sent_us;
static volatile bool radio_wanted = true; /* boot advertises via on_sync */

static void handle_json_message(const uint8_t *bytes, size_t length)
{
    printf("familiar: rx json (%u bytes)\n", (unsigned)length);
    if (length == 0 || length >= REASSEMBLY_MAX) return;
    static char text[REASSEMBLY_MAX + 1];
    memcpy(text, bytes, length);
    text[length] = '\0';
    /* Envelope scan: find "id":N with a dumb scan (no JSON lib on this
       path yet; the P2 watch client will own real parsing). */
    const char *id_key = strstr(text, "\"id\"");
    if (id_key == NULL) return; /* event or malformed: nothing owed */
    const char *colon = strchr(id_key + 4, ':');
    if (colon == NULL) return;
    long id = strtol(colon + 1, NULL, 10);
    if (id != 0 && id == weather_outstanding_id) {
        /* The relay answered our weather request: print the reading and
           decompose the RTT using the phone's own http stamp (t.http). */
        weather_outstanding_id = 0;
        double total_ms = (esp_timer_get_time() - weather_sent_us) / 1000.0;
        const char *timing = strstr(text, "\"t\":");
        const char *http_stamp =
            timing != NULL ? strstr(timing, "\"http\":") : NULL;
        double phone_ms = http_stamp != NULL ? strtod(http_stamp + 7, NULL) : -1;
        /* Anchor to the "current" object: the sibling "current_units"
           block also holds a temperature_2m, but as the string "°C". */
        const char *current = strstr(text, "\"current\":");
        const char *temperature =
            current != NULL ? strstr(current, "\"temperature_2m\":") : NULL;
        if (temperature != NULL) {
            printf("familiar: weather London %.1fC\n",
                   strtod(temperature + 17, NULL));
        } else {
            printf("familiar: weather reply without temperature: %s\n", text);
        }
        if (phone_ms >= 0) {
            printf("familiar: rtt %.1fms = phone(parse+http) %.1f + ble/os %.1f (%uB reply)\n",
                   total_ms, phone_ms, total_ms - phone_ms, (unsigned)length);
        } else {
            printf("familiar: rtt %.1fms (no phone timing in reply)\n", total_ms);
        }
        return;
    }
    static char reply[REASSEMBLY_MAX + 64];
    int reply_length = snprintf(reply, sizeof(reply),
                                "{\"id\":%ld,\"status\":599,\"body\":{\"echo\":%s}}",
                                id, text);
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
    if (kind == 1) {
        ping_note_echo(payload, payload_length);
        return;
    }
    if (kind != 0) return;

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
                          FAMILIAR_DEVICE_ID, FAMILIAR_PROTO);
    return os_mbuf_append(ctxt->om, info, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/*
 * RX frames hop from the NimBLE host task to a worker via this queue.
 * The host-task callback must stay tiny: the original in-callback parse
 * (512B flat buffer + printf + newlib on NimBLE's ~4KB stack) overflowed
 * into adjacent heap and corrupted the host's own event queue — the
 * IWDT-spinning-on-a-garbage-spinlock crash of pocket-pet#1. Statics in
 * rx_access/worker are safe: each runs on exactly one task.
 */
typedef struct {
    uint16_t length;
    uint8_t bytes[512];
} rx_item_t;

static QueueHandle_t rx_queue;

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    static rx_item_t item;
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length > sizeof(item.bytes)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    ble_hs_mbuf_to_flat(ctxt->om, item.bytes, sizeof(item.bytes), &length);
    item.length = length;
    if (rx_queue != NULL) {
        /* Full queue drops the frame: resume/retry is protocol-level, and
           the host task must never block on a slow (or hostile) sender. */
        xQueueSend(rx_queue, &item, 0);
    }
    return 0;
}

static void worker_task(void *arg)
{
    (void)arg;
    static rx_item_t item;
    while (true) {
        if (xQueueReceive(rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            feed_frame(item.bytes, item.length);
        }
    }
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
            printf("familiar: central connected\n");
        } else {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("familiar: central disconnected (reason=%d)\n", event->disconnect.reason);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        tx_subscribed = false;
        reassembly_open = false;
        if (radio_wanted) start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_value_handle) {
            tx_subscribed = event->subscribe.cur_notify;
            printf("familiar: tx %ssubscribed\n", tx_subscribed ? "" : "un");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        printf("familiar: mtu=%d\n", event->mtu.value);
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
        /* ~250-300ms intervals: advertising runs the whole screen-on
           session, and the fast default (~60ms) buys nothing — the phone's
           pending connect lands within a beat either way. */
        .itvl_min = BLE_GAP_ADV_ITVL_MS(244),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(306),
    };
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)"pika";
    fields.name_len = 4;
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
        printf("familiar: advertise failed rc=%d\n", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    printf("familiar: advertising as pika\n");
    start_advertising();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Real telemetry while subscribed: the P2 relay forwards these verbatim. */
bool device_familiar_radio_active(void)
{
    /* True from stack start until teardown fully completes — a connected
       central, advertising, and mid-stop all block light sleep. */
    return stack_running;
}

void device_familiar_weather(void)
{
    /* Demo of the request/response path: ask the relay for London weather
       and print the temperature when the reply lands. */
    static long next_id = 100;
    weather_outstanding_id = ++next_id;
    char request[256];
    snprintf(request, sizeof(request),
             "{\"id\":%ld,\"m\":\"http\",\"p\":{\"method\":\"GET\","
             "\"url\":\"https://api.open-meteo.com/v1/forecast"
             "?latitude=51.5072&longitude=-0.1276"
             "&current=temperature_2m\"}}",
             weather_outstanding_id);
    printf("familiar: asking relay for weather (id=%ld)\n",
           weather_outstanding_id);
    weather_sent_us = esp_timer_get_time();
    send_json(request);
}

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
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
        /* Once a minute, exercise the request/response path too. */
        if (tick++ % 6 == 0) device_familiar_weather();
    }
}

void device_familiar_ping(uint32_t count, uint32_t interval_ms)
{
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE || !tx_subscribed) {
        printf("famping: no subscribed central\n");
        return;
    }
    if (count == 0 || count > 10000) count = 200;
    if (interval_ms == 0) interval_ms = 100;
    free(ping_rtts);
    ping_rtts = heap_caps_malloc(count * sizeof(int64_t), MALLOC_CAP_SPIRAM);
    if (ping_rtts == NULL) {
        printf("famping: alloc failed\n");
        return;
    }
    ping_target = count;
    ping_received = 0;

    printf("famping: %lu pings at %lums\n", (unsigned long)count,
           (unsigned long)interval_ms);
    uint32_t sent_ok = 0;
    for (uint32_t seq = 0; seq < count; seq++) {
        uint8_t payload[12] = {'P', 'G', seq & 0xFF, (seq >> 8) & 0xFF};
        int64_t now = esp_timer_get_time();
        memcpy(payload + 4, &now, sizeof(now));
        if (send_frame(payload, sizeof(payload), 0x01 /* binary, only */) == 0) {
            sent_ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        if ((seq & 0x1F) == 0x1F) {
            printf("famping: %lu/%lu sent, %lu echoed\n", (unsigned long)(seq + 1),
                   (unsigned long)count, (unsigned long)ping_received);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(3000)); /* grace for stragglers */

    uint32_t got = ping_received < ping_target ? ping_received : ping_target;
    if (got == 0) {
        printf("famping: sent_ok=%lu echoed=0 — nothing came back\n",
               (unsigned long)sent_ok);
        return;
    }
    qsort(ping_rtts, got, sizeof(int64_t), compare_int64);
    int64_t sum = 0;
    for (uint32_t i = 0; i < got; i++) sum += ping_rtts[i];
    int64_t mean = sum / got;
    int64_t variance = 0;
    for (uint32_t i = 0; i < got; i++) {
        int64_t delta = ping_rtts[i] - mean;
        variance += delta * delta / got;
    }
    printf("famping RESULT sent=%lu echoed=%lu lost=%lu\n",
           (unsigned long)sent_ok, (unsigned long)got,
           (unsigned long)(sent_ok - got));
    printf("famping RTT ms: min=%.1f p50=%.1f p95=%.1f max=%.1f mean=%.1f stddev=%.1f\n",
           ping_rtts[0] / 1000.0, ping_rtts[got / 2] / 1000.0,
           ping_rtts[(uint32_t)(got * 0.95)] / 1000.0, ping_rtts[got - 1] / 1000.0,
           mean / 1000.0, sqrt((double)variance) / 1000.0);
    printf("famping RAW us:");
    for (uint32_t i = 0; i < got; i++) printf(" %lld", (long long)ping_rtts[i]);
    printf("\n");
}

/* BLE follows the screen (same policy as wifi had): advertise and hold
   connections while the face is lit or on USB power; when dark on battery,
   terminate the session and stop advertising so the manual light-sleep
   loop can engage (it freezes the BLE controller — a live session and
   sleep are mutually exclusive). The phone's pending connect re-links
   within a couple seconds of the next screen-on. */
static void radio_policy_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
#ifdef FROLIC_SLEEP_ON_VBUS
        /* Bench: ignore vbus so the dark-loop teardown runs on USB. */
        bool want = device_state_get() == DEVICE_STATE_ACTIVE;
#else
        bool want = device_state_get() == DEVICE_STATE_ACTIVE ||
                    axp2101_vbus_present();
#endif
        if (want == radio_wanted) continue;
        radio_wanted = want;
        if (want) {
            ble_stack_start(); /* advertises via on_sync */
            printf("familiar: screen on — BLE stack up\n");
        } else {
            if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                vTaskDelay(pdMS_TO_TICKS(200)); /* let the terminate land */
            }
            ble_stack_stop();
            printf("familiar: dark on battery — BLE stack down (controller off)\n");
        }
    }
}

/* Full stack up/down, controller included. The dark-hours teardown MUST
   reach the controller: manual esp_light_sleep_start ignores the pm locks
   the controller holds, and force-sleeping under an active controller
   wedges the sleep entry/exit handshake until the RTC watchdog resets the
   chip (~once per 32min of dark sleep, measured 2026-08-15) — and burns
   ~23%%/h keeping the RF clock domain powered. Stopped controller = clean
   sleeps and dark drain back to fuel-gauge noise. */

static void ble_stack_start(void)
{
    if (stack_running) return;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        printf("familiar: nimble init failed (%s)\n", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("pika");
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    nimble_port_freertos_init(host_task);
    stack_running = true;
}

static void ble_stack_stop(void)
{
    if (!stack_running) return;
    connection_handle = BLE_HS_CONN_HANDLE_NONE;
    tx_subscribed = false;
    if (nimble_port_stop() == 0) {
        nimble_port_deinit(); /* also disables + deinits the controller */
    }
    /* Cleared LAST: sleep eligibility reads this across tasks, and a light
       sleep during stack teardown is an IWDT reset (seen 2026-08-15 22:19,
       the one crash of the first clean night). */
    stack_running = false;
}

void device_familiar_init(void)
{
    rx_queue = xQueueCreateWithCaps(8, sizeof(rx_item_t), MALLOC_CAP_SPIRAM);
    ble_stack_start();
    xTaskCreate(worker_task, "famwrk", 4096, NULL, 4, NULL);
    xTaskCreate(telemetry_task, "famtel", 3072, NULL, 3, NULL);
    xTaskCreate(radio_policy_task, "famradio", 3072, NULL, 3, NULL);
    printf("familiar: BLE peripheral up\n");
}
