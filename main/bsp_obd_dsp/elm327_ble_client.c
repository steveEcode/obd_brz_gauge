#include "elm327_ble_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "app_obd_dsp/vehicle_custom_config.h"
#include "racechrono_ble_diy.h"
#include "ble_adv_util.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include "nvs_storage.h"

// UUID 常量
#define UUID16_OBD_SERVICE      0xFFF0  // 常见ELM327 BLE适配器 (FFF1写/FFF2通知)
#define UUID16_OBD_SERVICE_18F0  0x18F0  // IOS-Vlink / Vlink (2AF1写/2AF0通知)
#define UUID16_OBD_SERVICE_FF12  0xFF12  // 部分Viecar等适配器配置服务(FF15写/FF14通知)
#define UUID16_OBD_WRITE_CHAR    0xFFF1
#define UUID16_CCCD              0x2902

static const char *TAG = "elm327_ble";

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;   // 0 是合法的真实接口号, 用 NONE 才能区分"尚未注册"
static uint16_t s_conn_id = 0xFFFF;
static esp_bd_addr_t s_peer_bda = {0};
static volatile bool s_connected = false;
static bool s_have_service = false;
static uint16_t s_service_start = 0x0001, s_service_end = 0xFFFF; // default: full range
static uint16_t s_all_attr_end = 0xFFFF; // tracks highest seen end handle
static bool s_have_18f0 = false;          // 0x18F0 服务 (IOS-Vlink 真正OBD通信服务)
static uint16_t s_18f0_start = 0, s_18f0_end = 0;
static bool s_have_ff12 = false;          // 0xFF12 服务 (备选)
static uint16_t s_ff12_start = 0, s_ff12_end = 0;
static uint16_t s_char_write_handle = 0; // FFF1
static uint16_t s_char_notify_handle = 0; // 优先 FFF2，没有则回落 FFF1
static uint16_t s_cccd_handle = 0;
static esp_gatt_write_type_t s_write_type = ESP_GATT_WRITE_TYPE_RSP; // 写类型，根据特征属性自动选择
static elm327_ble_callbacks_t s_cbs = {0};
static char s_target_name[32] = "OBDII";
// 精确 MAC 匹配: 设置后 match_device_target 只认这一个地址, 忽略名字(防止同名设备误连)
static esp_bd_addr_t s_target_bda = {0};
static bool s_target_bda_valid = false;

// ---- 扫描模式相关 ----
static bool s_scan_only_mode = false;  // true=仅扫描不连接
static ble_scan_found_cb_t s_scan_cb = NULL;
static ble_scan_result_t s_scan_list[BLE_SCAN_MAX_DEVICES];
static int s_scan_count = 0;
static bool s_ble_inited = false;  // BLE 协议栈是否已初始化
static bool s_poll_task_started = false; // 轮询任务是否已创建
static TaskHandle_t s_poll_task_handle = NULL; // poll task 句柄，用于 task notification 唤醒
static volatile bool s_notify_ready = false;   // CCCD 通知订阅完成才置位; init/轮询据此放行(防握手响应丢失)
static volatile int64_t s_last_obd_valid_us = 0; // 最近一次收到有效OBD数据的时间; 超时无数据触发自愈重初始化
static uint8_t s_oil_query_mode = 0;     // 当前查询模式索引 (0-2)

// ---- 数据驱动 override 状态 ----
static const vehicle_override_t *s_ov = NULL;  // 当前车型的覆盖配置
static const oil_formula_t *s_oil_formula_pri = NULL;   // 主油温公式
static const oil_formula_t *s_oil_formula_sec = NULL;   // 备用油温公式
static bool s_oil_use_override = false;  // true=用 override 公式, false=用旧 enum
static uint8_t s_oil_override_idx = 0;   // 0=primary, 1=secondary
static uint8_t s_oil_override_fail = 0;  // 当前公式连续失败次数
#define OIL_OVERRIDE_FAIL_MAX 5
static int s_mode21_oil_idx = 33;        // Mode21 机油温字节索引，自适应更新
static int16_t s_last_mode21_oil = -100; // 上次Mode21解析出的油温
static int s_mode21_hold_cnt = 0;        // ZC6 一致性 hold 计数；持续噪声帧数，超阈值才采纳新值
static int64_t s_last_mode21_oil_us = 0; // 上次接受油温值的时间戳(us)

// ---- 基于车型的油温查询策略 ----
static oil_temp_query_mode_t s_oil_mode_priority[4] = {
    OIL_TEMP_MODE_PID_5C,
    OIL_TEMP_MODE_UDS_22_10_17,
    OIL_TEMP_MODE_TOYOTA_21_01,
};  // 默认优先级，启动后从车型配置更新
static uint32_t s_oil_mode_fail_count[12] = {0};  // 每个模式(poll idx 0~11)的连续失败次数
#define OIL_MODE_FAIL_THRESHOLD 5  // 某模式失败次数达到此值后才切换到下一个
#define OBD_POLL_SLOT_GAP_MS   30  // 轮询槽间空等(ms); 原100, 调小提高刷新率, 太小压垮克隆头
static bool s_vehicle_profile_inited = false;

// 油温诊断统计
static struct {
    uint32_t mode0_ok;  // 01 5C 成功次数
    uint32_t mode1_ok;  // 22 10 17 成功次数
    uint32_t mode2_ok;  // 21 01 成功次数
    uint32_t mode3_ok;  // 22 11 1F (Mazda) 成功次数
    uint32_t mode4_ok;  // 22 13 10 (Mazda) 成功次数
    uint32_t mode5_ok;  // CAN 0x441 (Porsche) 成功次数
    uint32_t mode6_ok;  // 22 58 22 (MINI/BMW) 成功次数
    uint32_t mode7_ok;  // 22 44 02 (BMW F系) 成功次数
    uint32_t mode8_ok;  // 22 03 F3 (BMW G系) 成功次数
    uint32_t mode9_ok;  // 22 44 02 G公式 (BMW G系) 成功次数
    uint32_t mode10_ok; // 22 D0 02 (BMW G系) 成功次数
    uint32_t mode11_ok; // 22 11 1F (BMW, A-50) 成功次数
    uint32_t mode0_fail;
    uint32_t mode1_fail;
    uint32_t mode2_fail;
    uint32_t mode3_fail;
    uint32_t mode4_fail;
    uint32_t mode5_fail;
    uint32_t mode6_fail;
    uint32_t mode7_fail;
    uint32_t mode8_fail;
    uint32_t mode9_fail;
    uint32_t mode10_fail;
    uint32_t mode11_fail;
    int16_t last_raw_temp; // 原始温度（未过滤）
    int16_t last_filtered_temp; // 过滤后温度
} s_oil_diag = {0};

static int8_t s_oil_temp_offset = 0;  // 用户校准偏移量，单位 °C

// 增加全局 ready 标志
static volatile bool s_elm_ready = true; // 初始允许发送首条 ATZ
static volatile bool s_expect_mode21 = false; // true=上条命令是 21 01，等待 61 01 响应
static volatile bool s_porsche_441_seen = false; // 本次监听是否成功解析到 0x441 帧
static volatile bool s_zc6_can_rpm_seen = false; // 本次监听是否成功解析到 0x140 帧(ZC/N6 CAN RPM)
static uint8_t s_can_rpm_fail_count = 0;         // CAN 140 连续失败计数
#define CAN_RPM_FAIL_THRESHOLD 3                  // 连续失败此次数后回退 01 0C

// ---- ZC6 CAN 持续监听模式 (ATCM/ATCF + ATMA, 帧来一帧解一帧) ----
static volatile bool s_zc6_can_monitor_active = false;
#define ZC6_CAN_MONITOR_BUF_SIZE 160
static char s_zc6_can_monitor_buf[ZC6_CAN_MONITOR_BUF_SIZE];
static size_t s_zc6_can_monitor_len = 0;
static int64_t s_zc6_can_monitor_entered_us = 0;   // 进入监听时间
static int64_t s_zc6_can_monitor_last_sample_us = 0; // 最近一次成功解析
static uint32_t s_zc6_can_monitor_obd_cycle = 0;   // 监听中 OBD 查询轮次计数
#define ZC6_CAN_OBD_INTERVAL 50                     // 每 50 轮(~6s) 退出 ATMA 查一次 OBD PID
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd);

// 多包响应累积缓冲区（21 01 等长响应分多个BLE包）
#define ACCUM_BUF_SIZE 512
static char s_accum_buf[ACCUM_BUF_SIZE];
static size_t s_accum_len = 0;
static int64_t s_accum_start_us = 0; // 累积开始时间 (us)

// ---- 自动协议检测相关 ----
static volatile int s_protocol_detect_idx = -1;  // -1=未在检测，0-10=正在尝试协议号
static volatile bool s_protocol_detect_got_response = false;  // 是否收到有效响应
static volatile int32_t s_protocol_detect_rpm = -1;  // 检测到的 RPM 值（-1=无）

// ---- 油温查询模式转换 ----
// 将 oil_temp_query_mode_t 转换为轮询索引 (0-2)
static inline uint8_t oil_mode_to_poll_idx(oil_temp_query_mode_t mode) {
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C: return 0;
        case OIL_TEMP_MODE_UDS_22_10_17: return 1;
        case OIL_TEMP_MODE_TOYOTA_21_01: return 2;
        case OIL_TEMP_MODE_MAZDA_22_111F: return 3;
        case OIL_TEMP_MODE_MAZDA_22_1310: return 4;
        case OIL_TEMP_MODE_PORSCHE_CAN_441: return 5;
        case OIL_TEMP_MODE_MINI_22_5822: return 6;
        case OIL_TEMP_MODE_BMW_22_4402: return 7;
        case OIL_TEMP_MODE_BMW_22_03F3: return 8;
        case OIL_TEMP_MODE_BMW_G_22_4402: return 9;
        case OIL_TEMP_MODE_BMW_22_D002: return 10;
        case OIL_TEMP_MODE_BMW_22_111F: return 11;
        default: return 0;
    }
}

static uint8_t get_active_vehicle_idx_safe(void) {
    // 越界由 vehicle_profile_get 归零，这里直接返回当前索引。
    return nvs_cfg_get()->vehicle_profile_idx;
}

static const char *get_vehicle_fixed_header_cmd(void) {
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->obd_functional_addr) {
        return "ATSH7DF\r"; // 功能寻址(全车广播), 同手机 APP; BMW 用此, 否则物理 7E0 可能无响应
    }
    return "ATSH7E0\r";     // 物理寻址发动机 ECU; 斯巴鲁/默认
}

// 初始化油温查询策略（从车型配置读取 primary/secondary/tertiary 优先级链）
static void init_oil_temp_strategy(void) {
    // 取 profile 的完整优先级链：primary 连续失败(阈值次)后回退到 secondary、tertiary。
    const oil_temp_strategy_t *st = vehicle_profile_get_oil_temp_strategy();
    s_oil_mode_priority[0] = st ? st->primary    : OIL_TEMP_MODE_PID_5C;
    s_oil_mode_priority[1] = st ? st->secondary  : OIL_TEMP_MODE_NONE;
    s_oil_mode_priority[2] = st ? st->tertiary   : OIL_TEMP_MODE_NONE;
    s_oil_mode_priority[3] = st ? st->quaternary : OIL_TEMP_MODE_NONE;

    // ---- 数据驱动 override 初始化 ----
    s_ov = vehicle_profile_get_override();
    s_oil_formula_pri = s_ov ? s_ov->oil_primary : NULL;
    s_oil_formula_sec = s_ov ? s_ov->oil_secondary : NULL;
    s_oil_use_override = (s_oil_formula_pri != NULL);
    s_oil_override_idx = 0;
    s_oil_override_fail = 0;

    if (s_oil_mode_priority[0] == OIL_TEMP_MODE_TOYOTA_21_01) {
        // ZC/N6 固定 d[33]
        s_mode21_oil_idx = 33;
    }
    s_last_mode21_oil = -100;
    s_last_mode21_oil_us = 0;
    s_mode21_hold_cnt = 0;

    // 重置失败计数
    memset(s_oil_mode_fail_count, 0, sizeof(s_oil_mode_fail_count));
    s_oil_query_mode = 0;
    s_vehicle_profile_inited = true;

    const vehicle_profile_t *profile = vehicle_profile_get_active();
    ESP_LOGI(TAG, "Oil temp strategy for [%s]: Primary=%d",
             profile ? profile->name : "UNKNOWN", s_oil_mode_priority[0]);
}

// 根据当前策略选择下一个查询模式
static oil_temp_query_mode_t get_next_oil_query_mode(uint8_t *poll_idx) {
    if (!s_vehicle_profile_inited) {
        init_oil_temp_strategy();
    }
    
    // 遍历优先级列表，找到首个有效且未过度失败的模式
    for (int i = 0; i < 4; i++) {
        oil_temp_query_mode_t mode = s_oil_mode_priority[i];
        if (mode == OIL_TEMP_MODE_NONE) continue;
        
        uint8_t idx = oil_mode_to_poll_idx(mode);
        // 如果这个模式失败次数少于阈值，或者所有模式都超过阈值了，采用这个
        if (s_oil_mode_fail_count[idx] < OIL_MODE_FAIL_THRESHOLD) {
            *poll_idx = idx;
            return mode;
        }
    }
    
    // 所有模式都失败过多，重置失败计数并使用首要模式
    memset(s_oil_mode_fail_count, 0, sizeof(s_oil_mode_fail_count));
    *poll_idx = oil_mode_to_poll_idx(s_oil_mode_priority[0]);
    return s_oil_mode_priority[0];
}

// 记录油温查询的成功/失败（内部使用）
static void record_oil_temp_success(oil_temp_query_mode_t mode) {
    uint8_t idx = oil_mode_to_poll_idx(mode);
    s_oil_mode_fail_count[idx] = 0;  // 成功，清零失败计数
    
    // 更新诊断统计
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C:
            s_oil_diag.mode0_ok++;
            break;
        case OIL_TEMP_MODE_UDS_22_10_17:
            s_oil_diag.mode1_ok++;
            break;
        case OIL_TEMP_MODE_TOYOTA_21_01:
            s_oil_diag.mode2_ok++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_111F:
            s_oil_diag.mode3_ok++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_1310:
            s_oil_diag.mode4_ok++;
            break;
        case OIL_TEMP_MODE_PORSCHE_CAN_441:
            s_oil_diag.mode5_ok++;
            break;
        case OIL_TEMP_MODE_MINI_22_5822:
            s_oil_diag.mode6_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_4402:
            s_oil_diag.mode7_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_03F3:
            s_oil_diag.mode8_ok++;
            break;
        case OIL_TEMP_MODE_BMW_G_22_4402:
            s_oil_diag.mode9_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_D002:
            s_oil_diag.mode10_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_111F:
            s_oil_diag.mode11_ok++;
            break;
        default:
            break;
    }
    ESP_LOGD(TAG, "Oil temp query SUCCESS for mode %u (fail_count reset to 0)", mode);
}

