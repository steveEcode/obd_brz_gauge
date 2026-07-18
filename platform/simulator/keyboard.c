#include "keyboard.h"

#include <SDL2/SDL.h>
#include <string.h>

static simulator_keyboard_state_t keyboard_state;

static void reset_momentary_keys(void)
{
    keyboard_state.throttle_pressed = 0;
    keyboard_state.brake_pressed = 0;
    keyboard_state.steer_left_pressed = 0;
    keyboard_state.steer_right_pressed = 0;
}

void simulator_keyboard_init(void)
{
    memset(&keyboard_state, 0, sizeof(keyboard_state));
}

void simulator_keyboard_update(void)
{
    SDL_Event event;

    reset_momentary_keys();

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            keyboard_state.quit_requested = 1;
            continue;
        }

        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            const SDL_Keycode key = event.key.keysym.sym;

            if (key == SDLK_t) {
                keyboard_state.traffic_mode_enabled =
                    keyboard_state.traffic_mode_enabled ? 0 : 1;
            }

            if (key == SDLK_ESCAPE) {
                keyboard_state.quit_requested = 1;
            }
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    keyboard_state.throttle_pressed =
        keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP];

    keyboard_state.brake_pressed =
        keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN];

    keyboard_state.steer_left_pressed =
        keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT];

    keyboard_state.steer_right_pressed =
        keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT];
}

const simulator_keyboard_state_t *simulator_keyboard_get_state(void)
{
    return &keyboard_state;
}
