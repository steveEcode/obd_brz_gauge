#ifndef VEHICLE_STATE_H
#define VEHICLE_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Engine and transmission */
    float rpm;
    float speed_kph;
    float throttle_pct;
    int8_t gear;

    /* Temperatures */
    float coolant_temp_c;
    float oil_temp_c;
    float intake_temp_c;

    /* Electrical */
    float battery_voltage;

    /* Vehicle motion */
    float longitudinal_g;
    float lateral_g;
    float vertical_g;

    /* Additional values */
    float engine_load_pct;
    float oil_pressure_psi;

    /* State flags */
    uint8_t ignition_on;
    uint8_t engine_running;
    uint8_t obd_connected;
} vehicle_state_t;

#ifdef __cplusplus
}
#endif

#endif
