/**
 * @file touch_driver.h
 * @brief CST816D capacitive touch driver for the ESP32-2424S012C board.
 *
 * Uses the ESP-IDF v5/v6 I2C master API (driver/i2c_master.h), not the legacy
 * driver/i2c.h. The chip is polled; the INT pin is not used.
 */

#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h" // ← correct header for ESP-IDF v5.x / v6.x
#include "driver/gpio.h"
#include "lvgl.h"

// ── Hardwired GPIO pins on the ESP32-2424S012C PCB ───────────────────────────
#define TOUCH_PIN_SDA GPIO_NUM_4
#define TOUCH_PIN_SCL GPIO_NUM_5
#define TOUCH_PIN_INT GPIO_NUM_0 // interrupt (optional, not used in polling mode)
#define TOUCH_PIN_RST GPIO_NUM_1 // reset

// ── I2C bus configuration ─────────────────────────────────────────────────────
#define TOUCH_I2C_PORT I2C_NUM_0 // which I2C peripheral (0 or 1)
#define TOUCH_I2C_FREQ_HZ 400000 // 400 kHz fast-mode
#define TOUCH_I2C_CLK_SRC I2C_CLK_SRC_DEFAULT
// Glitch filter: ignore noise pulses shorter than 7 I2C clock cycles.
// Recommended value from Espressif for reliable communication.
#define TOUCH_I2C_GLITCH_CNT 7

// ── CST816D chip constants ────────────────────────────────────────────────────
#define TOUCH_I2C_ADDR 0x15       // fixed 7-bit I2C address
#define TOUCH_REG_DATA_START 0x01 // first register of the 6-byte touch report

// ── Touch report byte layout (6 bytes read from TOUCH_REG_DATA_START) ────────
#define TOUCH_REPORT_BYTES 6
#define TOUCH_BYTE_GESTURE 0 // gesture ID (see TOUCH_GESTURE_* below)
#define TOUCH_BYTE_COUNT 1   // number of touch points (lower nibble)
#define TOUCH_BYTE_X_HIGH 2  // X bits [11:8] in lower nibble
#define TOUCH_BYTE_X_LOW 3   // X bits [7:0]
#define TOUCH_BYTE_Y_HIGH 4  // Y bits [11:8] in lower nibble
#define TOUCH_BYTE_Y_LOW 5   // Y bits [7:0]

// ── Bitmasks for extracting coordinate nibbles ────────────────────────────────
#define TOUCH_COORD_NIBBLE_MASK 0x0F // lower 4 bits = coord high nibble
#define TOUCH_COUNT_MASK 0x0F        // lower 4 bits = touch point count

// ── Hardware-decoded gesture IDs ──────────────────────────────────────────────
#define TOUCH_GESTURE_NONE 0x00
#define TOUCH_GESTURE_SWIPE_UP 0x01
#define TOUCH_GESTURE_SWIPE_DOWN 0x02
#define TOUCH_GESTURE_SWIPE_LEFT 0x03
#define TOUCH_GESTURE_SWIPE_RIGHT 0x04
#define TOUCH_GESTURE_LONG_PRESS 0x05

// ── Timing constants ──────────────────────────────────────────────────────────
#define TOUCH_RESET_HOLD_MS 10  // hold RST LOW for 10ms during init
#define TOUCH_WAKE_MS 50        // wait 50ms after RST goes HIGH before I2C
#define TOUCH_I2C_TIMEOUT_MS 10 // max wait per I2C transaction

// ── Public functions ──────────────────────────────────────────────────────────

/**
 * @brief  Creates the I2C master bus, registers the CST816D on it and
 *         hardware-resets the chip.
 *
 *         Call once at startup, after lvgl_port_init() and before registering
 *         cst816d_read() as an LVGL input device.
 *
 * @return ESP_OK on success, or the esp_err_t from i2c_new_master_bus() /
 *         i2c_master_bus_add_device() if bus or device setup fails.
 */
esp_err_t cst816d_init(void);

/**
 * @brief  LVGL input-device read callback. Registered with
 *         lv_indev_set_read_cb() in app_main(); LVGL calls it on every input
 *         poll.
 *
 * @param  indev  The LVGL input device being read (unused).
 * @param  data   Output. Receives the touch point and
 *                LV_INDEV_STATE_PR while a finger is down, otherwise
 *                LV_INDEV_STATE_REL (point left unchanged).
 */
void cst816d_read(lv_indev_t *indev, lv_indev_data_t *data);

/**
 * @brief  Reads the current touch point directly from the chip, without LVGL.
 *         Used by cst816d_read() and by the diagnostic touch_read_task.
 *
 * @param  out_x  Output. X coordinate, clamped to 0..239. Only written on true.
 * @param  out_y  Output. Y coordinate, clamped to 0..239. Only written on true.
 * @return true if a finger is down; false if there is no touch or the chip
 *         did not respond on I2C.
 */
bool cst816d_read_raw(uint16_t *out_x, uint16_t *out_y);