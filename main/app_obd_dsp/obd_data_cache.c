#include "obd_data_cache.h"
#include "vehicle_profiles.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <inttypes.h>


// Simple globals protected by a critical section
static volatile uint16_t s_rpm = 0;
static volatile uint8_t  s_speed = 0;
static volatile int16_t  s_coolant_temp = -40;
static volatile int16_t  s_oil_temp = -100;  // actual oil temp °C, -100=invalid
static volatile int16_t  s_intake_temp = -40;
static volatile int16_t  s_load_pct = -1;   // engine load 0~100%, -1=invalid
static volatile int16_t  s_tps = -1;         // throttle opening 0~100%, -1=invalid
static volatile int32_t  s_bat_mv = -1;     // battery voltage mV, -1=invalid
static volatile int16_t  s_oil_pressure_x10 = -1; // oil pressure, 0.1bar, -1=invalid
static volatile int16_t  s_brake_temp_x10 = -1000; // brake temp, 0.1°C, -1000=invalid
static volatile int16_t  s_boost_x10 = -32768; // boost gauge pressure, 0.1bar (can be negative = vacuum), -32768=invalid
static volatile int8_t   s_gear = 127;          // direct gear value: -1=R, 0=N, 1+=forward gear, 127=invalid
static volatile int16_t  s_afr_x100 = -1;       // air-fuel ratio AFR, ×100 (1470=14.7:1), -1=invalid
static volatile brake_rs485_status_t s_brake_rs485_status = BRAKE_RS485_IDLE;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define RPM_SMOOTH_TIME_MS   30    // deprecated: RPM is passed through raw and needs no smoothing; kept for compatibility with old references
#define SPEED_SMOOTH_TIME_MS 300   // speed ramp-up/down time constant (ms); the UI side already has anim_step, keep this small
#define FALL_TO_ZERO_MS      500   // fall-to-zero ramp-down time constant (ms)

// Smoothing state (advanced on the setter side, getters only read; unaffected by the number of callers)
static volatile uint16_t s_rpm_smooth = 0;
static volatile uint8_t  s_speed_smooth = 0;
static TickType_t s_speed_last_tick = 0;
static float s_speed_smooth_f = 0.f;

// RPM override layer: during multi-gauge linkage tests, the master gauge injects simulated RPM here.
// When enabled, obd_data_get_rpm() returns the override value (used for both local display and ESP-NOW broadcast);
// the real OBD RPM keeps being written to s_rpm_smooth and is restored immediately once the override is cleared.
static volatile bool     s_rpm_override_en = false;
static volatile uint16_t s_rpm_override_val = 0;

