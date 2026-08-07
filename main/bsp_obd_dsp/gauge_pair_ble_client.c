// 三连表蓝牙配对: 从表侧一次性 BLE 中心设备 —— 扫描附近 "SkyGauge" 主表广播、
// 连接、读取其配对特征值(本机 ESP-NOW MAC)、读到后立即断开。不做持续通知订阅、
// 不做断线自动重连(这是用户主动触发的一次性配对动作，不是长连接)。
//
// 只会跑在从表设备上；从表从不初始化 elm327_ble_client.c，所以这里独立注册
// 自己的 GAP/GATTC 回调，不存在回调表冲突。

#include "gauge_pair_ble_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ble_adv_util.h"

static const char *TAG = "gauge_pair_ble";

#define GAUGE_PAIR_APP_ID     0x53
#define GAUGE_PAIR_NAME_PREFIX "SkyGauge"
#define GAUGE_PAIR_TIMEOUT_US  (8 * 1000 * 1000)

static bool s_ble_inited = false;

// ---- 扫描状态 ----
static bool s_scanning = false;
static int  s_pending_scan_duration = 10;
static gauge_pair_scan_cb_t s_scan_cb = NULL;
static gauge_pair_scan_result_t s_scan_list[GAUGE_PAIR_SCAN_MAX_DEVICES];
static int s_scan_count = 0;

// ---- 连接/配对状态 ----
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;
static bool s_gattc_connected = false;
static bool s_pairing = false;               // 一次配对流程正在进行
static uint8_t s_target_bda[6] = {0};
static char s_target_name[32] = {0};
static bool s_have_pair_svc = false;
static uint16_t s_pair_svc_start = 0, s_pair_svc_end = 0;
static uint16_t s_char_mac_handle = 0;
static gauge_pair_result_cb_t s_pair_cb = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void report_result(bool ok, const uint8_t mac[6]);

static void pair_timeout_cb(void *arg) {
    (void)arg;
    if (!s_pairing) return;
    ESP_LOGW(TAG, "Pairing timeout");
    if (s_gattc_connected) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }
    report_result(false, NULL);
}

static void ensure_timeout_timer(void) {
    if (s_timeout_timer) return;
    const esp_timer_create_args_t args = {
        .callback = pair_timeout_cb,
        .name = "gauge_pair_to",
    };
    esp_timer_create(&args, &s_timeout_timer);
}

static void report_result(bool ok, const uint8_t mac[6]) {
    if (!s_pairing) return;   // 已经报告过一次, 忽略后续(如 READ 成功后紧跟的 DISCONNECT)
    s_pairing = false;
    if (s_timeout_timer) esp_timer_stop(s_timeout_timer);

    gauge_pair_result_cb_t cb = s_pair_cb;
    s_pair_cb = NULL;
    char name_copy[32];
    strncpy(name_copy, s_target_name, sizeof(name_copy) - 1);
    name_copy[sizeof(name_copy) - 1] = '\0';

    if (cb) cb(ok, ok ? name_copy : NULL, ok ? mac : NULL);
}

