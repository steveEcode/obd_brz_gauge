#ifndef MOCK_ECU_H
#define MOCK_ECU_H

#include <stdint.h>

#include "vehicle_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void mock_ecu_init(void);

void mock_ecu_update(uint32_t elapsed_ms);

const vehicle_state_t *mock_ecu_get_state(void);

void mock_ecu_set_throttle(float throttle_pct);

void mock_ecu_set_brake(uint8_t pressed);

void mock_ecu_set_steering(float steering_value);

void mock_ecu_set_traffic_mode(uint8_t enabled);

#ifdef __cplusplus
}
#endif

#endif
