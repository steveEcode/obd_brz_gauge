#include "obd_data_cache.h"
#include "vehicle_profiles.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <inttypes.h>


// 使用简单全局变量 + 临界区保护
static volatile uint16_t s_rpm = 0;
static volatile uint8_t  s_speed = 0;
static volatile int16_t  s_coolant_temp = -40;
static volatile int16_t  s_oil_temp = -100;  // 真实机油温度 °C, -100=无效
static volatile int16_t  s_intake_temp = -40;
static volatile int16_t  s_load_pct = -1;   // 发动机负荷 0~100%, -1=无效
static volatile int16_t  s_tps = -1;         // 节气门开度 0~100%, -1=无效
static volatile int32_t  s_bat_mv = -1;     // 电压 mV, -1=无效
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void obd_data_set_rpm(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm = rpm;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_speed(uint8_t kmh)
{
    portENTER_CRITICAL(&s_mux);
    s_speed = kmh;
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
    // 有效范围 -20~150°C，超出视为解析错误丢弃
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

#define RPM_SMOOTH_TIME_MS   1000  // 转速缓升缓降时间常数 (ms)
#define SPEED_SMOOTH_TIME_MS 1000  // 速度缓升缓降时间常数 (ms)
#define FALL_TO_ZERO_MS      500  // 归零缓降时间常数 (ms)

// 实时转速（缓升缓降）获取
uint16_t obd_data_get_rpm(void)
{
    static TickType_t last_tick = 0;
    static float smooth = 0.f;

    uint16_t raw;
    portENTER_CRITICAL(&s_mux);
    raw = s_rpm;
    portEXIT_CRITICAL(&s_mux);

    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = (now_tick - last_tick) * portTICK_PERIOD_MS;
    if (dt_ms > 1000) dt_ms = 1000;

    uint32_t tc = (raw == 0) ? FALL_TO_ZERO_MS : RPM_SMOOTH_TIME_MS;
    float alpha = (float)dt_ms / (float)tc;
    if (alpha > 1.0f) alpha = 1.0f;

    smooth += alpha * ((float)raw - smooth);
    last_tick = now_tick;

    return (uint16_t)(smooth + 0.5f);
}


// 实时速度（缓升缓降）获取
uint8_t obd_data_get_speed(void)
{
    static TickType_t last_tick = 0;
    static float smooth = 0.f; // 保留小数以获得更细腻的过渡

    // 1. 取原始速度
    uint8_t raw;
    portENTER_CRITICAL(&s_mux);
    raw = s_speed;
    portEXIT_CRITICAL(&s_mux);

    // 2. 计算距离上次调用的时间，单位 ms
    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = (now_tick - last_tick) * portTICK_PERIOD_MS;
    if (dt_ms > 1000) dt_ms = 1000; // 限制单次过大步长，防止休眠后跳变

    // 3. 时间常数的一阶滤波 alpha = dt / SPEED_SMOOTH_TIME_MS
    uint32_t tc = (raw == 0) ? FALL_TO_ZERO_MS : SPEED_SMOOTH_TIME_MS;
    float alpha = (float)dt_ms / (float)tc;
    if (alpha > 1.0f) alpha = 1.0f;

    // 4. 更新平滑值
    smooth += alpha * ((float)raw - smooth);
    last_tick = now_tick;
    return (uint8_t)(smooth + 0.5f); // 四舍五入返回
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

/**
 * @brief 根据转速和车速计算并判断档位
 * @param rpm 发动机转速 (RPM)
 * @param speed 车速 (km/h)
 * @return 计算出的档位
 */
enGear calculate_gear(float rpm, float speed) {
    static enGear s_last_gear = GEAR_NEUTRAL;
    // 1. 检查输入数据有效性
    if (rpm <= 0 || speed <= 0) {
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }

    // 2. 使用当前车辆配置计算总传动比
    const vehicle_profile_t *profile = vehicle_profile_get_active();
    float calc_const = vehicle_profile_calc_constant(profile);
    float total_ratio = rpm / (speed * calc_const);
    ESP_LOGD("gear", "RPM=%.0f Speed=%.1f ratio=%.2f", rpm, speed, total_ratio);

    // 3. 与各档位范围进行比较
    uint8_t range_count = 0;
    const gear_ratio_range_t *ranges = vehicle_profile_get_gear_ranges(&range_count);
    for (int i = 0; i < range_count; i++) {
        if (total_ratio >= ranges[i].min_ratio &&
            total_ratio <= ranges[i].max_ratio) {
            s_last_gear = ranges[i].gear;
            return ranges[i].gear;
        }
    }
    
    // 4. 如果在所有范围外，检查是否可能为空档（转速高车速为零）
    if (rpm > 800 && speed < 5) { // 怠速以上且几乎静止
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }
    
    // 5. 无法识别的传动比 返回上一次档位
    return s_last_gear;
}


/**
 * @brief 里程统计任务
 * @param pvParameter 参数
 * @return 无
 * @note  
 * @note 里程统计任务
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
 * @brief 初始化里程统计任务
 * @return 无
 * @note  
 * @note 初始化里程统计任务
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
  