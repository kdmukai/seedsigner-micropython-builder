#include <stdio.h>
#include <string.h>
#include <exception>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp_i2c.h"
#include "bsp_display.h"

#include "esp_lvgl_port.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_axs15231b.h"

#include "display_manager.h"
#include "esp_io_expander_tca9554.h"

/* Physical panel dimensions (portrait) */
#define LCD_H_RES 320
#define LCD_V_RES 480

/* Logical display dimensions (landscape, presented to LVGL / application) */
#define DISP_H_RES LCD_V_RES   /* 480 */
#define DISP_V_RES LCD_H_RES   /* 320 */

#define LINES_PER_BAND 80

static const char *TAG = "display_manager";

esp_io_expander_handle_t expander_handle = NULL;

esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_touch_handle_t touch_handle = NULL;

static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

/* ── AXS15231B RASET workaround (custom flush + DMA bounce + rotation) ──
 *
 * The AXS15231B display controller has a confirmed hardware defect:
 * CASET/RASET commands have no effect over QSPI. esp_lcd_panel_draw_bitmap()
 * always starts from (0,0) regardless of coordinates passed.
 *
 * Additionally, lv_display_set_rotation() does not work with direct_mode
 * in LVGL v9 — the buffer layout is not transformed.
 *
 * Solution: LVGL renders in landscape (480x320) into a SPIRAM framebuffer.
 * The flush callback rotates pixels 90° CW from landscape → portrait while
 * copying into DMA bounce buffers, then sends 320-wide portrait bands to the
 * panel. Byte swap (RGB565 endianness) is done in the same per-pixel loop.
 *
 * CRITICAL: The LVGL port lock must be held across lvgl_port_add_disp() and
 * the callback overrides. Without this, the LVGL task can run a frame with
 * the default esp_lvgl_port flush callback, which sends partial draw_bitmap
 * calls that corrupt the panel's write pointer due to the RASET bug.
 */
static SemaphoreHandle_t flush_done_sem = NULL;
static uint8_t *swap_buf[2] = {NULL, NULL};

static bool flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_sem, &woken);
    return (woken == pdTRUE);
}

static void axs15231b_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    /* px_map: 480×320 landscape framebuffer (row-major, RGB565)
     * Panel:  320×480 portrait (row-major, 320 pixels per row)
     *
     * 90° CW rotation: panel pixel (px, py) ← framebuffer pixel (py, DISP_V_RES-1-px)
     * Combined with RGB565 byte swap in a single pass per band. */
    uint16_t *fb = (uint16_t *)px_map;
    int buf_idx = 0;

    for (int py = 0; py < LCD_V_RES; py += LINES_PER_BAND) {
        int band_h = (py + LINES_PER_BAND > LCD_V_RES)
                     ? LCD_V_RES - py : LINES_PER_BAND;
        uint16_t *dst = (uint16_t *)swap_buf[buf_idx];

        /* Loop order: px outer, by inner — so fb reads are sequential
         * along framebuffer rows (fb_x = py+by increments by 1).
         * SPIRAM cache lines are 32 bytes (16 pixels); sequential reads
         * hit cache, strided reads (old order) miss on every pixel. */
        for (int px = 0; px < LCD_H_RES; px++) {
            int fb_y = DISP_V_RES - 1 - px;
            int fb_row_offset = fb_y * DISP_H_RES + py;
            for (int by = 0; by < band_h; by++) {
                uint16_t pixel = fb[fb_row_offset + by];
                dst[by * LCD_H_RES + px] = (pixel >> 8) | (pixel << 8);
            }
        }

        if (py > 0) {
            xSemaphoreTake(flush_done_sem, portMAX_DELAY);
        }

        esp_lcd_panel_draw_bitmap(panel_handle, 0, py, LCD_H_RES, py + band_h, dst);
        buf_idx ^= 1;
    }

    xSemaphoreTake(flush_done_sem, portMAX_DELAY);
    lv_display_flush_ready(disp);
}


void io_expander_init(i2c_master_bus_handle_t bus_handle)
{
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(bus_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &expander_handle));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander_handle, IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander_handle, IO_EXPANDER_PIN_NUM_1, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander_handle, IO_EXPANDER_PIN_NUM_1, 1));
    vTaskDelay(pdMS_TO_TICKS(200));
}


static void touch_init(i2c_master_bus_handle_t bus_handle)
{
    esp_lcd_panel_io_handle_t touch_io_handle;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    touch_io_config.scl_speed_hz = 400000;  /* Macro leaves this at 0; must set manually */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &touch_io_config, &touch_io_handle));

    /* Touch IC reports in physical portrait coordinates.
     * swap_xy + mirror_x transforms to landscape for LVGL. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,    /* physical portrait width = 320 */
        .y_max = LCD_V_RES,    /* physical portrait height = 480 */
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_axs15231b(touch_io_handle, &tp_cfg, &touch_handle));
}


static void lvgl_port_setup(void)
{
    /* Allocate DMA bounce buffers for RASET workaround */
    flush_done_sem = xSemaphoreCreateBinary();
    swap_buf[0] = (uint8_t *)heap_caps_malloc(LCD_H_RES * LINES_PER_BAND * 2, MALLOC_CAP_DMA);
    swap_buf[1] = (uint8_t *)heap_caps_malloc(LCD_H_RES * LINES_PER_BAND * 2, MALLOC_CAP_DMA);
    assert(swap_buf[0] != NULL && swap_buf[1] != NULL);

    /* LVGL port init */
    lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 1024 * 10,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    lvgl_port_init(&port_cfg);

    /* Display config: landscape dimensions for LVGL, direct_mode SPIRAM buffer.
     * The flush callback handles 90° rotation to the portrait panel. */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DISP_H_RES * DISP_V_RES,
        .trans_size = 0,
        .hres = DISP_H_RES,    /* landscape width = 480 */
        .vres = DISP_V_RES,    /* landscape height = 320 */
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .direct_mode = true,
        },
    };

    /* Hold LVGL lock across display add + callback overrides to prevent
     * the LVGL task from running a frame with the default flush callback.
     * The default flush sends partial draw_bitmap calls which corrupt the
     * panel's write pointer due to the AXS15231B RASET bug. */
    lvgl_port_lock(0);

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    /* Override flush callback with RASET workaround + rotation */
    lv_display_set_flush_cb(lvgl_disp, axs15231b_flush_cb);

    /* Register DMA completion callback (replaces esp_lvgl_port's default) */
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = flush_ready_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, lvgl_disp);

    lvgl_port_unlock();

    /* Touch input via registry driver */
    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
}


extern "C" void init(void)
{
    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_init();

    io_expander_init(i2c_bus_handle);
    bsp_display_init(&io_handle, &panel_handle, DISP_H_RES * DISP_V_RES);
    touch_init(i2c_bus_handle);
    bsp_display_brightness_init();
    bsp_display_set_brightness(100);

    lvgl_port_setup();
}


extern "C" const char *run_screen(display_manager_ui_callback_t cb, void *ctx)
{
    if (!cb) {
        return "null screen callback";
    }
    if (!lvgl_port_lock(0)) {
        return "display lock unavailable";
    }

    const char *err = NULL;
    try {
        cb(ctx);
    } catch (const std::exception &e) {
        err = e.what();
    } catch (...) {
        err = "unknown exception in screen callback";
    }

    lvgl_port_unlock();
    return err;
}
