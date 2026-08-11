#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "device_wifi.h"
#include "device_state.h"
#include "device_flush_gate.h"

#ifdef FROLIC_DEBUG
#define WIFI_TRACE(...) printf("WIFI-TRACE: " __VA_ARGS__)
#else
#define WIFI_TRACE(...)
#endif

#define SETUP_SSID "pocket-pet"
#define PORTAL_IP "192.168.4.1"
#define BOOT_BUTTON GPIO_NUM_0

/*
 * Provisioning design note: credentials are validated by REBOOTING into pure
 * station mode, never by connecting the STA interface while the softAP runs.
 * APSTA station-connect forces radio channel-switching that corrupts the QSPI
 * display pipeline on this board (and made association itself unreliable).
 * A failed attempt records an error and reboots back into the portal.
 */

#define MAX_NETWORKS 5

typedef struct {
    char ssid[33];
    char password[65];
} known_network_t;

static known_network_t networks[MAX_NETWORKS];
static int network_count;
static int active_index = -1;     /* the network we're connecting/connected to */
static int validating_index = -1; /* freshly-entered creds awaiting proof */
static char last_error[96];
static bool in_portal;
static bool radio_active;
static bool offline; /* no known network reachable at the last attempt */

/* ---------- NVS ---------- */

static void slot_key(char *out, const char *prefix, int index)
{
    sprintf(out, "%s%d", prefix, index);
}

static bool load_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) return false;

    /* Migrate the original single-slot keys into slot 0. */
    size_t size = sizeof(networks[0].ssid);
    if (nvs_get_str(handle, "ssid", networks[0].ssid, &size) == ESP_OK) {
        size = sizeof(networks[0].password);
        if (nvs_get_str(handle, "password", networks[0].password, &size) == ESP_OK) {
            nvs_set_str(handle, "ssid0", networks[0].ssid);
            nvs_set_str(handle, "pass0", networks[0].password);
        }
        nvs_erase_key(handle, "ssid");
        nvs_erase_key(handle, "password");
        nvs_commit(handle);
    }

    network_count = 0;
    for (int i = 0; i < MAX_NETWORKS; i++) {
        char key[8];
        slot_key(key, "ssid", i);
        size = sizeof(networks[network_count].ssid);
        if (nvs_get_str(handle, key, networks[network_count].ssid, &size) != ESP_OK) continue;
        slot_key(key, "pass", i);
        size = sizeof(networks[network_count].password);
        if (nvs_get_str(handle, key, networks[network_count].password, &size) != ESP_OK) continue;
        if (networks[network_count].ssid[0] != '\0') network_count++;
    }
    nvs_close(handle);
    return network_count > 0;
}

static void store_all(nvs_handle_t handle)
{
    for (int i = 0; i < MAX_NETWORKS; i++) {
        char key[8];
        slot_key(key, "ssid", i);
        if (i < network_count) nvs_set_str(handle, key, networks[i].ssid);
        else nvs_erase_key(handle, key);
        slot_key(key, "pass", i);
        if (i < network_count) nvs_set_str(handle, key, networks[i].password);
        else nvs_erase_key(handle, key);
    }
}

/* Appends (or updates a same-SSID entry); evicts the oldest when full. The
   new entry is marked validating so a bad password still round-trips to the
   portal instead of silently polluting the list. */
static void save_credentials(const char *ssid, const char *password)
{
    load_credentials();
    int index = -1;
    for (int i = 0; i < network_count; i++) {
        if (strcmp(networks[i].ssid, ssid) == 0) index = i;
    }
    if (index < 0) {
        if (network_count == MAX_NETWORKS) {
            memmove(&networks[0], &networks[1], sizeof(networks[0]) * (MAX_NETWORKS - 1));
            network_count--;
        }
        index = network_count++;
    }
    strlcpy(networks[index].ssid, ssid, sizeof(networks[index].ssid));
    strlcpy(networks[index].password, password, sizeof(networks[index].password));

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &handle));
    store_all(handle);
    ESP_ERROR_CHECK(nvs_set_u8(handle, "validating", 1));
    ESP_ERROR_CHECK(nvs_set_u8(handle, "validating_idx", (uint8_t)index));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

