#include "frame.h"

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t s_i2c;

i2c_master_bus_handle_t board_i2c(void)
{
    return s_i2c;
}

void board_leds_off(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&conf);
    // LEDs are active low.
    gpio_set_level(PIN_LED_GREEN, 1);
    gpio_set_level(PIN_LED_RED, 1);
}

void board_init(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_LED_GREEN);
    gpio_hold_dis(PIN_LED_RED);
    gpio_hold_dis(PIN_AUDIO_PA);
    for (int pin = 0; pin <= 21; pin++) {
        if (rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_hold_dis(pin);
            rtc_gpio_deinit(pin);
        }
    }

    // BOOT and KEY buttons, active low.
    gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << PIN_BOOT) | (1ULL << PIN_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&buttons);

    // Keep the speaker amplifier off.
    gpio_config_t pa = {.pin_bit_mask = 1ULL << PIN_AUDIO_PA, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&pa);
    gpio_set_level(PIN_AUDIO_PA, 0);
    board_leds_off();

    // A low pulse on the AXP2101 IRQ pin wakes the PMIC from its sleep mode.
    gpio_config_t irq = {.pin_bit_mask = 1ULL << PIN_AXP_IRQ, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&irq);
    gpio_set_level(PIN_AXP_IRQ, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_AXP_IRQ, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    irq.mode = GPIO_MODE_INPUT;
    irq.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&irq);

    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));

    // SHTC3 sleep command; absence is harmless.
    i2c_master_dev_handle_t shtc3;
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x70, .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(s_i2c, &dev, &shtc3) == ESP_OK) {
        const uint8_t cmd[] = {0xB0, 0x98};
        if (i2c_master_transmit(shtc3, cmd, sizeof(cmd), 50) != ESP_OK) {
            ESP_LOGD(TAG, "SHTC3 not responding");
        }
        i2c_master_bus_rm_device(shtc3);
    }
}

void board_prepare_sleep(void)
{
    gpio_set_level(PIN_AUDIO_PA, 0);
    gpio_hold_en(PIN_AUDIO_PA);
    board_leds_off();
    gpio_hold_en(PIN_LED_GREEN);
    gpio_hold_en(PIN_LED_RED);
    // Digital pads keep their held level in deep sleep only with the global hold enabled.
    gpio_deep_sleep_hold_en();

    // Isolate unused RTC pads to avoid leakage.
    for (int pin = 0; pin <= 21; pin++) {
        if (!rtc_gpio_is_valid_gpio(pin) || pin == PIN_BOOT || pin == PIN_KEY || pin == PIN_AUDIO_PA ||
            pin == PIN_AXP_IRQ) {
            continue;
        }
        rtc_gpio_init(pin);
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_isolate(pin);
    }

    // BOOT (GPIO0) and KEY (GPIO4) wake the frame; both are active low with pull-ups.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    const gpio_num_t wake_pins[] = {PIN_BOOT, PIN_KEY};
    for (size_t i = 0; i < sizeof(wake_pins) / sizeof(wake_pins[0]); i++) {
        gpio_hold_dis(wake_pins[i]);
        rtc_gpio_init(wake_pins[i]);
        rtc_gpio_set_direction(wake_pins[i], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(wake_pins[i]);
        rtc_gpio_pulldown_dis(wake_pins[i]);
        rtc_gpio_hold_dis(wake_pins[i]);
    }
    esp_sleep_enable_ext1_wakeup_io((1ULL << PIN_BOOT) | (1ULL << PIN_KEY), ESP_EXT1_WAKEUP_ANY_LOW);
}
