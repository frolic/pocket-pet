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
#include "device_wifi.h"

#define SETUP_SSID "pocket-pikachu"
#define PORTAL_IP "192.168.4.1"
#define BOOT_BUTTON GPIO_NUM_0

static char stored_ssid[33];
static char stored_password[65];

/* ---------- credential store ---------- */

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

/* ---------- station + SNTP (normal operation) ---------- */

static void station_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && !esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
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
        packet[2] = 0x81; /* response, recursion available */
        packet[3] = 0x80;
        packet[6] = 0;    /* one answer */
        packet[7] = 1;
        uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
        memcpy(packet + length, answer, sizeof(answer));
        sendto(sock, packet, length + sizeof(answer), 0, (struct sockaddr *)&from, from_length);
    }
}

static const char PORTAL_PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>pocket pikachu</title><style>"
    "body{font-family:-apple-system,sans-serif;background:#88c878;margin:0;padding:24px;}"
    ".card{max-width:340px;margin:40px auto;background:#f8f8e8;border:4px solid #585048;"
    "border-radius:12px;padding:24px;}h1{font-size:20px;margin:0 0 4px;}p{color:#585048;margin:0 0 16px;}"
    "input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;margin:6px 0 14px;"
    "border:2px solid #585048;border-radius:8px;background:#fff;}"
    "button{width:100%;font-size:16px;padding:12px;background:#f8d030;border:2px solid #585048;"
    "border-radius:8px;font-weight:700;}"
    "</style></head><body><div class=card><h1>&#9889; pocket pikachu</h1>"
    "<p>Tell Raichu which wifi to use. He'll remember it and reboot.</p>"
    "<form method=POST action=/save>"
    "<label>Network name<input name=ssid autocapitalize=off autocorrect=off required></label>"
    "<label>Password<input name=password type=password></label>"
    "<button>Save &amp; restart</button></form></div></body></html>";

static const char SAVED_PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>saved</title></head><body style='font-family:sans-serif;background:#88c878;"
    "text-align:center;padding-top:80px;'><h1>&#9889; Got it!</h1>"
    "<p>Raichu is rebooting onto your wifi.</p></body></html>";

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

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    if (strcmp(request->uri, "/") == 0) {
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, PORTAL_PAGE, HTTPD_RESP_USE_STRLEN);
    }
    /* Captive-portal probes and everything else: bounce to the form. */
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(request, NULL, 0);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t portal_save_handler(httpd_req_t *request)
{
    char body[256] = {0};
    int received = httpd_req_recv(request, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;

    char ssid[64] = {0};
    char password[96] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(password);
    if (ssid[0] != '\0') save_credentials(ssid, password);

    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, SAVED_PAGE, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void portal_start(void)
{
    esp_netif_create_default_wifi_ap();
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, NULL);

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
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

    if (load_credentials()) {
        station_start();
    } else {
        portal_start();
    }
}
