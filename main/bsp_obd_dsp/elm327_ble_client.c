#include "elm327_ble_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "app_obd_dsp/obd_data_cache.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
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

// 增加全局 ready 标志
static volatile bool s_elm_ready = true; // 初始允许发送首条 ATZ
static volatile bool s_expect_mode21 = false; // true=上条命令是 21 01，等待 61 01 响应
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd);

// 多包响应累积缓冲区（21 01 等长响应分多个BLE包）
#define ACCUM_BUF_SIZE 512
static char s_accum_buf[ACCUM_BUF_SIZE];
static size_t s_accum_len = 0;
static int64_t s_accum_start_us = 0; // 累积开始时间 (us)
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
static void default_on_parsed_rpm(uint16_t rpm) { ESP_LOGD(TAG, "RPM: %u", rpm); obd_data_set_rpm(rpm); }
static void default_on_parsed_speed(uint8_t kmh) { ESP_LOGD(TAG, "SPEED: %u km/h", kmh); obd_data_set_speed(kmh); }
static void default_on_parsed_coolant_temp(uint32_t coolant_temp) { ESP_LOGD(TAG, "CLT: %u C", coolant_temp); obd_data_set_coolant_temp((int16_t)coolant_temp); }
static void default_on_parsed_intake_temp(uint32_t intake_temp) { ESP_LOGD(TAG, "IAT: %u C", intake_temp); obd_data_set_intake_temp((int16_t)intake_temp); }
static void default_on_parsed_oil_temp(uint32_t oil_temp) { ESP_LOGD(TAG, "OIL: %d C", (int)oil_temp); obd_data_set_oil_temp((int16_t)oil_temp); }
static void default_on_parsed_load_pct(uint32_t load_pct) { ESP_LOGD(TAG, "LOAD: %u%%", load_pct); obd_data_set_load_pct((int16_t)load_pct); }
static void default_on_parsed_control_module_voltage(uint32_t bat_mv) { ESP_LOGD(TAG, "BAT: %u.%uV", bat_mv/1000, (bat_mv%1000)/100); obd_data_set_bat_mv((int32_t)bat_mv); }
static void default_on_parsed_throttle_position(uint32_t tps_pct) { ESP_LOGD(TAG, "TPS: %u%%", tps_pct); obd_data_set_tps((int16_t)tps_pct); }
 
