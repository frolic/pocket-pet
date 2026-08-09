#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "device_wifi.h"

#define SETUP_SSID "pocket-pikachu"
#define PORTAL_IP "192.168.4.1"
#define BOOT_BUTTON GPIO_NUM_0

/*
 * Provisioning design note: credentials are validated by REBOOTING into pure
 * station mode, never by connecting the STA interface while the softAP runs.
 * APSTA station-connect forces radio channel-switching that corrupts the QSPI
 * display pipeline on this board (and made association itself unreliable).
 * A failed attempt records an error and reboots back into the portal.
 */

static char stored_ssid[33];
static char stored_password[65];
static char last_error[96];

/* ---------- NVS ---------- */

static bool load_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_size = sizeof(stored_ssid);
    size_t password_size = sizeof(stored_password);
    bool ok = nvs_get_str(handle, "ssid", stored_ssid, &ssid_size) == ESP_OK &&
              nvs_get_str(handle, "password", stored_password, &password_size) == ESP_OK;
    nvs_close(handle);
    return ok && stored_ssid[0] != '\0';
}

static void save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "password", password));
    ESP_ERROR_CHECK(nvs_set_u8(handle, "validating", 1));
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

static int station_failures;
static bool station_ever_connected;

static void station_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *event = data;
            printf("device_wifi: disconnected from '%.32s' reason=%d rssi=%d\n",
                   (const char *)event->ssid, (int)event->reason, (int)event->rssi);
        }
        if (id == WIFI_EVENT_STA_DISCONNECTED && !station_ever_connected) {
            int limit = 15;
            if (++station_failures >= limit) {
                printf("device_wifi: cannot join '%s' — rebooting into setup portal\n", stored_ssid);
                char message[96];
                snprintf(message, sizeof(message),
                         "Couldn't join '%.32s' — check the password.", stored_ssid);
                set_last_error(message);
                set_u8("validating", 0);
                set_u8("force_portal", 1);
                esp_restart();
            }
        }
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        station_ever_connected = true;
        set_u8("validating", 0);
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
        printf("device_wifi: connected to '%s'\n", stored_ssid);
    }
}

static void station_start(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, station_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, station_event, NULL));

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, stored_ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, stored_password, sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem power-save transitions can glitch the QSPI display pipeline. */
    esp_wifi_set_ps(WIFI_PS_NONE);
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
    "<p>If it can't join, the <b>pocket-pikachu</b> network comes back in about half a "
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

static void portal_start(void)
{
    load_and_clear_last_error();

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    wifi_config_t config = {
        .ap = {
            .ssid = SETUP_SSID,
            .ssid_len = strlen(SETUP_SSID),
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    /* APSTA only so the scan works — the STA interface never connects here. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Offer ourselves as DNS in DHCP leases — without this clients have no
       DNS at all, the captive probe never resolves, and no sheet appears. */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(PORTAL_IP);
    esp_netif_dhcps_stop(ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    dhcps_offer_t dns_offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dns_offer, sizeof(dns_offer)));
    esp_netif_dhcps_start(ap_netif);

    portal_scan_networks();
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, NULL);

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 8192;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = portal_save_handler};
    httpd_uri_t any_uri = {.uri = "/*", .method = HTTP_GET, .handler = portal_get_handler};
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &any_uri);
    printf("device_wifi: setup portal at http://%s (join '%s')\n", PORTAL_IP, SETUP_SSID);
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
        station_start();
    } else {
        portal_start();
    }
}
