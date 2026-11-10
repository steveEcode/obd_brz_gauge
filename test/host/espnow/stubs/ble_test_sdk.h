#pragma once
/* Minimal host ABI for the actual BLE wrappers. No controller/radio emulation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_GATT_OK 0
#define ESP_GATT_IF_NONE 0xFF
#define ESP_MAC_WIFI_STA 0
#define ESP_UUID_LEN_16 2
#define ESP_GATT_UUID_PRI_SERVICE 0x2800
#define ESP_GATT_UUID_CHAR_DECLARE 0x2803
#define ESP_GATT_UUID_CHAR_CLIENT_CONFIG 0x2902
#define ESP_GATT_AUTO_RSP 1
#define ESP_GATT_PERM_READ 1
#define ESP_GATT_PERM_WRITE 2
#define ESP_GATT_CHAR_PROP_BIT_READ 2
#define ESP_GATT_CHAR_PROP_BIT_WRITE 8
#define ESP_GATT_CHAR_PROP_BIT_NOTIFY 16
#define ADV_TYPE_IND 0
#define BLE_ADDR_TYPE_PUBLIC 0
#define ADV_CHNL_ALL 7
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY 0

typedef uint8_t esp_gatt_if_t;
typedef uint8_t esp_bd_addr_t[6];
typedef enum {
    ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,
    ESP_GAP_BLE_SCAN_RESULT_EVT, ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT,
    ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT, ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,
    ESP_GAP_BLE_ADV_START_COMPLETE_EVT
} esp_gap_ble_cb_event_t;
#define ESP_GAP_SEARCH_INQ_CMPL_EVT 1
#define ESP_GAP_SEARCH_INQ_RES_EVT 2
#define BLE_SCAN_TYPE_ACTIVE 1
#define BLE_SCAN_FILTER_ALLOW_ALL 0
#define BLE_SCAN_DUPLICATE_DISABLE 0
#define ESP_BLE_AD_TYPE_NAME_CMPL 9
#define ESP_BLE_AD_TYPE_NAME_SHORT 8
typedef union {
    struct { int status; } scan_param_cmpl, scan_start_cmpl, adv_start_cmpl;
    struct {
        int search_evt;
        esp_bd_addr_t bda;
        uint8_t ble_addr_type, ble_adv[62], adv_data_len, scan_rsp_len;
        int rssi;
    } scan_rst;
} esp_ble_gap_cb_param_t;
typedef enum { ESP_GATTC_REG_EVT } esp_gattc_cb_event_t;
typedef union { struct { int status; uint16_t app_id; } reg; } esp_ble_gattc_cb_param_t;
typedef enum {
    ESP_GATTS_REG_EVT, ESP_GATTS_CREAT_ATTR_TAB_EVT, ESP_GATTS_CONNECT_EVT,
    ESP_GATTS_DISCONNECT_EVT, ESP_GATTS_WRITE_EVT
} esp_gatts_cb_event_t;
typedef union {
    struct { int status; uint16_t app_id; } reg;
    struct {
        int status; uint16_t num_handle; uint16_t *handles;
        struct { union { uint16_t uuid16; } uuid; } svc_uuid;
    } add_attr_tab;
    struct { int link_role; uint16_t conn_id; esp_bd_addr_t remote_bda; } connect;
    struct { uint16_t conn_id; esp_bd_addr_t remote_bda; } disconnect;
    struct { uint16_t handle, len; uint8_t *value; } write;
} esp_ble_gatts_cb_param_t;
typedef void (*esp_gap_ble_cb_t)(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t *);
typedef void (*esp_gattc_cb_t)(esp_gattc_cb_event_t, esp_gatt_if_t, esp_ble_gattc_cb_param_t *);
typedef void (*esp_gatts_cb_t)(esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t *);
typedef struct {
    int scan_type, own_addr_type, scan_filter_policy;
    uint16_t scan_interval, scan_window;
    int scan_duplicate;
} esp_ble_scan_params_t;
typedef struct {
    uint16_t adv_int_min, adv_int_max;
    int adv_type, own_addr_type, channel_map, adv_filter_policy;
} esp_ble_adv_params_t;
typedef struct {
    struct { int auto_rsp; } attr_control;
    struct { uint16_t uuid_length; uint8_t *uuid_p; uint16_t perm, max_length, length; uint8_t *value; } att_desc;
} esp_gatts_attr_db_t;

typedef enum { ESP_BT_CONTROLLER_STATUS_IDLE, ESP_BT_CONTROLLER_STATUS_INITED,
               ESP_BT_CONTROLLER_STATUS_ENABLED } esp_bt_controller_status_t;
typedef enum { ESP_BLUEDROID_STATUS_UNINITIALIZED, ESP_BLUEDROID_STATUS_INITIALIZED,
               ESP_BLUEDROID_STATUS_ENABLED } esp_bluedroid_status_t;
typedef struct { int unused; } esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() ((esp_bt_controller_config_t){0})
#define ESP_BT_MODE_CLASSIC_BT 1
#define ESP_BT_MODE_BLE 2

const char *esp_err_to_name(esp_err_t err);
int64_t esp_timer_get_time(void);
esp_err_t esp_read_mac(uint8_t *mac, int type);
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t cb);
esp_err_t esp_ble_gattc_register_callback(esp_gattc_cb_t cb);
esp_err_t esp_ble_gatts_register_callback(esp_gatts_cb_t cb);
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *params);
esp_err_t esp_ble_gap_start_scanning(uint32_t duration);
esp_err_t esp_ble_gap_stop_scanning(void);
esp_err_t esp_ble_gap_stop_advertising(void);
esp_err_t esp_ble_gap_set_device_name(const char *name);
esp_err_t esp_ble_gap_config_adv_data_raw(uint8_t *data, uint32_t len);
esp_err_t esp_ble_gap_config_scan_rsp_data_raw(uint8_t *data, uint32_t len);
esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t *params);
esp_err_t esp_ble_gatt_set_local_mtu(uint16_t mtu);
esp_err_t esp_ble_gattc_app_register(uint16_t app);
esp_err_t esp_ble_gatts_app_register(uint16_t app);
esp_err_t esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *db, esp_gatt_if_t iface, uint8_t count, uint8_t instance);
esp_err_t esp_ble_gatts_start_service(uint16_t handle);
esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t iface, uint16_t conn, uint16_t handle, uint16_t len, uint8_t *data, bool confirm);
esp_bt_controller_status_t esp_bt_controller_get_status(void);
esp_bluedroid_status_t esp_bluedroid_get_status(void);
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg);
esp_err_t esp_bt_controller_enable(int mode);
esp_err_t esp_bt_controller_disable(void);
esp_err_t esp_bt_controller_deinit(void);
esp_err_t esp_bt_controller_mem_release(int mode);
esp_err_t esp_bluedroid_init(void);
esp_err_t esp_bluedroid_enable(void);
esp_err_t esp_bluedroid_disable(void);
esp_err_t esp_bluedroid_deinit(void);

/* Logs consume and type-check arguments but do not flood the test output. */
void ble_test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGE(...) ble_test_log(__VA_ARGS__)
#define ESP_LOGW(...) ble_test_log(__VA_ARGS__)
#define ESP_LOGI(...) ble_test_log(__VA_ARGS__)
#define ESP_LOGD(...) ble_test_log(__VA_ARGS__)