static void ble_ensure_init(void) {
    if (s_ble_inited) return;

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

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(GAUGE_PAIR_APP_ID));
    s_ble_inited = true;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (s_scanning) esp_ble_gap_start_scanning(s_pending_scan_duration);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (!s_scanning) break;
        esp_ble_gap_cb_param_t *pr = param;
        if (pr->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        char dev_name[32] = {0};
        ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len,
                          pr->scan_rst.scan_rsp_len, dev_name, sizeof(dev_name));
        if (strncmp(dev_name, GAUGE_PAIR_NAME_PREFIX, strlen(GAUGE_PAIR_NAME_PREFIX)) != 0) break;
        if (s_scan_count >= GAUGE_PAIR_SCAN_MAX_DEVICES) break;

        bool exists = false;
        for (int i = 0; i < s_scan_count; i++) {
            if (strcmp(s_scan_list[i].name, dev_name) == 0) { exists = true; break; }
        }
        if (exists) break;

        snprintf(s_scan_list[s_scan_count].name, sizeof(s_scan_list[s_scan_count].name), "%s", dev_name);
        memcpy(s_scan_list[s_scan_count].addr, pr->scan_rst.bda, 6);
        s_scan_list[s_scan_count].rssi = pr->scan_rst.rssi;
        s_scan_count++;
        ESP_LOGI(TAG, "Found master [%d]: %s (RSSI %d)", s_scan_count, dev_name, pr->scan_rst.rssi);
        if (s_scan_cb) s_scan_cb(&s_scan_list[s_scan_count - 1], s_scan_count);
        break;
    }

    default:
        break;
    }
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        s_gattc_if = gattc_if;
        break;

    case ESP_GATTC_CONNECT_EVT: {
        s_gattc_connected = true;
        s_conn_id = param->connect.conn_id;
        // 不带过滤条件地枚举全部 service, 在 SEARCH_RES_EVT 里自己比对 UUID(和 elm327_ble_client.c
        // 的服务发现方式一致, 避免依赖按 UUID 过滤搜索的行为)。
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;
    }

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Open failed status=%d", param->open.status);
            report_result(false, NULL);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        const esp_gatt_id_t *srvc_id = &param->search_res.srvc_id;
        if (srvc_id->uuid.len == ESP_UUID_LEN_16 && srvc_id->uuid.uuid.uuid16 == GAUGE_PAIR_SERVICE_UUID) {
            s_have_pair_svc = true;
            s_pair_svc_start = param->search_res.start_handle;
            s_pair_svc_end = param->search_res.end_handle;
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (!s_have_pair_svc) {
            ESP_LOGW(TAG, "Pair service not found on peer");
            report_result(false, NULL);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }

        uint16_t char_count = 0;
        esp_err_t ret = esp_ble_gattc_get_attr_count(gattc_if, s_conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                                      s_pair_svc_start, s_pair_svc_end, 0, &char_count);
        if (ret != ESP_OK || char_count == 0) {
            ESP_LOGW(TAG, "No characteristics in pair service");
            report_result(false, NULL);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }

        esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)malloc(char_count * sizeof(esp_gattc_char_elem_t));
        if (!chars) {
            report_result(false, NULL);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        ret = esp_ble_gattc_get_all_char(gattc_if, s_conn_id, s_pair_svc_start, s_pair_svc_end,
                                          chars, &char_count, 0);
        s_char_mac_handle = 0;
        if (ret == ESP_OK) {
            for (int i = 0; i < char_count; i++) {
                if (chars[i].uuid.len == ESP_UUID_LEN_16 && chars[i].uuid.uuid.uuid16 == GAUGE_PAIR_CHAR_MAC) {
                    s_char_mac_handle = chars[i].char_handle;
                    break;
                }
            }
        }
        free(chars);

        if (s_char_mac_handle == 0) {
            ESP_LOGW(TAG, "Pair MAC characteristic not found");
            report_result(false, NULL);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }

        esp_ble_gattc_read_char(gattc_if, s_conn_id, s_char_mac_handle, ESP_GATT_AUTH_REQ_NONE);
        break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
        if (param->read.status == ESP_GATT_OK && param->read.handle == s_char_mac_handle &&
            param->read.value_len >= 6) {
            report_result(true, param->read.value);
        } else {
            ESP_LOGW(TAG, "Read pair MAC failed status=%d len=%d", param->read.status, param->read.value_len);
            report_result(false, NULL);
        }
        esp_ble_gattc_close(gattc_if, s_conn_id);
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        s_gattc_connected = false;
        s_conn_id = 0xFFFF;
        s_have_pair_svc = false;
        s_pair_svc_start = s_pair_svc_end = 0;
        s_char_mac_handle = 0;
        report_result(false, NULL);   // 若已经在 READ_CHAR_EVT 里报告过, report_result 会直接忽略
        break;

    default:
        break;
    }
}

void gauge_pair_ble_scan_start(int duration_s, gauge_pair_scan_cb_t cb) {
    ble_ensure_init();
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    s_scanning = true;
    s_pending_scan_duration = duration_s;

    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x60,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_ble_gap_set_scan_params(&scan_params);
}

void gauge_pair_ble_scan_stop(void) {
    s_scanning = false;
    esp_ble_gap_stop_scanning();
}

void gauge_pair_ble_connect(const uint8_t addr[6], const char *name, gauge_pair_result_cb_t cb) {
    if (!addr || s_pairing) return;
    ble_ensure_init();
    gauge_pair_ble_scan_stop();

    s_pairing = true;
    s_pair_cb = cb;
    memcpy(s_target_bda, addr, 6);
    if (name) {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    } else {
        s_target_name[0] = '\0';
    }
    s_have_pair_svc = false;
    s_pair_svc_start = s_pair_svc_end = 0;
    s_char_mac_handle = 0;

    ensure_timeout_timer();
    esp_timer_start_once(s_timeout_timer, GAUGE_PAIR_TIMEOUT_US);

    esp_ble_gattc_open(s_gattc_if, (uint8_t *)s_target_bda, BLE_ADDR_TYPE_PUBLIC, true);
}
