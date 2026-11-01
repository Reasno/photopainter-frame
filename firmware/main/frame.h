#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// Waveshare ESP32-S3-PhotoPainter pin map.
#define PIN_EPD_DC GPIO_NUM_8
#define PIN_EPD_CS GPIO_NUM_9
#define PIN_EPD_SCK GPIO_NUM_10
#define PIN_EPD_MOSI GPIO_NUM_11
#define PIN_EPD_RST GPIO_NUM_12
#define PIN_EPD_BUSY GPIO_NUM_13

#define PIN_I2C_SDA GPIO_NUM_47
#define PIN_I2C_SCL GPIO_NUM_48
#define PIN_AXP_IRQ GPIO_NUM_21

#define PIN_BOOT GPIO_NUM_0
#define PIN_KEY GPIO_NUM_4
#define PIN_AUDIO_PA GPIO_NUM_7
#define PIN_LED_GREEN GPIO_NUM_42
#define PIN_LED_RED GPIO_NUM_45

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define EPD_BUF_SIZE (EPD_WIDTH * EPD_HEIGHT / 2)

#define PHOTO_WIDTH 480
#define PHOTO_HEIGHT 800

// Panel color indices.
enum {
    EPD_BLACK = 0x0,
    EPD_WHITE = 0x1,
    EPD_YELLOW = 0x2,
    EPD_RED = 0x3,
    EPD_BLUE = 0x5,
    EPD_GREEN = 0x6,
};

typedef struct {
    int percent;  // -1 when no battery is present
    bool charging;
    bool usb;
} battery_status_t;

i2c_master_bus_handle_t board_i2c(void);
void board_init(void);
void board_leds_off(void);
void board_prepare_sleep(void);

bool pmu_init(void);
battery_status_t pmu_battery(void);
void pmu_sleep_rails(void);

bool epd_init(void);
// Converts a 480x800 24-bit BMP into the panel buffer and refreshes the screen.
bool epd_show_portrait_bmp(const uint8_t *bmp, size_t len);
void epd_sleep(void);

// Returns a heap buffer with the BMP (caller frees) or NULL.
uint8_t *net_fetch_photo(const battery_status_t *battery, size_t *out_len);
void net_stop(void);
