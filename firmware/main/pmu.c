#include "frame.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// AXP2101 power management IC on I2C address 0x34.
#define AXP_ADDR 0x34
#define REG_STATUS1 0x00
#define REG_STATUS2 0x01
#define REG_SLEEP_WAKEUP 0x26
#define REG_ADC_CTRL 0x30
#define REG_INTEN1 0x40
#define REG_INTSTS1 0x48
#define REG_BAT_DET 0x68
#define REG_DC_ONOFF 0x80
#define REG_DC1_VOL 0x82
#define REG_LDO_ONOFF0 0x90
#define REG_LDO_ONOFF1 0x91
#define REG_ALDO4_VOL 0x95
#define REG_BAT_PERCENT 0xA4

static const char *TAG = "pmu";
static i2c_master_dev_handle_t s_pmu;

static int pmu_read(uint8_t reg)
{
    uint8_t value = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (i2c_master_transmit_receive(s_pmu, &reg, 1, &value, 1, 100) == ESP_OK) {
            return value;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}

static bool pmu_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[] = {reg, value};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (i2c_master_transmit(s_pmu, buf, sizeof(buf), 100) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static void pmu_update(uint8_t reg, uint8_t clear, uint8_t set)
{
    int value = pmu_read(reg);
    if (value >= 0) {
        pmu_write(reg, (uint8_t)((value & ~clear) | set));
    }
}

bool pmu_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(board_i2c(), &dev, &s_pmu) != ESP_OK || pmu_read(REG_STATUS1) < 0) {
        ESP_LOGE(TAG, "AXP2101 not found");
        return false;
    }
    // Leave PMIC sleep mode, then enable battery detection and voltage ADC for the fuel gauge.
    pmu_update(REG_SLEEP_WAKEUP, 0x01, 0);
    pmu_update(REG_BAT_DET, 0, 0x01);
    pmu_update(REG_ADC_CTRL, 0, 0x01);
    // DCDC1 3.3 V (system) and ALDO4 3.3 V (e-paper).
    pmu_write(REG_DC1_VOL, (3300 - 1500) / 100);
    pmu_update(REG_DC_ONOFF, 0, 0x01);
    pmu_update(REG_ALDO4_VOL, 0x1F, (3300 - 500) / 100);
    pmu_update(REG_LDO_ONOFF0, 0, 1 << 3);
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
}

battery_status_t pmu_battery(void)
{
    battery_status_t status = {.percent = -1};
    int s1 = pmu_read(REG_STATUS1);
    int s2 = pmu_read(REG_STATUS2);
    if (s1 < 0 || s2 < 0) {
        return status;
    }
    status.usb = (s1 & (1 << 5)) && !(s2 & (1 << 3));
    status.charging = ((s2 >> 5) == 0x01);
    if (s1 & (1 << 3)) {
        int pct = pmu_read(REG_BAT_PERCENT);
        if (pct >= 0 && pct <= 100) {
            status.percent = pct;
        }
    }
    ESP_LOGI(TAG, "battery=%d%% charging=%d usb=%d", status.percent, status.charging, status.usb);
    return status;
}

void pmu_sleep_rails(void)
{
    if (!s_pmu) {
        return;
    }
    // Disable and clear all PMIC interrupts.
    for (int i = 0; i < 3; i++) {
        pmu_write(REG_INTEN1 + i, 0x00);
        pmu_write(REG_INTSTS1 + i, 0xFF);
    }
    // Wake sources for PMIC sleep: IRQ pin low; DCDC/LDO voltages restored on wake.
    pmu_update(REG_SLEEP_WAKEUP, 1 << 3, (1 << 2) | (1 << 4));
    pmu_update(REG_ADC_CTRL, 0x01, 0);
    pmu_update(REG_BAT_DET, 0x01, 0);
    pmu_update(REG_SLEEP_WAKEUP, 0, 0x01);
    // DC2-DC5 off, keep DC1 (ESP32 supply).
    pmu_update(REG_DC_ONOFF, 0x1E, 0);
    // ALDO1/2, BLDO1/2, CPUSLDO, DLDO1 off; ALDO4 (panel) and ALDO3 (audio) last.
    pmu_update(REG_LDO_ONOFF0, (1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7), 0);
    pmu_update(REG_LDO_ONOFF1, 0x01, 0);
    pmu_update(REG_LDO_ONOFF0, (1 << 3) | (1 << 2), 0);
}
