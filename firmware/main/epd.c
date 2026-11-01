#include "frame.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 7.3" Spectra 6 (E6) panel, 800x480, 4 bits per pixel.
static const char *TAG = "epd";
static spi_device_handle_t s_spi;
static uint8_t *s_buf;

#define EPD_CHUNK 4000

static bool wait_busy(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_EPD_BUSY) == 0) {
        if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
            ESP_LOGE(TAG, "busy timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void spi_send(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void send_command(uint8_t cmd)
{
    gpio_set_level(PIN_EPD_DC, 0);
    gpio_set_level(PIN_EPD_CS, 0);
    spi_send(&cmd, 1);
    gpio_set_level(PIN_EPD_CS, 1);
}

static void send_data(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    spi_send(data, len);
    gpio_set_level(PIN_EPD_CS, 1);
}

static void command(uint8_t cmd, const uint8_t *data, size_t len)
{
    send_command(cmd);
    if (len) {
        send_data(data, len);
    }
}

#define CMD(c, ...)                                   \
    do {                                              \
        const uint8_t d_[] = {__VA_ARGS__};           \
        command((c), d_, sizeof(d_));                 \
    } while (0)

static bool epd_setup_bus(void)
{
    if (s_spi) {
        return true;
    }
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_EPD_BUSY, .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    gpio_set_level(PIN_EPD_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_EPD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    s_buf = heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no framebuffer");
        return false;
    }
    return true;
}

// Resets the panel and sends the init sequence; called before every refresh.
bool epd_init(void)
{
    if (!epd_setup_bus()) {
        return false;
    }

    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!wait_busy(5000)) {
        return false;
    }

    CMD(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);
    CMD(0x01, 0x3F);
    CMD(0x00, 0x5F, 0x69);
    CMD(0x03, 0x00, 0x54, 0x00, 0x44);
    CMD(0x05, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(0x06, 0x6F, 0x1F, 0x17, 0x49);
    CMD(0x08, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(0x30, 0x03);
    CMD(0x50, 0x3F);
    CMD(0x60, 0x02, 0x00);
    CMD(0x61, 0x03, 0x20, 0x01, 0xE0);
    CMD(0x84, 0x01);
    CMD(0xE3, 0x2F);
    send_command(0x04);  // power on
    return wait_busy(5000);
}

static void set_pixel(int x, int y, uint8_t color)
{
    uint32_t idx = (uint32_t)y * (EPD_WIDTH / 2) + (x >> 1);
    if (x & 1) {
        s_buf[idx] = (s_buf[idx] & 0xF0) | color;
    } else {
        s_buf[idx] = (s_buf[idx] & 0x0F) | (color << 4);
    }
}

static uint8_t nearest_color(uint8_t r, uint8_t g, uint8_t b)
{
    static const struct {
        uint8_t r, g, b, index;
    } palette[] = {
        {0, 0, 0, EPD_BLACK},     {255, 255, 255, EPD_WHITE}, {255, 255, 0, EPD_YELLOW},
        {255, 0, 0, EPD_RED},     {0, 0, 255, EPD_BLUE},      {0, 255, 0, EPD_GREEN},
    };
    uint8_t best = EPD_WHITE;
    int best_d = INT32_MAX;
    for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        int dr = r - palette[i].r, dg = g - palette[i].g, db = b - palette[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = palette[i].index;
        }
    }
    return best;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void refresh(void)
{
    send_command(0x10);
    // SPI DMA cannot read PSRAM directly; bounce through internal RAM.
    static uint8_t chunk[EPD_CHUNK];
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    for (size_t off = 0; off < EPD_BUF_SIZE; off += EPD_CHUNK) {
        size_t n = EPD_BUF_SIZE - off < EPD_CHUNK ? EPD_BUF_SIZE - off : EPD_CHUNK;
        memcpy(chunk, s_buf + off, n);
        spi_send(chunk, n);
    }
    gpio_set_level(PIN_EPD_CS, 1);

    send_command(0x04);
    wait_busy(5000);
    CMD(0x06, 0x6F, 0x1F, 0x17, 0x49);
    CMD(0x12, 0x00);  // refresh, about 25 s
    wait_busy(60000);
    CMD(0x02, 0x00);  // power off
    wait_busy(5000);
}

bool epd_show_portrait_bmp(const uint8_t *bmp, size_t len)
{
    if (len < 54 || bmp[0] != 'B' || bmp[1] != 'M') {
        ESP_LOGE(TAG, "not a BMP");
        return false;
    }
    uint32_t offset = le32(bmp + 10);
    int32_t width = (int32_t)le32(bmp + 18);
    int32_t height = (int32_t)le32(bmp + 22);
    uint16_t bpp = bmp[28] | (bmp[29] << 8);
    uint32_t compression = le32(bmp + 30);
    bool bottom_up = height > 0;
    if (height < 0) {
        height = -height;
    }
    size_t stride = ((size_t)width * 3 + 3) & ~(size_t)3;
    if (width != PHOTO_WIDTH || height != PHOTO_HEIGHT || bpp != 24 || compression != 0 ||
        offset + stride * height > len) {
        ESP_LOGE(TAG, "unsupported BMP %ldx%ld %u bpp", (long)width, (long)height, bpp);
        return false;
    }

    // Portrait pixel (x, y) is shown at panel pixel (y, 479 - x).
    for (int y = 0; y < PHOTO_HEIGHT; y++) {
        const uint8_t *row = bmp + offset + stride * (bottom_up ? PHOTO_HEIGHT - 1 - y : y);
        for (int x = 0; x < PHOTO_WIDTH; x++) {
            const uint8_t *px = row + x * 3;
            set_pixel(y, EPD_HEIGHT - 1 - x, nearest_color(px[2], px[1], px[0]));
        }
    }
    refresh();
    return true;
}

void epd_sleep(void)
{
    if (s_spi) {
        CMD(0x07, 0xA5);
    }
}
