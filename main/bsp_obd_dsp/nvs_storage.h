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
    uint8_t default_page;   // 默认页面: 0=Temp, 1=Main, 2=Gear, 3=RPM, 4=Speed, 5=Info, 6=Brake, 7=OilP, 8=Needle
    uint8_t brightness_day; // 亮度 10-100, 0=未配置(用 100)
    uint8_t vehicle_profile_idx; // 车辆配置索引, 0=BRZ ZC6, 1=BRZ ZD8
    uint16_t brake_temp_warn_c; // 刹车温度警告阈值 °C
    uint16_t oil_pressure_warn_x10; // 机油压力警告阈值 0.1bar
    uint8_t temp_display_map[3]; // TEMP 页三行显示项映射
    uint8_t info_display_map[5]; // INFO 页五宫格显示项映射
    uint8_t needle_source_idx;   // 指针页数据源 (disp_item_t 值, 默认 0=CLT)
    uint8_t device_role;         // 三连表角色: 0=主表(连 ELM327), 1=从表(收主表数据)
    uint8_t chart_source_idx;    // 通用曲线页显示的数据项 (disp_item_t 值, 默认 8=OILP)
    uint16_t rpm_warn_threshold; // 转速报警阈值 (rpm), 0=未设置(默认 6000)
    uint8_t rpm_warn_anim_en;    // 转速报警动画开关: 0=关, 1=开
    uint8_t espnow_master_mac[6];// 从表绑定的主表 MAC 地址 (全 0 表示未绑定/自动接收)
} nvs_user_cfg_t;

/*------------------ 运行统计（定期落盘） ------------------*/
typedef struct {
    uint64_t odometer_m;   // 累计里程 (m)
    uint64_t trip_m;       // 本次行程里程 (m)
    uint64_t run_time_s;   // 累计行驶时间 (s)
    uint16_t max_speed_kmh; // 最大速度 km/h
    uint16_t avg_speed_kmh;
    uint32_t trip_run_time_s; // 本次行程行驶时间 (s)
    uint8_t  rsv[2];
} nvs_stat_t;

esp_err_t nvs_storage_init(void);

/* 用户配置接口 */
const nvs_user_cfg_t * nvs_cfg_get(void);
esp_err_t nvs_cfg_set(const nvs_user_cfg_t *cfg);

// 曲线页每数据项独立报警阈值(原始值单位, 值>=阈值报警; 32767=关闭)。item = disp_item_t 值。
int16_t nvs_chart_alarm_get(uint8_t item);
void    nvs_chart_alarm_set(uint8_t item, int16_t raw_threshold);

// 三连表开机动画: 0=OFF, 1=RACE AS ONE, 2=VIDEO。独立 blob, 不改 cfg 结构体。
uint8_t nvs_intro_enable_get(void);           // 0=OFF 1=RACE 2=VIDEO
void    nvs_intro_enable_set(uint8_t en);
uint8_t nvs_device_position_get(void);        // 1/2/3
void    nvs_device_position_set(uint8_t pos);

// 开机模式: 0=默认 Sky Gauge 动画, 1=自定义开机图片, 2=视频动画(boot_block)
uint8_t nvs_boot_mode_get(void);
void    nvs_boot_mode_set(uint8_t mode);

/* 运行统计接口 */
const nvs_stat_t * nvs_stat_get(void);
void nvs_stat_reset_trip(void);
void nvs_stat_update_speed(uint8_t speed_kmh,uint32_t dt_ms);
nvs_stat_t nvs_stat_get_mileage(void);
