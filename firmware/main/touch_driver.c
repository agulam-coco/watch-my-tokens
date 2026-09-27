/**
 * @file touch_driver.c
 * @brief CST816D touch driver implementation (ESP-IDF v5/v6 I2C master API).
 *        See touch_driver.h for the public API and register layout.
 */
#include "touch_driver.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Matches LCD_H_RES / LCD_V_RES in main.c — this board's screen is 240x240
#define TOUCH_SCREEN_W 240
#define TOUCH_SCREEN_H 240

// ── Module-level I2C handles ──────────────────────────────────────────────────
// bus_handle  → represents the physical I2C wires (SDA + SCL pins)
// dev_handle  → represents the CST816D chip sitting on those wires
static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;

// ─────────────────────────────────────────────────────────────────────────────
/**
 * @brief  Creates the I2C bus, adds the CST816D and hardware-resets it.
 * @return ESP_OK on success, or the I2C setup error. See touch_driver.h.
 */
esp_err_t cst816d_init(void)
{
    esp_err_t err;

    // ── Step A: Create the I2C master bus ─────────────────────────────────────
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_PIN_SDA,
        .scl_io_num = TOUCH_PIN_SCL,
        .glitch_ignore_cnt = TOUCH_I2C_GLITCH_CNT,
        .clk_source = TOUCH_I2C_CLK_SRC,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C bus created: SDA=GPIO%d SCL=GPIO%d @ %d Hz",
             TOUCH_PIN_SDA, TOUCH_PIN_SCL, TOUCH_I2C_FREQ_HZ);

    // ── Step B: Add the CST816D as a device on the bus ────────────────────────
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_FREQ_HZ,
    };

    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "CST816D device registered at I2C address 0x%02X", TOUCH_I2C_ADDR);

    // ── Step C: Hardware reset of the CST816D ─────────────────────────────────
    gpio_reset_pin(TOUCH_PIN_RST);
    gpio_set_direction(TOUCH_PIN_RST, GPIO_MODE_OUTPUT);

    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_HOLD_MS)); // hold LOW 10ms
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(TOUCH_WAKE_MS)); // wait 50ms before first I2C

    ESP_LOGI(TAG, "CST816D reset complete — ready");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
/**
 * @brief  Reads the 6-byte touch report and decodes the first touch point.
 *
 * @param  out_x  Output. X coordinate, clamped to 0..239. Only written on true.
 * @param  out_y  Output. Y coordinate, clamped to 0..239. Only written on true.
 * @return true if a finger is down; false if no touch or the I2C read failed.
 */
bool cst816d_read_raw(uint16_t *out_x, uint16_t *out_y)
{
    uint8_t reg = TOUCH_REG_DATA_START;    // register to start reading from
    uint8_t buf[TOUCH_REPORT_BYTES] = {0}; // 6-byte receive buffer

    // ── i2c_master_transmit_receive ───────────────────────────────────────────
    // In one atomic transaction it:
    //   1. Writes 1 byte  (reg = 0x01) to tell the chip where to start
    //   2. Reads  6 bytes back (the touch report)
    esp_err_t err = i2c_master_transmit_receive(
        s_dev_handle,
        &reg, 1,
        buf, TOUCH_REPORT_BYTES,
        TOUCH_I2C_TIMEOUT_MS);

    if (err != ESP_OK)
    {
        // Chip didn't respond
        return false;
    }

    // ── Extract touch count ───────────────────────────────────────────────────
    uint8_t touch_count = buf[TOUCH_BYTE_COUNT] & TOUCH_COUNT_MASK;
    if (touch_count == 0)
    {
        return false; // no finger on screen
    }

    // ── Extract X coordinate (12-bit value across 2 bytes) ───────────────────
    uint16_t x = ((uint16_t)(buf[TOUCH_BYTE_X_HIGH] & TOUCH_COORD_NIBBLE_MASK) << 8) | (uint16_t)buf[TOUCH_BYTE_X_LOW];

    // ── Extract Y coordinate (same structure) ─────────────────────────────────
    uint16_t y = ((uint16_t)(buf[TOUCH_BYTE_Y_HIGH] & TOUCH_COORD_NIBBLE_MASK) << 8) | (uint16_t)buf[TOUCH_BYTE_Y_LOW];

    // ── Clamp to screen bounds ────────────────────────────────────────────────
    if (x >= TOUCH_SCREEN_W)
    {
        x = TOUCH_SCREEN_W - 1;
    }
    if (y >= TOUCH_SCREEN_H)
    {
        y = TOUCH_SCREEN_H - 1;
    }

    *out_x = x;
    *out_y = y;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
/**
 * @brief  LVGL input-device read callback, registered in app_main().
 *
 * @param  indev  The LVGL input device being read (unused).
 * @param  data   Output. Touch point and pressed/released state for LVGL.
 */
void cst816d_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev; // suppress unused-parameter warning

    uint16_t x, y;
    bool pressed = cst816d_read_raw(&x, &y);

    if (!pressed)
    {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = LV_INDEV_STATE_PR;
}