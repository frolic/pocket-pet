#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "device_wifi.h"
#include "step_source.h"
#include "battery_source.h"

/*
 * Dev OTA loop: every interval, open a radio window, compare the version the
 * Mac's build server advertises against the running build, and self-update
 * (or just drop a telemetry ping into the server's access log). Compiled in
 * only when FROLIC_OTA_URL is defined (dev builds).
 */

#ifdef FROLIC_OTA_URL

#define OTA_INTERVAL_MS 90000
#define OTA_FIRST_CHECK_MS 20000

static bool fetch_version(char *out, size_t out_size)
{
    esp_http_client_config_t config = {
        .url = FROLIC_OTA_URL "/version.txt",
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int length = esp_http_client_read(client, out, out_size - 1);
        if (length > 0 && esp_http_client_get_status_code(client) == 200) {
            out[length] = '\0';
            char *newline = strpbrk(out, "\r\n");
            if (newline != NULL) *newline = '\0';
            ok = out[0] != '\0';
        }
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void send_telemetry(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             FROLIC_OTA_URL "/telemetry?up=%lld&steps=%lu&bat=%d&qmi=%d&version=%s",
             esp_timer_get_time() / 1000000,
             (unsigned long)step_source_total(),
             battery_source_percent(),
             step_source_status(),
             esp_app_get_description()->version);
    esp_http_client_config_t config = {.url = url, .timeout_ms = 3000};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return;
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

static void run_update(void)
{
    printf("device_ota: new build available — updating\n");
    esp_http_client_config_t http_config = {
        .url = FROLIC_OTA_URL "/frolic.bin",
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {.http_config = &http_config};
    esp_err_t result = esp_https_ota(&ota_config);
    if (result == ESP_OK) {
        printf("device_ota: update written — rebooting\n");
        esp_restart();
    }
    printf("device_ota: update failed (%s)\n", esp_err_to_name(result));
}

static void ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_MS));
    while (true) {
        if (device_wifi_window_begin(12000)) {
            char version[64];
            if (fetch_version(version, sizeof(version))) {
                const char *running = esp_app_get_description()->version;
                if (strcmp(version, running) != 0) {
                    run_update(); /* only returns on failure */
                } else {
                    send_telemetry();
                }
            }
            device_wifi_window_end();
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_INTERVAL_MS));
    }
}

void device_ota_start(void)
{
    printf("device_ota: dev loop armed (%s, running %s)\n",
           FROLIC_OTA_URL, esp_app_get_description()->version);
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
}

#else

void device_ota_start(void)
{
}

#endif
