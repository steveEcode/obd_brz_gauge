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
#include "espnow_link.h"   // ESPNOW_ROLE_* (device_role 默认/边界)

#define TAG                   "nvs_storage"
#define NS_CFG                "cfg"
#define KEY_CFG               "settings"
#define KEY_CHART_ALARM       "chartalarm"
#define CHART_ALARM_N         11   // = DISP_ITEM_COUNT (需与 ui.c disp_item_t 同步)
#define CHART_ALARM_OFF       32767 // 报警阈值"关闭"哨兵值(超不到, 不误红)
#define KEY_MG_EXTRA          "mgextra"   // 三连表开机动画设置
#define KEY_CFG_VERSION       "cfgver"    // 配置版本号 (缺失=v0)
#define CFG_VERSION_CURRENT   1           // 当前版本; 每次加字段 +1

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
                        .device_role = ESPNOW_ROLE_STANDALONE, // 新设备(NVS无cfg)默认单机, 不启WiFi/ESP-NOW; 老设备被load_blob覆盖回原值
                        .rpm_warn_threshold = 6000,
                        .rpm_warn_anim_en = 0,
                    };
static nvs_stat_t     s_stat = {0};   // 仅运行时内存统计, 不落盘(每次开机清零, 省 flash 寿命)
static SemaphoreHandle_t s_mux;

// 每数据项报警阈值(原始值单位), 索引=disp_item_t: CLT,IAT,OIL,LOD,TPS,RPM,SPD,BAT,OIP,BKT,BST
// 默认只给油压(8.0bar=x10 80)和刹车温(600°C=x10 6000)保留旧报警值, 其余关闭(不误红)
static int16_t s_chart_alarm[CHART_ALARM_N] = {
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    80, 6000, CHART_ALARM_OFF
};

// 三连表开机动画设置(独立 blob): 默认 关闭, 位置 1
static struct __attribute__((packed)) {
    uint8_t intro_enable;   // 0/1
    uint8_t device_position; // 1/2/3
    uint8_t boot_mode;      // 0=默认动画, 1=自定义图片, 2=视频
} s_mg = { 0, 1, 0 };