static void record_oil_temp_failure(oil_temp_query_mode_t mode) {
    uint8_t idx = oil_mode_to_poll_idx(mode);
    s_oil_mode_fail_count[idx]++;
    
    // 更新诊断统计
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C:
            s_oil_diag.mode0_fail++;
            break;
        case OIL_TEMP_MODE_UDS_22_10_17:
            s_oil_diag.mode1_fail++;
            break;
        case OIL_TEMP_MODE_TOYOTA_21_01:
            s_oil_diag.mode2_fail++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_111F:
            s_oil_diag.mode3_fail++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_1310:
            s_oil_diag.mode4_fail++;
            break;
        case OIL_TEMP_MODE_PORSCHE_CAN_441:
            s_oil_diag.mode5_fail++;
            break;
        case OIL_TEMP_MODE_MINI_22_5822:
            s_oil_diag.mode6_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_4402:
            s_oil_diag.mode7_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_03F3:
            s_oil_diag.mode8_fail++;
            break;
        case OIL_TEMP_MODE_BMW_G_22_4402:
            s_oil_diag.mode9_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_D002:
            s_oil_diag.mode10_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_111F:
            s_oil_diag.mode11_fail++;
            break;
        default:
            break;
    }
    ESP_LOGD(TAG, "Oil temp query FAILED for mode %u (fail_count now %u)", mode, s_oil_mode_fail_count[idx]);
}

// 默认回调与轮询任务（可选）
static void default_on_connected(void) { ESP_LOGD(TAG, "OBD BLE connected"); }
static void default_on_disconnected(void) { ESP_LOGD(TAG, "OBD BLE disconnected"); }
static void default_on_raw_notify(const uint8_t *data, size_t len) {
    // 仅 debug 级别打印原始数据（生产不输出）
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        char printbuf[128] = {0};
        size_t plen = (len < sizeof(printbuf)-1) ? len : sizeof(printbuf)-1;
        for (size_t i = 0; i < plen; i++) {
            printbuf[i] = (data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.';
        }
        ESP_LOGD(TAG, "RAW[%d]: %s", (int)len, printbuf);
    }
    // 若接收到 '>'，表示 ELM 准备好，可发送下一条
    // xTaskNotify 立刻唤醒 poll task，避免 10ms 轮询开销
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '>') {
            s_elm_ready = true;
            if (s_poll_task_handle) xTaskNotify(s_poll_task_handle, 0, eNoAction);
            break;
        }
    }
}

// ---- 协议自动检测函数 ----
// 尝试所有协议（1-11），通过发送 01 0C （读 RPM）来判断协议是否有效
static int elm327_auto_detect_protocol(void) {
    ESP_LOGD(TAG, "=== Starting protocol auto-detect ===");
    
    // 先发送通用初始化命令（不涉及协议选择）
    const char *init_cmds[] = {
        "ATZ\r",        // 复位
        "ATE0\r",       // Echo off
        "ATAT1\r",      // 自适应时序
        "ATST 19\r",    // 设置超时
    };
    
    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 尝试协议 1-11
    for (int proto = 1; proto <= 11; proto++) {
        ESP_LOGD(TAG, "[DETECT] Trying protocol %d...", proto);
        
        // 设置协议
        char atsp_cmd[16];
        snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", proto);
        elm327_ble_send_ascii_blocking(atsp_cmd);
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 初始化检测状态
        s_protocol_detect_idx = proto;
        s_protocol_detect_got_response = false;
        s_protocol_detect_rpm = -1;
        
        // 发送测试命令 01 0C (读 RPM)
        elm327_ble_send_ascii_blocking("01 0C\r");
        esp_log_level_t prev_level = esp_log_level_get(TAG);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        ESP_LOGD(TAG, "[DETECT] Sent 01 0C, waiting...");
        esp_log_level_set(TAG, prev_level);
        
        // 等待响应最多 2 秒
        uint32_t wait_ms = 0;
        while (wait_ms < 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
            
            if (s_protocol_detect_got_response) {
                // 成功！
                s_protocol_detect_idx = -1;  // 结束检测模式
                ESP_LOGD(TAG, "[DETECT] Protocol %d: SUCCESS! (RPM=%ld)", proto, s_protocol_detect_rpm);
                return proto;
            }
        }
        
        ESP_LOGW(TAG, "[DETECT] Protocol %d: No valid response (timeout)", proto);
    }
    
    s_protocol_detect_idx = -1;  // 结束检测模式
    ESP_LOGW(TAG, "=== Protocol auto-detect FAILED ===");
    return 0;  // 返回 0 表示检测失败，使用默认协议 6
}

static void default_on_parsed_rpm(uint16_t rpm) { ESP_LOGD(TAG, "RPM: %u", rpm); obd_data_set_rpm(rpm); }
static void default_on_parsed_speed(uint8_t kmh) {
    // 车速校正: 乘当前车型 speed_scale(如 BMW X1 ×1.0606); 未设/≤0 视为 1.0
    const vehicle_profile_t *p = vehicle_profile_get_active();
    float sc = (p && p->speed_scale > 0.0f) ? p->speed_scale : 1.0f;
    int32_t v = (int32_t)((float)kmh * sc + 0.5f);
    if (v > 255) v = 255;
    // CAN 车型: 转速<800时强制车速=0, 避免静止时CAN总线噪声导致车速非零爬升
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->can_broadcast_mode && obd_data_get_rpm() < 800)
        v = 0;
    // 所有车型: ≤2km/h 视为静止, 避免低速噪声
    if (v <= 2) v = 0;
    ESP_LOGD(TAG, "SPEED: %u -> %d km/h (x%.4f)", kmh, (int)v, sc);
    obd_data_set_speed((uint8_t)v);
}
static void default_on_parsed_coolant_temp(uint32_t coolant_temp) { ESP_LOGD(TAG, "CLT: %u C", coolant_temp); obd_data_set_coolant_temp((int16_t)coolant_temp); }
static void default_on_parsed_intake_temp(uint32_t intake_temp) { ESP_LOGD(TAG, "IAT: %u C", intake_temp); obd_data_set_intake_temp((int16_t)intake_temp); }

// 内联工具函数：应用油温偏移量后存储
static inline void obd_data_set_oil_temp_with_offset(int16_t temp) {
    int16_t adjusted = temp + s_oil_temp_offset;
    // 确保仍在有效范围
    if (adjusted < -20) adjusted = -20;
    if (adjusted > 150) adjusted = 150;
    if (s_oil_temp_offset != 0) {
        ESP_LOGD(TAG, "OIL offset applied: %d + %d = %d", temp, s_oil_temp_offset, adjusted);
    }
    obd_data_set_oil_temp(adjusted);
}

// float → int16_t 四舍五入（正值安全，油温范围内无需处理负值精度）
static inline int16_t oil_f2i(float f) { return (int16_t)(f + 0.5f); }

static void default_on_parsed_oil_temp(uint32_t oil_temp)
{
    // float 内部追踪，避免整数截断导致 1°C 变化永远不更新
    // 例: filtered=90, raw=91 → 0.65*90+0.35*91=90.35 → 取整显示 90，
    //     但 float 继续累积，再次读 91 → 0.65*90.35+0.35*91=90.578 → 显示 91 ✓
    static float s_oil_filtered = -100.0f;
    static int16_t s_oil_pending = -100;
    static uint8_t s_oil_pending_cnt = 0;

    int16_t in = (int16_t)oil_temp;
    s_oil_diag.last_raw_temp = in;

    if (in < -20 || in > 150) {
        ESP_LOGD(TAG, "OIL: Out of range raw=%d", in);
        return;
    }

    // 1. 初始化
    if (s_oil_filtered <= -40.0f) {
        s_oil_filtered = (float)in;
        s_oil_pending = in;
        s_oil_pending_cnt = 1;
        s_oil_diag.last_filtered_temp = in;
        ESP_LOGI(TAG, "OIL: Init with raw=%d", in);
        obd_data_set_oil_temp_with_offset(in);
        return;
    }

    float fdiff = s_oil_filtered - (float)in;
    int diff = (int)(fdiff >= 0 ? fdiff : -fdiff);

    // 2. 小变化（<=5°C）：加权平均，float 精度保证 1°C 渐变正确累积
    if (diff <= 5) {
        s_oil_pending = -100;
        s_oil_pending_cnt = 0;
        s_oil_filtered = 0.65f * s_oil_filtered + 0.35f * (float)in;
        int16_t disp = oil_f2i(s_oil_filtered);
        s_oil_diag.last_filtered_temp = disp;
        ESP_LOGD(TAG, "OIL: raw=%d filtered=%.2f disp=%d", in, s_oil_filtered, disp);
        obd_data_set_oil_temp_with_offset(disp);
        return;
    }

    // 3. 中等变化（5~15°C）：4 次确认后采纳
    if (diff <= 15) {
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            ESP_LOGD(TAG, "OIL: Medium spike first occurrence raw=%d, need confirmation", in);
            return;
        }
        if (s_oil_pending_cnt >= 4) {
            s_oil_filtered = 0.5f * s_oil_filtered + 0.5f * (float)in;
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            int16_t disp = oil_f2i(s_oil_filtered);
            s_oil_diag.last_filtered_temp = disp;
            ESP_LOGI(TAG, "OIL: Medium change confirmed raw=%d filtered=%.2f disp=%d", in, s_oil_filtered, disp);
            obd_data_set_oil_temp_with_offset(disp);
        } else {
            ESP_LOGD(TAG, "OIL: Medium spike pending (%u/%d)", s_oil_pending_cnt, 4);
        }
        return;
    }

    // 4. 大变化（>15°C）：3 次确认后直接采纳
    ESP_LOGW(TAG, "OIL: Large spike filtered=%.1f raw=%d (Δ=%d)", s_oil_filtered, in, diff);
    if (diff >= 20) {
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            return;
        }
        if (s_oil_pending_cnt >= 3) {
            s_oil_filtered = (float)in;
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            s_oil_diag.last_filtered_temp = in;
            ESP_LOGI(TAG, "OIL: Large change ACCEPTED raw=%d (confirmed 3x)", in);
            obd_data_set_oil_temp_with_offset(in);
        }
    }
}
static void default_on_parsed_load_pct(uint32_t load_pct) { ESP_LOGD(TAG, "LOAD: %u%%", load_pct); obd_data_set_load_pct((int16_t)load_pct); }
static void default_on_parsed_control_module_voltage(uint32_t bat_mv) { ESP_LOGD(TAG, "BAT: %u.%uV", bat_mv/1000, (bat_mv%1000)/100); obd_data_set_bat_mv((int32_t)bat_mv); }
static void default_on_parsed_throttle_position(uint32_t tps_pct) { ESP_LOGD(TAG, "TPS: %u%%", tps_pct); obd_data_set_tps((int16_t)tps_pct); }
static void default_on_parsed_gear(int8_t gear) {
    // BMW G CAN 广播档位 (0x3F9 byte6 nibble), raw−4 映射:
    //   3→R, 4→N, 5→1, 6→2, …  (0-2 保留/Park 等)
    if (gear == 3) {
        obd_data_set_gear(-1);  // R
    } else if (gear == 4) {
        obd_data_set_gear(GEAR_NEUTRAL);  // N/P
    } else if (gear >= 5 && gear <= 12) {
        obd_data_set_gear((int8_t)(gear - 4));  // 5→1, 6→2, ...
    } else {
        obd_data_set_gear(127);  // 无效/未知
    }
}
// MAP(kPa) → 涡轮表压(0.1bar)：表压 = (MAP - 大气≈100kPa)，10kPa = 0.1bar
static void default_on_parsed_manifold_pressure(uint32_t map_kpa) {
    int16_t boost_x10 = (int16_t)(((int32_t)map_kpa - 100) / 10);
    if (boost_x10 < 0) boost_x10 = 0; // 不显示负压(真空)，从0起
    ESP_LOGD(TAG, "MAP: %u kPa -> boost %d.%d bar", map_kpa, boost_x10/10, boost_x10%10);
    obd_data_set_boost_x10(boost_x10);
}

// PID 01 44: Commanded Equivalence Ratio (λ), 公式: (A*256+B)/32768, 范围 0~<2
// λ=1.0 理想空燃比(汽油~14.7:1), λ<1 浓, λ>1 稀
// 转换: AFR = λ × 14.7, 存储为 ×100 (1470 = 14.70:1)
static void default_on_parsed_afr(uint32_t afr_x100) {
    ESP_LOGD(TAG, "AFR: %d.%02d:1 (λ=%.3f)", afr_x100/100, afr_x100%100, (float)afr_x100/1470.0f);
    obd_data_set_afr_x100((int16_t)afr_x100);
}

// 保时捷 997.2/987.2：油温油压在 CAN 广播帧 0x441，需用 ELM327 监听模式抓取。
// 流程：过滤只收 441 → 开帧头 → 监听若干帧 → 停止 → 还原(关帧头/恢复自动收地址)。
// 帧解析在通知回调里(见 "441 " 分支)；本函数只负责按时序下发监听指令。
// 注意：监听模式与普通请求/应答不同，且依赖适配器(廉价克隆可能不支持 ATMA/ATCRA)。
static void porsche_read_can_441(void) {
    // 油温/油压变化慢(以分钟为单位), 不需要每轮都做这次 520ms 阻塞式 CAN 监听。
    // 隔几轮跳过一次, 省下来的时间让 RPM/其它槽转得更快, 显示保持上次读数即可。
    const uint8_t skip_rounds = 4;
    static uint8_t s_round = 0;
    if (++s_round < skip_rounds) return;
    s_round = 0;

    elm327_ble_send_ascii_blocking("AT CRA 441\r"); // 接收过滤：只收 ID=0x441
    elm327_ble_send_ascii_blocking("AT H1\r");        // 帧头打开：监听输出带 ID，便于识别 441
    // 启动监听（ATMA 持续刷帧、不产生 '>'，手动管理 ready 标志）
    uint8_t cmd[8];
    size_t n = elm327_ble_ascii_cmd_to_bytes("AT MA\r", cmd, sizeof(cmd));
    s_porsche_441_seen = false;
    if (n) { s_elm_ready = false; elm327_ble_send_command(cmd, n); }
    vTaskDelay(pdMS_TO_TICKS(400));   // 监听窗口拉长到 400ms, 容纳广播较慢的 441 帧
    uint8_t stop = '\r';
    elm327_ble_send_command(&stop, 1); // 任意字符停止监听 → ELM 吐出帧 + '>'(触发解析)
    vTaskDelay(pdMS_TO_TICKS(120));    // 等通知回调解析完该帧
    // 诊断: 明确打印本轮监听结果, 便于串口排查(抓到=解析问题, 没抓到=总线无441/适配器不支持ATMA)
    if (s_porsche_441_seen) {
        ESP_LOGD(TAG, "[441] frame captured & parsed OK");
    } else {
        ESP_LOGW(TAG, "[441] NO 441 frame in window (check FULL[] log above: ATMA returned what?)");
        record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
    }
    // 还原：关帧头、恢复自动接收地址(否则后续普通 PID 响应被过滤/错解析)
    elm327_ble_send_ascii_blocking("AT H0\r");
    elm327_ble_send_ascii_blocking("AT AR\r");
}

