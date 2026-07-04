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
#include "racechrono_ble_diy.h"
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

static esp_gatt_if_t s_gattc_if = 0;
static uint16_t s_conn_id = 0xFFFF;
static esp_bd_addr_t s_peer_bda = {0};
static bool s_connected = false;
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

// ---- 扫描模式相关 ----
static bool s_scan_only_mode = false;  // true=仅扫描不连接
static ble_scan_found_cb_t s_scan_cb = NULL;
static ble_scan_result_t s_scan_list[BLE_SCAN_MAX_DEVICES];
static int s_scan_count = 0;
static bool s_ble_inited = false;  // BLE 协议栈是否已初始化
static bool s_poll_task_started = false; // 轮询任务是否已创建
static volatile bool s_notify_ready = false;   // CCCD 通知订阅完成才置位; init/轮询据此放行(防握手响应丢失)
static volatile int64_t s_last_obd_valid_us = 0; // 最近一次收到有效OBD数据的时间; 超时无数据触发自愈重初始化
static uint8_t s_oil_query_mode = 0;     // 当前查询模式索引 (0-2)
static int s_mode21_oil_idx = 33;        // Mode21 机油温字节索引，自适应更新
static int16_t s_last_mode21_oil = -100; // 上次Mode21解析出的油温

// ---- 基于车型的油温查询策略 ----
static oil_temp_query_mode_t s_oil_mode_priority[4] = {
    OIL_TEMP_MODE_PID_5C,
    OIL_TEMP_MODE_UDS_22_10_17,
    OIL_TEMP_MODE_TOYOTA_21_01,
};  // 默认优先级，启动后从车型配置更新
static uint32_t s_oil_mode_fail_count[8] = {0};  // 每个模式(poll idx 0~7)的连续失败次数
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
    uint32_t mode0_fail;
    uint32_t mode1_fail;
    uint32_t mode2_fail;
    uint32_t mode3_fail;
    uint32_t mode4_fail;
    uint32_t mode5_fail;
    uint32_t mode6_fail;
    uint32_t mode7_fail;
    int16_t last_raw_temp; // 原始温度（未过滤）
    int16_t last_filtered_temp; // 过滤后温度
} s_oil_diag = {0};

static int8_t s_oil_temp_offset = 0;  // 用户校准偏移量，单位 °C

// 增加全局 ready 标志
static volatile bool s_elm_ready = true; // 初始允许发送首条 ATZ
static volatile bool s_expect_mode21 = false; // true=上条命令是 21 01，等待 61 01 响应
static volatile bool s_porsche_441_seen = false; // 本次监听是否成功解析到 0x441 帧
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
    // ZC6/ZD8 的 secondary/tertiary 已置 NONE → 仍是单一固定模式(行为不变)；
    // MX-5 = 1310 → 回退 111F，两种 Mazda 油温 PID 都覆盖。
    const oil_temp_strategy_t *st = vehicle_profile_get_oil_temp_strategy();
    s_oil_mode_priority[0] = st ? st->primary   : OIL_TEMP_MODE_PID_5C;
    s_oil_mode_priority[1] = st ? st->secondary : OIL_TEMP_MODE_NONE;
    s_oil_mode_priority[2] = st ? st->tertiary  : OIL_TEMP_MODE_NONE;
    if (s_oil_mode_priority[0] == OIL_TEMP_MODE_TOYOTA_21_01) {
        s_mode21_oil_idx = 33; // ZC6 默认固定索引
    }

    // 重置失败计数
    memset(s_oil_mode_fail_count, 0, sizeof(s_oil_mode_fail_count));
    s_oil_query_mode = 0;
    s_vehicle_profile_inited = true;
    
    const vehicle_profile_t *profile = vehicle_profile_get_active();
    ESP_LOGI(TAG, "Oil temp strategy fixed for [%s]: Primary=%d",
             profile ? profile->name : "UNKNOWN", s_oil_mode_priority[0]);
}

// 根据当前策略选择下一个查询模式
static oil_temp_query_mode_t get_next_oil_query_mode(uint8_t *poll_idx) {
    if (!s_vehicle_profile_inited) {
        init_oil_temp_strategy();
    }
    
    // 遍历优先级列表，找到首个有效且未过度失败的模式
    for (int i = 0; i < 3; i++) {
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
        default:
            break;
    }
    ESP_LOGD(TAG, "Oil temp query FAILED for mode %u (fail_count now %u)", mode, s_oil_mode_fail_count[idx]);
}

