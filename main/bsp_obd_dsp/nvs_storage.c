#include "nvs_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "export_path/ui.h"
#include "app_obd_dsp/vehicle_profiles.h"

#define TAG                   "nvs_storage"
#define NS_CFG                "cfg"
#define KEY_CFG               "settings"
#define NS_STAT               "stat"
#define KEY_STAT              "runtime"
#define KEY_CHART_ALARM       "chartalarm"
#define CHART_ALARM_N         11   // = DISP_ITEM_COUNT (需与 ui.c disp_item_t 同步)
#define STAT_FLUSH_PERIOD_MS  30000 //30s 落盘

static nvs_user_cfg_t s_cfg =   { 
                        .protocol = 0, //车辆OBD的协议类型选择 0:自动,1~9:固定协议 默认为自动
                        .theme_cfg.theme = 1,//主题配置
                        .theme_cfg.user_theme_domiant_color = COLOR_DOMIANT_PINK,//主题主色调颜色
                        .theme_cfg.user_theme_secondary_color = COLOR_SECONDARY_PINK,//主题副色调颜色
                        .ble_device_name = "", // 空串=使用默认 "OBDII"
                        .temp_display_map = {0, 1, 2}, // CLT, IAT, OIL
                        .info_display_map = {0, 2, 3, 4, 1}, // CLT, OIL, LOAD, TPS, IAT
                        .brake_temp_warn_c = 600,
                        .oil_pressure_warn_x10 = 80,
                    };
static nvs_stat_t     s_stat = {0};
static bool           s_stat_dirty = false;
static SemaphoreHandle_t s_mux;

// 每数据项报警阈值(原始值单位), 索引=disp_item_t: CLT,IAT,OIL,LOD,TPS,RPM,SPD,BAT,OIP,BKT,BST
// 默认只给油压(8.0bar=x10 80)和刹车温(600°C=x10 6000)保留旧报警值, 其余 32767=关闭(不误红)
static int16_t s_chart_alarm[CHART_ALARM_N] = {
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 80, 6000, 32767
};

/* 前向声明 */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len);
static esp_err_t save_blob(const char *ns,const char *key,const void *data,size_t len);
static void stat_flush_task(void *arg);

esp_err_t nvs_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
    load_blob(NS_STAT, KEY_STAT, &s_stat, sizeof(s_stat));
    {   // 曲线报警阈值: NVS 有则加载, 无/长度不符则保持静态默认(不覆盖, 故不会全变0)
        nvs_handle_t h; size_t sz = sizeof(s_chart_alarm);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_CHART_ALARM, s_chart_alarm, &sz);
            nvs_close(h);
        }
    }

    /* 新增字段默认值修复 (旧NVS数据中rsv[x]全为0) */
    if(s_cfg.brightness_day == 0) s_cfg.brightness_day = 100;
    if(s_cfg.default_page > 6) s_cfg.default_page = 0; // 0=Temp,1=Info,2=Chart,3=Needle,4=Gear,5=Rpm,6=Speed(刹车温已并入Chart)
    if(s_cfg.needle_source_idx >= 10) s_cfg.needle_source_idx = 0; // DISP_ITEM_COUNT=10
    if(s_cfg.device_role > 1) s_cfg.device_role = 0; // 三连表角色: 0=主 1=从, 越界归零到主表
    if(s_cfg.chart_source_idx >= 11) s_cfg.chart_source_idx = 8; // 曲线数据项越界→默认 OILP(旧NVS该字节为0=CLT亦可, 这里统一到OILP)
    // 车型索引按已注册的 profile 数量限界（越界归零到 ZC6）
    uint8_t vehicle_count = 0;
    vehicle_profile_get_all(&vehicle_count);
    if(vehicle_count > 0 && s_cfg.vehicle_profile_idx >= vehicle_count) s_cfg.vehicle_profile_idx = 0;
    if(s_cfg.brake_temp_warn_c < 10 || s_cfg.brake_temp_warn_c > 1200) s_cfg.brake_temp_warn_c = 600;
    if(s_cfg.oil_pressure_warn_x10 > 100) s_cfg.oil_pressure_warn_x10 = 80;

    // TEMP/INFO 自定义显示项映射范围校验: 0~9
    for (int i = 0; i < 3; ++i) {
        if (s_cfg.temp_display_map[i] > 9) s_cfg.temp_display_map[i] = (uint8_t)i;
    }
    for (int i = 0; i < 5; ++i) {
        if (s_cfg.info_display_map[i] > 9) {
            static const uint8_t def_map[5] = {0, 2, 3, 4, 1};
            s_cfg.info_display_map[i] = def_map[i];
        }
    }

    s_mux = xSemaphoreCreateMutex();
    xTaskCreate(stat_flush_task, "nvs_flush", 2048, NULL, 4, NULL);
    return ESP_OK;
}

/* 用户配置 */
const nvs_user_cfg_t * nvs_cfg_get(void){ return &s_cfg; }

esp_err_t nvs_cfg_set(const nvs_user_cfg_t *cfg)
{
    if(!cfg) return ESP_ERR_INVALID_ARG;
    if(memcmp(cfg,&s_cfg,sizeof(s_cfg))==0) return ESP_OK;
    s_cfg=*cfg;
    return save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
}