static void erase_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, key, value);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static uint8_t get_u8(const char *key)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open("wifi", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

static void set_last_error(const char *message)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "last_error", message);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_and_clear_last_error(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) return;
    size_t size = sizeof(last_error);
    if (nvs_get_str(handle, "last_error", last_error, &size) != ESP_OK) {
        last_error[0] = '\0';
    }
    nvs_erase_key(handle, "last_error");
    nvs_commit(handle);
    nvs_close(handle);
}

/* ---------- station mode (normal operation + validation boot) ---------- */

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void ensure_wifi_inited(void);

static int station_failures;
static bool station_ever_connected;
static bool validating_boot; /* creds fresh from the portal: unproven */
static EventGroupHandle_t window_events;
static volatile bool window_mode;
#define WINDOW_GOT_IP_BIT BIT0

/* Radio and animation corrupt each other on this board, and normal operation
   doesn't need wifi: once the clock syncs, the radio shuts down entirely.
   Future sync windows re-enable it briefly with the pet asleep. */
static void radio_off_timer_cb(void *arg)
{
    printf("device_wifi: clock synced — radio off until next sync window\n");
    esp_sntp_stop();
    esp_wifi_stop();
    device_flush_gate_open();
    radio_active = false;
    device_state_release_radio();
}

static void on_time_synced(struct timeval *tv)
{
    printf("device_wifi: SNTP time sync complete\n");
    /* Let SNTP finish its bookkeeping, then silence the radio. */
    const esp_timer_create_args_t timer_args = {
        .callback = radio_off_timer_cb,
        .name = "radio_off",
    };
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        esp_timer_create(&timer_args, &timer);
        esp_timer_start_once(timer, 3 * 1000 * 1000);
    }
}

static void give_up_offline(void);

static void station_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    WIFI_TRACE("station_event base=%s id=%d\n", base == WIFI_EVENT ? "WIFI" : "IP", (int)id);
    /* Connecting is procedural (scan + pick, then connect) — STA_START is
       not a trigger here. */
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_STOP || id == WIFI_EVENT_STA_START)) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (station_ever_connected) {
            /* Mid-window drops retry; outside a window stay quiet. */
            if (window_mode) esp_wifi_connect();
            return;
        }
        wifi_event_sta_disconnected_t *event = data;
        printf("device_wifi: disconnected from '%.32s' reason=%d rssi=%d\n",
               (const char *)event->ssid, (int)event->reason, (int)event->rssi);
        int limit = 15;
        if (++station_failures >= limit) {
            const char *ssid = active_index >= 0 ? networks[active_index].ssid : "?";
            if (validating_boot) {
                /* Unproven portal creds: assume a bad password. */
                printf("device_wifi: cannot join '%s' — rebooting into setup portal\n", ssid);
                char message[96];
                snprintf(message, sizeof(message),
                         "Couldn't join '%.32s' — check the password.", ssid);
                set_last_error(message);
                set_u8("validating", 0);
                set_u8("force_portal", 1);
                esp_restart();
            }
            /* Proven creds, network unreachable (rebooted away from home):
               continue offline rather than trapping in setup. */
            give_up_offline();
            return;
        }
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        station_ever_connected = true;
        offline = false;
        if (validating_index >= 0) {
            set_u8("validating", 0);
            validating_index = -1;
        }
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            sntp_set_time_sync_notification_cb(on_time_synced);
            esp_sntp_init();
        }
        printf("device_wifi: connected to '%s'\n",
               active_index >= 0 ? networks[active_index].ssid : "?");
        if (window_events != NULL) {
            xEventGroupSetBits(window_events, WINDOW_GOT_IP_BIT);
        }
    }
}

/* Blocking all-channel scan; returns the index of the best known network
   (a validating entry wins outright so fresh creds get proven). */
static int pick_network(void)
{
    wifi_scan_config_t scan_config = {0};
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return -1;
    uint16_t record_count = 20;
    static wifi_ap_record_t records[20];
    if (esp_wifi_scan_get_ap_records(&record_count, records) != ESP_OK) return -1;
    int best = -1;
    int best_rssi = -128;
    for (int r = 0; r < record_count; r++) {
        for (int n = 0; n < network_count; n++) {
            if (strcmp((const char *)records[r].ssid, networks[n].ssid) != 0) continue;
            if (n == validating_index) return n;
            if (records[r].rssi > best_rssi) {
                best_rssi = records[r].rssi;
                best = n;
            }
        }
    }
    return best;
}

