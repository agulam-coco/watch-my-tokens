#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "touch_driver.h"

static const char *TAG = "main";

// Confirmed pinout for ESP32-2424S012C-Y
#define PIN_MOSI 7
#define PIN_SCLK 6
#define PIN_CS 10
#define PIN_DC 2
#define PIN_RST -1 // Not wired, GC9A01 resets via software command
#define PIN_BL 3

#define LCD_H_RES 240
#define LCD_V_RES 240

// ── UI object handles, reachable from the serial line handler ────────────────
static lv_obj_t *s_arc = NULL;
static lv_obj_t *s_agents_label = NULL;
static lv_obj_t *s_pct_label = NULL;
static lv_obj_t *s_cost_label = NULL;
static lv_obj_t *s_approve_btn = NULL;
static bool s_alert_active = false;
static uint32_t s_press_start_ms = 0;
static bool s_press_active = false;

// ── Serial protocol dispatcher ────────────────────────────────────────────────
// Parses one complete line per the AgentPager serial spec and updates the UI.
//   STATS:<agents>:<pct>:<cost>
//   ALERT:<command text>
//   SUMMARY:<text>
//   SCREEN:HOME
//   SCREEN:LOG:<approved>:<denied>
static void handle_line(const char *line)
{
    ESP_LOGI(TAG, "Got line: %s", line);
    if (strncmp(line, "STATS:", 6) == 0)
    {
        int agents = 0, pct = 0;
        float cost = 0.0f;
        sscanf(line + 6, "%d:%d:%f", &agents, &pct, &cost);
        char agents_buf[32];
        snprintf(agents_buf, sizeof(agents_buf), "%d AGENTS", agents);
        char pct_buf[32];
        snprintf(pct_buf, sizeof(pct_buf), "TOKENS %d%%", pct);
        char cost_buf[32];
        snprintf(cost_buf, sizeof(cost_buf), "$%.2f today", cost);
        lvgl_port_lock(0);
        if (!s_alert_active)
        {
            if (s_arc)
            {
                lv_arc_set_value(s_arc, pct);
                if (pct >= 80)
                    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF3B30), LV_PART_INDICATOR);
                else
                    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);
            }
            if (s_agents_label)
                lv_label_set_text(s_agents_label, agents_buf);
            if (s_pct_label)
                lv_label_set_text(s_pct_label, pct_buf);
            if (s_cost_label)
                lv_label_set_text(s_cost_label, cost_buf);
        }
        lvgl_port_unlock();
    }
    else if (strncmp(line, "ALERT:", 6) == 0)
    {
        const char *cmd = line + 6;
        lvgl_port_lock(0);
        s_alert_active = true;
        if (s_agents_label)
            lv_label_set_text(s_agents_label, "APPROVE?");
        if (s_pct_label)
            lv_label_set_text(s_pct_label, cmd);
        if (s_cost_label)
            lv_label_set_text(s_cost_label, "");
        if (s_cost_label)
            lv_obj_add_flag(s_cost_label, LV_OBJ_FLAG_HIDDEN);
        if (s_arc)
        {
            lv_arc_set_value(s_arc, 100);
            lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF3B30), LV_PART_INDICATOR);
            lv_obj_set_style_shadow_color(s_arc, lv_color_hex(0xFF3B30), 0);
            lv_obj_set_style_shadow_width(s_arc, 12, 0);
            lv_obj_set_style_shadow_spread(s_arc, 0, 0);
            lv_obj_set_style_shadow_opa(s_arc, LV_OPA_70, 0);
        }
        if (s_approve_btn)
        {
            lv_obj_clear_flag(s_approve_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_approve_btn, lv_color_hex(0xFF9500), 0);
        }

        lvgl_port_unlock();
        lv_refr_now(NULL);
    }
    else if (strncmp(line, "SUMMARY:", 8) == 0)
    {
        const char *text = line + 8;
        lvgl_port_lock(0);
        if (s_pct_label && s_alert_active)
            lv_label_set_text(s_pct_label, text);
        lvgl_port_unlock();
    }
    else if (strncmp(line, "SCREEN:HOME", 11) == 0)
    {
        lvgl_port_lock(0);
        s_alert_active = false;
        if (s_arc)
        {
            lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);
            lv_obj_set_style_shadow_width(s_arc, 0, 0);
        }
        if (s_cost_label)
            lv_obj_clear_flag(s_cost_label, LV_OBJ_FLAG_HIDDEN);
        if (s_approve_btn)
            lv_obj_add_flag(s_approve_btn, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
else if (strncmp(line, "SCREEN:LOG:", 11) == 0)
{
    int approved = 0, denied = 0;
    sscanf(line + 11, "%d:%d", &approved, &denied);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d approved\n%d denied", approved, denied);
    lvgl_port_lock(0);
    if (s_agents_label)
        lv_label_set_text(s_agents_label, buf);
    lvgl_port_unlock();
}
}

// Reads raw bytes directly off the USB-Serial-JTAG peripheral (this board's
// USB port only exposes USB-Serial-JTAG, not UART0's GPIO pins), assembles
// them into lines, and dispatches each complete line to handle_line().
static void serial_read_task(void *arg)
{
    uint8_t byte;
    char line[128];
    size_t line_len = 0;

    ESP_LOGI(TAG, "serial_read_task started, reading USB-Serial-JTAG bytes");

    while (1)
    {
        int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (n > 0)
        {
            if (byte == '\n' || byte == '\r')
            {
                if (line_len > 0)
                {
                    line[line_len] = '\0';
                    handle_line(line);
                    line_len = 0;
                }
            }
            else if (line_len < sizeof(line) - 1)
            {
                line[line_len++] = (char)byte;
            }
        }
    }
}