/* 曲线报警阈值 */
int16_t nvs_chart_alarm_get(uint8_t item){
    return (item < CHART_ALARM_N) ? s_chart_alarm[item] : 32767;
}
void nvs_chart_alarm_set(uint8_t item, int16_t raw_threshold){
    if(item >= CHART_ALARM_N) return;
    if(s_chart_alarm[item] == raw_threshold) return;
    s_chart_alarm[item] = raw_threshold;
    save_blob(NS_CFG, KEY_CHART_ALARM, s_chart_alarm, sizeof(s_chart_alarm));
}

/* 统计 */
const nvs_stat_t * nvs_stat_get(void){return &s_stat;}
void nvs_stat_add_odometer(uint32_t d){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_stat.odometer_m+=d;
    s_stat_dirty=true;
    xSemaphoreGive(s_mux);
}
void nvs_stat_add_runtime(uint32_t d){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_stat.run_time_s+=d;
    s_stat_dirty=true;
    xSemaphoreGive(s_mux);
}
/**
 * @brief 更新行驶统计
 * @param speed_kmh 速度km/h
 * @param dt_ms 时间ms
 * @return 无
 * @note 如果速度为0，则不更新行驶统计
 * @note 如果时间小于1000ms，则不更新行驶统计
 */
void nvs_stat_update_speed(uint8_t speed_kmh, uint32_t dt_ms)
{
    if(dt_ms<1000) return;
    if(speed_kmh == 0) return;

    xSemaphoreTake(s_mux,portMAX_DELAY);
    /* 1. 距离 = v(km/h)*dt(ms)/3.6  (m) */
    double dist_m = ((double)speed_kmh * (double)dt_ms) / 3.6e3;
    s_stat.odometer_m += (uint64_t)dist_m;
    s_stat.trip_m     += (uint64_t)dist_m;

    /* 2. 时间 */
    s_stat.run_time_s += dt_ms/1000;
    s_stat.trip_run_time_s += dt_ms/1000;

    /* 3. 最高速度 */
    if(speed_kmh > s_stat.max_speed_kmh) s_stat.max_speed_kmh = speed_kmh;

    /* 4. 平均速度 = 本次行程距离 / 本次行程时间 (m/s) -> km/h */
    if(s_stat.trip_run_time_s){
        double avg_ms = (double)s_stat.trip_m / (double)s_stat.trip_run_time_s; // m/s
        s_stat.avg_speed_kmh = (uint16_t)(avg_ms * 3.6 + 0.5);
        if(s_stat.avg_speed_kmh > s_stat.max_speed_kmh) s_stat.avg_speed_kmh = s_stat.max_speed_kmh;
    }

    s_stat_dirty = true;
    xSemaphoreGive(s_mux);
}

/*
 * @brief 重置本次行程统计
 * @return 无
 * @note  
 * @note 清除本次行程统计，包括里程、最大速度、平均速度、行驶时间
*/
void nvs_stat_reset_trip(void){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_stat.trip_m=0;
    s_stat.max_speed_kmh=0;
    s_stat.avg_speed_kmh=0;
    s_stat.run_time_s=0;
    s_stat.trip_run_time_s=0;
    s_stat_dirty=true;
    xSemaphoreGive(s_mux);
}

/*获取行程统计信息
 * @return 行程统计信息结构体
 * @note  
 * @note 获取本次行程统计信息，包括里程、最大速度、平均速度、行驶时间
*/
nvs_stat_t nvs_stat_get_mileage(void){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    nvs_stat_t stat = s_stat;
    xSemaphoreGive(s_mux);
    return stat;
}


/* 后台任务 */
static void stat_flush_task(void *arg){
    while(1){
        vTaskDelay(pdMS_TO_TICKS(STAT_FLUSH_PERIOD_MS));
        if(!s_stat_dirty) continue;
        // 锁内只做快照 + 清脏标志(极短临界区); 真正耗时的 flash 写入放到锁外,
        // 避免持锁跨越 NVS I/O 阻塞里程统计等写 s_stat 的任务。
        nvs_stat_t snapshot;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        snapshot = s_stat;
        s_stat_dirty = false;   // 若写盘期间有新数据写入, 写方会再置脏, 下一轮再落盘(不丢更新)
        xSemaphoreGive(s_mux);

        if (save_blob(NS_STAT, KEY_STAT, &snapshot, sizeof(snapshot)) != ESP_OK) {
            // 写失败 → 重新标脏, 下一轮重试
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_stat_dirty = true;
            xSemaphoreGive(s_mux);
        }
    }
}

/* 工具函数 */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len)
{
    nvs_handle_t h; size_t size=len; esp_err_t err;
    if(nvs_open(ns,NVS_READONLY,&h)==ESP_OK){
        err=nvs_get_blob(h,key,out,&size);
        nvs_close(h);
        if(err==ESP_OK && size==len) return ESP_OK;
    }
    memset(out,0,len);
    return save_blob(ns,key,out,len);
}

static esp_err_t save_blob(const char *ns,const char *key,const void *data,size_t len)
{
    nvs_handle_t h; esp_err_t err=nvs_open(ns,NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    err=nvs_set_blob(h,key,data,len);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err;
}
