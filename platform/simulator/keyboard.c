#include "keyboard.h"

#include <SDL2/SDL.h>
#include <string.h>

static simulator_keyboard_state_t keyboard_state;

static uint8_t s_t_was_pressed;
static uint8_t s_escape_was_pressed;

static void reset_momentary_keys(void)
{
    keyboard_state.throttle_pressed = 0;
    keyboard_state.brake_pressed = 0;
    keyboard_state.steer_left_pressed = 0;
    keyboard_state.steer_right_pressed = 0;
}

void simulator_keyboard_init(void)
{
    memset(
        &keyboard_state,
        0,
        sizeof(keyboard_state)
    );

    s_t_was_pressed = 0;
    s_escape_was_pressed = 0;
}

void simulator_keyboard_update(void)
{
    reset_momentary_keys();

    /*
     * 只更新键盘状态，不使用 SDL_PollEvent()。
     *
     * 鼠标事件必须留给 LVGL SDL 驱动处理，
     * 否则点击和滑动手势会失效。
     */
    SDL_PumpEvents();

    /*
     * 只从队列取出窗口关闭事件。
     * 不取鼠标和普通键盘事件。
     */
    SDL_Event event;

    while (
        SDL_PeepEvents(
            &event,
            1,
            SDL_GETEVENT,
            SDL_QUIT,
            SDL_QUIT
        ) > 0
    ) {
        keyboard_state.quit_requested = 1;
    }

    const Uint8 *keys =
        SDL_GetKeyboardState(NULL);

    const uint8_t t_pressed =
        keys[SDL_SCANCODE_T] ? 1u : 0u;

    if (t_pressed && !s_t_was_pressed) {
        keyboard_state.traffic_mode_enabled =
            keyboard_state.traffic_mode_enabled
                ? 0u
                : 1u;
    }

    s_t_was_pressed = t_pressed;

    const uint8_t escape_pressed =
        keys[SDL_SCANCODE_ESCAPE] ? 1u : 0u;

    if (
        escape_pressed &&
        !s_escape_was_pressed
    ) {
        keyboard_state.quit_requested = 1;
    }

    s_escape_was_pressed = escape_pressed;

    keyboard_state.throttle_pressed =
        keys[SDL_SCANCODE_W] ||
        keys[SDL_SCANCODE_UP];

    keyboard_state.brake_pressed =
        keys[SDL_SCANCODE_S] ||
        keys[SDL_SCANCODE_DOWN];

    keyboard_state.steer_left_pressed =
        keys[SDL_SCANCODE_A] ||
        keys[SDL_SCANCODE_LEFT];

    keyboard_state.steer_right_pressed =
        keys[SDL_SCANCODE_D] ||
        keys[SDL_SCANCODE_RIGHT];
}

const simulator_keyboard_state_t *
simulator_keyboard_get_state(void)
{
    return &keyboard_state;
}