/* Scans and connects to the best visible known network. False if none. */
static bool connect_best(void)
{
    int index = pick_network();
    if (index < 0) {
        offline = true;
        return false;
    }
    active_index = index;
    printf("device_wifi: joining '%s'\n", networks[index].ssid);
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, networks[index].ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, networks[index].password, sizeof(config.sta.password));
    /* WPA2/WPA3-transition routers reject non-PMF clients with AUTH_FAIL
       (reason 202) — a zeroed config disables PMF capability. */
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    /* Mesh networks broadcast several BSSIDs; scan all channels and take
       the strongest 2.4GHz one. */
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_connect();
    return true;
}

static void give_up_offline(void)
{
    printf("device_wifi: no known network reachable — continuing offline\n");
    offline = true;
    esp_wifi_stop();
    device_flush_gate_open();
    radio_active = false;
    device_state_release_radio();
}

static void station_start(void)
{
    esp_netif_create_default_wifi_sta();
    ensure_wifi_inited();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, station_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, station_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    device_flush_gate_close();
    ESP_ERROR_CHECK(esp_wifi_start());
    radio_active = true;
    /* Modem power-save transitions can glitch the QSPI display pipeline. */
    esp_wifi_set_ps(WIFI_PS_NONE);
#ifdef FROLIC_DEBUG
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
#endif
    if (!connect_best()) {
        if (validating_index >= 0) {
            char message[96];
            snprintf(message, sizeof(message),
                     "Couldn't find '%.32s' — is it in range?",
                     networks[validating_index].ssid);
            set_last_error(message);
            set_u8("validating", 0);
            set_u8("force_portal", 1);
            esp_restart();
        }
        give_up_offline();
    }
}

/* ---------- captive portal (setup mode) ---------- */

/* Minimal DNS server answering every A query with the portal IP, so phones
   auto-open the setup page when they join the network. */
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t packet[256];
    while (true) {
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        int length = recvfrom(sock, packet, sizeof(packet) - 16, 0,
                              (struct sockaddr *)&from, &from_length);
        if (length < 12) continue;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
        memcpy(packet + length, answer, sizeof(answer));
        sendto(sock, packet, length + sizeof(answer), 0, (struct sockaddr *)&from, from_length);
    }
}

