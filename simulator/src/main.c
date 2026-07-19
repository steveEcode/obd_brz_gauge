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
#include "simulator_role_runtime.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/espnow_simulator.h"

#ifdef SIMULATOR_USE_GITHUB_UI
#include "ui.h"
#endif

#define SIM_WIDTH  360
#define SIM_HEIGHT 360




static const char *simulator_window_title(
    simulator_role_t role
)
{
    switch (role) {
        case SIMULATOR_ROLE_MASTER:
            return "BRZ Gauge - MASTER";

        case SIMULATOR_ROLE_SLAVE_LEFT:
            return "BRZ Gauge - SLAVE LEFT";

        case SIMULATOR_ROLE_SLAVE_RIGHT:
            return "BRZ Gauge - SLAVE RIGHT";

        default:
            return "BRZ Gauge";
    }
}

static void simulator_apply_window_title(
    simulator_role_t role
)
{
    /*
     * 每个模拟器进程只创建一个 SDL 窗口。
     * SDL 的第一个窗口 ID 通常为 1。
     */
    SDL_Window *window = SDL_GetWindowFromID(1);

    if (window == NULL) {
        window = SDL_GetKeyboardFocus();
    }

    if (window == NULL) {
        window = SDL_GetMouseFocus();
    }

    if (window == NULL) {
        fprintf(
            stderr,
            "[SIMULATOR] Cannot find SDL window: %s\n",
            SDL_GetError()
        );
        return;
    }

    SDL_SetWindowTitle(
        window,
        simulator_window_title(role)
    );

    /*
     * 三联表固定排列：
     * SLAVE LEFT | MASTER | SLAVE RIGHT
     */
    int column = 1;

    switch (role) {
        case SIMULATOR_ROLE_SLAVE_LEFT:
            column = 0;
            break;

        case SIMULATOR_ROLE_MASTER:
            column = 1;
            break;

        case SIMULATOR_ROLE_SLAVE_RIGHT:
            column = 2;
            break;

        default:
            column = 1;
            break;
    }

    int window_width = 0;
    int window_height = 0;

    SDL_GetWindowSize(
        window,
        &window_width,
        &window_height
    );

    const int gap = 24;
    int window_x =
        40 + column * (window_width + gap);

    int window_y = 80;

    /*
     * 能取得桌面可用范围时，将三个窗口整体居中。
     * 取不到时使用上面的固定坐标。
     */
    int display_index =
        SDL_GetWindowDisplayIndex(window);

    SDL_Rect usable_bounds;

    if (
        display_index >= 0 &&
        SDL_GetDisplayUsableBounds(
            display_index,
            &usable_bounds
        ) == 0
    ) {
        const int total_width =
            window_width * 3 + gap * 2;

        if (total_width <= usable_bounds.w) {
            const int start_x =
                usable_bounds.x +
                (usable_bounds.w - total_width) / 2;

            window_x =
                start_x +
                column * (window_width + gap);

            window_y =
                usable_bounds.y +
                (usable_bounds.h - window_height) / 2;
        } else {
            /*
             * 屏幕宽度不足时改为错位排列，
             * 避免三个窗口完全重叠。
             */
            window_x =
                usable_bounds.x +
                40 +
                column * 60;

            window_y =
                usable_bounds.y +
                40 +
                column * 45;
        }
    }

    SDL_SetWindowPosition(
        window,
        window_x,
        window_y
    );
}


static lv_obj_t *s_slave_status_panel;
static lv_obj_t *s_slave_status_label;
static uint8_t s_slave_ever_linked;

static void simulator_slave_status_init(
    simulator_role_t role
)
{
    if (role == SIMULATOR_ROLE_MASTER) {
        return;
    }

    s_slave_status_panel =
        lv_obj_create(lv_layer_top());

    lv_obj_set_size(
        s_slave_status_panel,
        300,
        112
    );

    lv_obj_center(s_slave_status_panel);

    lv_obj_clear_flag(
        s_slave_status_panel,
        LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_CLICKABLE
    );

    lv_obj_set_style_radius(
        s_slave_status_panel,
        24,
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_color(
        s_slave_status_panel,
        lv_color_hex(0x080808),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        s_slave_status_panel,
        230,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        s_slave_status_panel,
        3,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_color(
        s_slave_status_panel,
        lv_color_hex(0xFF3B30),
        LV_PART_MAIN
    );

    s_slave_status_label =
        lv_label_create(s_slave_status_panel);

    lv_obj_set_width(
        s_slave_status_label,
        270
    );

    lv_label_set_long_mode(
        s_slave_status_label,
        LV_LABEL_LONG_WRAP
    );

    lv_obj_set_style_text_color(
        s_slave_status_label,
        lv_color_white(),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_align(
        s_slave_status_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN
    );

    lv_obj_center(s_slave_status_label);

    s_slave_ever_linked = 0u;

    lv_label_set_text(
        s_slave_status_label,
        "WAITING FOR MASTER"
    );
}

static void simulator_slave_status_update(
    simulator_role_t role
)
{
    if (
        role == SIMULATOR_ROLE_MASTER ||
        s_slave_status_panel == NULL ||
        s_slave_status_label == NULL
    ) {
        return;
    }

    if (espnow_link_slave_has_data()) {
        s_slave_ever_linked = 1u;

        lv_obj_add_flag(
            s_slave_status_panel,
            LV_OBJ_FLAG_HIDDEN
        );

        return;
    }

    if (s_slave_ever_linked) {
        lv_label_set_text(
            s_slave_status_label,
            "MASTER OFFLINE\n"
            "WAITING FOR MASTER"
        );
    } else {
        lv_label_set_text(
            s_slave_status_label,
            "WAITING FOR MASTER"
        );
    }

    lv_obj_clear_flag(
        s_slave_status_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(
        s_slave_status_panel
    );
}

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

    simulator_role_runtime_set(simulator_role);

    lv_init();
    simulator_keyboard_init();
    sdl_init();
    simulator_apply_window_title(simulator_role);

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

    mock_ecu_init();


#ifdef SIMULATOR_USE_GITHUB_UI
    ui_init();
#else
    create_round_preview();
#endif

    if (simulator_role == SIMULATOR_ROLE_MASTER) {
        espnow_link_start_master();
    } else {
        espnow_link_start_slave();
    }

    simulator_slave_status_init(
        simulator_role
    );

    simulator_slave_status_update(
        simulator_role
    );

    
    uint32_t previous_ms = SDL_GetTicks();

    /*
     * WSLg/X11 可能在窗口刚创建时覆盖初始位置。
     * 启动后的前约 2 秒重复定位，等待窗口管理器稳定。
     */
    uint32_t next_window_layout_ms =
        previous_ms + 100u;

    uint8_t window_layout_attempts = 0u;

    while (1) {
        uint32_t current_ms = SDL_GetTicks();
        uint32_t elapsed_ms = current_ms - previous_ms;
        previous_ms = current_ms;

        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
        }

        simulator_keyboard_update();

        if (
            window_layout_attempts < 20u &&
            (int32_t)(
                current_ms -
                next_window_layout_ms
            ) >= 0
        ) {
            simulator_apply_window_title(
                simulator_role
            );

            window_layout_attempts++;
            next_window_layout_ms =
                current_ms + 100u;
        }

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

        espnow_link_simulator_update(elapsed_ms);

        simulator_slave_status_update(
            simulator_role
        );


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
    espnow_link_simulator_shutdown();
    return 0;
}
