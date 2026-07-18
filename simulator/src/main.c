#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "lvgl.h"
#include "lv_drv_conf.h"
#include "sdl/sdl.h"

#include "mock_ecu.h"
#include "vehicle_state.h"
#include "keyboard.h"
#include "simulator_role.h"

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

int main(int argc, char **argv)
{
    simulator_role_t simulator_role;

    if (
        simulator_role_parse(
            argc,
            argv,
            &simulator_role
        ) != 0
    ) {
        fprintf(
            stderr,
            "Usage: %s "
            "[--role master|slave-left|slave-right]\n",
            argv[0]
        );

        return 2;
    }

    printf(
        "Simulator role: %s\n",
        simulator_role_name(simulator_role)
    );

    lv_init();
    simulator_keyboard_init();
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
    mock_ecu_init();
    
    uint32_t previous_ms = SDL_GetTicks();

    while (1) {
        uint32_t current_ms = SDL_GetTicks();
        uint32_t elapsed_ms = current_ms - previous_ms;
        previous_ms = current_ms;

        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
        }

        simulator_keyboard_update();

        const simulator_keyboard_state_t *keyboard =
            simulator_keyboard_get_state();

        mock_ecu_set_throttle(
            keyboard->throttle_pressed ? 100.0f : 0.0f
        );

        mock_ecu_set_brake(
            keyboard->brake_pressed
        );

        float steering = 0.0f;

        if (keyboard->steer_left_pressed) {
            steering -= 1.0f;
        }

        if (keyboard->steer_right_pressed) {
            steering += 1.0f;
        }

        mock_ecu_set_steering(steering);

        mock_ecu_set_traffic_mode(
            keyboard->traffic_mode_enabled
        );

        if (keyboard->quit_requested) {
            break;
        }

        mock_ecu_update(elapsed_ms);

        const vehicle_state_t *vehicle = mock_ecu_get_state();
        const float speed_mph = vehicle->speed_kph * 0.621371f;

        printf(
            "RPM: %6.0f  Speed: %6.1f mph  Coolant: %5.1f C  "
            "Gear: %d  Lat G: %+5.2f  Traffic: %s\r",
            vehicle->rpm,
            speed_mph,
            vehicle->coolant_temp_c,
            (int) vehicle->gear,
            vehicle->lateral_g,
            keyboard->traffic_mode_enabled ? "ON " : "OFF"
        );

        fflush(stdout);
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