static void obd_poll_task(void *arg) {
    for(;;)
    {
        if(s_connected)
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    uint32_t tick_count = 0;
    char atsp_cmd[16];
    const nvs_user_cfg_t *cfg = nvs_cfg_get();
    snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", cfg->protocol);

    const char *init_cmds[] = {
        "ATZ\r",        // 复位
        "ATE0\r",       // Echo off
        "ATL0\r",       // 行宽 off
        "ATS1\r",       // 空格 on/off
        "ATH0\r",       // 关闭头部数据（可选）ATH1是打開
        "ATAT1\r",      // 适应时序
        "ATST 19\r",    // 设置超时
        atsp_cmd,         // 设置协议
        "ATSH7E0\r",    // Toyota ECU 请求帧 ID (7E0)，Mode 21 01 油温需要
    };

    for (size_t i = 0; i < (sizeof(init_cmds) / sizeof(init_cmds[0])); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        ESP_LOGI(TAG, " AT init Cmd send %s",init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    // 协议选择后做一次能力探测，加速稳定
    elm327_ble_send_ascii_blocking("01 00\r");
    ESP_LOGI(TAG, " CMD 01 00 send \n");
    vTaskDelay(pdMS_TO_TICKS(100));

    // 8-slot 轮询: 0=RPM, 1=IAT, 2=Speed, 3=CLT, 4=Load(0x04), 5=TPS(0x11), 6=OIL(22 10 17), 7=BAT(0x42)
    while (1)
    {
        if (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
            case 6: // Toyota 增强 PID 机油温 Mode 21 01，位置 AC(d[28])，公式 AC-40
                elm327_ble_send_ascii_blocking("21 01\r");
                s_expect_mode21 = true;  // 标记：下一条响应应为 61 01
                ESP_LOGI(TAG, "[Slot6] Send 21 01 (Toyota oil temp)");
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
            default:
                break;
        }
 
        tick_count++;
        if(tick_count >= 8)
        {
            tick_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms per slot, IAT ~200ms refresh
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

static void start_scan(void) {
    esp_ble_gap_start_scanning(10); // 10s
}

static bool match_device_name(const uint8_t *adv_data, uint8_t adv_data_len, const char *name) {
    if (name == NULL || name[0] == '\0') return true;
    uint8_t len = 0;
    uint8_t *p = (uint8_t *)esp_ble_resolve_adv_data((uint8_t *)adv_data, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    if (p && len) {
        return (len == strlen(name) && memcmp(p, name, len) == 0);
    }
    p = (uint8_t *)esp_ble_resolve_adv_data((uint8_t *)adv_data, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
    if (p && len) {
        return (len == strlen(name) && memcmp(p, name, len) == 0);
    }
    return false;
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
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        start_scan();
        break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *pr = param;
        if (pr->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // 提取设备名
            uint8_t name_len = 0;
            uint8_t *name_p = (uint8_t *)esp_ble_resolve_adv_data(
                pr->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            if (!name_p || !name_len) {
                name_p = (uint8_t *)esp_ble_resolve_adv_data(
                    pr->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
            }

            if (s_scan_only_mode) {
                // 扫描模式：收集设备列表
                if (name_p && name_len > 0 && s_scan_count < BLE_SCAN_MAX_DEVICES) {
                    char dev_name[32] = {0};
                    int copy_len = name_len < 31 ? name_len : 31;
                    memcpy(dev_name, name_p, copy_len);

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
                if (match_device_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len, s_target_name)) {
                    ESP_LOGI(TAG, "Found target %s, connecting...", s_target_name);
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

        if (p61 != NULL) {
            // Mode 21 多帧响应 (Toyota 2101)
            // 只有确认发出了 21 01 命令才解析，防止其他响应的数据字节碰巧包含 "61 01"
            s_expect_mode21 = false;
            uint32_t d[64] = {0};
            int count = parse_mode21_data(buf, d, 64);
            // d[33]: 油温字节, 公式 d[33]-40 = 油温°C (实测确认)
            if (count >= 34 && s_cbs.on_parsed_oil_temp) {
                int32_t oil_c = (int32_t)d[33] - 40;
                s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
            } else {
                ESP_LOGW(TAG, "21 01 parse failed: count=%d", count);
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
                switch (pid) {
                    case 0x04: // 发动机负荷 (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_load_pct)
                            s_cbs.on_parsed_load_pct((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x05: // 水温
                        if (dc >= 1 && s_cbs.on_parsed_coolant_temp)
                            s_cbs.on_parsed_coolant_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x0C: // 转速
                        if (dc >= 2 && s_cbs.on_parsed_rpm)
                            s_cbs.on_parsed_rpm((uint16_t)(((d[0] << 8) | d[1]) / 4));
                        break;
                    case 0x0D: // 车速
                        if (dc >= 1 && s_cbs.on_parsed_speed_kmh)
                            s_cbs.on_parsed_speed_kmh((uint8_t)d[0]);
                        break;
                    case 0x0F: // 进气温度
                        if (dc >= 1 && s_cbs.on_parsed_intake_temp)
                            s_cbs.on_parsed_intake_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x11: // 节气门开度 TPS (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_throttle_position)
                            s_cbs.on_parsed_throttle_position((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x5C: // 机油温度 (标准PID, BRZ待验证)
                        if (dc >= 1 && s_cbs.on_parsed_oil_temp)
                            s_cbs.on_parsed_oil_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x42: // 电池电压 (mV)
                        if (dc >= 2 && s_cbs.on_parsed_control_module_voltage)
                            s_cbs.on_parsed_control_module_voltage((d[0] << 8) | d[1]);
                        break;
                    default:
                        ESP_LOGD(TAG, "Unhandled PID 0x%02X", pid);
                        break;
                }
            }
        } else if (p62 != NULL) {
            // Mode 22 响应: "62 HH LL DD ..."
            uint32_t mode22 = 0, ph = 0, pl = 0, d0 = 0;
            int values = sscanf(p62, "%x %x %x %x", &mode22, &ph, &pl, &d0);
            if (values >= 4 && mode22 == 0x62 && s_cbs.on_parsed_oil_temp) {
                uint32_t pid16 = (ph << 8) | pl;
                if (pid16 == 0x1017 || pid16 == 0x0011 || pid16 == 0x1C00)
                    s_cbs.on_parsed_oil_temp((uint32_t)((int32_t)d0 - 40));
            }
        } else {
            // 无效数据或纯文本（NO DATA、SEARCHING、OK 等）
            if (strstr(buf, "NO DATA")) {
                ESP_LOGI(TAG, "NO DATA for last PID"); // 诊断: 哪个PID无数据
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
        if (s_cbs.on_disconnected) s_cbs.on_disconnected();
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

