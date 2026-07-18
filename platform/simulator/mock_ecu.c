#include "mock_ecu.h"

#include <math.h>
#include <stddef.h>

static vehicle_state_t vehicle;

static float target_throttle_pct;
static float steering_input;
static uint8_t brake_pressed;
static uint8_t traffic_mode;

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static float approach_float(
    float current,
    float target,
    float rate_per_second,
    float delta_seconds
)
{
    const float difference = target - current;
    const float maximum_step = rate_per_second * delta_seconds;

    if (difference > maximum_step) {
        return current + maximum_step;
    }

    if (difference < -maximum_step) {
        return current - maximum_step;
    }

    return target;
}

static int8_t calculate_gear(float speed_kph)
{
    if (speed_kph < 1.0f) {
        return 0;
    }

    if (speed_kph < 20.0f) {
        return 1;
    }

    if (speed_kph < 40.0f) {
        return 2;
    }

    if (speed_kph < 65.0f) {
        return 3;
    }

    if (speed_kph < 95.0f) {
        return 4;
    }

    if (speed_kph < 130.0f) {
        return 5;
    }

    return 6;
}

void mock_ecu_init(void)
{
    vehicle.rpm = 850.0f;
    vehicle.speed_kph = 0.0f;
    vehicle.throttle_pct = 0.0f;
    vehicle.gear = 0;

    vehicle.coolant_temp_c = 78.0f;
    vehicle.oil_temp_c = 72.0f;
    vehicle.intake_temp_c = 25.0f;

    vehicle.battery_voltage = 14.1f;

    vehicle.longitudinal_g = 0.0f;
    vehicle.lateral_g = 0.0f;
    vehicle.vertical_g = 1.0f;

    vehicle.engine_load_pct = 18.0f;
    vehicle.oil_pressure_psi = 28.0f;

    vehicle.ignition_on = 1;
    vehicle.engine_running = 1;
    vehicle.obd_connected = 1;

    target_throttle_pct = 0.0f;
    steering_input = 0.0f;
    brake_pressed = 0;
    traffic_mode = 0;
}

void mock_ecu_update(uint32_t elapsed_ms)
{
    const float delta_seconds = (float) elapsed_ms / 1000.0f;

    vehicle.throttle_pct = approach_float(
        vehicle.throttle_pct,
        target_throttle_pct,
        120.0f,
        delta_seconds
    );

    float acceleration = vehicle.throttle_pct * 0.055f;

    if (brake_pressed) {
        acceleration -= 8.0f;
    } else {
        acceleration -= 0.55f;
    }

    if (traffic_mode && vehicle.speed_kph > 18.0f) {
        acceleration -= 3.5f;
    }

    vehicle.speed_kph += acceleration * delta_seconds;
    vehicle.speed_kph = clamp_float(vehicle.speed_kph, 0.0f, 220.0f);

    vehicle.gear = calculate_gear(vehicle.speed_kph);

    float target_rpm = 850.0f;

    if (vehicle.gear > 0) {
        target_rpm =
            900.0f +
            vehicle.speed_kph * (38.0f / (float) vehicle.gear) +
            vehicle.throttle_pct * 22.0f;
    }

    if (brake_pressed && vehicle.speed_kph < 2.0f) {
        target_rpm = 850.0f;
    }

    target_rpm = clamp_float(target_rpm, 750.0f, 7800.0f);

    vehicle.rpm = approach_float(
        vehicle.rpm,
        target_rpm,
        5500.0f,
        delta_seconds
    );

    vehicle.engine_load_pct = clamp_float(
        12.0f + vehicle.throttle_pct * 0.88f,
        0.0f,
        100.0f
    );

    vehicle.longitudinal_g = clamp_float(
        acceleration / 9.80665f,
        -1.2f,
        1.2f
    );

    vehicle.lateral_g = approach_float(
        vehicle.lateral_g,
        steering_input * vehicle.speed_kph / 140.0f,
        2.5f,
        delta_seconds
    );

    vehicle.lateral_g = clamp_float(vehicle.lateral_g, -1.2f, 1.2f);

    vehicle.coolant_temp_c = approach_float(
        vehicle.coolant_temp_c,
        92.0f + vehicle.engine_load_pct * 0.06f,
        0.8f,
        delta_seconds
    );

    vehicle.oil_temp_c = approach_float(
        vehicle.oil_temp_c,
        96.0f + vehicle.engine_load_pct * 0.10f,
        0.5f,
        delta_seconds
    );

    vehicle.oil_pressure_psi =
        18.0f + vehicle.rpm * 0.007f;

    vehicle.battery_voltage =
        14.1f + sinf(vehicle.rpm * 0.001f) * 0.05f;
}

const vehicle_state_t *mock_ecu_get_state(void)
{
    return &vehicle;
}

void mock_ecu_set_throttle(float throttle_pct)
{
    target_throttle_pct = clamp_float(throttle_pct, 0.0f, 100.0f);
}

void mock_ecu_set_brake(uint8_t pressed)
{
    brake_pressed = pressed ? 1 : 0;
}

void mock_ecu_set_steering(float steering_value)
{
    steering_input = clamp_float(steering_value, -1.0f, 1.0f);
}

void mock_ecu_set_traffic_mode(uint8_t enabled)
{
    traffic_mode = enabled ? 1 : 0;
}
