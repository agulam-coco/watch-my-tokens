/**
 * @file main.c
 * @brief watch-my-tokens (AgentPager) firmware for the ESP32-2424S012C
 *        (ESP32-C3 + 1.28" GC9A01 round LCD + CST816D touch).
 *
 * Shows Claude Code usage stats sent by the bridge over USB-Serial-JTAG, and
 * turns into an approval prompt when the bridge sends ALERT:. A short tap on
 * the button sends BTN:APPROVE; holding it for 1 s sends BTN:DENY.
 */
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

// ── LCD pinout, confirmed on the ESP32-2424S012C-Y ───────────────────────────
#define PIN_MOSI 7
#define PIN_SCLK 6
#define PIN_CS 10
#define PIN_DC 2
#define PIN_RST -1 // Not wired, GC9A01 resets via software command
#define PIN_BL 3

#define LCD_H_RES 240
#define LCD_V_RES 240

// ── UI state shared by the serial handler and the LVGL event callbacks ───────
// LVGL objects created in app_main() and updated from other tasks/callbacks.
static lv_obj_t *s_arc = NULL;
static lv_obj_t *s_agents_label = NULL;
static lv_obj_t *s_pct_label = NULL;
static lv_obj_t *s_cost_label = NULL;
static lv_obj_t *s_approve_btn = NULL;
static bool s_alert_active = false;        // true while an ALERT: prompt is on screen
static uint32_t s_press_start_ms = 0;      // lv_tick at which the current button press began
static bool s_press_active = false;        // true while the button is being held
static lv_obj_t *s_success_overlay = NULL; // green ✓ overlay, or NULL when not showing
static lv_obj_t *s_deny_overlay = NULL;    // red ✕ overlay, or NULL when not showing

// ── Serial protocol dispatcher ────────────────────────────────────────────────
/**
 * @brief  Parses one line from the bridge and updates the UI. Takes the LVGL
 *         lock itself, so it is safe to call from a non-LVGL task.
 *
 *         Supported messages (unknown lines are logged and ignored):
 *           STATS:<agents>:<pct>:<cost>    update ring and labels (ignored during an alert)
 *           ALERT:<command text>           show the approval prompt
 *           SUMMARY:<text>                 replace the command text during an alert
 *           SCREEN:HOME                    leave alert mode, restore dashboard styling
 *           SCREEN:LOG:<approved>:<denied> show the approve/deny tally
 *
 * @param  line  NUL-terminated line without the trailing newline.
 */
