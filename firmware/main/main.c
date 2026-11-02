// PhotoPainter photo frame.
// On battery: once a day around midnight, or when BOOT/KEY is pressed, fetch one picture from
// Home Assistant, show it, and return to deep sleep.
// On USB power: stay awake with Wi-Fi on, change the picture every 5 minutes and on BOOT/KEY.
// KEY and scheduled updates advance the shuffled list; BOOT moves to the previous photo.

#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "frame";

// Wake around 00:05 local time; retry sooner when the clock is unknown or the update failed.
#define WAKE_MINUTE_OF_DAY 5
#define RETRY_SECONDS (60 * 60)
#define TZ_CHINA "CST-8"

#define USB_INTERVAL_S (5 * 60)
#define USB_CHECK_MS 5000
#define BUTTON_POLL_MS 50
#define BUTTON_DEBOUNCE_MS 30

typedef enum {
    WAIT_TIMEOUT,
    WAIT_NEXT,
    WAIT_PREVIOUS,
    WAIT_UNPLUGGED,
} wait_result_t;

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

// Fetches and shows one picture. Wi-Fi is turned off before the panel refresh unless keep_wifi is set.
static bool update_picture(const battery_status_t *battery, bool keep_wifi, photo_direction_t direction)
{
    size_t len = 0;
    uint8_t *photo = NULL;
    if (net_connect()) {
        photo = net_fetch_photo(battery, direction, &len);
    }
    if (!keep_wifi) {
        net_stop();
    }
    bool shown = false;
    if (photo) {
        if (epd_init()) {
            shown = epd_show_portrait_bmp(photo, len);
            epd_sleep();
        }
        heap_caps_free(photo);
    }
    return shown;
}

static bool button_down(void)
{
    return gpio_get_level(PIN_BOOT) == 0 || gpio_get_level(PIN_KEY) == 0;
}

// Waits until `until_us`, a fresh BOOT/KEY press, or removal of USB power.
static wait_result_t wait_on_usb(int64_t until_us)
{
    bool armed = !button_down();
    int64_t next_usb_check = esp_timer_get_time() + USB_CHECK_MS * 1000LL;
    while (esp_timer_get_time() < until_us) {
        if (!button_down()) {
            armed = true;
        } else if (armed) {
            gpio_num_t pin = gpio_get_level(PIN_BOOT) == 0 ? PIN_BOOT : PIN_KEY;
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(pin) == 0) {
                return pin == PIN_BOOT ? WAIT_PREVIOUS : WAIT_NEXT;
            }
        }
        if (esp_timer_get_time() >= next_usb_check) {
            next_usb_check += USB_CHECK_MS * 1000LL;
            if (!pmu_usb()) {
                return WAIT_UNPLUGGED;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    return WAIT_TIMEOUT;
}

static void deep_sleep(uint64_t seconds)
{
    net_stop();
    // A held button must be released before enabling level-triggered wake-up.
    while (button_down()) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
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
    uint64_t wake_pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
    photo_direction_t direction = (wake_pins & (1ULL << PIN_BOOT)) ? PHOTO_PREVIOUS : PHOTO_NEXT;
    ESP_LOGI(TAG, "wake cause %d", cause);

    board_init();
    pmu_init();
    battery_status_t battery = pmu_battery();
    bool shown = update_picture(&battery, battery.usb, direction);

    if (battery.usb) {
        ESP_LOGI(TAG, "USB power: Wi-Fi stays on, new picture every %d s", USB_INTERVAL_S);
        for (;;) {
            wait_result_t result = wait_on_usb(esp_timer_get_time() + USB_INTERVAL_S * 1000000LL);
            if (result == WAIT_UNPLUGGED) {
                ESP_LOGI(TAG, "USB removed: daily updates on battery");
                break;
            }
            battery = pmu_battery();
            direction = result == WAIT_PREVIOUS ? PHOTO_PREVIOUS : PHOTO_NEXT;
            shown = update_picture(&battery, true, direction);
        }
    }

    uint64_t sleep_s = seconds_until_next_wake();
    if (!shown && sleep_s > RETRY_SECONDS) {
        sleep_s = RETRY_SECONDS;
    }
    deep_sleep(sleep_s);
}