/* 前向声明 */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len);
static esp_err_t save_blob(const char *ns,const char *key,const void *data,size_t len);

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
    // 里程/行程统计不再落盘(见 s_stat 声明处注释), 保持 {0} 每次开机从零开始, 不从 NVS 加载。
    {   // 曲线报警阈值: NVS 有则加载, 无/长度不符则保持静态默认(不覆盖, 故不会全变0)
        nvs_handle_t h; size_t sz = sizeof(s_chart_alarm);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_CHART_ALARM, s_chart_alarm, &sz);
            nvs_close(h);
        }
    }
    {   // 三连表开机动画设置: 同上, 有则加载否则保持默认
        nvs_handle_t h; size_t sz = sizeof(s_mg);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_MG_EXTRA, &s_mg, &sz);
            nvs_close(h);
        }
        if (s_mg.device_position < 1 || s_mg.device_position > 3) s_mg.device_position = 1;
        if (s_mg.intro_enable > 4) s_mg.intro_enable = 0;
        if (s_mg.boot_mode > 2) s_mg.boot_mode = 0;
        ESP_LOGI("nvs", "mg loaded: intro=%u pos=%u boot=%u (blob_sz=%u)", s_mg.intro_enable, s_mg.device_position, s_mg.boot_mode, (unsigned)sz);
    }

    /* ---- 配置版本迁移 ----
       注意: 这一段版本号迁移目前只覆盖 s_mg (KEY_MG_EXTRA) 这个独立 blob。
       nvs_user_cfg_t (s_cfg) 走的是另一套机制——load_blob() 里"旧 blob 比当前结构体
       小就按旧长度部分拷贝、结构体尾部新字段保持编译期默认值、然后按新大小整体重写"的
       通用扩容逻辑, 这套逻辑要求给 nvs_user_cfg_t 新增字段时必须加在结构体末尾, 否则旧数据
       会被错误地解释到别的字段上。给 s_cfg 加字段时留意这个约束, 不要在中间插入新字段。 */
    {
        nvs_handle_t h;
        uint8_t stored_ver = 0;
        bool has_ver = false;
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, KEY_CFG_VERSION, &stored_ver) == ESP_OK) has_ver = true;
            nvs_close(h);
        }
        if (!has_ver || stored_ver < CFG_VERSION_CURRENT) {
            ESP_LOGW("nvs", "Config migration v%u → v%u", stored_ver, CFG_VERSION_CURRENT);
            // v0 → v1: boot_mode 字段新增, 默认 0 (SKY GAUGE)
            if (stored_ver < 1) {
                s_mg.boot_mode = 0;
                save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
            }
            // 未来: if (stored_ver < 2) { ... 迁移 v1→v2 的字段 ... }
            // 写入新版本号
            if (nvs_open(NS_CFG, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, KEY_CFG_VERSION, CFG_VERSION_CURRENT);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI("nvs", "Config migration done, now v%u", CFG_VERSION_CURRENT);
        }
    }

    /* 新增字段默认值修复 (旧NVS数据中rsv[x]全为0) */
    if(s_cfg.brightness_day < 10) s_cfg.brightness_day = 100; // 有效范围 10-100, 0/未配置或越界都归 100
    if(s_cfg.default_page > 6) s_cfg.default_page = 0; // 0=Temp,1=Info,2=Chart,3=Needle,4=Gear,5=Rpm,6=Speed(刹车温已并入Chart)
    if(s_cfg.needle_source_idx >= 11) s_cfg.needle_source_idx = 0; // DISP_ITEM_COUNT=11 (CLT..BOOST)
    if(s_cfg.device_role > 2) s_cfg.device_role = ESPNOW_ROLE_STANDALONE; // 角色: 0=主 1=从 2=单机, 越界归单机
    if(s_cfg.chart_source_idx >= 11) s_cfg.chart_source_idx = 8; // 曲线数据项越界→默认 OILP(旧NVS该字节为0=CLT亦可, 这里统一到OILP)
    // 车型索引按已注册的 profile 数量限界（越界归零到 ZC/N6）
    uint8_t vehicle_count = 0;
    vehicle_profile_get_all(&vehicle_count);
    if(vehicle_count > 0 && s_cfg.vehicle_profile_idx >= vehicle_count) s_cfg.vehicle_profile_idx = 0;
    if(s_cfg.brake_temp_warn_c < 10 || s_cfg.brake_temp_warn_c > 1200) s_cfg.brake_temp_warn_c = 600;
    if(s_cfg.oil_pressure_warn_x10 > 100) s_cfg.oil_pressure_warn_x10 = 80;
    // 0=未设置/旧NVS越界 → 默认 6000; 集中在这里夹紧, 调用方(ui.c / ui_ScreenPageRpmWarn.c)不用各自重复判断
    if(s_cfg.rpm_warn_threshold < 1000) s_cfg.rpm_warn_threshold = 6000;

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
    return (item < CHART_ALARM_N) ? s_chart_alarm[item] : CHART_ALARM_OFF;
}
void nvs_chart_alarm_set(uint8_t item, int16_t raw_threshold){
    if(item >= CHART_ALARM_N) return;
    if(s_chart_alarm[item] == raw_threshold) return;
    s_chart_alarm[item] = raw_threshold;
    save_blob(NS_CFG, KEY_CHART_ALARM, s_chart_alarm, sizeof(s_chart_alarm));
}

/* 三连表开机动画设置 */
uint8_t nvs_intro_enable_get(void){ return s_mg.intro_enable; }
void nvs_intro_enable_set(uint8_t en){
    if(en > 4) return;
    if(s_mg.intro_enable == en) return;
    s_mg.intro_enable = en;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}
uint8_t nvs_device_position_get(void){ return s_mg.device_position; }
void nvs_device_position_set(uint8_t pos){
    if(pos < 1 || pos > 3) return;
    if(s_mg.device_position == pos) return;
    s_mg.device_position = pos;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}
uint8_t nvs_boot_mode_get(void){ return s_mg.boot_mode; }
void nvs_boot_mode_set(uint8_t mode){
    if(mode > 2) return;
    if(s_mg.boot_mode == mode) return;
    s_mg.boot_mode = mode;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}

/* 统计 */
const nvs_stat_t * nvs_stat_get(void){return &s_stat;}
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

/* 工具函数 */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len)
{
    nvs_handle_t h; esp_err_t err;
    if(nvs_open(ns,NVS_READONLY,&h)==ESP_OK){
        // 先查实际存储长度
        size_t stored_len = 0;
        err = nvs_get_blob(h, key, NULL, &stored_len);
        if(err == ESP_OK && stored_len > 0){
            size_t copy_len = (stored_len < len) ? stored_len : len;
            // 部分加载: 旧数据拷贝能拷的, 新字段保持静态默认值
            err = nvs_get_blob(h, key, out, &copy_len);
            nvs_close(h);
            if(err == ESP_OK){
                if(stored_len < len){
                    // struct 变大了, 用新大小重写一次 NVS(保留已加载的旧字段+默认新字段)
                    save_blob(ns, key, out, len);
                }
                return ESP_OK;
            }
            return err;
        }
        nvs_close(h);
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
