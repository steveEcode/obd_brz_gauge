#ifndef SIMULATOR_KEYBOARD_H
#define SIMULATOR_KEYBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t throttle_pressed;
    uint8_t brake_pressed;
    uint8_t steer_left_pressed;
    uint8_t steer_right_pressed;
    uint8_t traffic_mode_enabled;
    uint8_t quit_requested;
} simulator_keyboard_state_t;

void simulator_keyboard_init(void);

void simulator_keyboard_update(void);

const simulator_keyboard_state_t *simulator_keyboard_get_state(void);

#ifdef __cplusplus
}
#endif

#endif

