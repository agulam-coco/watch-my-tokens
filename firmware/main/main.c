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
static lv_obj_t *s_status_label = NULL;  // "PENDING APPROVAL" / "N agents  P%"
static lv_obj_t *s_summary_label = NULL; // alert command text / cost / summary
static lv_obj_t *s_serial_label = NULL;  // raw fallback for unrecognized lines
static bool s_alert_active = false;

static void arc_anim_cb(void *var, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)var, value);
}

// Callback for fading in the opacity of an object
static void opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

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

        char status_buf[64];
        snprintf(status_buf, sizeof(status_buf), "%d agents  %d%%", agents, pct);
        char cost_buf[32];
        snprintf(cost_buf, sizeof(cost_buf), "$%.2f", cost);

        lvgl_port_lock(0);
        s_alert_active = false;
        if (s_arc)
            lv_arc_set_value(s_arc, pct);
        if (s_status_label)
            lv_label_set_text(s_status_label, status_buf);
        if (s_summary_label)
            lv_label_set_text(s_summary_label, cost_buf);
        lvgl_port_unlock();
    }
    else if (strncmp(line, "ALERT:", 6) == 0)
    {
        const char *cmd = line + 6;
        lvgl_port_lock(0);
        s_alert_active = true;
        if (s_status_label)
            lv_label_set_text(s_status_label, "PENDING APPROVAL");
        if (s_summary_label)
            lv_label_set_text(s_summary_label, cmd);
        lvgl_port_unlock();
    }
    else if (strncmp(line, "SUMMARY:", 8) == 0)
    {
        const char *text = line + 8;
        lvgl_port_lock(0);
        if (s_summary_label && !s_alert_active)
            lv_label_set_text(s_summary_label, text);
        lvgl_port_unlock();
    }
    else if (strncmp(line, "SCREEN:HOME", 11) == 0)
    {
        lvgl_port_lock(0);
        s_alert_active = false;
        if (s_status_label)
            lv_label_set_text(s_status_label, "");
        lvgl_port_unlock();
    }
    else if (strncmp(line, "SCREEN:LOG:", 11) == 0)
    {
        int approved = 0, denied = 0;
        sscanf(line + 11, "%d:%d", &approved, &denied);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d approved / %d denied", approved, denied);
        lvgl_port_lock(0);
        if (s_status_label)
            lv_label_set_text(s_status_label, buf);
        lvgl_port_unlock();
    }
    else
    {
        // Unknown/legacy line — echo it for debugging
        lvgl_port_lock(0);
        if (s_serial_label)
            lv_label_set_text(s_serial_label, line);
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

// APPROVE button tap handler — sends BTN:APPROVE back over serial.
static void approve_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "TAPPED APPROVE");
    const char *msg = "BTN:APPROVE\r\n";
    usb_serial_jtag_write_bytes((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(100));
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

    // ── Build the UI ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Drawing UI");
    lvgl_port_lock(0);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

    lv_obj_t *arc = lv_arc_create(lv_scr_act());
    lv_obj_set_size(arc, 236, 236);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_color(arc, lv_color_hex(0xFB9204), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(arc, 0, LV_PART_MAIN);

    s_arc = arc;

    // Status line (top): "PENDING APPROVAL" or "N agents  P%"
    s_status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -60);

    // Summary line (middle): alert command / cost / free-text summary
    s_summary_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_summary_label, "");
    lv_obj_set_style_text_color(s_summary_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(s_summary_label, LV_ALIGN_CENTER, 0, -20);

    // Raw fallback line, for unrecognized input during testing
    s_serial_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_serial_label, "waiting...");
    lv_obj_set_style_text_color(s_serial_label, lv_color_hex(0x888888), 0);
    lv_obj_align(s_serial_label, LV_ALIGN_CENTER, 0, 20);

    // APPROVE button
    lv_obj_t *approve_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(approve_btn, 140, 48);
    lv_obj_set_style_radius(approve_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(approve_btn, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_color(approve_btn, lv_color_hex(0xFB9204), 0);
    lv_obj_set_style_shadow_width(approve_btn, 0, 0);
    lv_obj_add_event_cb(approve_btn, approve_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *approve_label = lv_label_create(approve_btn);
    lv_label_set_text(approve_label, "APPROVE");
    lv_obj_set_style_text_color(approve_label, lv_color_hex(0x000000), 0);
    lv_obj_center(approve_label);

    lvgl_port_unlock();

    // Arc fill/empty demo animation (remove once STATS: is driving it live)
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_playback_time(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, arc);
    lv_anim_set_exec_cb(&a_opa, opa_anim_cb);
    lv_anim_set_values(&a_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a_opa, 1500);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_opa);

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
    }

    ESP_LOGI(TAG, "Setup complete");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}