static void handle_line(const char *line)
{
    ESP_LOGI(TAG, "Got line: %s", line);
    if (strncmp(line, "STATS:", 6) == 0)
    {
        int agents = 0, pct = 0;
        float cost = 0.0f;
        sscanf(line + 6, "%d:%d:%f", &agents, &pct, &cost);
        char agents_buf[32];

        if (agents == 1)
            snprintf(agents_buf, sizeof(agents_buf), "%d AGENT", agents);
        else
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

/**
 * @brief  FreeRTOS task that reads bytes from USB-Serial-JTAG, assembles them
 *         into lines and passes each complete line to handle_line().
 *
 *         This board's USB port only exposes USB-Serial-JTAG, not UART0, so
 *         all host communication goes through it. Lines end at '\n' or '\r';
 *         characters past 127 in one line are dropped. Never returns.
 *
 * @param  arg  Unused.
 */
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

/**
 * @brief  Puts the dashboard back in its home state (amber ring at 0, default
 *         labels, button hidden) so the device can reset itself right after a
 *         tap or hold, without waiting for the bridge. The next STATS: line
 *         fills in the real values.
 *
 *         Unlike SCREEN:HOME, this also resets the label text and ring value.
 *         Caller must hold the LVGL lock.
 */
static void reset_to_home_screen(void)
{
    s_alert_active = false;
    if (s_arc)
    {
        lv_arc_set_value(s_arc, 0); // will be overwritten by next STATS:, fine as a placeholder
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);
        lv_obj_set_style_shadow_width(s_arc, 0, 0);
    }
    if (s_agents_label)
        lv_label_set_text(s_agents_label, "0 AGENTS");
    if (s_pct_label)
        lv_label_set_text(s_pct_label, "TOKENS 0%");
    if (s_cost_label)
    {
        lv_label_set_text(s_cost_label, "$0.00 today");
        lv_obj_clear_flag(s_cost_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_approve_btn)
        lv_obj_add_flag(s_approve_btn, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief  LVGL animation step for the deny overlay's fade-out.
 * @param  var    The overlay object (lv_obj_t *).
 * @param  value  Current opacity, LV_OPA_COVER down to LV_OPA_TRANSP.
 */
static void deny_overlay_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/**
 * @brief  Runs when the deny fade-out finishes: deletes the overlay and clears
 *         s_deny_overlay.
 * @param  a  The finished animation; a->var is the overlay object.
 */
static void deny_overlay_done_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj)
        lv_obj_delete(obj);
    s_deny_overlay = NULL;
}

/**
 * @brief  Resets to the home screen, then flashes a full-screen red overlay
 *         with an ✕ (held 500 ms, then a 200 ms fade-out). Any overlay already
 *         showing is removed first. Caller must hold the LVGL lock.
 */
static void show_deny_animation(void)
{
    if (s_success_overlay)
    {
        lv_anim_delete(s_success_overlay, NULL);
        lv_obj_delete(s_success_overlay);
        s_success_overlay = NULL;
    }
    if (s_deny_overlay)
    {
        lv_anim_delete(s_deny_overlay, NULL);
        lv_obj_delete(s_deny_overlay);
        s_deny_overlay = NULL;
    }

    reset_to_home_screen();

    // Full-screen RED overlay
    s_deny_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_deny_overlay);
    lv_obj_set_size(s_deny_overlay, 240, 240);
    lv_obj_center(s_deny_overlay);
    lv_obj_set_style_bg_color(s_deny_overlay, lv_color_hex(0xE60000), 0);
    lv_obj_set_style_bg_opa(s_deny_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_deny_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // X glyph
    lv_obj_t *x_mark = lv_label_create(s_deny_overlay);
    lv_label_set_text(x_mark, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(x_mark, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(x_mark, lv_color_white(), 0);
    lv_obj_center(x_mark);

    lv_obj_move_foreground(s_deny_overlay);
    lv_obj_set_style_opa(s_deny_overlay, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_deny_overlay);
    lv_anim_set_exec_cb(&a, deny_overlay_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_delay(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, deny_overlay_done_cb);
    lv_anim_start(&a);
}

/**
 * @brief  LVGL animation step for the success overlay's fade-out.
 * @param  var    The overlay object (lv_obj_t *).
 * @param  value  Current opacity, LV_OPA_COVER down to LV_OPA_TRANSP.
 */
static void success_overlay_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/**
 * @brief  Runs when the success fade-out finishes: deletes the overlay and
 *         clears s_success_overlay.
 * @param  a  The finished animation; a->var is the overlay object.
 */
static void success_overlay_done_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj)
        lv_obj_delete(obj);
    s_success_overlay = NULL;
}

/**
 * @brief  Resets to the home screen, then flashes a full-screen green overlay
 *         with a ✓ (held 500 ms, then a 200 ms fade-out). Any overlay already
 *         showing is removed first. Caller must hold the LVGL lock.
 */
static void show_success_animation(void)
{
    if (s_success_overlay)
    {
        lv_anim_delete(s_success_overlay, NULL);
        lv_obj_delete(s_success_overlay);
        s_success_overlay = NULL;
    }
    if (s_deny_overlay)
    {
        lv_anim_delete(s_deny_overlay, NULL);
        lv_obj_delete(s_deny_overlay);
        s_deny_overlay = NULL;
    }

    reset_to_home_screen();

    // Full-screen GREEN overlay — no separate circle needed
    s_success_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_success_overlay);
    lv_obj_set_size(s_success_overlay, 240, 240);
    lv_obj_center(s_success_overlay);
    lv_obj_set_style_bg_color(s_success_overlay, lv_color_hex(0x00E060), 0);
    lv_obj_set_style_bg_opa(s_success_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_success_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Checkmark glyph, big and centered
    lv_obj_t *check = lv_label_create(s_success_overlay);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(check, lv_color_white(), 0);
    lv_obj_center(check);

    lv_obj_move_foreground(s_success_overlay);

    // Appear instantly at full opacity — no fade-in, avoids a slow blended
    // entrance. Hold briefly, then a quick fade-out.
    lv_obj_set_style_opa(s_success_overlay, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_success_overlay);
    lv_anim_set_exec_cb(&a, success_overlay_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 200);                  // faster fade-out
    lv_anim_set_delay(&a, 500);                     // shorter hold
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); // smoother easing than linear
    lv_anim_set_completed_cb(&a, success_overlay_done_cb);
    lv_anim_start(&a);
}

/**
 * @brief  Diagnostic FreeRTOS task: polls the CST816D every 30 ms, bypassing
 *         LVGL, and logs "TOUCH x=.. y=.." while a finger is down. It does not
 *         affect the UI. Never returns.
 * @param  arg  Unused.
 */
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

/**
 * @brief  LV_EVENT_PRESSED handler: records when the press started so that
 *         approve_btn_pressing_cb() can measure the hold time.
 * @param  e  The LVGL event (unused).
 */
static void approve_btn_pressed_cb(lv_event_t *e)
{
    s_press_start_ms = lv_tick_get();
    s_press_active = true;
}

/**
 * @brief  LV_EVENT_PRESSING handler, called repeatedly while the button is
 *         held. Blends the button colour from amber toward red in proportion
 *         to the hold time (full red at the 1 s DENY threshold) and flickers
 *         it every 100 ms. Does nothing unless an alert is showing.
 * @param  e  The LVGL event (unused).
 */
static void approve_btn_pressing_cb(lv_event_t *e)
{
    if (!s_press_active || !s_alert_active)
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

/**
 * @brief  LV_EVENT_RELEASED handler: ends the hold so the pulsing effect stops.
 * @param  e  The LVGL event (unused).
 */
static void approve_btn_released_cb(lv_event_t *e)
{
    s_press_active = false;
}

/**
 * @brief  LV_EVENT_SHORT_CLICKED handler (tap released before 1 s): sends
 *         "BTN:APPROVE" to the bridge and plays the green success animation.
 * @param  e  The LVGL event (unused).
 */
static void approve_btn_click_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "TAPPED APPROVE");
    const char *msg = "BTN:APPROVE\r\n";
    usb_serial_jtag_write_bytes((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(100));

    lvgl_port_lock(0);
    show_success_animation();
    lvgl_port_unlock();
}

/**
 * @brief  LV_EVENT_LONG_PRESSED handler (held for 1 s): sends "BTN:DENY" to
 *         the bridge and plays the red deny animation.
 * @param  e  The LVGL event (unused).
 */
static void approve_btn_longpress_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "HELD -> DENY");
    const char *msg = "BTN:DENY\r\n";
    usb_serial_jtag_write_bytes((const uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(100));

    lvgl_port_lock(0);
    show_deny_animation();
    lvgl_port_unlock();
}

/**
 * @brief  Firmware entry point. Brings up USB-Serial-JTAG, the backlight, the
 *         SPI bus and GC9A01 panel, and LVGL; draws the dashboard; starts the
 *         serial reader task; then starts touch input. Afterwards it idles
 *         forever while the tasks and LVGL do the work.
 */
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
    lv_obj_add_event_cb(approve_btn, approve_btn_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
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
