#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"

#include "mock_ecu.h"
#include "simulator_role_runtime.h"
#include "vehicle_state.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

typedef struct {
    bool valid;

    uint16_t rpm;
    uint8_t speed;

    int16_t coolant_temp;
    int16_t oil_temp;
    int16_t intake_temp;
    int16_t load_pct;
    int16_t tps;
    int32_t bat_mv;

    int16_t oil_pressure_x10;
    int16_t boost_x10;
    int16_t brake_temp_x10;

    brake_rs485_status_t brake_status;
} simulator_obd_cache_t;

static simulator_obd_cache_t s_received = {
    .valid = false,
    .oil_pressure_x10 = -1,
    .boost_x10 = INT16_MIN,
    .brake_temp_x10 = -1000,
    .brake_status = BRAKE_RS485_IDLE,
};

static const vehicle_state_t *current_vehicle(void)
{
    return mock_ecu_get_state();
}

static bool use_received_data(void)
{
    return
        simulator_role_runtime_is_slave() &&
        s_received.valid;
}

static int16_t float_to_i16(float value)
{
    if (value > (float)INT16_MAX) {
        return INT16_MAX;
    }

    if (value < (float)INT16_MIN) {
        return INT16_MIN;
    }

    return (int16_t)(
        value >= 0.0f
            ? value + 0.5f
            : value - 0.5f
    );
}

static uint16_t float_to_u16(float value)
{
    if (value <= 0.0f) {
        return 0;
    }

    if (value >= (float)UINT16_MAX) {
        return UINT16_MAX;
    }

    return (uint16_t)(value + 0.5f);
}

static uint8_t float_to_u8(float value)
{
    if (value <= 0.0f) {
        return 0;
    }

    if (value >= (float)UINT8_MAX) {
        return UINT8_MAX;
    }

    return (uint8_t)(value + 0.5f);
}

/* 从 Simulator Bus 收包时调用，与真机从表接口保持一致。 */

void obd_data_set_rpm(uint16_t rpm)
{
    s_received.rpm = rpm;
    s_received.valid = true;
}

void obd_data_set_speed(uint8_t kmh)
{
    s_received.speed = kmh;
    s_received.valid = true;
}

void obd_data_set_coolant_temp(int16_t temp)
{
    s_received.coolant_temp = temp;
    s_received.valid = true;
}

void obd_data_set_oil_temp(int16_t temp)
{
    s_received.oil_temp = temp;
    s_received.valid = true;
}

void obd_data_set_intake_temp(int16_t temp)
{
    s_received.intake_temp = temp;
    s_received.valid = true;
}

void obd_data_set_load_pct(int16_t pct)
{
    s_received.load_pct = pct;
    s_received.valid = true;
}

void obd_data_set_tps(int16_t pct)
{
    s_received.tps = pct;
    s_received.valid = true;
}

void obd_data_set_bat_mv(int32_t mv)
{
    s_received.bat_mv = mv;
    s_received.valid = true;
}

void obd_data_set_oil_pressure_x10(int16_t pressure_x10)
{
    s_received.oil_pressure_x10 = pressure_x10;
    s_received.valid = true;
}

void obd_data_set_boost_x10(int16_t boost_x10)
{
    s_received.boost_x10 = boost_x10;
    s_received.valid = true;
}

void obd_data_set_brake_temp_x10(int16_t temp_x10)
{
    s_received.brake_temp_x10 = temp_x10;
    s_received.valid = true;
}

void obd_data_set_brake_rs485_status(
    brake_rs485_status_t status
)
{
    s_received.brake_status = status;
    s_received.valid = true;
}

uint16_t obd_data_get_rpm(void)
{
    if (use_received_data()) {
        return s_received.rpm;
    }

    return float_to_u16(current_vehicle()->rpm);
}

uint8_t obd_data_get_speed(void)
{
    if (use_received_data()) {
        return s_received.speed;
    }

    return float_to_u8(current_vehicle()->speed_kph);
}

int16_t obd_data_get_coolant_temp(void)
{
    if (use_received_data()) {
        return s_received.coolant_temp;
    }

    return float_to_i16(
        current_vehicle()->coolant_temp_c
    );
}

int16_t obd_data_get_oil_temp(void)
{
    if (use_received_data()) {
        return s_received.oil_temp;
    }

    return float_to_i16(
        current_vehicle()->oil_temp_c
    );
}

int16_t obd_data_get_intake_temp(void)
{
    if (use_received_data()) {
        return s_received.intake_temp;
    }

    return float_to_i16(
        current_vehicle()->intake_temp_c
    );
}

int16_t obd_data_get_load_pct(void)
{
    if (use_received_data()) {
        return s_received.load_pct;
    }

    return float_to_i16(
        current_vehicle()->engine_load_pct
    );
}

int16_t obd_data_get_tps(void)
{
    if (use_received_data()) {
        return s_received.tps;
    }

    return float_to_i16(
        current_vehicle()->throttle_pct
    );
}

int32_t obd_data_get_bat_mv(void)
{
    if (use_received_data()) {
        return s_received.bat_mv;
    }

    return (int32_t)(
        current_vehicle()->battery_voltage *
        1000.0f +
        0.5f
    );
}

int16_t obd_data_get_oil_pressure_x10(void)
{
    if (use_received_data()) {
        return s_received.oil_pressure_x10;
    }

    /*
     * VehicleState stores PSI.
     * 1 PSI = 0.0689476 bar.
     */
    return float_to_i16(
        current_vehicle()->oil_pressure_psi *
        0.689476f
    );
}

int16_t obd_data_get_boost_x10(void)
{
    if (use_received_data()) {
        return s_received.boost_x10;
    }

    return INT16_MIN;
}

int16_t obd_data_get_brake_temp_x10(void)
{
    if (use_received_data()) {
        return s_received.brake_temp_x10;
    }

    return -1000;
}

brake_rs485_status_t obd_data_get_brake_rs485_status(void)
{
    if (use_received_data()) {
        return s_received.brake_status;
    }

    return BRAKE_RS485_IDLE;
}

enGear calculate_gear(float rpm, float speed)
{
    static enGear s_last_gear = GEAR_NEUTRAL;

    if (rpm <= 0.0f || speed <= 0.0f) {
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }

    const vehicle_profile_t *profile =
        vehicle_profile_get_active();

    float calc_const =
        vehicle_profile_calc_constant(profile);

    float total_ratio =
        rpm / (speed * calc_const);

    uint8_t range_count = 0;

    const gear_ratio_range_t *ranges =
        vehicle_profile_get_gear_ranges(
            &range_count
        );

    for (uint8_t index = 0;
         index < range_count;
         index++) {
        if (
            total_ratio >= ranges[index].min_ratio &&
            total_ratio <= ranges[index].max_ratio
        ) {
            s_last_gear = ranges[index].gear;
            return ranges[index].gear;
        }
    }

    if (rpm > 800.0f && speed < 5.0f) {
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }

    return s_last_gear;
}
