#include <stdint.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "lvgl.h"
#include "lv_drv_conf.h"
#include "sdl/sdl.h"

#define SIM_WIDTH  360
#define SIM_HEIGHT 360

static void create_round_preview(void)
{
    lv_obj_t *screen = lv_scr_act();

    /* 方形 framebuffer 的四角保持黑色，模拟实体圆屏不可见区域。 */
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *circle = lv_obj_create(screen);
    lv_obj_set_size(circle, 356, 356);
    lv_obj_center(circle);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(circle, lv_color_hex(0x5AC8FA), LV_PART_MAIN);
    lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(circle);
    lv_label_set_text(title, "BRZ GAUGE");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *subtitle = lv_label_create(circle);
    lv_label_set_text(subtitle, "360 x 360 ROUND DISPLAY");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5AC8FA), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 18);
}

int main(void)
{
    lv_init();
    sdl_init();

    static lv_disp_draw_buf_t draw_buffer;
    static lv_color_t pixel_buffer[SIM_WIDTH * 40];

    lv_disp_draw_buf_init(
        &draw_buffer,
        pixel_buffer,
        NULL,
        SIM_WIDTH * 40
    );

    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.draw_buf = &draw_buffer;
    display_driver.flush_cb = sdl_display_flush;
    display_driver.hor_res = SIM_WIDTH;
    display_driver.ver_res = SIM_HEIGHT;
    lv_disp_drv_register(&display_driver);

    static lv_indev_drv_t pointer_driver;
    lv_indev_drv_init(&pointer_driver);
    pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&pointer_driver);

    create_round_preview();

    uint32_t previous_ms = SDL_GetTicks();

    while (1) {
        uint32_t current_ms = SDL_GetTicks();
        uint32_t elapsed_ms = current_ms - previous_ms;
        previous_ms = current_ms;

        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
        }

        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