// ZC/N6：转速在 CAN 广播帧 0x140 (bits 16-29, 14bit LE, 直接 rpm)。
// ---- ZC/N6 CAN 持续监听: 进入/退出/逐字节喂入/逐行解析 ----

static void zc6_can_monitor_enter(void)
{
    // 不设过滤, 全部帧通过, 软件只解析 override 规则中的 CAN ID
    const char *cmds[] = {
        "ATE0\r", "ATL0\r", "ATS1\r", "ATH1\r", "ATMA\r",
    };
    s_zc6_can_monitor_active = false;
    s_zc6_can_monitor_len = 0;
    s_zc6_can_monitor_buf[0] = '\0';
    s_zc6_can_monitor_entered_us = 0;
    s_zc6_can_monitor_last_sample_us = 0;
    s_accum_len = 0;
    s_accum_buf[0] = '\0';

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        elm327_ble_send_ascii_blocking(cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(i + 1 == sizeof(cmds) / sizeof(cmds[0]) ? 80 : 30));
    }
    s_zc6_can_monitor_active = true;
    s_zc6_can_monitor_entered_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[ZC/N6 CAN] Entered ATMA monitor (0x140/141/0D1)");
}

static void zc6_can_monitor_exit(void)
{
    s_zc6_can_monitor_active = false;
    s_elm_ready = true;
    uint8_t stop = '\r';
    elm327_ble_send_command(&stop, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    s_accum_len = 0;
    s_accum_buf[0] = '\0';
    // 轻量还原: 清过滤+关帧头, 不做 ATZ 全复位 (省 ~500ms)
    elm327_ble_send_ascii_blocking("AT AR\r");
    elm327_ble_send_ascii_blocking("AT H0\r");
    ESP_LOGI(TAG, "[ZC/N6 CAN] Exited ATMA (light restore)");
}

// 逐行解析: ZC/N6(0x140/0x360) + ZD8(0x40/0x345), 总线无此帧自动跳过
static bool zc6_can_monitor_parse_line(const char *line)
{
    // 通用 CAN 帧解析: 从 ATMA 行提取 CAN ID + 数据, 应用 override 规则
    const vehicle_override_t *ov = vehicle_profile_get_override();
    if (!ov || !ov->can_rules || ov->can_rule_count == 0) {
        ESP_LOGD(TAG, "[CAN] no override/rules for active profile");
        return false;
    }

    // ---- 诊断: 采样打印所有 ATMA 行中的 CAN ID (前 60 次) ----
    {
        static uint32_t s_can_line_count = 0;
        static uint16_t s_seen_ids[32] = {0};
        static uint8_t s_seen_count = 0;
        if (s_can_line_count < 60) {
            uint16_t id = 0;
            int n = sscanf(line, "%hx ", &id);
            if (n == 1 && id > 0 && id < 0x800) {
                bool dup = false;
                for (uint8_t j = 0; j < s_seen_count; j++)
                    if (s_seen_ids[j] == id) { dup = true; break; }
                if (!dup && s_seen_count < 32) {
                    s_seen_ids[s_seen_count++] = id;
                    ESP_LOGI(TAG, "[CAN] new ID: 0x%03X (line=%lu)", id, (unsigned long)s_can_line_count);
                }
            }
        }
        s_can_line_count++;
    }

    // 行首解析 CAN ID (ATMA 行固定格式 "<ID> <D0> <D1> ... <Dn>", ID 在最前)。
    // 注意: 不能像以前那样在整行里 strstr 找 "<id> " 子串——如果某个数据字节的十六进制
    // 文本恰好等于另一个被监听 ID(比如油温字节=0x40, 车型里还监听着 0x040), 会在数据段里
    // 误命中, 把这一帧错当成别的 ID、真正的 ID 反而被跳过, 对应通道就一直读不到。
    uint16_t line_id = 0;
    if (sscanf(line, "%hx ", &line_id) != 1) return false;

    bool id_watched = false;
    for (uint8_t i = 0; i < ov->can_rule_count; i++) {
        if (ov->can_rules[i].can_id == line_id) { id_watched = true; break; }
    }
    if (!id_watched) return false;

    uint8_t data[8] = {0};
    int vals = sscanf(line, "%*x %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
                      &data[0],&data[1],&data[2],&data[3],
                      &data[4],&data[5],&data[6],&data[7]);
    if (vals < 1) return false;

    float channels[CH_COUNT];
    for (int c = 0; c < CH_COUNT; c++) channels[c] = -32768.0f;
    can_apply_rules(ov->can_rules, ov->can_rule_count, line_id, data, channels);

    if (channels[CH_RPM] >= 0 && s_cbs.on_parsed_rpm)
        s_cbs.on_parsed_rpm((uint16_t)channels[CH_RPM]);
    if (channels[CH_SPEED] >= 0 && s_cbs.on_parsed_speed_kmh)
        s_cbs.on_parsed_speed_kmh((uint8_t)channels[CH_SPEED]);
    if (channels[CH_OIL_TEMP] > -40 && channels[CH_OIL_TEMP] <= 215 && s_cbs.on_parsed_oil_temp)
        s_cbs.on_parsed_oil_temp((uint32_t)(int16_t)channels[CH_OIL_TEMP]);
    if (channels[CH_COOLANT] > -40 && channels[CH_COOLANT] <= 215 && s_cbs.on_parsed_coolant_temp)
        s_cbs.on_parsed_coolant_temp((uint32_t)(int16_t)channels[CH_COOLANT]);
    if (channels[CH_TPS] >= 0 && s_cbs.on_parsed_throttle_position)
        s_cbs.on_parsed_throttle_position((uint32_t)channels[CH_TPS]);
    if (channels[CH_GEAR] > 0 && channels[CH_GEAR] < 127 && s_cbs.on_parsed_gear)
        s_cbs.on_parsed_gear((int8_t)channels[CH_GEAR]);

    s_zc6_can_monitor_last_sample_us = esp_timer_get_time();
    s_last_obd_valid_us = esp_timer_get_time();
    return true;
}

// 逐字节喂入: 按 \r\n 切行, 每行调 parse_line
static void zc6_can_monitor_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '>') { s_elm_ready = true; continue; }
        if (ch == '\r' || ch == '\n') {
            if (s_zc6_can_monitor_len > 0) {
                s_zc6_can_monitor_buf[s_zc6_can_monitor_len] = '\0';
                zc6_can_monitor_parse_line(s_zc6_can_monitor_buf);
                s_zc6_can_monitor_len = 0;
            }
            continue;
        }
        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) continue;
        if (s_zc6_can_monitor_len + 1 >= ZC6_CAN_MONITOR_BUF_SIZE)
            s_zc6_can_monitor_len = 0;
        s_zc6_can_monitor_buf[s_zc6_can_monitor_len++] = ch;
    }
}

