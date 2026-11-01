#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "frame_secrets.h"

static const char *TAG = "net";
static EventGroupHandle_t s_events;
static bool s_started;
static int s_retries;

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1
#define WIFI_TIMEOUT_MS 20000
#define MAX_PHOTO_BYTES (1300 * 1024)

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_retries <= 5) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static bool wifi_connect(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t conf = {0};
    strlcpy((char *)conf.sta.ssid, FRAME_WIFI_SSID, sizeof(conf.sta.ssid));
    strlcpy((char *)conf.sta.password, FRAME_WIFI_PASS, sizeof(conf.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &conf));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        return false;
    }
    return true;
}

// Home Assistant sends its Unix time in X-Frame-Time; the frame keeps its clock from it.
static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "X-Frame-Time") == 0) {
        long long now = atoll(event->header_value);
        if (now > 1700000000LL) {
            struct timeval tv = {.tv_sec = (time_t)now};
            settimeofday(&tv, NULL);
        }
    }
    return ESP_OK;
}

uint8_t *net_fetch_photo(const battery_status_t *battery, size_t *out_len)
{
    *out_len = 0;
    if (!wifi_connect()) {
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s?token=%s&battery=%d&charging=%d", FRAME_HA_URL, FRAME_HA_TOKEN, battery->percent,
             battery->charging || battery->usb ? 1 : 0);
    esp_http_client_config_t conf = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .event_handler = on_http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&conf);
    uint8_t *body = NULL;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed");
        goto done;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || length <= 0 || length > MAX_PHOTO_BYTES) {
        ESP_LOGE(TAG, "HTTP status %d length %lld", status, (long long)length);
        goto done;
    }
    body = heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
    if (!body) {
        goto done;
    }
    size_t received = 0;
    while (received < (size_t)length) {
        int n = esp_http_client_read(client, (char *)body + received, length - received);
        if (n <= 0) {
            break;
        }
        received += n;
    }
    if (received != (size_t)length) {
        ESP_LOGE(TAG, "short read %u/%lld", (unsigned)received, (long long)length);
        heap_caps_free(body);
        body = NULL;
        goto done;
    }
    *out_len = received;
    ESP_LOGI(TAG, "received %u bytes", (unsigned)received);
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return body;
}

void net_stop(void)
{
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        s_started = false;
    }
}