void obd_data_rpm_override_set(bool en, uint16_t val)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_override_en = en;
    s_rpm_override_val = val;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_rpm(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm = rpm;
    s_rpm_smooth = rpm;  // CAN 100Hz data is already clean, no smoothing needed
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_speed(uint8_t kmh)
{
    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = (now_tick - s_speed_last_tick) * portTICK_PERIOD_MS;
    if (dt_ms > 1000) dt_ms = 1000;
    s_speed_last_tick = now_tick;

    uint32_t tc = (kmh == 0) ? FALL_TO_ZERO_MS : SPEED_SMOOTH_TIME_MS;
    float alpha = (float)dt_ms / (float)tc;
    if (alpha > 1.0f) alpha = 1.0f;
    s_speed_smooth_f += alpha * ((float)kmh - s_speed_smooth_f);

    uint8_t smoothed = (uint8_t)(s_speed_smooth_f + 0.5f);
    portENTER_CRITICAL(&s_mux);
    s_speed = kmh;
    s_speed_smooth = smoothed;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_coolant_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_oil_temp(int16_t temp)
{
    // Valid range -20~150°C; values outside are treated as parse errors and dropped
    if (temp < -20 || temp > 150) return;
    portENTER_CRITICAL(&s_mux);
    s_oil_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_intake_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_intake_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

// RPM/speed: smoothing is done on the setter side; getters return the smoothed value directly (safe with multiple callers)
uint16_t obd_data_get_rpm(void)
{
    uint16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_rpm_override_en ? s_rpm_override_val : s_rpm_smooth;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

uint8_t obd_data_get_speed(void)
{
    uint8_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_speed_smooth;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

int16_t obd_data_get_coolant_temp(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_coolant_temp;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

int16_t obd_data_get_oil_temp(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_oil_temp;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

int16_t obd_data_get_intake_temp(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_intake_temp;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_load_pct(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_load_pct = pct;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_load_pct(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_load_pct;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_tps(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_tps = pct;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_tps(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_tps;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_bat_mv(int32_t mv)
{
    portENTER_CRITICAL(&s_mux);
    s_bat_mv = mv;
    portEXIT_CRITICAL(&s_mux);
}

int32_t obd_data_get_bat_mv(void)
{
    int32_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_bat_mv;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_oil_pressure_x10(int16_t pressure_x10)
{
    // Plausible range: 0.0bar ~ 20.0bar
    if (pressure_x10 < 0 || pressure_x10 > 200) return;
    portENTER_CRITICAL(&s_mux);
    s_oil_pressure_x10 = pressure_x10;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_oil_pressure_x10(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_oil_pressure_x10;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_boost_x10(int16_t boost_x10)
{
    // Plausible boost gauge pressure range: -1.5bar (vacuum) ~ +30.0bar
    if (boost_x10 < -15 || boost_x10 > 300) return;
    portENTER_CRITICAL(&s_mux);
    s_boost_x10 = boost_x10;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_boost_x10(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_boost_x10;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_brake_temp_x10(int16_t temp_x10)
{
    // Plausible range: -50.0°C ~ 1200.0°C
    if (temp_x10 < -500 || temp_x10 > 12000) return;
    portENTER_CRITICAL(&s_mux);
    s_brake_temp_x10 = temp_x10;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_brake_rs485_status(brake_rs485_status_t status)
{
    portENTER_CRITICAL(&s_mux);
    s_brake_rs485_status = status;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_brake_temp_x10(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_brake_temp_x10;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_gear(int8_t gear)
{
    portENTER_CRITICAL(&s_mux);
    s_gear = gear;
    portEXIT_CRITICAL(&s_mux);
}

int8_t obd_data_get_gear(void)
{
    int8_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_gear;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_afr_x100(int16_t afr_x100)
{
    // Plausible range: 8.0:1 ~ 22.0:1 (800~2200 ×100)
    if (afr_x100 < 800 || afr_x100 > 2200) return;
    portENTER_CRITICAL(&s_mux);
    s_afr_x100 = afr_x100;
    portEXIT_CRITICAL(&s_mux);
}

int16_t obd_data_get_afr_x100(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_afr_x100;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

/**
 * @brief Compute and determine the gear from RPM and vehicle speed
 * @param rpm engine speed (RPM)
 * @param speed vehicle speed (km/h)
 * @return the computed gear
 */
enGear calculate_gear(float rpm, float speed) {
    static enGear s_last_gear = GEAR_NEUTRAL;
    // 1. Check input data validity
    if (rpm <= 0 || speed <= 0) {
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }

    // 2. Compute the total gear ratio using the active vehicle profile
    const vehicle_profile_t *profile = vehicle_profile_get_active();
    float calc_const = vehicle_profile_calc_constant(profile);
    float total_ratio = rpm / (speed * calc_const);
    ESP_LOGD("gear", "RPM=%.0f Speed=%.1f ratio=%.2f", rpm, speed, total_ratio);

    // 3. Compare against each gear's ratio range
    uint8_t range_count = 0;
    const gear_ratio_range_t *ranges = vehicle_profile_get_gear_ranges(&range_count);
    for (int i = 0; i < range_count; i++) {
        if (total_ratio >= ranges[i].min_ratio &&
            total_ratio <= ranges[i].max_ratio) {
            s_last_gear = ranges[i].gear;
            return ranges[i].gear;
        }
    }
    
    // 4. Outside all ranges: check if it could be neutral (high RPM, near-zero speed)
    if (rpm > 800 && speed < 5) { // Above idle and nearly stationary
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }
    
    // 5. Unrecognized ratio: return the last gear
    return s_last_gear;
}


/**
 * @brief Mileage statistics timer callback
 * @param pvParameter argument
 * @return none
 * @note
 * @note Mileage statistics task
 */
static void mileage_timer_cb(void* arg)
{
    static uint16_t usPrintCnt = 0;
    nvs_stat_update_speed(obd_data_get_speed(), 1000);

    if(obd_data_get_speed() > 0){
        usPrintCnt++;
        if(usPrintCnt >= 20){
            usPrintCnt = 0;
            nvs_stat_t stat = nvs_stat_get_mileage();
            ESP_LOGI("MileageStat", " odometer: %" PRIu64 ", trip: %" PRIu64 ", run_time: %" PRIu64 ", max_speed: %d, avg_speed: %d, speed: %d", stat.odometer_m, stat.trip_m, stat.run_time_s, stat.max_speed_kmh, stat.avg_speed_kmh, obd_data_get_speed());
        }
    }
}

/**
 * @brief Initialize the mileage statistics task
 * @return none
 * @note
 * @note Initialize the mileage statistics task
 */
void vMileageDataStatisticTask(void)
{
    ESP_LOGI("MileageStat", "MileageStatTask Init Start");
    static esp_timer_handle_t s_timer = NULL;
    if(!s_timer){
        const esp_timer_create_args_t args={
            .callback = mileage_timer_cb,
            .arg = NULL,
            .name = "mile_stat"
        };
        if(esp_timer_create(&args,&s_timer)==ESP_OK){
            esp_timer_start_periodic(s_timer, 1000000); //1s
        }
    }
}
  