// 默认回调与轮询任务（可选）
static void default_on_connected(void) { ESP_LOGI(TAG, "OBD BLE connected"); }
static void default_on_disconnected(void) { ESP_LOGI(TAG, "OBD BLE disconnected"); }
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
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '>') { s_elm_ready = true; break; }
    }
}

// ---- 协议自动检测函数 ----
// 尝试所有协议（1-11），通过发送 01 0C （读 RPM）来判断协议是否有效
static int elm327_auto_detect_protocol(void) {
    ESP_LOGI(TAG, "=== Starting protocol auto-detect ===");
    
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
        ESP_LOGI(TAG, "[DETECT] Trying protocol %d...", proto);
        
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
        ESP_LOGI(TAG, "[DETECT] Sent 01 0C, waiting...");
        esp_log_level_set(TAG, prev_level);
        
        // 等待响应最多 2 秒
        uint32_t wait_ms = 0;
        while (wait_ms < 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
            
            if (s_protocol_detect_got_response) {
                // 成功！
                s_protocol_detect_idx = -1;  // 结束检测模式
                ESP_LOGI(TAG, "[DETECT] Protocol %d: SUCCESS! (RPM=%ld)", proto, s_protocol_detect_rpm);
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
static void default_on_parsed_speed(uint8_t kmh) { ESP_LOGD(TAG, "SPEED: %u km/h", kmh); obd_data_set_speed(kmh); }
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

static void default_on_parsed_oil_temp(uint32_t oil_temp)
{
    static int16_t s_oil_filtered = -100;
    static int16_t s_oil_pending = -100;
    static uint8_t s_oil_pending_cnt = 0;
    static uint32_t s_total_updates = 0;
    
    s_total_updates++;
    int16_t in = (int16_t)oil_temp;
    s_oil_diag.last_raw_temp = in;
    
    // 基础范围检查：-20 到 150°C
    if (in < -20 || in > 150) {
        ESP_LOGW(TAG, "OIL: Out of range raw=%d", in);
        return;
    }

    // ---- 改进的平滑算法 ----
    // 1. 初始化阶段：直接接受前 3 个有效值
    if (s_oil_filtered <= -40) {
        s_oil_filtered = in;
        s_oil_pending = in;
        s_oil_pending_cnt = 1;
        s_oil_diag.last_filtered_temp = in;
        ESP_LOGI(TAG, "OIL: Init with raw=%d", in);
        obd_data_set_oil_temp_with_offset(s_oil_filtered);
        return;
    }

    int diff = abs((int)in - (int)s_oil_filtered);

    // 2. 小变化（<=5°C）：快速响应，直接采纳
    if (diff <= 5) {
        s_oil_pending = -100;
        s_oil_pending_cnt = 0;
        // 加权平均：65% 历史 + 35% 新值（响应更快）
        s_oil_filtered = (int16_t)((65 * s_oil_filtered + 35 * in) / 100);
        s_oil_diag.last_filtered_temp = s_oil_filtered;
        ESP_LOGD(TAG, "OIL: Small change raw=%d filtered=%d", in, s_oil_filtered);
        obd_data_set_oil_temp_with_offset(s_oil_filtered);
        return;
    }

    // 3. 中等变化（5~15°C）：需要 2 次确认
    if (diff <= 15) {
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            ESP_LOGD(TAG, "OIL: Medium spike first occurrence raw=%d, need confirmation", in);
            return;
        }
        
        if (s_oil_pending_cnt >= 2) {
            // 确认无误，采纳
            s_oil_filtered = (int16_t)((50 * s_oil_filtered + 50 * in) / 100);
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            s_oil_diag.last_filtered_temp = s_oil_filtered;
            ESP_LOGI(TAG, "OIL: Medium change confirmed raw=%d filtered=%d", in, s_oil_filtered);
            obd_data_set_oil_temp_with_offset(s_oil_filtered);
            return;
        }
        
        // 暂时不处理
        ESP_LOGD(TAG, "OIL: Medium spike pending (%u/%d)", s_oil_pending_cnt, 2);
        return;
    }

    // ---- 4. 大变化（>15°C）----
    ESP_LOGW(TAG, "OIL: Large spike filtered=%d raw=%d (Δ=%d)", s_oil_filtered, in, diff);
    
    if (diff >= 20) {
        // 需要 3 次确认来信任大跳变
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            return;
        }
        
        if (s_oil_pending_cnt >= 3) {
            s_oil_filtered = in;  // 大跳变直接采纳
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            s_oil_diag.last_filtered_temp = s_oil_filtered;
            ESP_LOGI(TAG, "OIL: Large change ACCEPTED raw=%d filtered=%d (confirmed 3x)", in, s_oil_filtered);
            obd_data_set_oil_temp_with_offset(s_oil_filtered);
        }
    }
}
static void default_on_parsed_load_pct(uint32_t load_pct) { ESP_LOGD(TAG, "LOAD: %u%%", load_pct); obd_data_set_load_pct((int16_t)load_pct); }
static void default_on_parsed_control_module_voltage(uint32_t bat_mv) { ESP_LOGD(TAG, "BAT: %u.%uV", bat_mv/1000, (bat_mv%1000)/100); obd_data_set_bat_mv((int32_t)bat_mv); }
static void default_on_parsed_throttle_position(uint32_t tps_pct) { ESP_LOGD(TAG, "TPS: %u%%", tps_pct); obd_data_set_tps((int16_t)tps_pct); }
// MAP(kPa) → 涡轮表压(0.1bar)：表压 = (MAP - 大气≈100kPa)，10kPa = 0.1bar
static void default_on_parsed_manifold_pressure(uint32_t map_kpa) {
    int16_t boost_x10 = (int16_t)(((int32_t)map_kpa - 100) / 10);
    if (boost_x10 < 0) boost_x10 = 0; // 不显示负压(真空)，从0起
    ESP_LOGD(TAG, "MAP: %u kPa -> boost %d.%d bar", map_kpa, boost_x10/10, boost_x10%10);
    obd_data_set_boost_x10(boost_x10);
}

// 保时捷 997.2/987.2：油温油压在 CAN 广播帧 0x441，需用 ELM327 监听模式抓取。
// 流程：过滤只收 441 → 开帧头 → 监听若干帧 → 停止 → 还原(关帧头/恢复自动收地址)。
// 帧解析在通知回调里(见 "441 " 分支)；本函数只负责按时序下发监听指令。
// 注意：监听模式与普通请求/应答不同，且依赖适配器(廉价克隆可能不支持 ATMA/ATCRA)。
static void porsche_read_can_441(void) {
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
        ESP_LOGI(TAG, "[441] frame captured & parsed OK");
    } else {
        ESP_LOGW(TAG, "[441] NO 441 frame in window (check FULL[] log above: ATMA returned what?)");
        record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
    }
    // 还原：关帧头、恢复自动接收地址(否则后续普通 PID 响应被过滤/错解析)
    elm327_ble_send_ascii_blocking("AT H0\r");
    elm327_ble_send_ascii_blocking("AT AR\r");
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
        ESP_LOGI(TAG, "Vehicle '%s' forces protocol %d (skip auto-detect)", vp_proto->name, protocol_to_use);
    } else if (protocol_to_use == 0) {
        // 自动协议检测
        ESP_LOGI(TAG, "Protocol auto-detect enabled (current NVS: 0-auto)");
        int detected_proto = elm327_auto_detect_protocol();
        if (detected_proto > 0) {
            protocol_to_use = (uint8_t)detected_proto;
            nvs_user_cfg_t new_cfg = *cfg;
            new_cfg.protocol = protocol_to_use;
            nvs_cfg_set(&new_cfg);
            ESP_LOGI(TAG, "Protocol auto-detect SUCCESS! Saving protocol %d to NVS", protocol_to_use);
        } else {
            protocol_to_use = 6;
            ESP_LOGW(TAG, "Protocol auto-detect FAILED, using fallback protocol 6");
        }
    }

    snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", protocol_to_use);
    const char *fixed_header_cmd = get_vehicle_fixed_header_cmd();
    const char *init_cmds[] = {
        "ATZ\r", "ATE0\r", "ATL0\r", "ATS1\r", "ATH0\r", "ATAT1\r", "ATST 19\r",
        atsp_cmd, fixed_header_cmd,
    };
    for (size_t i = 0; i < (sizeof(init_cmds) / sizeof(init_cmds[0])); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        ESP_LOGI(TAG, " AT init Cmd send %s", init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    // 总线预热: 多发几次 01 00, 给整车 CAN/ELM 协议握手时间(冷启动总线可能还没醒)
    for (int probe = 0; probe < 3; ++probe) {
        elm327_ble_send_ascii_blocking("01 00\r");
        ESP_LOGI(TAG, " CMD 01 00 probe #%d", probe);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // ---- 初始化油温查询策略（基于车型配置） ----
    init_oil_temp_strategy();
    const vehicle_profile_t *active_profile = vehicle_profile_get_active();
    ESP_LOGI(TAG, "Active vehicle profile: %s", active_profile ? active_profile->name : "Unknown");
    s_last_obd_valid_us = esp_timer_get_time();   // 给一个新的"有效数据"起点, 避免刚初始化就触发自愈
}

static void obd_poll_task(void *arg) {
    uint32_t tick_count = 0;
    bool inited = false;
    uint8_t heal_attempts = 0;   // 连续自愈次数; 重发ATZ若干次仍无数据则升级为强制重连

    // 8-slot 轮询: 0=RPM, 1=IAT, 2=Speed, 3=CLT, 4=Load(0x04), 5=TPS(0x11), 6=OIL(车型策略), 7=BAT(0x42)
    while (1)
    {
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
        if ((esp_timer_get_time() - s_last_obd_valid_us) < 2000000) heal_attempts = 0;
        // 自愈: 连续 >5s 收不到任何有效数据(没响应/SEARCHING/UNABLE TO CONNECT/NO DATA 全覆盖)。
        if ((esp_timer_get_time() - s_last_obd_valid_us) > 5000000) {
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
            case 6: // 机油温自动查询（基于车型策略）
                {
                    uint8_t poll_idx = 0;
                    oil_temp_query_mode_t mode = get_next_oil_query_mode(&poll_idx);
                    
                    if (mode == OIL_TEMP_MODE_PID_5C) {
                        elm327_ble_send_ascii_blocking("01 5C\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 01 5C (Standard oil temp PID)");
                    } else if (mode == OIL_TEMP_MODE_UDS_22_10_17) {
                        elm327_ble_send_ascii_blocking("22 10 17\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 22 10 17 (UDS oil temp)");
                    } else if (mode == OIL_TEMP_MODE_TOYOTA_21_01) {
                        elm327_ble_send_ascii_blocking("21 01\r");
                        s_expect_mode21 = true;
                        ESP_LOGI(TAG, "[Slot6] Send 21 01 (Toyota oil temp)");
                    } else if (mode == OIL_TEMP_MODE_MAZDA_22_111F) {
                        elm327_ble_send_ascii_blocking("22 11 1F\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 22 11 1F (Mazda oil temp)");
                    } else if (mode == OIL_TEMP_MODE_MAZDA_22_1310) {
                        elm327_ble_send_ascii_blocking("22 13 10\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 22 13 10 (Mazda oil temp)");
                    } else if (mode == OIL_TEMP_MODE_PORSCHE_CAN_441) {
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Porsche CAN 0x441 monitor (oil temp+press)");
                        porsche_read_can_441();
                    } else if (mode == OIL_TEMP_MODE_MINI_22_5822) {
                        elm327_ble_send_ascii_blocking("22 58 22\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 22 58 22 (MINI/BMW oil temp)");
                    } else if (mode == OIL_TEMP_MODE_BMW_22_4402) {
                        elm327_ble_send_ascii_blocking("22 44 02\r");
                        s_expect_mode21 = false;
                        ESP_LOGI(TAG, "[Slot6] Send 22 44 02 (BMW F-series oil temp)");
                    } else {
                        ESP_LOGW(TAG, "[Slot6] No valid oil temp mode available");
                        s_expect_mode21 = false;
                    }
                    s_oil_query_mode = poll_idx;
                }
                break;
            case 2://车速
                elm327_ble_send_ascii_blocking("01 0D\r");
                ESP_LOGD(TAG, "Send 01 0D");
                break;
            case 3://冷却液温度
                elm327_ble_send_ascii_blocking("01 05\r");
                ESP_LOGD(TAG, "Send 01 05");
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
            default:
                break;
        }

        tick_count++;
        if(tick_count >= 9)
        {
            tick_count = 0;
        }

        // 槽间空等: 原为 100ms(纯空转, 加在"等响应"之上)。调小可显著提高刷新率,
        // 仍轮询全部 PID → 不影响三连表主表广播。太小会压垮廉价克隆头, 30ms 为稳妥折中。
        vTaskDelay(pdMS_TO_TICKS(OBD_POLL_SLOT_GAP_MS));
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

// 从 Mode21 数据中提取机油温字节（以 BRZ ZC6 为参考）
// BRZ ZC6 通常在固定位置，但支持自适应搜索
static bool extract_mode21_oil_temp(const uint32_t *d, int count, int32_t *oil_c) {
    if (!d || count <= 0 || !oil_c) return false;

    int16_t coolant = obd_data_get_coolant_temp();
    
    ESP_LOGI(TAG, "Mode21 extract: total_count=%d, coolant=%d", count, coolant);

    // ZC6 固定地址 + 固定索引优先，避免自适应误选。
    if (get_active_vehicle_idx_safe() == 0 && count > 33) {
        int32_t zc6_fixed = (int32_t)d[33] - 40;
        if (zc6_fixed >= -10 && zc6_fixed <= 150) {
            s_mode21_oil_idx = 33;
            s_oil_diag.mode2_ok++;
            *oil_c = zc6_fixed;
            ESP_LOGI(TAG, "Mode21 ZC6 fixed idx=33 -> %dC", (int)zc6_fixed);
            return true;
        }
        ESP_LOGW(TAG, "Mode21 ZC6 fixed idx invalid: raw=%u", (unsigned)d[33]);
    }

    // ---- 策略 1: 使用上次找到的索引（快速路径）----
    if (s_mode21_oil_idx >= 0 && s_mode21_oil_idx < count) {
        int32_t c = (int32_t)d[s_mode21_oil_idx] - 40;
        if (c >= -10 && c <= 150) {  // 宽松范围验证
            *oil_c = c;
            ESP_LOGD(TAG, "Mode21: Using cached idx=%d -> %dC", s_mode21_oil_idx, (int)c);
            s_oil_diag.mode2_ok++;
            return true;
        }
    }

    // ---- 策略 2: 智能搜索 ----
    // 采用两阶段搜索：先严格检查与水温的差异（±25°C），再扩大范围
    int best_idx = -1;
    int32_t best_temp = 0;
    int best_distance = 32767;
    int strict_count = 0;

    for (int idx = 0; idx < count; idx++) {
        int32_t c = (int32_t)d[idx] - 40;

        // 基础范围检查：-10 到 150°C（宽泛）
        if (c < -10 || c > 150) continue;

        // 严格匹配：油温应比水温稍高或接近（通常 0~15°C差异）
        if (coolant > -40) {
            int diff = (int)c - (int)coolant;
            
            // 第一阶段：严格 ±25°C
            if (diff >= -25 && diff <= 25) {
                // 优先选择：油温略高于水温（偏热量损失的物理现象）
                int priority = (diff >= 0 && diff <= 15) ? 1000 : 
                               (diff > 15 && diff <= 25) ? 500 : 100;
                int score = priority - abs(diff);  // 差异越小分越高
                
                if (score > best_distance) {
                    best_distance = score;
                    best_idx = idx;
                    best_temp = c;
                    strict_count++;
                    ESP_LOGI(TAG, "  Strict match: idx=%d temp=%dC diff=%d score=%d", idx, (int)c, diff, score);
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
        ESP_LOGI(TAG, "Mode21 selected: idx=%d temp=%dC (strict_matches=%d)", best_idx, (int)best_temp, strict_count);
        s_mode21_oil_idx = best_idx;
        s_last_mode21_oil = (int16_t)best_temp;
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

static bool parse_mac_string(const char *s, esp_bd_addr_t out_bda) {
    if (!s) return false;
    unsigned int b[6] = {0};
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out_bda[i] = (uint8_t)b[i];
    return true;
}

static void extract_adv_name(const uint8_t *adv_data, uint8_t adv_data_len, uint8_t scan_rsp_len,
                             char *out_name, size_t out_name_len) {
    if (!out_name || out_name_len == 0) return;
    out_name[0] = '\0';
    if (!adv_data) return;

    uint8_t total_len = adv_data_len + scan_rsp_len;
    uint8_t idx = 0;
    while (idx + 1 < total_len) {
        uint8_t field_len = adv_data[idx];
        if (field_len == 0) break;
        if (idx + 1 + field_len > total_len) break;

        uint8_t ad_type = adv_data[idx + 1];
        uint8_t payload_len = field_len - 1;
        const uint8_t *payload = &adv_data[idx + 2];

        if ((ad_type == ESP_BLE_AD_TYPE_NAME_CMPL || ad_type == ESP_BLE_AD_TYPE_NAME_SHORT) && payload_len > 0) {
            size_t copy_len = payload_len < (out_name_len - 1) ? payload_len : (out_name_len - 1);
            memcpy(out_name, payload, copy_len);
            out_name[copy_len] = '\0';
            return;
        }
        idx += (uint8_t)(field_len + 1);
    }
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
    extract_adv_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len, pr->scan_rst.scan_rsp_len,
                     found_name, found_name_len);

    if (target_name == NULL || target_name[0] == '\0') return true;

    esp_bd_addr_t target_bda = {0};
    if (parse_mac_string(target_name, target_bda)) {
        return memcmp(pr->scan_rst.bda, target_bda, sizeof(esp_bd_addr_t)) == 0;
    }

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
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd)
{
    uint32_t waited_ms = 0;
    while (!s_elm_ready && waited_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    if (!s_elm_ready) {
        ESP_LOGW(TAG, "Timeout (>3s) waiting previous response, forcing send: %s", ascii_cmd);
        s_elm_ready = true; // 避免死锁，继续发送
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
            extract_adv_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len,
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
                        ESP_LOGI(TAG, "Scan found [%d]: %s (RSSI %d)", s_scan_count, dev_name, pr->scan_rst.rssi);
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
                    ESP_LOGI(TAG, "Found target %s (dev=%s), connecting...",
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
            ESP_LOGI(TAG, "Service found: UUID=0x%04X handle=%04X~%04X",
                     srvc_id->uuid.uuid.uuid16, sh, eh);
            if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE) {
                s_have_service = true;
                s_service_start = sh;
                s_service_end = eh;
                ESP_LOGI(TAG, "Target service FFF0 matched");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_18F0) {
                s_have_18f0 = true;
                s_18f0_start = sh;
                s_18f0_end = eh;
                ESP_LOGI(TAG, "Target service 18F0 matched (IOS-Vlink OBD)");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_FF12) {
                s_have_ff12 = true;
                s_ff12_start = sh;
                s_ff12_end = eh;
                ESP_LOGI(TAG, "Target service FF12 matched");
            }
        } else {
            ESP_LOGI(TAG, "Service found: UUID(long) handle=%04X~%04X", sh, eh);
        }
        // 记录最大handle范围，用于兜底全量查找
        if (eh > s_all_attr_end || s_all_attr_end == 0xFFFF) s_all_attr_end = eh;
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        ESP_LOGI(TAG, "Service discovery complete. have_FFF0=%d have_18F0=%d have_FF12=%d",
                 s_have_service, s_have_18f0, s_have_ff12);

        // 优先顺序: 0xFFF0 > 0x18F0(IOS-Vlink) > 0xFF12 > 全范围兜底
        if (!s_have_service) {
            if (s_have_18f0) {
                s_service_start = s_18f0_start;
                s_service_end   = s_18f0_end;
                ESP_LOGI(TAG, "Using 18F0 service range 0x%04X~0x%04X", s_service_start, s_service_end);
            } else if (s_have_ff12) {
                s_service_start = s_ff12_start;
                s_service_end   = s_ff12_end;
                ESP_LOGI(TAG, "Using FF12 service range 0x%04X~0x%04X", s_service_start, s_service_end);
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
        ESP_LOGI(TAG, "get_attr_count ret=%d, char_count=%d", ret, char_count);

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
        ESP_LOGI(TAG, "=== All characteristics (%d) ===", alloc_count);
        for (int i = 0; i < alloc_count; i++) {
            esp_gattc_char_elem_t *c = &chars[i];
            if (c->uuid.len == ESP_UUID_LEN_16) {
                ESP_LOGI(TAG, "  [%d] UUID=0x%04X handle=0x%04X prop=0x%02X",
                         i, c->uuid.uuid.uuid16, c->char_handle, c->properties);
            } else if (c->uuid.len == ESP_UUID_LEN_128) {
                ESP_LOGI(TAG, "  [%d] UUID128=%02X%02X...%02X%02X handle=0x%04X prop=0x%02X",
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
                ESP_LOGI(TAG, "  >> Selected as WRITE handle: 0x%04X (write_type=%s)",
                         s_char_write_handle,
                         s_write_type == ESP_GATT_WRITE_TYPE_NO_RSP ? "NO_RSP" : "RSP");
            }
            // 选取第一个具有NOTIFY属性的特征作为通知句柄
            if (s_char_notify_handle == 0 &&
                (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
                s_char_notify_handle = c->char_handle;
                ESP_LOGI(TAG, "  >> Selected as NOTIFY handle: 0x%04X", s_char_notify_handle);
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
            ESP_LOGI(TAG, "No NOTIFY char found, using WRITE handle 0x%04X for notify", s_char_notify_handle);
        }

        // 注册通知
        int sret = esp_ble_gattc_register_for_notify(gattc_if, s_peer_bda, s_char_notify_handle);
        ESP_LOGI(TAG, "register_for_notify handle=0x%04X ret=%d", s_char_notify_handle, sret);

        // 查找 CCCD
        esp_gattc_descr_elem_t descr_elems[2];
        uint16_t count = 2;
        esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = UUID16_CCCD } };
        ret = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
            s_char_notify_handle, cccd_uuid, descr_elems, &count);
        if (ret == ESP_OK && count > 0) {
            s_cccd_handle = descr_elems[0].handle;
            ESP_LOGI(TAG, "Found CCCD, handle: 0x%04X", s_cccd_handle);
        } else {
            ESP_LOGW(TAG, "CCCD not found (ret=%d cnt=%d)", ret, count);
        }
        enable_notify_if_ready();
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Notifications enabled");
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

        // ---- 累积多包数据直到收到 '>' （ELM327 提示符） ----
        // 累积超时保护：5秒内未收到 '>' 则强制刷新
        if (s_accum_len > 0) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - s_accum_start_us) > 5000000) {
                ESP_LOGW(TAG, "Accum timeout (>5s), flushing %d bytes", (int)s_accum_len);
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
        ESP_LOGI(TAG, "FULL[%d]: %.100s", (int)s_accum_len, buf); // 诊断: 打印每条完整响应

        // ELM327 可能在数据前附带 echo，用 strstr 全内部搜索响应头
        // 注意: p61 必须先于 p41 判断，因为 2101 的多帧响应体中可能包含 0x41 字节
        // 导致 "41 " 被错误匹配而跳过 Mode21 解析
        char *p61 = strstr(buf, "61 01"); // Mode 21 响应头 (精确匹配 "61 01")
        char *p41 = strstr(buf, "41 ");
        char *p62 = strstr(buf, "62 ");
        // 保时捷 CAN 广播帧 0x441 (ATH1 监听下形如 "441 D0 D1 ... D7")。仅当前车型用此模式才解析，
        // 且必须先于 p41 判断(因 "441 " 含子串 "41 ")。byte5=油温(x-60°C), byte7=油压(x/25.4 bar)。
        char *p441 = (s_oil_mode_priority[0] == OIL_TEMP_MODE_PORSCHE_CAN_441) ? strstr(buf, "441 ") : NULL;

        // 收到任一有效数据帧头 → 总线在响应, 刷新"有效数据"时间戳(自愈用)
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
                ESP_LOGI(TAG, "[CAN 441] RAW b0..b7=%02X %02X %02X %02X %02X %02X %02X %02X",
                         b0, b1, b2, b3, b4, b5, b6, b7);
                ESP_LOGI(TAG, "[CAN 441] oil=%dC | press: byte7=%d.%dbar byte6=%d.%dbar",
                         (int)oil_c, oilp_x10/10, oilp_x10%10, (b6*50/127)/10, (b6*50/127)%10);
            } else {
                ESP_LOGD(TAG, "[CAN 441] parse fail vals=%d", vals);
                record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
            }
        } else if (p61 != NULL) {
            // Mode 21 多帧响应 (Toyota 2101)
            // 只有确认发出了 21 01 命令才解析，防止其他响应的数据字节碰巧包含 "61 01"
            s_expect_mode21 = false;
            uint32_t d[64] = {0};
            int count = parse_mode21_data(buf, d, 64);
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
                                ESP_LOGI(TAG, "[PROTOCOL_DETECT] Protocol %d: RPM=%u OK", s_protocol_detect_idx, rpm_val);
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
                                ESP_LOGI(TAG, "[PID 0x5C] Standard oil temp: %dC", (int)oil_temp);
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
                    default:
                        ESP_LOGD(TAG, "Unhandled PID 0x%02X", pid);
                        break;
                }
            }
        } else if (p62 != NULL) {
            // Mode 22 响应: "62 HH LL D0 D1 ..."  (d0=A, d1=B)
            uint32_t mode22 = 0, ph = 0, pl = 0, d0 = 0, d1 = 0;
            int values = sscanf(p62, "%x %x %x %x %x", &mode22, &ph, &pl, &d0, &d1);
            if (values >= 4 && mode22 == 0x62 && s_cbs.on_parsed_oil_temp) {
                uint32_t pid16 = (ph << 8) | pl;
                if (pid16 == 0x1310) {
                    // Mazda Skyactiv 油温 PID 1310: (A*256+B)/100 - 40 (°C), 需 2 个数据字节
                    if (values >= 5) {
                        int32_t mazda_oil = (int32_t)(((d0 * 256) + d1) / 100) - 40;
                        if (mazda_oil >= -40 && mazda_oil <= 215) {
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
                    // Mazda Skyactiv 油温 PID 111F: A - 50 (°C)
                    int32_t mazda_oil = (int32_t)d0 - 50;
                    if (mazda_oil >= -40 && mazda_oil <= 215) {
                        record_oil_temp_success(OIL_TEMP_MODE_MAZDA_22_111F);
                        s_cbs.on_parsed_oil_temp((uint32_t)mazda_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 11 1F] Oil temp out of range: %d (raw=%02X)", (int)mazda_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_MAZDA_22_111F);
                    }
                } else if (pid16 == 0x5822) {
                    // MINI/BMW 油温 PID 5822: °C = A - 60 (°F = A*9/5 - 76)
                    int32_t mini_oil = (int32_t)d0 - 60;
                    if (mini_oil >= -40 && mini_oil <= 215) {
                        record_oil_temp_success(OIL_TEMP_MODE_MINI_22_5822);
                        s_cbs.on_parsed_oil_temp((uint32_t)mini_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 58 22] Oil temp out of range: %d (raw=%02X)", (int)mini_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_MINI_22_5822);
                    }
                } else if (pid16 == 0x4402) {
                    // BMW F系油温 PID 4402: °C = B - 64 (响应第二个数据字节 d1)
                    if (values >= 5) {
                        int32_t bmw_oil = (int32_t)d1 - 64;
                        if (bmw_oil >= -40 && bmw_oil <= 215) {
                            record_oil_temp_success(OIL_TEMP_MODE_BMW_22_4402);
                            s_cbs.on_parsed_oil_temp((uint32_t)bmw_oil);
                        } else {
                            ESP_LOGD(TAG, "[22 44 02] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_oil, (unsigned)d0, (unsigned)d1);
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                        }
                    } else {
                        record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                    }
                } else if (pid16 == 0x1017 || pid16 == 0x0011 || pid16 == 0x1C00) {
                    record_oil_temp_success(OIL_TEMP_MODE_UDS_22_10_17);
                    s_cbs.on_parsed_oil_temp((uint32_t)((int32_t)d0 - 40));
                } else {
                    record_oil_temp_failure(OIL_TEMP_MODE_UDS_22_10_17);
                }
            }
        } else {
            // 无效数据或纯文本（NO DATA、SEARCHING、OK 等）
            if (strstr(buf, "NO DATA")) {
                ESP_LOGI(TAG, "NO DATA for last PID"); // 诊断: 哪个PID无数据(超时由时间戳自愈处理)
            } else if (strstr(buf, "SEARCHING")) {
                ESP_LOGI(TAG, "ELM327 searching protocol...");
            } else {
                ESP_LOGI(TAG, "Other response: %.60s", buf); // 诊断: 其他未知响应
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


void elm327_ble_start_default(const char *target_name) {

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
        .on_parsed_manifold_pressure = default_on_parsed_manifold_pressure,
    };
    s_scan_only_mode = false;
    elm327_ble_init_and_start(target_name, &cbs);
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 3072, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

// ---- 扫描模式实现 ----

static void ble_ensure_init(void) {
    if (s_ble_inited) return;
    // 初始化 BLE 协议栈（不设目标名，仅初始化）
    elm327_ble_init_and_start(NULL, NULL);
}

void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb) {
    ble_ensure_init();
    s_scan_only_mode = true;
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    ESP_LOGI(TAG, "Starting scan-only mode (%ds)...", duration_s);
    esp_ble_gap_start_scanning(duration_s);
}

void elm327_ble_scan_only_stop(void) {
    esp_ble_gap_stop_scanning();
    s_scan_only_mode = false;
    ESP_LOGI(TAG, "Scan-only stopped. Found %d devices.", s_scan_count);
}

void elm327_ble_connect_by_name(const char *name) {
    if (!name || name[0] == '\0') return;
    ESP_LOGI(TAG, "Connect by name: %s", name);
    s_scan_only_mode = false;
    strncpy(s_target_name, name, sizeof(s_target_name) - 1);
    s_target_name[sizeof(s_target_name) - 1] = '\0';

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
    }
    // 开始扫描，找到后自动连接
    esp_ble_gap_start_scanning(15);
    // 创建轮询任务（如果还没有）
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 3072, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

bool elm327_ble_is_connected(void) {
    return s_connected;
}

void elm327_ble_disconnect(void) {
    if (s_connected && s_gattc_if != 0 && s_conn_id != 0xFFFF) {
        ESP_LOGI(TAG, "Disconnecting from BLE device...");
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
    
    ESP_LOGI(TAG, "OIL DIAG: Mode0(01 5C)=%u/%u, Mode1(22 10 17)=%u/%u, Mode2(21 01)=%u/%u, Mode3(22 11 1F)=%u/%u, Mode4(22 13 10)=%u/%u, Mode5(CAN 441)=%u/%u, Mode6(22 58 22)=%u/%u, Mode7(22 44 02)=%u/%u",
             out->mode0_ok, out->mode0_fail, out->mode1_ok, out->mode1_fail,
             out->mode2_ok, out->mode2_fail, s_oil_diag.mode3_ok, s_oil_diag.mode3_fail,
             s_oil_diag.mode4_ok, s_oil_diag.mode4_fail, s_oil_diag.mode5_ok, s_oil_diag.mode5_fail,
             s_oil_diag.mode6_ok, s_oil_diag.mode6_fail, s_oil_diag.mode7_ok, s_oil_diag.mode7_fail);
}

