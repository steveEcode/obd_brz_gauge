#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

//主题风格结构体
typedef struct {
    uint8_t  theme;         //0 为自定义主题，1-9系统主题  1.pink_Big_face_cat   ...  默认为 1;
    uint32_t user_theme_domiant_color;   //自定义主题主色调颜色
    uint32_t user_theme_secondary_color;   //自定义主题副色调颜色
    uint8_t rsv[5];        // 预留
} theme_cfg_t;

/*------------------ 用户配置（仅修改时写入） ------------------*/
typedef struct {
    uint8_t protocol;      // 0: 自动, 1~9: 固定协议
    theme_cfg_t theme_cfg;   // 主题配置
    char    ble_device_name[32]; // 上次连接的 BLE 设备名，空串表示未配置
    uint8_t default_page;   // 默认页面: 0=Temp, 1=Main, 2=Gear, 3=RPM, 4=Speed, 5=Info
    uint8_t brightness_day; // 亮度 10-100, 0=未配置(用100)
    uint8_t rsv[8];         // 预留
} nvs_user_cfg_t;

/*------------------ 运行统计（定期落盘） ------------------*/
typedef struct {
    uint64_t odometer_m;   // 累计里程 (m)
    uint64_t trip_m;       // 本次行程里程 (m)
    uint64_t run_time_s;   // 累计行驶时间 (s)
    uint16_t max_speed_kmh; // 最大速度km/h
    uint16_t avg_speed_kmh;
    uint8_t  rsv[6];
} nvs_stat_t;

esp_err_t nvs_storage_init(void);

/* 用户配置接口 */
const nvs_user_cfg_t * nvs_cfg_get(void);
esp_err_t nvs_cfg_set(const nvs_user_cfg_t *cfg);

/* 运行统计接口 */
const nvs_stat_t * nvs_stat_get(void);
void nvs_stat_add_odometer(uint32_t delta_m);
void nvs_stat_add_runtime(uint32_t delta_s);
void nvs_stat_add_trip(uint32_t delta_m);
void nvs_stat_reset_trip(void);
void nvs_stat_update_speed(uint8_t speed_kmh,uint32_t dt_ms);
nvs_stat_t nvs_stat_get_mileage(void);