// Diagnostic-only task for touch: polls the CST816D directly (bypassing
// LVGL's input device system) and logs raw (x, y) coordinates.
static void touch_read_task(void *arg)
{
    while (1)
    {
        uint16_t x, y;
        if (cst816d_read_raw(&x, &y))
        {
            ESP_LOGI(TAG, "TOUCH x=%u y=%u", x, y);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Fires once when a press begins on the button.
static void approve_btn_pressed_cb(lv_event_t *e)
{
    s_press_start_ms = lv_tick_get();
    s_press_active = true;
}

// Fires repeatedly while the button is held down — used to animate a
// pulsing flash that intensifies as the hold approaches the DENY threshold.
static void approve_btn_pressing_cb(lv_event_t *e)
{
    if (!s_press_active)
        return;

    uint32_t held_ms = lv_tick_elaps(s_press_start_ms);
    uint32_t threshold_ms = 1000; // matches lv_indev_set_long_press_time()

    // Fraction of the way to DENY, clamped 0..1.
    float frac = (float)held_ms / (float)threshold_ms;
    if (frac > 1.0f)
        frac = 1.0f;

    // Fast pulse: alternates brightness a few times per second, and the
    // pulse amplitude grows as frac approaches 1 — feels like it's "charging".
    uint32_t pulse_phase = (held_ms / 100) % 2; // toggles every 100ms
    lv_color_t base = lv_color_hex(0xFF9500);   // amber
    lv_color_t deny = lv_color_hex(0xFF3B30);   // red

    lv_color_t mixed = lv_color_mix(deny, base, (uint8_t)(frac * 255));
    lv_color_t shown = pulse_phase ? mixed : lv_color_darken(mixed, LV_OPA_20);

    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(s_approve_btn, shown, 0);
    lvgl_port_unlock();
}

// Fires when the press ends (release or long-press completion) — resets
// the pressing state so a stale flash doesn't linger on the next tap.
static void approve_btn_released_cb(lv_event_t *e)
{
    s_press_active = false;
}

// APPROVE — short tap, released before the long-press threshold.
static void approve_btn_click_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "TAPPED APPROVE");
    const char *msg = "BTN:APPROVE\r\n";
    usb_serial_jtag_write_bytes((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(100));
}

// DENY — press-and-hold past the long-press threshold.
static void approve_btn_longpress_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "HELD -> DENY");
    const char *msg = "BTN:DENY\r\n";
    usb_serial_jtag_write_bytes((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(100));

    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(s_approve_btn, lv_color_hex(0xFF3B30), 0);
    lvgl_port_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Installing USB-Serial-JTAG driver");
    usb_serial_jtag_driver_config_t usb_jtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t jtag_err = usb_serial_jtag_driver_install(&usb_jtag_cfg);
    if (jtag_err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install returned %d (may already be installed by console)", jtag_err);
    }

    ESP_LOGI(TAG, "Turning on backlight");
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(PIN_BL, 1);

    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Installing GC9A01 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initializing LVGL port");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 24,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    (void)disp;

    ESP_LOGI(TAG, "Drawing UI");
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    lv_obj_t *arc = lv_arc_create(lv_scr_act());
    lv_obj_set_size(arc, 228, 228);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_radius(arc, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A2100), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(arc, 0, LV_PART_MAIN);
    s_arc = arc;

    s_agents_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_agents_label, "0 AGENTS");
    lv_obj_set_style_text_color(s_agents_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_agents_label, &lv_font_montserrat_28, 0);
    lv_obj_set_width(s_agents_label, 180);
    lv_obj_set_style_text_align(s_agents_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_agents_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_agents_label, LV_ALIGN_CENTER, 0, -45);

    s_pct_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_pct_label, "TOKENS 0%");
    lv_obj_set_style_text_color(s_pct_label, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_pct_label, &lv_font_unscii_16, 0);
    lv_obj_set_width(s_pct_label, 200);
    lv_obj_set_height(s_pct_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_pct_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pct_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_pct_label, LV_ALIGN_CENTER, 0, 10);

    s_cost_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_cost_label, "$0.00 today");
    lv_obj_set_style_text_color(s_cost_label, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_text_font(s_cost_label, &lv_font_unscii_16, 0);
    lv_obj_align(s_cost_label, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *approve_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(approve_btn, 120, 40);
    lv_obj_set_style_radius(approve_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(approve_btn, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_color(approve_btn, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_shadow_width(approve_btn, 0, 0);
    lv_obj_add_flag(approve_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(approve_btn, approve_btn_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(approve_btn, approve_btn_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(approve_btn, approve_btn_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(approve_btn, approve_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(approve_btn, approve_btn_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *approve_label = lv_label_create(approve_btn);
    lv_label_set_text(approve_label, "APPROVE");
    lv_obj_set_style_text_color(approve_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(approve_label, &lv_font_unscii_16, 0);
    lv_obj_center(approve_label);
    s_approve_btn = approve_btn;

    lvgl_port_unlock();

    xTaskCreate(serial_read_task, "serial_read", 4096, NULL, 5, NULL);

    // Touch bring-up
    ESP_LOGI(TAG, "Initializing touch controller");
    esp_err_t touch_err = cst816d_init();
    if (touch_err != ESP_OK)
    {
        ESP_LOGE(TAG, "cst816d_init failed: %s", esp_err_to_name(touch_err));
    }
    else
    {
        xTaskCreate(touch_read_task, "touch_read", 4096, NULL, 5, NULL);

        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, cst816d_read);
        lv_indev_set_long_press_time(indev, 1000);
    }

    ESP_LOGI(TAG, "Setup complete");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
