/**
 * Display manager — thin MicroPython wrapper around board_common.
 *
 * All hardware init (I2C, display, touch, PMIC, backlight, LVGL port)
 * is handled by board_init(). This file just provides the init() and
 * run_screen() C functions that the MicroPython bindings call.
 */
#include <exception>

#include "board.h"
#include "esp_lvgl_port.h"

#include "display_manager.h"

static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

extern "C" void init(void)
{
    board_app_config_t cfg = { .landscape = true };
    board_init(&cfg, &lvgl_disp, &lvgl_touch_indev);
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