// ELM327 初始化序列(协议选择 + AT 配置 + 总线预热 + 油温策略)。
// 每次(重)连接、且通知订阅就绪后调用，避免握手响应在订阅前丢失。
static void do_elm_init(void) {
    char atsp_cmd[16];
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    // ---- 协议选择 ----
    uint8_t protocol_to_use = cfg->protocol;
    // 车型强制协议优先(如 BMW/保时捷 自动探测不稳, 直接锁协议6, 跳过探测)
    const vehicle_profile_t *vp_proto = vehicle_profile_get_active();
    if (vp_proto && vp_proto->forced_protocol != 0) {
        protocol_to_use = vp_proto->forced_protocol;
        ESP_LOGD(TAG, "Vehicle '%s' forces protocol %d (skip auto-detect)", vp_proto->name, protocol_to_use);
    } else if (protocol_to_use == 0) {
        // 自动协议检测
        ESP_LOGD(TAG, "Protocol auto-detect enabled (current NVS: 0-auto)");
        int detected_proto = elm327_auto_detect_protocol();
        if (detected_proto > 0) {
            protocol_to_use = (uint8_t)detected_proto;
            nvs_user_cfg_t new_cfg = *cfg;
            new_cfg.protocol = protocol_to_use;
            nvs_cfg_set(&new_cfg);
            ESP_LOGD(TAG, "Protocol auto-detect SUCCESS! Saving protocol %d to NVS", protocol_to_use);
        } else {
            protocol_to_use = 6;
            ESP_LOGW(TAG, "Protocol auto-detect FAILED, using fallback protocol 6");
        }
    }

    snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", protocol_to_use);
    const char *fixed_header_cmd = get_vehicle_fixed_header_cmd();
    // 超时命令: 优先用车型配置的 obd_timeout, 默认 0x19
    char atst_cmd[12];
    uint8_t timeout_val = (vp_proto && vp_proto->obd_timeout) ? vp_proto->obd_timeout : 0x19;
    snprintf(atst_cmd, sizeof(atst_cmd), "ATST %02X\r", timeout_val);
    const char *init_cmds[] = {
        "ATZ\r", "ATE0\r", "ATL0\r", "ATS1\r", "ATH0\r", "ATAT1\r", atst_cmd,
        atsp_cmd, fixed_header_cmd,
    };
    for (size_t i = 0; i < (sizeof(init_cmds) / sizeof(init_cmds[0])); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        ESP_LOGD(TAG, " AT init Cmd send %s", init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    // 总线预热: 多发几次 01 00, 给整车 CAN/ELM 协议握手时间(冷启动总线可能还没醒)
    for (int probe = 0; probe < 3; ++probe) {
        elm327_ble_send_ascii_blocking("01 00\r");
        ESP_LOGD(TAG, " CMD 01 00 probe #%d", probe);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // ---- 初始化油温查询策略（基于车型配置） ----
    init_oil_temp_strategy();
    s_can_rpm_fail_count = 0;  // 重连后重试 CAN RPM
    const vehicle_profile_t *active_profile = vehicle_profile_get_active();
    ESP_LOGD(TAG, "Active vehicle profile: %s", active_profile ? active_profile->name : "Unknown");
    s_last_obd_valid_us = esp_timer_get_time();   // 给一个新的"有效数据"起点, 避免刚初始化就触发自愈
}

static void obd_poll_task(void *arg) {
    s_poll_task_handle = xTaskGetCurrentTaskHandle();
    esp_task_wdt_add(NULL);  // 订阅看门狗
    uint32_t tick_count = 0;
    bool inited = false;
    uint8_t heal_attempts = 0;   // 连续自愈次数; 重发ATZ若干次仍无数据则升级为强制重连

    // 8-slot 轮询: 0=RPM, 1=IAT, 2=Speed, 3=CLT, 4=Load(0x04), 5=TPS(0x11), 6=OIL(车型策略), 7=BAT(0x42)
    while (1)
    {
        esp_task_wdt_reset();  // 喂狗
        // Showroom 模式: 暂停 OBD 轮询, 避免覆盖假数据
        extern bool ui_showroom_is_active(void);
        if (ui_showroom_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // 未连接或通知订阅未就绪 → 标记需重新初始化并等待
        if (!s_connected || !s_notify_ready) {
            inited = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        // (重)连接后通知就绪才初始化; 每次重连都会重跑(修复"必须断开重连才有读数")
        if (!inited) {
            vTaskDelay(pdMS_TO_TICKS(300));   // 给订阅再稳定一点时间
            do_elm_init();
            inited = true;
            tick_count = 0;
            continue;
        }
        // 数据正常流动 → 清零自愈计数
        if ((esp_timer_get_time() - s_last_obd_valid_us) < 10000000) heal_attempts = 0;
        // 自愈: 连续 >10s 收不到任何有效数据(没响应/SEARCHING/UNABLE TO CONNECT/NO DATA 全覆盖)。
        if ((esp_timer_get_time() - s_last_obd_valid_us) > 10000000) {
            s_last_obd_valid_us = esp_timer_get_time();
            heal_attempts++;
            if (heal_attempts >= 3) {
                // 重发 ATZ 已试几次仍无数据 → 强制断开蓝牙, DISCONNECT 回调会自动重连,
                // 等于自动执行了"手动断开重连"(重新订阅通知 + 重跑初始化, 此时总线多半已醒)。
                ESP_LOGW(TAG, "Self-heal escalate: force BLE reconnect (re-init didn't help)");
                heal_attempts = 0;
                inited = false;
                esp_ble_gattc_close(s_gattc_if, s_conn_id);
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            ESP_LOGW(TAG, "No valid OBD data >5s, re-init ELM (self-heal #%u)...", heal_attempts);
            inited = false;
            continue;
        }
        // ---- ZC/N6 CAN 模式: 转速走 CAN 0x140, 其余走标准 OBD (和普通 ZC/N6 完全一致) ----
        // 两阶段: ATMA 监听转速 → 每 N 轮退出, 跑一轮完整标准 OBD 轮询 → 再回 ATMA
        const vehicle_profile_t *vp_poll = vehicle_profile_get_active();
        bool can_broadcast = vp_poll && vp_poll->can_broadcast_mode;
        static bool s_zc_can_obd_phase = false;  // true=正在跑标准 OBD 轮询

        if (can_broadcast && !s_zc_can_obd_phase) {
            // ---- ATMA 阶段: 只收 0x140 转速 ----
            if (!s_zc6_can_monitor_active) {
                zc6_can_monitor_enter();
                s_zc6_can_monitor_obd_cycle = 0;
            }
            // 自愈
            if (s_zc6_can_monitor_entered_us > 0 &&
                (esp_timer_get_time() - s_last_obd_valid_us) > 10000000) {
                ESP_LOGW(TAG, "[ZC/N6 CAN] No data >10s, re-enter ATMA");
                zc6_can_monitor_exit();
                zc6_can_monitor_enter();
                s_zc6_can_monitor_obd_cycle = 0;
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            s_zc6_can_monitor_obd_cycle++;
            if (s_zc6_can_monitor_obd_cycle >= ZC6_CAN_OBD_INTERVAL) {
                // 切换到 OBD 阶段
                zc6_can_monitor_exit();
                s_zc_can_obd_phase = true;
                tick_count = 1;  // 从 slot1 开始, 跳过 slot0(RPM 已由 CAN 提供)
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        // ---- 标准 OBD 轮询 (ZC/N6 CAN 的 OBD 阶段 或 非 CAN 车型) ----
        // 完全复用下面的 switch(tick_count), 和普通 ZC/N6 一模一样
        if (can_broadcast && s_zc_can_obd_phase && tick_count == 0) {
            // 一轮标准 OBD 跑完, 回到 ATMA 阶段
            s_zc_can_obd_phase = false;
            zc6_can_monitor_enter();
            s_zc6_can_monitor_obd_cycle = 0;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        {
        switch(tick_count)
        {
            case 0://发动机转速
                elm327_ble_send_ascii_blocking("01 0C\r");
                ESP_LOGD(TAG, "Send 01 0C");
                break;
            case 1://进气温度
                elm327_ble_send_ascii_blocking("01 0F\r");
                ESP_LOGD(TAG, "Send 01 0F");
                break;
            case 6: // 机油温自动查询（基于车型策略）(CAN 模式由 0x360 提供, 跳过)
                if (can_broadcast) break;
                {
                    // ---- 数据驱动油温查询 ----
                    const oil_formula_t *oil_f = NULL;
                    if (s_oil_use_override) {
                        oil_f = (s_oil_override_idx == 0) ? s_oil_formula_pri : s_oil_formula_sec;
                    }

                    if (oil_f && oil_f->type != OIL_SPECIAL) {
                        // 通用公式: 自动构建命令
                        char cmd_buf[24];
                        // 功能寻址车型查 UDS 时需临时切物理寻址
                        bool need_phys = s_ov && s_ov->functional_addr && oil_f->type == OIL_UDS_22;
                        if (need_phys) elm327_ble_send_ascii_blocking("ATSH7E0\r");
                        if (oil_formula_build_cmd(oil_f, cmd_buf, sizeof(cmd_buf))) {
                            elm327_ble_send_ascii_blocking(cmd_buf);
                            ESP_LOGI(TAG, "[Slot6] Override oil: %s", cmd_buf);
                        }
                        if (need_phys) elm327_ble_send_ascii_blocking("ATSH7DF\r");
                        s_expect_mode21 = false;
                    } else if (oil_f && oil_f->type == OIL_SPECIAL && oil_f->special_id == 0) {
                        // Toyota Mode 21 01 (特殊多帧)
                        elm327_ble_send_ascii_blocking("21 01\r");
                        s_expect_mode21 = true;
                        ESP_LOGI(TAG, "[Slot6] Override oil: Toyota 21 01");
                    } else if (oil_f && oil_f->type == OIL_SPECIAL && oil_f->special_id == 1) {
                        // Porsche CAN 0x441
                        s_expect_mode21 = false;
                        porsche_read_can_441();
                        ESP_LOGI(TAG, "[Slot6] Override oil: Porsche CAN 441");
                    } else {
                        // 无 override: 走旧 enum 逻辑 (OBD2 Generic 等)
                        uint8_t poll_idx = 0;
                        oil_temp_query_mode_t mode = get_next_oil_query_mode(&poll_idx);
                        s_expect_mode21 = (mode == OIL_TEMP_MODE_TOYOTA_21_01);
                        if (mode == OIL_TEMP_MODE_PID_5C)
                            elm327_ble_send_ascii_blocking("01 5C\r");
                        else if (mode == OIL_TEMP_MODE_TOYOTA_21_01)
                            elm327_ble_send_ascii_blocking("21 01\r");
                        else if (mode == OIL_TEMP_MODE_PORSCHE_CAN_441)
                            porsche_read_can_441();
                        else {
                            // 其余旧 enum 走通用 UDS 构建
                            char cmd_buf[24];
                            oil_formula_t legacy_f = {0};
                            legacy_f.type = OIL_UDS_22;
                            legacy_f.pid_len = 2;
                            // 从旧 enum 映射 PID
                            switch (mode) {
                                case OIL_TEMP_MODE_UDS_22_10_17: legacy_f.pid[0]=0x10; legacy_f.pid[1]=0x17; break;
                                case OIL_TEMP_MODE_MAZDA_22_111F: legacy_f.pid[0]=0x11; legacy_f.pid[1]=0x1F; break;
                                case OIL_TEMP_MODE_MAZDA_22_1310: legacy_f.pid[0]=0x13; legacy_f.pid[1]=0x10; break;
                                case OIL_TEMP_MODE_MINI_22_5822: legacy_f.pid[0]=0x58; legacy_f.pid[1]=0x22; break;
                                case OIL_TEMP_MODE_BMW_22_4402:
                                case OIL_TEMP_MODE_BMW_G_22_4402: legacy_f.pid[0]=0x44; legacy_f.pid[1]=0x02; break;
                                case OIL_TEMP_MODE_BMW_22_03F3: legacy_f.pid[0]=0x03; legacy_f.pid[1]=0xF3; break;
                                case OIL_TEMP_MODE_BMW_22_D002: legacy_f.pid[0]=0xD0; legacy_f.pid[1]=0x02; break;
                                case OIL_TEMP_MODE_BMW_22_111F: legacy_f.pid[0]=0x11; legacy_f.pid[1]=0x1F; break;
                                default: legacy_f.type = OIL_STD_PID; legacy_f.pid[0]=0x5C; legacy_f.pid_len=1; break;
                            }
                            bool need_phys = (mode == OIL_TEMP_MODE_BMW_22_03F3 ||
                                              mode == OIL_TEMP_MODE_BMW_G_22_4402 ||
                                              mode == OIL_TEMP_MODE_BMW_22_D002 ||
                                              mode == OIL_TEMP_MODE_BMW_22_111F);
                            if (need_phys) elm327_ble_send_ascii_blocking("ATSH7E0\r");
                            if (oil_formula_build_cmd(&legacy_f, cmd_buf, sizeof(cmd_buf)))
                                elm327_ble_send_ascii_blocking(cmd_buf);
                            if (need_phys) elm327_ble_send_ascii_blocking("ATSH7DF\r");
                        }
                        s_oil_query_mode = poll_idx;
                    }
                }
                break;
            case 2://车速
                elm327_ble_send_ascii_blocking("01 0D\r");
                ESP_LOGD(TAG, "Send 01 0D");
                break;
            case 3://冷却液温度 (CAN 模式由 0x360 提供, 跳过)
                if (!can_broadcast) {
                    elm327_ble_send_ascii_blocking("01 05\r");
                    ESP_LOGD(TAG, "Send 01 05");
                }
                break;
            case 4://发动机负荷 (0x04, 0~100%)
                elm327_ble_send_ascii_blocking("01 04\r");
                ESP_LOGD(TAG, "[Slot4] Send 01 04 (engine load)");
                break;
            case 5://节气门开度 TPS (0x11, 0~100%)
                elm327_ble_send_ascii_blocking("01 11\r");
                ESP_LOGD(TAG, "[Slot5] Send 01 11 (TPS)");
                break;
            case 7://电池电压 (0x42)
                elm327_ble_send_ascii_blocking("01 42\r");
                ESP_LOGD(TAG, "[Slot7] Send 01 42 (bat voltage)");
                break;
            case 8://涡轮压力: 进气歧管绝对压力 (0x0B, kPa)，仅涡轮车型查询
                {
                    const vehicle_profile_t *vp = vehicle_profile_get_active();
                    if (vp && vp->has_boost) {
                        elm327_ble_send_ascii_blocking("01 0B\r");
                        ESP_LOGD(TAG, "[Slot8] Send 01 0B (boost/MAP)");
                    }
                }
                break;
            case 9://空燃比 AFR (01 44, Commanded Equivalence Ratio)
                elm327_ble_send_ascii_blocking("01 44\r");
                ESP_LOGD(TAG, "[Slot9] Send 01 44 (AFR/lambda)");
                break;
            default:
                break;
        }

        // RPM 提频: 标准 OBD 轮询车型下, 转速不应该被绑定在整轮(10槽)周期上才刷新一次。
        // slot0 本身已经查过 RPM, 这里给 slot1~9 每槽后面都补插一次 01 0C,
        // 转速刷新间隔从"一整轮"降到"一个槽", 其它慢变量(温度/电压等)刷新节奏不变。
        // CAN 广播车型转速走 ATMA 直通, 不需要(也不应该, 会和 ATMA 抢总线)。
        if (!can_broadcast && tick_count != 0) {
            elm327_ble_send_ascii_blocking("01 0C\r");
        }

        tick_count++;
        if(tick_count >= 10)
        {
            tick_count = 0;
        }
        } // end standard OBD poll block

        // 槽间空等: 优先用车型配置的 poll_gap_ms(如 MX-5 ND 升级了 150ms), 未配置则用全局默认 30ms。
        // 太小会压垮廉价蓝牙适配器; CAN 总线快速响应车型可放心调小。
        // 如果用户设置 poll_gap_ms = 0，则不用 vTaskDelay，直接过。
        {
            const vehicle_profile_t *vp_gap = vehicle_profile_get_active();
            uint32_t gap = (vp_gap && vp_gap->poll_gap_ms > 0)
                           ? vp_gap->poll_gap_ms : OBD_POLL_SLOT_GAP_MS;
            if (gap > 0) vTaskDelay(pdMS_TO_TICKS(gap));
        }
    }
}

// Mode 21 多帧解析器：从 "61 01" 之后提取所有数据字节
// 跳过 ELM327 行号前缀 ("N: ") 和 ISO-TP 连续帧序列字节 (0x20~0x2F)
// 返回提取到的字节数，结果存入 out[]
static int parse_mode21_data(const char *buf, uint32_t *out, int max_out) {
    const char *p = strstr(buf, "61 01");
    if (!p) return 0;
    p += 5; // 跳过 "61 01"
    if (*p == ' ') p++;

    int count = 0;
    bool new_line = false;

    while (*p && count < max_out) {
        if (*p == '>') break;
        if (*p == '\r' || *p == '\n') {
            new_line = true;
            p++;
            continue;
        }
        if (new_line) {
            // 跳过 "N: " 前缀（一个或多个数字 + 冒号 + 空格）
            while (isdigit((unsigned char)*p)) p++;
            if (*p == ':') p++;
            while (*p == ' ') p++;
            // 跳过 ISO-TP 连续帧序列字节 (0x20~0x2F)
            if (isxdigit((unsigned char)*p) && isxdigit((unsigned char)*(p+1))) {
                char tmp[3] = {*p, *(p+1), '\0'};
                unsigned bval = (unsigned)strtoul(tmp, NULL, 16);
                if (bval >= 0x20 && bval <= 0x2F) {
                    p += 2;
                    if (*p == ' ') p++;
                }
            }
            new_line = false;
            continue;
        }
        // 解析一个十六进制字节对
        if (isxdigit((unsigned char)*p) && isxdigit((unsigned char)*(p+1))) {
            char tmp[3] = {*p, *(p+1), '\0'};
            out[count++] = (uint32_t)strtoul(tmp, NULL, 16);
            p += 2;
        } else {
            p++;
        }
        if (*p == ' ') p++;
    }
    return count;
}

// 从 Mode21 数据中提取机油温字节（以 ZC/N6 为参考）
// ZC/N6: 永远只用 d[33]，不进入自适应搜索（自适应搜索可能误选其他字节导致跳到 60/70°C）
static bool extract_mode21_oil_temp(const uint32_t *d, int count, int32_t *oil_c) {
    if (!d || count <= 0 || !oil_c) return false;

    int16_t coolant = obd_data_get_coolant_temp();

    ESP_LOGD(TAG, "Mode21 extract: total_count=%d, coolant=%d", count, coolant);

    // ---- ZC/N6: 用尾部偏移定位, 适配38/39字节两种响应长度 ----
    // 38字节时油温在d[33]=d[38-5]; 39字节时在d[34]=d[39-5]。固定d[33]在39字节帧会读到错误字节。
    if (get_active_vehicle_idx_safe() == 1) {
        #define ZC_MODE21_OIL_TAIL_OFFSET 5
        int zc_idx = count - ZC_MODE21_OIL_TAIL_OFFSET;
        if (zc_idx < 0 || zc_idx >= count) {
            ESP_LOGW(TAG, "Mode21 ZC/N6 short response count=%d, skip", count);
            s_oil_diag.mode2_fail++;
            return false;
        }
        int32_t zc_temp = (int32_t)d[zc_idx] - 40;
        if (zc_temp < -10 || zc_temp > 150) {
            ESP_LOGW(TAG, "Mode21 ZC/N6 d[%d] out of range: raw=%u, skip", zc_idx, (unsigned)d[zc_idx]);
            s_oil_diag.mode2_fail++;
            return false;
        }
        // 一致性检查：油温在两次轮询间（~270ms）物理上不可能跳变超 8°C
        // 但如果距上次接受值 >3s（中间帧失败导致间隔大），直接接受（真实温度可能已变）
        int64_t now_us = esp_timer_get_time();
        bool time_gap = (s_last_mode21_oil_us == 0) || ((now_us - s_last_mode21_oil_us) > 3000000);
        bool consistent = (s_last_mode21_oil <= -50) || time_gap ||
                          (abs((int)zc_temp - (int)s_last_mode21_oil) <= 8);
        if (consistent) {
            s_last_mode21_oil = (int16_t)zc_temp;
            s_last_mode21_oil_us = now_us;
            s_mode21_hold_cnt = 0;
            s_oil_diag.mode2_ok++;
            *oil_c = zc_temp;
            ESP_LOGI(TAG, "Mode21 ZC/N6 bytes=%d d[%d]=0x%02X -> %dC", count, zc_idx, (unsigned)d[zc_idx], (int)zc_temp);
            return true;
        }
        // 一致性检查失败：hold 上次值，防止单帧噪声显示
        if (s_mode21_hold_cnt < 30) {
            s_mode21_hold_cnt++;
            s_oil_diag.mode2_ok++;
            *oil_c = s_last_mode21_oil;
            ESP_LOGW(TAG, "Mode21 ZC/N6 spike HELD(%d/30): prev=%d new=%d",
                     s_mode21_hold_cnt, (int)s_last_mode21_oil, (int)zc_temp);
            return true;
        }
        // Hold 超时（~8s 持续不一致）：视为真实温度变化，接受并重置基准
        ESP_LOGW(TAG, "Mode21 ZC/N6 hold timeout: accept %d (was %d)", (int)zc_temp, (int)s_last_mode21_oil);
        s_last_mode21_oil = (int16_t)zc_temp;
        s_last_mode21_oil_us = esp_timer_get_time();
        s_mode21_hold_cnt = 0;
        s_oil_diag.mode2_ok++;
        *oil_c = zc_temp;
        return true;
    }

    // ---- 策略 1: 使用上次找到的索引（快速路径）----
    if (s_mode21_oil_idx >= 0 && s_mode21_oil_idx < count) {
        int32_t c = (int32_t)d[s_mode21_oil_idx] - 40;
        bool in_range = (c >= -10 && c <= 150);
        // 油温在两次轮询间（~270ms）物理上不可能跳变超 8°C；用此过滤噪声字节
        bool consistent = (s_last_mode21_oil <= -50) || (abs((int)c - (int)s_last_mode21_oil) <= 8);
        if (in_range && consistent) {
            s_last_mode21_oil = (int16_t)c;
            s_mode21_hold_cnt = 0;
            *oil_c = c;
            ESP_LOGD(TAG, "Mode21: Using cached idx=%d -> %dC", s_mode21_oil_idx, (int)c);
            s_oil_diag.mode2_ok++;
            return true;
        }
        if (in_range && !consistent) {
            // 短期 hold 上次值，避免单帧噪声显示
            if (s_mode21_hold_cnt < 30) {
                s_mode21_hold_cnt++;
                s_oil_diag.mode2_ok++;
                *oil_c = s_last_mode21_oil;
                ESP_LOGW(TAG, "Mode21: Fast path spike HELD(%d/30) idx=%d prev=%d new=%d",
                         s_mode21_hold_cnt, s_mode21_oil_idx, (int)s_last_mode21_oil, (int)c);
                return true;
            }
            // Hold 超时：缓存索引的值在范围内但持续 ~8s 不一致，视为真实温度变化。
            // 接受新值并重置基准，不落入自适应搜索（自适应搜索可能误选其他字节导致跳变）。
            ESP_LOGW(TAG, "Mode21: Fast path hold timeout: accept new val=%d at idx=%d (was %d), reset baseline",
                     (int)c, s_mode21_oil_idx, (int)s_last_mode21_oil);
            s_last_mode21_oil = (int16_t)c;
            s_mode21_hold_cnt = 0;
            s_oil_diag.mode2_ok++;
            *oil_c = c;
            return true;
        }
        // 缓存索引的值越界（索引失效），才落入自适应搜索重新发现
    }

    // ---- 策略 2: 智能搜索 ----
    // 采用两阶段搜索：先严格检查与水温的差异（±25°C），再扩大范围
    int best_idx = -1;
    int32_t best_temp = 0;
    int best_distance = -1;
    int strict_count = 0;

    for (int idx = 0; idx < count; idx++) {
        int32_t c = (int32_t)d[idx] - 40;

        // 基础范围检查：-10 到 150°C（宽泛）
        if (c < -10 || c > 150) continue;

        // 严格匹配：油温通常比水温高 5~20°C；正好等于水温的字节大概率是水温回显，优先级降低
        if (coolant > -40) {
            int diff = (int)c - (int)coolant;

            // 第一阶段：严格 ±25°C
            if (diff >= -25 && diff <= 25) {
                // 油温略高于水温得最高分；恰好等于水温（diff≈0）得中等分（可能是水温回显字节）
                int priority = (diff > 0 && diff <= 20) ? 1000 :   // 理想：油温 > 水温
                               (diff >= -5 && diff <= 0) ? 700  :   // 冷车或等温：可接受
                               (diff > 20 && diff <= 25) ? 400  : 100; // 极热或偏冷
                int score = priority - abs(diff);  // 差异越小分越高
                
                if (score > best_distance) {
                    best_distance = score;
                    best_idx = idx;
                    best_temp = c;
                    strict_count++;
                    ESP_LOGD(TAG, "  Strict match: idx=%d temp=%dC diff=%d score=%d", idx, (int)c, diff, score);
                }
            }
        } else {
            // 水温无效时，取任何有效温度（但记录警告）
            if (best_idx < 0) {
                best_idx = idx;
                best_temp = c;
                ESP_LOGW(TAG, "  Fallback (no coolant): idx=%d temp=%dC", idx, (int)c);
            }
        }
    }

    if (best_idx >= 0) {
        ESP_LOGD(TAG, "Mode21 selected: idx=%d temp=%dC (strict_matches=%d)", best_idx, (int)best_temp, strict_count);
        s_mode21_oil_idx = best_idx;
        s_last_mode21_oil = (int16_t)best_temp;
        s_mode21_hold_cnt = 0;
        s_oil_diag.mode2_ok++;
        *oil_c = best_temp;
        return true;
    }
    
    ESP_LOGW(TAG, "Mode21: No valid candidate found (coolant=%d)", coolant);
    s_oil_diag.mode2_fail++;
    return false;
}

static void start_scan(void) {
    esp_ble_gap_start_scanning(10); // 10s
}

static void normalize_name(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_len; i++) {
        char c = src[i];
        if (c == ' ' || c == '-' || c == '_' || c == '\t') continue;
        dst[out++] = (char)tolower((unsigned char)c);
    }
    dst[out] = '\0';
}

static bool match_device_target(const esp_ble_gap_cb_param_t *pr, const char *target_name,
                                char *found_name, size_t found_name_len) {
    if (!pr) return false;
    ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len, pr->scan_rst.scan_rsp_len,
                     found_name, found_name_len);

    // 已绑定精确 MAC: 只认这一个地址, 忽略名字, 防止同名(甚至同型号广播名)设备误连
    if (s_target_bda_valid) {
        return memcmp(pr->scan_rst.bda, s_target_bda, sizeof(esp_bd_addr_t)) == 0;
    }

    if (target_name == NULL || target_name[0] == '\0') return true;

    if (found_name[0] == '\0') return false;

    char found_norm[40] = {0};
    char target_norm[40] = {0};
    normalize_name(found_name, found_norm, sizeof(found_norm));
    normalize_name(target_name, target_norm, sizeof(target_norm));
    if (found_norm[0] == '\0' || target_norm[0] == '\0') return false;

    // 兼容短名/截断名: 全等、包含、被包含均视为命中
    return (strcmp(found_norm, target_norm) == 0) ||
           (strstr(found_norm, target_norm) != NULL) ||
           (strstr(target_norm, found_norm) != NULL);
}

static void request_discovery(void) {
    // NULL = 发现所有服务，兼容不同UUID的ELM327适配器
    esp_ble_gattc_search_service(s_gattc_if, s_conn_id, NULL);
}

static void enable_notify_if_ready(void) {
    if (s_cccd_handle) {
        uint8_t notify_en[2] = {0x01, 0x00};
        esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, s_cccd_handle,
                                       sizeof(notify_en), notify_en,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

void elm327_ble_init_and_start(const char *target_name, const elm327_ble_callbacks_t *cbs) {
    if (cbs) s_cbs = *cbs;
    if (target_name && target_name[0]) {
        strncpy(s_target_name, target_name, sizeof(s_target_name)-1);
        s_target_name[sizeof(s_target_name)-1] = '\0';
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    }
    if (!esp_bluedroid_get_status()) {
        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    } else if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));
    s_ble_inited = true;
}

bool elm327_ble_send_command(const uint8_t *data, size_t len) {
    if (!s_connected || s_char_write_handle == 0) {
        s_elm_ready = true; // 无法发送时恢复标志，防止一直超时
        return false;
    }
    if (len == 0 || data == NULL) { s_elm_ready = true; return false; }
    esp_err_t err = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_char_write_handle,
                                             len, (uint8_t *)data,
                                             s_write_type, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) s_elm_ready = true; // 发送失败也要恢复
    return err == ESP_OK;
}

// 阻塞直到上一个响应结束（收到 '>'）后再发送
// 使用 FreeRTOS task notification 替代 10ms 轮询: 收到 '>' 时 xTaskNotify 立刻唤醒，零等待开销
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd)
{
    if (!s_elm_ready) {
        uint32_t waited_ms = 0;
        while (!s_elm_ready && waited_ms < 3000) {
            // 最多等 10ms（作为兜底），但 '>' 到达时 xTaskNotify 会提前唤醒
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            waited_ms += 10;
            // 喂狗: 这个等待循环本身可能跑到接近 3s(适配器无响应/协议探测连续多次超时时),
            // TWDT 超时配置只有 5s, 之前这里不喂狗导致协议探测连续超时几次就会把 obd_poll
            // 任务的看门狗喂饱触发重启(实测复现过)。
            esp_task_wdt_reset();
        }
        if (!s_elm_ready) {
            ESP_LOGW(TAG, "Timeout (>3s) waiting previous response, forcing send: %s", ascii_cmd);
            s_elm_ready = true;
        }
    }
    s_elm_ready = false;
    uint8_t buf[32];
    size_t n = elm327_ble_ascii_cmd_to_bytes(ascii_cmd, buf, sizeof(buf));
    if (n) return elm327_ble_send_command(buf, n);
    else {
        s_elm_ready = true;
        return false;
    }
}

// 将 ASCII 指令(如 "01 0C\r")复制到输出缓冲区，同时去除空白字符，保持 ELM327 所需的 ASCII 格式
size_t elm327_ble_ascii_cmd_to_bytes(const char *ascii, uint8_t *out_buf, size_t out_buf_len) {
    size_t out = 0;
    const char *p = ascii;
    while (*p && out < out_buf_len) {
        if (*p == ' ' || *p == '\t') {
            p++;                // 跳过空白符
            continue;
        }
        out_buf[out++] = (uint8_t)(*p++); // 直接复制 ASCII 字节
    }
    return out;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    racechrono_ble_diy_handle_gap_event(event, param);

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        start_scan();
        break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *pr = param;
        if (pr->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            char dev_name[32] = {0};
            ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len,
                             pr->scan_rst.scan_rsp_len, dev_name, sizeof(dev_name));

            if (s_scan_only_mode) {
                // 扫描模式：收集设备列表
                if (dev_name[0] != '\0' && s_scan_count < BLE_SCAN_MAX_DEVICES) {
                    // 检查是否已存在
                    bool exists = false;
                    for (int i = 0; i < s_scan_count; i++) {
                        if (strcmp(s_scan_list[i].name, dev_name) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        strncpy(s_scan_list[s_scan_count].name, dev_name, 31);
                        memcpy(s_scan_list[s_scan_count].addr, pr->scan_rst.bda, 6);
                        s_scan_list[s_scan_count].rssi = pr->scan_rst.rssi;
                        s_scan_count++;
                        ESP_LOGD(TAG, "Scan found [%d]: %s (RSSI %d)", s_scan_count, dev_name, pr->scan_rst.rssi);
                        if (s_scan_cb) s_scan_cb(&s_scan_list[s_scan_count - 1], s_scan_count);
                    }
                }
            } else {
                // 正常模式：匹配名称后连接
                bool matched = match_device_target(pr, s_target_name, dev_name, sizeof(dev_name));
                ESP_LOGD(TAG, "Scan: name=%s rssi=%d target=%s match=%d",
                         dev_name[0] ? dev_name : "<no-name>", pr->scan_rst.rssi,
                         s_target_name, matched);
                if (matched) {
                    ESP_LOGD(TAG, "Found target %s (dev=%s), connecting...",
                             s_target_name, dev_name[0] ? dev_name : "<no-name>");
                    esp_ble_gap_stop_scanning();
                    esp_ble_gattc_open(s_gattc_if, pr->scan_rst.bda, pr->scan_rst.ble_addr_type, true);
                }
            }
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
    default:
        break;
    }
}

// 在多行累积缓冲区里定位某个 CAN ID 的帧头 token, 只认"位于行首"的匹配。
// 不能直接 strstr 整个缓冲区: 如果某帧的数据字节文本恰好等于另一个被监听 ID(比如
// 油温字节=0x40, 车型还监听着 0x040), 会在数据段里误命中、把这帧错当成别的 ID。
static const char *find_can_id_token(const char *buf, uint16_t id) {
    char upper[8], lower[8];
    snprintf(upper, sizeof(upper), "%X ", id);
    snprintf(lower, sizeof(lower), "%x ", id);
    for (int pass = 0; pass < 2; pass++) {
        const char *needle = (pass == 0) ? upper : lower;
        const char *scan = buf;
        while ((scan = strstr(scan, needle)) != NULL) {
            if (scan == buf || scan[-1] == '\r' || scan[-1] == '\n') return scan;
            scan += 1; // 数据字节里的巧合命中, 跳过继续找真正的行首
        }
    }
    return NULL;
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT: {
        s_gattc_if = gattc_if;
        esp_ble_scan_params_t scan_params = {
            .scan_type              = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval          = 0x60,
            .scan_window            = 0x30,
            .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
        };
        esp_ble_gap_set_scan_params(&scan_params);
        break;
    }
    case ESP_GATTC_CONNECT_EVT: {
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        if (s_cbs.on_connected) s_cbs.on_connected();
        request_discovery();
        break;
    }
    case ESP_GATTC_OPEN_EVT: {
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed status=%d", param->open.status);
            start_scan();
        }
        break;
    }
    case ESP_GATTC_SEARCH_RES_EVT: {
        const esp_gatt_id_t *srvc_id = &param->search_res.srvc_id;
        uint16_t sh = param->search_res.start_handle;
        uint16_t eh = param->search_res.end_handle;
        if (srvc_id->uuid.len == ESP_UUID_LEN_16) {
            ESP_LOGD(TAG, "Service found: UUID=0x%04X handle=%04X~%04X",
                     srvc_id->uuid.uuid.uuid16, sh, eh);
            if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE) {
                s_have_service = true;
                s_service_start = sh;
                s_service_end = eh;
                ESP_LOGD(TAG, "Target service FFF0 matched");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_18F0) {
                s_have_18f0 = true;
                s_18f0_start = sh;
                s_18f0_end = eh;
                ESP_LOGD(TAG, "Target service 18F0 matched (IOS-Vlink OBD)");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_FF12) {
                s_have_ff12 = true;
                s_ff12_start = sh;
                s_ff12_end = eh;
                ESP_LOGD(TAG, "Target service FF12 matched");
            }
        } else {
            ESP_LOGD(TAG, "Service found: UUID(long) handle=%04X~%04X", sh, eh);
        }
        // 记录最大handle范围，用于兜底全量查找
        if (eh > s_all_attr_end || s_all_attr_end == 0xFFFF) s_all_attr_end = eh;
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        ESP_LOGD(TAG, "Service discovery complete. have_FFF0=%d have_18F0=%d have_FF12=%d",
                 s_have_service, s_have_18f0, s_have_ff12);

        // 优先顺序: 0xFFF0 > 0x18F0(IOS-Vlink) > 0xFF12 > 全范围兜底
        if (!s_have_service) {
            if (s_have_18f0) {
                s_service_start = s_18f0_start;
                s_service_end   = s_18f0_end;
                ESP_LOGD(TAG, "Using 18F0 service range 0x%04X~0x%04X", s_service_start, s_service_end);
            } else if (s_have_ff12) {
                s_service_start = s_ff12_start;
                s_service_end   = s_ff12_end;
                ESP_LOGD(TAG, "Using FF12 service range 0x%04X~0x%04X", s_service_start, s_service_end);
            } else {
                s_service_start = 0x0001;
                s_service_end = (s_all_attr_end > 0x0001) ? s_all_attr_end : 0xFFFF;
                ESP_LOGW(TAG, "FFF0/18F0/FF12 not found, using full range 0x0001~0x%04X", s_service_end);
            }
        }

        // 枚举全部特征，按属attr（WRITE/NOTIFY）选取
        uint16_t char_count = 0;
        esp_err_t ret = esp_ble_gattc_get_attr_count(gattc_if, s_conn_id,
            ESP_GATT_DB_CHARACTERISTIC, s_service_start, s_service_end, 0, &char_count);
        ESP_LOGD(TAG, "get_attr_count ret=%d, char_count=%d", ret, char_count);

        if (ret != ESP_OK || char_count == 0) {
            ESP_LOGE(TAG, "No characteristics found in range! Cannot communicate.");
            break;
        }

        // 分配特征数组
        uint16_t alloc_count = char_count;
        esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)malloc(alloc_count * sizeof(esp_gattc_char_elem_t));
        if (!chars) { ESP_LOGE(TAG, "malloc failed"); break; }

        ret = esp_ble_gattc_get_all_char(gattc_if, s_conn_id,
            s_service_start, s_service_end, chars, &alloc_count, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "get_all_char failed: %d", ret);
            free(chars); break;
        }

        // 打印全部特征，并自动选取写入/通知句柄
        ESP_LOGD(TAG, "=== All characteristics (%d) ===", alloc_count);
        for (int i = 0; i < alloc_count; i++) {
            esp_gattc_char_elem_t *c = &chars[i];
            if (c->uuid.len == ESP_UUID_LEN_16) {
                ESP_LOGD(TAG, "  [%d] UUID=0x%04X handle=0x%04X prop=0x%02X",
                         i, c->uuid.uuid.uuid16, c->char_handle, c->properties);
            } else if (c->uuid.len == ESP_UUID_LEN_128) {
                ESP_LOGD(TAG, "  [%d] UUID128=%02X%02X...%02X%02X handle=0x%04X prop=0x%02X",
                         i, c->uuid.uuid.uuid128[15], c->uuid.uuid.uuid128[14],
                            c->uuid.uuid.uuid128[1],  c->uuid.uuid.uuid128[0],
                            c->char_handle, c->properties);
            }
            // 选取第一个具有WRITE属性的特征作为写句柄
            if (s_char_write_handle == 0 &&
                (c->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
                s_char_write_handle = c->char_handle;
                // 优先用 WRITE_NR（无响应写），避免 status=3 (WRITE_NOT_PERMIT)
                s_write_type = (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR)
                               ? ESP_GATT_WRITE_TYPE_NO_RSP
                               : ESP_GATT_WRITE_TYPE_RSP;
                ESP_LOGD(TAG, "  >> Selected as WRITE handle: 0x%04X (write_type=%s)",
                         s_char_write_handle,
                         s_write_type == ESP_GATT_WRITE_TYPE_NO_RSP ? "NO_RSP" : "RSP");
            }
            // 选取第一个具有NOTIFY属性的特征作为通知句柄
            if (s_char_notify_handle == 0 &&
                (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
                s_char_notify_handle = c->char_handle;
                ESP_LOGD(TAG, "  >> Selected as NOTIFY handle: 0x%04X", s_char_notify_handle);
            }
        }
        free(chars);

        if (s_char_write_handle == 0) {
            ESP_LOGE(TAG, "No WRITE characteristic found! Cannot send commands.");
            break;
        }
        // 如果没有独立的NOTIFY特征，复用写句柄
        if (s_char_notify_handle == 0) {
            s_char_notify_handle = s_char_write_handle;
            ESP_LOGD(TAG, "No NOTIFY char found, using WRITE handle 0x%04X for notify", s_char_notify_handle);
        }

        // 注册通知
        int sret = esp_ble_gattc_register_for_notify(gattc_if, s_peer_bda, s_char_notify_handle);
        ESP_LOGD(TAG, "register_for_notify handle=0x%04X ret=%d", s_char_notify_handle, sret);

        // 查找 CCCD
        esp_gattc_descr_elem_t descr_elems[2];
        uint16_t count = 2;
        esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = UUID16_CCCD } };
        ret = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
            s_char_notify_handle, cccd_uuid, descr_elems, &count);
        if (ret == ESP_OK && count > 0) {
            s_cccd_handle = descr_elems[0].handle;
            ESP_LOGD(TAG, "Found CCCD, handle: 0x%04X", s_cccd_handle);
        } else {
            ESP_LOGW(TAG, "CCCD not found (ret=%d cnt=%d)", ret, count);
        }
        enable_notify_if_ready();
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGD(TAG, "Notifications enabled");
            s_notify_ready = true;   // 订阅就绪 → 放行轮询任务做 ELM 初始化
        } else {
            ESP_LOGW(TAG, "Enable notify failed status=%d", param->write.status);
        }
        break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
        if (s_cbs.on_raw_notify) s_cbs.on_raw_notify(param->notify.value, param->notify.value_len);
        const uint8_t *v = param->notify.value;
        int n = param->notify.value_len;

        // ZC6 CAN 持续监听模式: 逐字节喂入, 按行解析, 不走累积缓冲区
        if (s_zc6_can_monitor_active) {
            zc6_can_monitor_feed(v, (size_t)n);
            break;
        }

        // ---- 累积多包数据直到收到 '>' （ELM327 提示符） ----
        // 累积超时保护：10秒内未收到 '>' 则强制刷新 (ATMA 模式下可能长时间无 '>' 提示符)
        if (s_accum_len > 0) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - s_accum_start_us) > 10000000) {
                ESP_LOGW(TAG, "Accum timeout (>10s), flushing %d bytes", (int)s_accum_len);
                s_accum_len = 0;
                s_accum_buf[0] = '\0';
                s_elm_ready = true;
            }
        }
        if (s_accum_len == 0) {
            s_accum_start_us = esp_timer_get_time();
        }
        size_t space_left = ACCUM_BUF_SIZE - 1 - s_accum_len;
        size_t copy_n = ((size_t)n < space_left) ? (size_t)n : space_left;
        memcpy(s_accum_buf + s_accum_len, v, copy_n);
        s_accum_len += copy_n;
        s_accum_buf[s_accum_len] = '\0';

        // 没收到 '>' 就继续等
        if (memchr(s_accum_buf, '>', s_accum_len) == NULL) break;

        // 收到完整响应，开始解析
        char *buf = s_accum_buf;
        ESP_LOGD(TAG, "FULL[%d]: %.200s", (int)s_accum_len, buf); // 诊断: 打印每条完整响应

        // 油温相关响应 raw dump (便于确认 gt96 等适配器是否正确返回数据)
        {
            uint32_t first_tok = 0;
            int ntk = sscanf(buf, "%x", &first_tok);
            if (s_expect_mode21 && ntk == 1 && first_tok == 0x61) {
                ESP_LOGI(TAG, "[OIL RX] Mode21 raw[%d]: %.200s", (int)s_accum_len, buf);
            } else if (!s_expect_mode21 && ntk == 1 && first_tok == 0x62) {
                ESP_LOGI(TAG, "[OIL RX] Mode22 raw[%d]: %.200s", (int)s_accum_len, buf);
            } else if (strstr(buf, "441 ")) {
                ESP_LOGI(TAG, "[OIL RX] CAN441 raw[%d]: %.200s", (int)s_accum_len, buf);
            }
        }

        // ELM327 可能在数据前附带 echo，用 strstr 全内部搜索响应头
        // 注意: p61 必须先于 p41 判断，因为 2101 的多帧响应体中可能包含 0x41 字节
        // 导致 "41 " 被错误匹配而跳过 Mode21 解析
        char *p61 = strstr(buf, "61 01"); // Mode 21 响应头 (精确匹配 "61 01")
        char *p41 = strstr(buf, "41 ");
        char *p62 = strstr(buf, "62 ");
        // 保时捷 CAN 广播帧 0x441 (ATH1 监听下形如 "441 D0 D1 ... D7")。仅当前车型用此模式才解析，
        // 且必须先于 p41 判断(因 "441 " 含子串 "41 ")。byte5=油温(x-60°C), byte7=油压(x/25.4 bar)。
        char *p441 = (s_oil_mode_priority[0] == OIL_TEMP_MODE_PORSCHE_CAN_441) ? strstr(buf, "441 ") : NULL;
        // ---- CAN 广播帧解析: 数据驱动 ----
        const vehicle_profile_t *vp_can = vehicle_profile_get_active();
        const vehicle_override_t *ov_can = vehicle_profile_get_override();
        bool has_can_rules = ov_can && ov_can->can_rules && ov_can->can_rule_count > 0;
        // 兼容旧 can_broadcast_mode 标志
        bool ft86_can_mode = (vp_can && vp_can->can_broadcast_mode) || has_can_rules;

        // 收集 override 中所有唯一 CAN ID, 用 strstr 在 buf 中查找
        if (ft86_can_mode) {
            uint16_t seen_ids[8] = {0};
            uint8_t seen_count = 0;
            // 确定要扫描的规则表: override 优先, 否则用旧硬编码
            const can_rule_t *rules = has_can_rules ? ov_can->can_rules : NULL;
            uint8_t rule_count = has_can_rules ? ov_can->can_rule_count : 0;

            // 如果没有 override 规则但 can_broadcast_mode=true, 用旧内联解析 (兼容)
            if (!has_can_rules) {
                // 旧 ZC/N6 硬编码解析 (保留兼容)
                char *p140 = strstr(buf, "140 ");
                if (p140) {
                    unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
                    int vals = sscanf(p140, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
                    if (vals >= 9 && id == 0x140) {
                        uint16_t can_rpm = (uint16_t)(b2 | ((b3 & 0x3F) << 8));
                        s_zc6_can_rpm_seen = true;
                        if (s_cbs.on_parsed_rpm) s_cbs.on_parsed_rpm(can_rpm);
                        uint8_t tps_pct = (uint8_t)((uint32_t)b6 * 100 / 255);
                        if (s_cbs.on_parsed_throttle_position) s_cbs.on_parsed_throttle_position(tps_pct);
                    }
                }
                char *p360 = strstr(buf, "360 ");
                if (p360) {
                    unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
                    int vals = sscanf(p360, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
                    if (vals >= 9 && id == 0x360) {
                        int32_t oil_c = (int32_t)b2 - 40;
                        int32_t clt_c = (int32_t)b3 - 40;
                        if (oil_c >= -40 && oil_c <= 215 && s_cbs.on_parsed_oil_temp)
                            s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
                        if (clt_c >= -40 && clt_c <= 215 && s_cbs.on_parsed_coolant_temp)
                            s_cbs.on_parsed_coolant_temp((uint32_t)clt_c);
                    }
                }
                char *p0d1 = strstr(buf, "0D1 ");
                if (p0d1) {
                    unsigned id=0,b0,b1,b2,b3;
                    int vals = sscanf(p0d1, "%x %x %x %x %x", &id,&b0,&b1,&b2,&b3);
                    if (vals >= 4 && id == 0x0D1) {
                        uint8_t speed_kmh = (uint8_t)(((b0 | (b1 << 8)) * 157 + 5000) / 10000);
                        if (s_cbs.on_parsed_speed_kmh) s_cbs.on_parsed_speed_kmh(speed_kmh);
                    }
                }
                s_last_obd_valid_us = esp_timer_get_time();
                goto can_parse_done;
            }

            // ---- 通用 CAN 规则解析 ----
            // 收集唯一 CAN ID
            for (uint8_t i = 0; i < rule_count; i++) {
                bool found = false;
                for (uint8_t j = 0; j < seen_count; j++) {
                    if (seen_ids[j] == rules[i].can_id) { found = true; break; }
                }
                if (!found && seen_count < 8) seen_ids[seen_count++] = rules[i].can_id;
            }
            // 对每个唯一 CAN ID, 在 buf 中查找并解析 (只认行首匹配, 避免数据字节巧合误命中)
            for (uint8_t si = 0; si < seen_count; si++) {
                const char *p = find_can_id_token(buf, seen_ids[si]);
                if (!p) continue;
                // 解析 hex bytes
                uint8_t data[8] = {0};
                uint32_t parsed_id = 0;
                int vals = sscanf(p, "%x %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
                                  &parsed_id, &data[0],&data[1],&data[2],&data[3],
                                  &data[4],&data[5],&data[6],&data[7]);
                if (vals < 2 || parsed_id != seen_ids[si]) continue;

                // 应用规则
                float channels[CH_COUNT];
                for (int c = 0; c < CH_COUNT; c++) channels[c] = -32768.0f;
                can_apply_rules(rules, rule_count, seen_ids[si], data, channels);

                // 写入 obd_data_cache
                if (channels[CH_RPM] >= 0 && s_cbs.on_parsed_rpm)
                    s_cbs.on_parsed_rpm((uint16_t)channels[CH_RPM]);
                if (channels[CH_SPEED] >= 0 && s_cbs.on_parsed_speed_kmh)
                    s_cbs.on_parsed_speed_kmh((uint8_t)channels[CH_SPEED]);
                if (channels[CH_OIL_TEMP] > -40 && channels[CH_OIL_TEMP] <= 215 && s_cbs.on_parsed_oil_temp)
                    s_cbs.on_parsed_oil_temp((uint32_t)(int16_t)channels[CH_OIL_TEMP]);
                if (channels[CH_COOLANT] > -40 && channels[CH_COOLANT] <= 215 && s_cbs.on_parsed_coolant_temp)
                    s_cbs.on_parsed_coolant_temp((uint32_t)(int16_t)channels[CH_COOLANT]);
                if (channels[CH_TPS] >= 0 && s_cbs.on_parsed_throttle_position)
                    s_cbs.on_parsed_throttle_position((uint32_t)channels[CH_TPS]);
                if (channels[CH_GEAR] > 0 && channels[CH_GEAR] < 127 && s_cbs.on_parsed_gear)
                    s_cbs.on_parsed_gear((int8_t)channels[CH_GEAR]);
                if (channels[CH_LOAD] >= 0 && s_cbs.on_parsed_load_pct)
                    s_cbs.on_parsed_load_pct((int16_t)channels[CH_LOAD]);

                s_last_obd_valid_us = esp_timer_get_time();
                ESP_LOGD(TAG, "[CAN 0x%03X] parsed %d vals", seen_ids[si], vals);
            }
            can_parse_done: ;
        }
        // 收到任一有效数据帧头 → 刷新"有效数据"时间戳
        if (p41 || p62 || p61 || p441) s_last_obd_valid_us = esp_timer_get_time();

        if (p441 != NULL) {
            unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
            int vals = sscanf(p441, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
            if (vals >= 9 && id == 0x441) {
                // byte5: 油温, 公式取自当前车型 profile (°C = x*num/den+off); 不同代系数不同
                const oil_temp_strategy_t *st441 = vehicle_profile_get_oil_temp_strategy();
                int32_t num = st441 ? st441->can_num : 1;
                int32_t den = st441 ? st441->can_den : 1;
                int32_t off = st441 ? st441->can_off : -60;
                if (den == 0) { num = 1; den = 1; off = -60; }  // 未配置→默认 997.2(x-60)
                int32_t oil_c = (int32_t)b5 * num / den + off;
                // 油压: byte/25.4 bar == byte*50/127 (0.1bar). 参考主推 byte7, 部分扫描器在 byte6。
                // 先用 byte7; 同时把 b6/b7 都打日志, 上车对照实际油压(怠速~1-2bar/高转~4-5bar)确认是哪个字节。
                int16_t oilp_x10 = (int16_t)((b7 * 50) / 127);  // byte7: 油压, 0x7F(127)=5.0bar
                s_porsche_441_seen = true;
                if (oil_c >= -40 && oil_c <= 215 && s_cbs.on_parsed_oil_temp) {
                    record_oil_temp_success(OIL_TEMP_MODE_PORSCHE_CAN_441);
                    s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
                } else {
                    record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
                }
                obd_data_set_oil_pressure_x10(oilp_x10);        // 同帧油压 → OILP 显示
                // 诊断: 完整帧 + 各候选字节, 便于核对油压到底在 byte6 还是 byte7
                ESP_LOGD(TAG, "[CAN 441] RAW b0..b7=%02X %02X %02X %02X %02X %02X %02X %02X",
                         b0, b1, b2, b3, b4, b5, b6, b7);
                ESP_LOGI(TAG, "[CAN 441] bytes=8 b5=0x%02X formula=%d*%d/%d%+d -> %dC",
                         b5, b5, (int)num, (int)den, (int)off, (int)oil_c);
            } else {
                ESP_LOGD(TAG, "[CAN 441] parse fail vals=%d", vals);
                record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
            }
        } else if (p61 != NULL && s_expect_mode21) {
            // Mode 21 多帧响应 (Toyota 2101)
            // s_expect_mode21 守卫：只有确认发出了 21 01 命令才解析，
            // 防止其他 PID 响应的数据字节碰巧包含 "61 01" 被误触发（如转速~6208rpm时 41 0C 61 01）
            s_expect_mode21 = false;
            uint32_t d[64] = {0};
            int count = parse_mode21_data(buf, d, 64);
            // 全量 dump: 分两段打印，避免 ESP_LOGI 截断（每字节约11字符，30字节超256字符限制）
            { char _hx[256]; int _o, _h = count / 2;
              _o = 0; for(int _i=0;_i<_h;_i++) _o+=snprintf(_hx+_o,sizeof(_hx)-_o,"[%d]%02X(%d) ",_i,(unsigned)d[_i],(int)d[_i]-40);
              ESP_LOGD(TAG,"[21 01] bytes=%d [0-%d]: %s", count, _h-1, _hx);
              _o = 0; for(int _i=_h;_i<count;_i++) _o+=snprintf(_hx+_o,sizeof(_hx)-_o,"[%d]%02X(%d) ",_i,(unsigned)d[_i],(int)d[_i]-40);
              ESP_LOGD(TAG,"[21 01] [%d-%d]: %s", _h, count-1, _hx); }
            int32_t oil_c = 0;
            if (extract_mode21_oil_temp(d, count, &oil_c)) {
                ESP_LOGI(TAG, "Mode21 oil temp=%dC (idx=%d, bytes=%d)", 
                         (int)oil_c, s_mode21_oil_idx, count);
                record_oil_temp_success(OIL_TEMP_MODE_TOYOTA_21_01);
                // 通过回调统一走平滑+偏移处理
                if (s_cbs.on_parsed_oil_temp) s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
            } else {
                ESP_LOGW(TAG, "21 01 parse failed: count=%d", count);
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
            }
        } else if (p41 != NULL && !s_expect_mode21) {
            // Mode 01 响应: "41 PP DD ..."
            uint32_t d[6] = {0};
            uint32_t mode = 0, pid = 0;
            int values = sscanf(p41, "%x %x %x %x %x %x %x %x",
                &mode, &pid, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]);
            // 油温 PID 0x5C 的原始响应 dump
            if (pid == 0x5C) {
                ESP_LOGI(TAG, "[OIL RX] PID0x5C raw[%d]: %.200s", (int)s_accum_len, buf);
            }
            ESP_LOGD(TAG, "OBD mode01 mode=%02X pid=%02X d=%02X %02X %02X val=%d",
                     mode, pid, d[0], d[1], d[2], values);
            if (values >= 3 && mode == 0x41) {
                int dc = values - 2;

                // 如果正在协议检测，只处理 RPM（0x0C）
                if (s_protocol_detect_idx >= 0 && pid != 0x0C) {
                    break;  // 跳过非目标 PID
                }

                switch (pid) {
                    case 0x04: // 发动机负荷 (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_load_pct && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_load_pct((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x05: // 水温
                        if (dc >= 1 && s_cbs.on_parsed_coolant_temp && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_coolant_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x0C: // 转速
                        if (dc >= 2) {
                            uint16_t rpm_val = (uint16_t)(((d[0] << 8) | d[1]) / 4);

                            if (s_protocol_detect_idx >= 0) {
                                // 协议检测模式
                                s_protocol_detect_rpm = (int32_t)rpm_val;
                                s_protocol_detect_got_response = true;
                                ESP_LOGD(TAG, "[PROTOCOL_DETECT] Protocol %d: RPM=%u OK", s_protocol_detect_idx, rpm_val);
                            } else {
                                // 正常模式
                                if (s_cbs.on_parsed_rpm)
                                    s_cbs.on_parsed_rpm(rpm_val);
                            }
                        }
                        break;
                    case 0x0D: // 车速
                        if (dc >= 1 && s_cbs.on_parsed_speed_kmh && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_speed_kmh((uint8_t)d[0]);
                        break;
                    case 0x0F: // 进气温度
                        if (dc >= 1 && s_cbs.on_parsed_intake_temp && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_intake_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x11: // 节气门开度 TPS (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_throttle_position && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_throttle_position((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x0B: // 进气歧管绝对压力 MAP (kPa) → 涡轮表压
                        if (dc >= 1 && s_cbs.on_parsed_manifold_pressure && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_manifold_pressure((uint32_t)d[0]);
                        break;
                    case 0x5C: // 油温 PID (标准，用于 ZD8)
                        if (dc >= 1 && s_cbs.on_parsed_oil_temp && s_protocol_detect_idx < 0) {
                            int32_t oil_temp = (int32_t)d[0] - 40;
                            // 验证范围：-40 到 215°C
                            if (oil_temp >= -40 && oil_temp <= 215) {
                                ESP_LOGI(TAG, "[PID 0x5C] bytes=%d raw=0x%02X -> %dC", dc, (unsigned)d[0], (int)oil_temp);
                                record_oil_temp_success(OIL_TEMP_MODE_PID_5C);
                                s_cbs.on_parsed_oil_temp((uint32_t)oil_temp);
                            } else {
                                ESP_LOGD(TAG, "[PID 0x5C] Oil temp out of range: %d (raw=%02X)", (int)oil_temp, d[0]);
                                record_oil_temp_failure(OIL_TEMP_MODE_PID_5C);
                            }
                        }
                        break;
                    case 0x42: // 电池电压 (mV)
                        if (dc >= 2 && s_cbs.on_parsed_control_module_voltage && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_control_module_voltage((d[0] << 8) | d[1]);
                        break;
                    case 0x44: // 空燃比 AFR - Commanded Equivalence Ratio (λ)
                        // λ = (A*256+B)/32768, 范围 0~<2
                        // AFR = λ × 14.7, 存储 ×100: 1470 = 14.70:1
                        if (dc >= 2 && s_cbs.on_parsed_afr && s_protocol_detect_idx < 0) {
                            uint32_t raw = (d[0] << 8) | d[1];
                            // λ = raw / 32768, AFR×100 = λ × 1470
                            uint32_t afr_x100 = (raw * 1470UL) / 32768UL;
                            if (afr_x100 >= 800 && afr_x100 <= 2200) {
                                s_cbs.on_parsed_afr(afr_x100);
                            }
                        }
                        break;
                    default:
                        ESP_LOGD(TAG, "Unhandled PID 0x%02X", pid);
                        break;
                }
            }
        } else if (p62 != NULL) {
            // Mode 22 响应: "62 HH LL D0 D1 ..."  (d0=A, d1=B)
            // 如果期望 Mode21 但收到 Mode22，清除期望标志并记录失败
            if (s_expect_mode21) {
                ESP_LOGW(TAG, "21 01 expected but got Mode22 response");
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
                s_expect_mode21 = false;
            }
            uint32_t mode22 = 0, ph = 0, pl = 0, d0 = 0, d1 = 0;
            int values = sscanf(p62, "%x %x %x %x %x", &mode22, &ph, &pl, &d0, &d1);
            if (values >= 4 && mode22 == 0x62 && s_cbs.on_parsed_oil_temp) {
                uint32_t pid16 = (ph << 8) | pl;

                // ---- 数据驱动 override 解析 ----
                if (s_oil_use_override) {
                    const oil_formula_t *oil_f = (s_oil_override_idx == 0) ? s_oil_formula_pri : s_oil_formula_sec;
                    if (oil_f && oil_f->type == OIL_UDS_22) {
                        uint32_t expect_pid = (oil_f->pid[0] << 8) | oil_f->pid[1];
                        if (pid16 == expect_pid) {
                            uint32_t resp_data[4] = {d0, d1, 0, 0};
                            int16_t temp = oil_formula_parse_resp(oil_f, resp_data, (uint8_t)(values - 3));
                            if (temp != -32768) {
                                ESP_LOGI(TAG, "[Override 22 %02X%02X] -> %dC", oil_f->pid[0], oil_f->pid[1], temp);
                                s_oil_override_fail = 0;
                                record_oil_temp_success(s_oil_mode_priority[0]);
                                s_cbs.on_parsed_oil_temp((uint32_t)temp);
                            } else {
                                s_oil_override_fail++;
                                record_oil_temp_failure(s_oil_mode_priority[0]);
                                if (s_oil_override_fail >= OIL_OVERRIDE_FAIL_MAX && s_oil_formula_sec) {
                                    s_oil_override_idx = 1;
                                    s_oil_override_fail = 0;
                                    ESP_LOGW(TAG, "[Override] primary failed %d times, switch to secondary", OIL_OVERRIDE_FAIL_MAX);
                                }
                            }
                            goto oil_temp_done;
                        }
                    }
                }

                // ---- 旧 PID switch/case (无 override 或 PID 不匹配时) ----
                if (pid16 == 0x1310) {
                    // Mazda Skyactiv 油温 PID 1310: (A*256+B)/100 - 40 (°C), 需 2 个数据字节
                    if (values >= 5) {
                        int32_t mazda_oil = (int32_t)(((d0 * 256) + d1) / 100) - 40;
                        if (mazda_oil >= -40 && mazda_oil <= 215) {
                            ESP_LOGI(TAG, "[22 13 10] bytes=%d raw=(%02X,%02X) -> %dC", values-2, (unsigned)d0, (unsigned)d1, (int)mazda_oil);
                            record_oil_temp_success(OIL_TEMP_MODE_MAZDA_22_1310);
                            s_cbs.on_parsed_oil_temp((uint32_t)mazda_oil);
                        } else {
                            ESP_LOGD(TAG, "[22 13 10] Oil temp out of range: %d (A=%02X B=%02X)", (int)mazda_oil, (unsigned)d0, (unsigned)d1);
                            record_oil_temp_failure(OIL_TEMP_MODE_MAZDA_22_1310);
                        }
                    } else {
                        record_oil_temp_failure(OIL_TEMP_MODE_MAZDA_22_1310);
                    }
                } else if (pid16 == 0x111F) {
                    // PID 111F: °C = A - 50 (Mazda Skyactiv 和 BMW 均用此公式)
                    // 根据当前策略决定记录哪个 mode 的统计
                    bool is_bmw_111f = (s_oil_mode_priority[0] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[1] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[2] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[3] == OIL_TEMP_MODE_BMW_22_111F);
                    oil_temp_query_mode_t mode_111f = is_bmw_111f ? OIL_TEMP_MODE_BMW_22_111F
                                                                  : OIL_TEMP_MODE_MAZDA_22_111F;
                    int32_t oil_111f = (int32_t)d0 - 50;
                    if (oil_111f >= -40 && oil_111f <= 215) {
                        ESP_LOGI(TAG, "[22 11 1F] bytes=%d raw=0x%02X -> %dC (%s)", values-2, (unsigned)d0, (int)oil_111f,
                                 is_bmw_111f ? "BMW" : "Mazda");
                        record_oil_temp_success(mode_111f);
                        s_cbs.on_parsed_oil_temp((uint32_t)oil_111f);
                    } else {
                        ESP_LOGD(TAG, "[22 11 1F] Oil temp out of range: %d (raw=%02X)", (int)oil_111f, (unsigned)d0);
                        record_oil_temp_failure(mode_111f);
                    }
                } else if (pid16 == 0x5822) {
                    // MINI/BMW 油温 PID 5822: °C = A - 60 (°F = A*9/5 - 76)
                    int32_t mini_oil = (int32_t)d0 - 60;
                    if (mini_oil >= -40 && mini_oil <= 215) {
                        ESP_LOGI(TAG, "[22 58 22] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)mini_oil);
                        record_oil_temp_success(OIL_TEMP_MODE_MINI_22_5822);
                        s_cbs.on_parsed_oil_temp((uint32_t)mini_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 58 22] Oil temp out of range: %d (raw=%02X)", (int)mini_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_MINI_22_5822);
                    }
                } else if (pid16 == 0x4402) {
                    if (s_oil_mode_priority[0] == OIL_TEMP_MODE_BMW_G_22_4402 ||
                        s_oil_mode_priority[1] == OIL_TEMP_MODE_BMW_G_22_4402 ||
                        s_oil_mode_priority[2] == OIL_TEMP_MODE_BMW_G_22_4402) {
                        // BMW G系油温 PID 4402: °C = (A*256+B)*191.25/255-48 (双字节)
                        if (values >= 5) {
                            int32_t raw = (int32_t)d0 * 256 + (int32_t)d1;
                            int32_t bmw_g_oil = (int32_t)(raw * 191.25f / 255.0f - 48.0f);
                            if (bmw_g_oil >= -48 && bmw_g_oil <= 143) {
                                ESP_LOGI(TAG, "[22 44 02 G] bytes=%d raw=%04X -> %dC", values-2, (unsigned)raw, (int)bmw_g_oil);
                                record_oil_temp_success(OIL_TEMP_MODE_BMW_G_22_4402);
                                s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                            } else {
                                ESP_LOGD(TAG, "[22 44 02 G] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_g_oil, (unsigned)d0, (unsigned)d1);
                                record_oil_temp_failure(OIL_TEMP_MODE_BMW_G_22_4402);
                            }
                        } else {
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_G_22_4402);
                        }
                    } else {
                        // BMW F系油温 PID 4402: °C = B - 64 (响应第二个数据字节 d1)
                        if (values >= 5) {
                            int32_t bmw_oil = (int32_t)d1 - 64;
                            if (bmw_oil >= -40 && bmw_oil <= 215) {
                                ESP_LOGI(TAG, "[22 44 02 F] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d1, (int)bmw_oil);
                                record_oil_temp_success(OIL_TEMP_MODE_BMW_22_4402);
                                s_cbs.on_parsed_oil_temp((uint32_t)bmw_oil);
                            } else {
                                ESP_LOGD(TAG, "[22 44 02 F] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_oil, (unsigned)d0, (unsigned)d1);
                                record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                            }
                        } else {
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                        }
                    }
                } else if (pid16 == 0xD002) {
                    // BMW G系油底壳油温 PID D002: °C = (A*256+B)*191.25/255-48 (双字节)
                    if (values >= 5) {
                        int32_t raw = (int32_t)d0 * 256 + (int32_t)d1;
                        int32_t bmw_g_oil = (int32_t)(raw * 191.25f / 255.0f - 48.0f);
                        if (bmw_g_oil >= -48 && bmw_g_oil <= 143) {
                            ESP_LOGI(TAG, "[22 D0 02] bytes=%d raw=%04X -> %dC", values-2, (unsigned)raw, (int)bmw_g_oil);
                            record_oil_temp_success(OIL_TEMP_MODE_BMW_22_D002);
                            s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                        } else {
                            ESP_LOGD(TAG, "[22 D0 02] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_g_oil, (unsigned)d0, (unsigned)d1);
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_D002);
                        }
                    } else {
                        record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_D002);
                    }
                } else if (pid16 == 0x03F3) {
                    // BMW G系油温 PID 03F3: °C = A - 40 (Header 7E0 物理寻址)
                    int32_t bmw_g_oil = (int32_t)d0 - 40;
                    if (bmw_g_oil >= -40 && bmw_g_oil <= 215) {
                        ESP_LOGI(TAG, "[22 03 F3] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)bmw_g_oil);
                        record_oil_temp_success(OIL_TEMP_MODE_BMW_22_03F3);
                        s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 03 F3] Oil temp out of range: %d (raw=%02X)", (int)bmw_g_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_03F3);
                    }
                } else if (pid16 == 0x1017 || pid16 == 0x0011 || pid16 == 0x1C00) {
                    ESP_LOGI(TAG, "[22 10 17] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)d0 - 40);
                    record_oil_temp_success(OIL_TEMP_MODE_UDS_22_10_17);
                    s_cbs.on_parsed_oil_temp((uint32_t)((int32_t)d0 - 40));
                } else {
                    record_oil_temp_failure(OIL_TEMP_MODE_UDS_22_10_17);
                }
            }
            oil_temp_done: ;
        } else {
            // 无效数据或纯文本（NO DATA、SEARCHING、OK 等）
            if (strstr(buf, "NO DATA")) {
                ESP_LOGD(TAG, "NO DATA for last PID"); // 诊断: 哪个PID无数据(超时由时间戳自愈处理)
            } else if (strstr(buf, "SEARCHING")) {
                ESP_LOGD(TAG, "ELM327 searching protocol...");
            } else {
                ESP_LOGD(TAG, "Other response: %.60s", buf); // 诊断: 其他未知响应
            }
            // 如果期望 Mode21 但收到无关响应，也记录失败
            if (s_expect_mode21) {
                ESP_LOGW(TAG, "21 01 expected but got: %.40s", buf);
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
                s_expect_mode21 = false;
            }
        }

        // 收到完整响应后清空累积缓冲区
        s_accum_len = 0;
        s_accum_buf[0] = '\0';
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Write failed status=%d", param->write.status);
            s_elm_ready = true; // 写失败时也要释放，防止轮询任务永久卡住
        }
        break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
        s_connected = false;
        s_notify_ready = false;   // 断开 → 通知失效, 重连后须重新订阅+重新初始化
        s_conn_id = 0xFFFF;
        s_have_service = false;
        s_service_start = 0x0001;
        s_service_end = 0xFFFF;
        s_all_attr_end = 0xFFFF;
        s_have_18f0 = false;
        s_18f0_start = s_18f0_end = 0;
        s_have_ff12 = false;
        s_ff12_start = s_ff12_end = 0;
        s_write_type = ESP_GATT_WRITE_TYPE_RSP; // 断开后重置写类型
        s_expect_mode21 = false;
        s_char_write_handle = s_char_notify_handle = s_cccd_handle = 0;
        s_accum_len = 0; s_accum_buf[0] = '\0'; // 清空响应累积缓冲区
        s_zc6_can_monitor_active = false;        // 重置 CAN 持续监听
        s_zc6_can_monitor_len = 0;
        s_last_mode21_oil = -100;
        s_mode21_hold_cnt = 0;
        s_protocol_detect_idx = -1;  // 清理协议检测状态
        s_protocol_detect_got_response = false;
        s_protocol_detect_rpm = -1;
        s_oil_query_mode = 0;  // 重置油温查询计数
        if (s_cbs.on_disconnected) s_cbs.on_disconnected();
        // 断开后总是重新扫描:
        //  - 扫描模式: 继续列设备
        //  - 正常模式: 自动重连目标(掉线/自愈强制断开后自动回连, 无需人工重连)
        start_scan();
        break;
    }
    default:
        break;
    }
}


void elm327_ble_start_default(const char *target_name, const uint8_t mac[6]) {

    const elm327_ble_callbacks_t cbs = {
        .on_connected = default_on_connected,
        .on_disconnected = default_on_disconnected,
        .on_raw_notify = default_on_raw_notify,
        .on_parsed_rpm = default_on_parsed_rpm,
        .on_parsed_speed_kmh = default_on_parsed_speed,
        .on_parsed_coolant_temp = default_on_parsed_coolant_temp,
        .on_parsed_intake_temp = default_on_parsed_intake_temp,
        .on_parsed_oil_temp = default_on_parsed_oil_temp,
        .on_parsed_load_pct = default_on_parsed_load_pct,
        .on_parsed_control_module_voltage = default_on_parsed_control_module_voltage,
        .on_parsed_throttle_position = default_on_parsed_throttle_position,
        .on_parsed_gear = default_on_parsed_gear,
        .on_parsed_manifold_pressure = default_on_parsed_manifold_pressure,
        .on_parsed_afr = default_on_parsed_afr,
    };
    s_scan_only_mode = false;
    bool mac_set = mac && (mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) != 0;
    if (mac_set) {
        memcpy(s_target_bda, mac, sizeof(esp_bd_addr_t));
        s_target_bda_valid = true;
    } else {
        s_target_bda_valid = false;
    }
    elm327_ble_init_and_start(target_name, &cbs);
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 4096, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

// ---- 扫描模式实现 ----

static void ble_ensure_init(void) {
    if (s_ble_inited) return;
    // 初始化 BLE 协议栈（不设目标名，仅初始化）
    elm327_ble_init_and_start(NULL, NULL);
}

// 供其他仅需要蓝牙外围广播(RaceChrono DIY / SkyGauge 配对)、暂不需要连接 OBD 设备的场景调用:
// 幂等地把控制器+Bluedroid+GAP/GATTC 回调启起来，不发起任何 ELM327 扫描/连接。
void elm327_ble_ensure_stack_init(void) {
    ble_ensure_init();
}

void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb) {
    ble_ensure_init();
    s_scan_only_mode = true;
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    ESP_LOGD(TAG, "Starting scan-only mode (%ds)...", duration_s);
    esp_ble_gap_start_scanning(duration_s);
}

void elm327_ble_scan_only_stop(void) {
    esp_ble_gap_stop_scanning();
    s_scan_only_mode = false;
    ESP_LOGD(TAG, "Scan-only stopped. Found %d devices.", s_scan_count);
}

void elm327_ble_connect_by_addr(const uint8_t mac[6], const char *name) {
    if (!mac) return;
    ESP_LOGD(TAG, "Connect by addr: %02X:%02X:%02X:%02X:%02X:%02X (%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name ? name : "");
    s_scan_only_mode = false;
    if (name && name[0]) {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    }
    memcpy(s_target_bda, mac, sizeof(esp_bd_addr_t));
    s_target_bda_valid = true;

    // 设置默认回调（如果还没有）
    if (!s_cbs.on_connected) {
        s_cbs.on_connected = default_on_connected;
        s_cbs.on_disconnected = default_on_disconnected;
        s_cbs.on_raw_notify = default_on_raw_notify;
        s_cbs.on_parsed_rpm = default_on_parsed_rpm;
        s_cbs.on_parsed_speed_kmh = default_on_parsed_speed;
        s_cbs.on_parsed_coolant_temp = default_on_parsed_coolant_temp;
        s_cbs.on_parsed_intake_temp = default_on_parsed_intake_temp;
        s_cbs.on_parsed_oil_temp = default_on_parsed_oil_temp;
        s_cbs.on_parsed_load_pct = default_on_parsed_load_pct;
        s_cbs.on_parsed_control_module_voltage = default_on_parsed_control_module_voltage;
        s_cbs.on_parsed_throttle_position = default_on_parsed_throttle_position;
        s_cbs.on_parsed_manifold_pressure = default_on_parsed_manifold_pressure;
        s_cbs.on_parsed_afr = default_on_parsed_afr;
    }
    // 开始扫描，找到后自动连接
    esp_ble_gap_start_scanning(15);
    // 创建轮询任务（如果还没有）
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 4096, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

bool elm327_ble_is_connected(void) {
    return s_connected;
}

void elm327_ble_disconnect(void) {
    if (s_connected && s_gattc_if != 0 && s_conn_id != 0xFFFF) {
        ESP_LOGD(TAG, "Disconnecting from BLE device...");
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }
}

const char *elm327_ble_get_connected_name(void) {
    return s_target_name;
}

// ---- 油温校准接口实现 ----
void elm327_oil_temp_set_offset(int8_t offset_c) {
    s_oil_temp_offset = offset_c;
    ESP_LOGI(TAG, "OIL temp offset set to %d°C", offset_c);
}

int8_t elm327_oil_temp_get_offset(void) {
    return s_oil_temp_offset;
}

void elm327_oil_temp_get_diag(elm327_oil_diag_t *out) {
    if (!out) return;
    out->mode0_ok = s_oil_diag.mode0_ok;
    out->mode1_ok = s_oil_diag.mode1_ok;
    out->mode2_ok = s_oil_diag.mode2_ok;
    out->mode0_fail = s_oil_diag.mode0_fail;
    out->mode1_fail = s_oil_diag.mode1_fail;
    out->mode2_fail = s_oil_diag.mode2_fail;
    out->last_raw = s_oil_diag.last_raw_temp;
    out->last_filtered = s_oil_diag.last_filtered_temp;
    out->current_mode = s_oil_query_mode;
    
    ESP_LOGI(TAG, "OIL DIAG: Mode0(01 5C)=%u/%u, Mode1(22 10 17)=%u/%u, Mode2(21 01)=%u/%u, Mode3(22 11 1F Mz)=%u/%u, Mode4(22 13 10)=%u/%u, Mode5(CAN 441)=%u/%u, Mode6(22 58 22)=%u/%u, Mode7(22 44 02 F)=%u/%u, Mode8(22 03 F3)=%u/%u, Mode9(22 44 02 G)=%u/%u, Mode10(22 D0 02)=%u/%u, Mode11(22 11 1F BM)=%u/%u",
             out->mode0_ok, out->mode0_fail, out->mode1_ok, out->mode1_fail,
             out->mode2_ok, out->mode2_fail, s_oil_diag.mode3_ok, s_oil_diag.mode3_fail,
             s_oil_diag.mode4_ok, s_oil_diag.mode4_fail, s_oil_diag.mode5_ok, s_oil_diag.mode5_fail,
             s_oil_diag.mode6_ok, s_oil_diag.mode6_fail, s_oil_diag.mode7_ok, s_oil_diag.mode7_fail,
             s_oil_diag.mode8_ok, s_oil_diag.mode8_fail,
             s_oil_diag.mode9_ok, s_oil_diag.mode9_fail,
             s_oil_diag.mode10_ok, s_oil_diag.mode10_fail,
             s_oil_diag.mode11_ok, s_oil_diag.mode11_fail);
}

