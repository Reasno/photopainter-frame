// PhotoPainter photo frame: once a day around midnight, or when BOOT/KEY is pressed,
// fetch one picture from Home Assistant, show it, and return to deep sleep.

#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "frame";

// Wake around 00:05 local time; retry sooner when the clock is unknown or the update failed.
#define WAKE_MINUTE_OF_DAY 5
#define RETRY_SECONDS (60 * 60)
#define TZ_CHINA "CST-8"

static uint64_t seconds_until_next_wake(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return RETRY_SECONDS;
    }
    struct tm local;
    localtime_r(&now, &local);
    int seconds_today = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int target = WAKE_MINUTE_OF_DAY * 60;
    int64_t wait = target - seconds_today;
    if (wait <= 60) {
        wait += 24 * 3600;
    }
    return (uint64_t)wait;
}

static void deep_sleep(uint64_t seconds)
{
    net_stop();
    epd_sleep();
    board_prepare_sleep();
    pmu_sleep_rails();
    ESP_LOGI(TAG, "deep sleep for %llu s", (unsigned long long)seconds);
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}

void app_main(void)
{
    setenv("TZ", TZ_CHINA, 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "wake cause %d", cause);

    board_init();
    pmu_init();
    battery_status_t battery = pmu_battery();

    size_t len = 0;
    uint8_t *photo = net_fetch_photo(&battery, &len);
    net_stop();

    bool shown = false;
    if (photo) {
        if (epd_init()) {
            shown = epd_show_portrait_bmp(photo, len);
        }
        heap_caps_free(photo);
    }

    uint64_t sleep_s = seconds_until_next_wake();
    if (!shown && sleep_s > RETRY_SECONDS) {
        sleep_s = RETRY_SECONDS;
    }
    deep_sleep(sleep_s);
}