static const char PORTAL_HEAD[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>pocket pikachu</title><style>"
    "body{font-family:-apple-system,sans-serif;background:#88c878;margin:0;padding:24px;}"
    ".card{max-width:340px;margin:40px auto;background:#f8f8e8;border:4px solid #585048;"
    "border-radius:12px;padding:24px;}h1{font-size:20px;margin:0 0 4px;}p{color:#585048;margin:0 0 16px;}"
    ".err{color:#c03a2f;font-weight:600;}"
    "input,select{width:100%;box-sizing:border-box;font-size:16px;padding:10px;margin:6px 0 14px;"
    "border:2px solid #585048;border-radius:8px;background:#fff;}"
    "button{width:100%;font-size:16px;padding:12px;background:#f8d030;border:2px solid #585048;"
    "border-radius:8px;font-weight:700;}"
    "</style></head><body><div class=card><h1>&#9889; pocket pikachu</h1>"
    "<p>Pick the wifi Raichu should use. He'll remember it and reboot.</p>";

static const char PORTAL_FORM[] =
    "<form method=POST action=/save>"
    "<label>Network<select name=ssid "
    "onchange=\"document.getElementById('o').style.display=this.value?'none':'block'\">";

static const char PORTAL_TAIL[] =
    "<option value=''>Other...</option></select></label>"
    "<div id=o style='display:none'><label>Network name"
    "<input name=ssid_other autocapitalize=off autocorrect=off></label></div>"
    "<label>Password<input name=password type=password></label>"
    "<button>Save &amp; restart</button></form></div></body></html>";

static const char SAVED_PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>saved</title></head><body style='font-family:sans-serif;background:#88c878;"
    "text-align:center;padding:80px 24px 0;'><h1>&#9889; Got it!</h1>"
    "<p>Raichu is rebooting to try your wifi.</p>"
    "<p>If it can't join, the <b>pocket-pet</b> network comes back in about half a "
    "minute — rejoin it to see what went wrong and retry.</p></body></html>";

static void url_decode(char *text)
{
    char *out = text;
    for (char *in = text; *in != '\0'; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            char hex[3] = {in[1], in[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

/* Networks are scanned once at portal start; the handler renders from the
   static list (scan buffers overflow the httpd task stack). */
#define PORTAL_MAX_NETWORKS 15
static char portal_networks[PORTAL_MAX_NETWORKS][33];
static int portal_network_count;

static void portal_scan_networks(void)
{
    wifi_scan_config_t scan_config = {0};
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return;
    static wifi_ap_record_t records[20];
    uint16_t count = 20;
    esp_wifi_scan_get_ap_records(&count, records);
    for (int i = 0; i < count && portal_network_count < PORTAL_MAX_NETWORKS; i++) {
        const char *name = (const char *)records[i].ssid;
        if (name[0] == '\0') continue;
        bool duplicate = false;
        for (int j = 0; j < portal_network_count; j++) {
            if (strcmp(portal_networks[j], name) == 0) duplicate = true;
        }
        if (duplicate) continue;
        strlcpy(portal_networks[portal_network_count++], name, 33);
    }
    printf("device_wifi: portal scan found %d networks\n", portal_network_count);
}

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    if (strcmp(request->uri, "/") == 0) {
        httpd_resp_set_type(request, "text/html");
        httpd_resp_send_chunk(request, PORTAL_HEAD, HTTPD_RESP_USE_STRLEN);
        if (last_error[0] != '\0') {
            char banner[160];
            snprintf(banner, sizeof(banner), "<p class=err>%s</p>", last_error);
            httpd_resp_send_chunk(request, banner, HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_send_chunk(request, PORTAL_FORM, HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < portal_network_count; i++) {
            char option[64];
            snprintf(option, sizeof(option), "<option>%s</option>", portal_networks[i]);
            httpd_resp_send_chunk(request, option, HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_send_chunk(request, PORTAL_TAIL, HTTPD_RESP_USE_STRLEN);
        return httpd_resp_send_chunk(request, NULL, 0);
    }
    /* Captive-portal probes and everything else: bounce to the form. */
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t portal_save_handler(httpd_req_t *request)
{
    char body[256] = {0};
    int received = httpd_req_recv(request, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;

    char ssid[64] = {0};
    char password[96] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    if (ssid[0] == '\0') {
        httpd_query_key_value(body, "ssid_other", ssid, sizeof(ssid));
    }
    httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(password);
    if (ssid[0] != '\0') {
        printf("device_wifi: saving ssid='%s' password_length=%d\n", ssid, (int)strlen(password));
        save_credentials(ssid, password);
    }

    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, SAVED_PAGE, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static bool wifi_inited;
static esp_netif_t *portal_ap_netif;
static httpd_handle_t portal_server;
static bool dns_task_spawned;

static void ensure_wifi_inited(void)
{
    if (wifi_inited) return;
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    wifi_inited = true;
}

/* Runtime-safe: callable at boot (force_portal) or from a live session
   (wifi-icon tap). The radio must be silent on entry. */
static void portal_start(void)
{
    load_and_clear_last_error();

    if (portal_ap_netif == NULL) portal_ap_netif = esp_netif_create_default_wifi_ap();
    ensure_wifi_inited();
    wifi_config_t config = {
        .ap = {
            .ssid = SETUP_SSID,
            .ssid_len = strlen(SETUP_SSID),
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    /* APSTA only so the scan works — the STA interface never connects here. */
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    device_flush_gate_close();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Offer ourselves as DNS in DHCP leases — without this clients have no
       DNS at all, the captive probe never resolves, and no sheet appears. */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(PORTAL_IP);
    esp_netif_dhcps_stop(portal_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(portal_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    dhcps_offer_t dns_offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(portal_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dns_offer, sizeof(dns_offer)));
    esp_netif_dhcps_start(portal_ap_netif);

    portal_scan_networks();
    if (!dns_task_spawned) {
        /* Blocks in recvfrom when the AP is down — safe to keep across
           portal sessions. */
        xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, NULL);
        dns_task_spawned = true;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 8192;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&portal_server, &server_config));
    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = portal_save_handler};
    httpd_uri_t any_uri = {.uri = "/*", .method = HTTP_GET, .handler = portal_get_handler};
    httpd_register_uri_handler(portal_server, &save_uri);
    httpd_register_uri_handler(portal_server, &any_uri);
    printf("device_wifi: setup portal at http://%s (join '%s')\n", PORTAL_IP, SETUP_SSID);
    in_portal = true;
    radio_active = true;
}

/* Sync windows: briefly raise the radio outside the boot sync. The caller
   owns the window and must end it; the main loop's radio-active handling
   (pet paused, banner) covers the visuals. */
bool device_wifi_window_begin(uint32_t timeout_ms)
{
    if (in_portal || radio_active || network_count == 0) return false;
    if (window_events == NULL) window_events = xEventGroupCreate();
    xEventGroupClearBits(window_events, WINDOW_GOT_IP_BIT);
    window_mode = true;
    /* The device state machine only grants windows from DOZING, so the
       screen is already dark and static here. */
    radio_active = true;
    device_flush_gate_close();
    if (esp_wifi_start() != ESP_OK) {
        device_flush_gate_open();
        radio_active = false;
        window_mode = false;
        return false;
    }
    if (!connect_best()) {
        /* No known network in range: a ~2s scan, not a 12s timeout. */
        esp_wifi_stop();
        device_flush_gate_open();
        radio_active = false;
        window_mode = false;
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(window_events, WINDOW_GOT_IP_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & WINDOW_GOT_IP_BIT) == 0) {
        printf("device_wifi: window connect timed out\n");
        device_wifi_window_end();
        return false;
    }
    return true;
}

void device_wifi_window_end(void)
{
    esp_wifi_stop();
    device_flush_gate_open();
    window_mode = false;
    radio_active = false;
}

bool device_wifi_is_offline(void)
{
    return offline;
}

static void enter_portal_task(void *arg)
{
    (void)arg;
    if (!in_portal && !radio_active) {
        printf("device_wifi: entering setup portal (runtime)\n");
        device_state_portal();
        portal_start();
    }
    vTaskDelete(NULL);
}

void device_wifi_request_portal(void)
{
    /* Safe from LVGL context: the heavy lifting (which must wait on the
       LVGL task via the flush gate) runs in its own task. */
    xTaskCreate(enter_portal_task, "portal_in", 4096, NULL, 4, NULL);
}

void device_wifi_portal_exit(void)
{
    if (!in_portal) return;
    printf("device_wifi: leaving setup portal\n");
    if (portal_server != NULL) {
        httpd_stop(portal_server);
        portal_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    device_flush_gate_open();
    in_portal = false;
    radio_active = false;
    device_state_portal_exit();
}

bool device_wifi_in_portal(void)
{
    return in_portal;
}

bool device_wifi_radio_active(void)
{
    return radio_active;
}

void device_wifi_start(void)
{
    /* Local clock renders in London time, DST-aware, once SNTP lands. */
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Escape hatch: hold BOOT while powering on to re-enter wifi setup. */
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&button);
    if (gpio_get_level(BOOT_BUTTON) == 0) {
        printf("device_wifi: BOOT held at boot — clearing stored wifi\n");
        erase_credentials();
    }

    bool force_portal = get_u8("force_portal") != 0;
    if (force_portal) set_u8("force_portal", 0);

    if (!force_portal && load_credentials()) {
        validating_boot = get_u8("validating") != 0;
        validating_index = validating_boot ? (int)get_u8("validating_idx") : -1;
        if (validating_index >= network_count) validating_index = network_count - 1;
        /* Freeze the scene BEFORE the radio, and give the first full paint
           a moment to drain — the boot scan corrupts in-flight flushes. */
        device_state_boot_sync();
        vTaskDelay(pdMS_TO_TICKS(1200));
        station_start();
    } else {
        device_state_portal();
        portal_start();
    }
}
