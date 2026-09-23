#include "wired_can_obd.h"

#include <inttypes.h>
#include <stdint.h>
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"

#define WIRED_CAN_TX_GPIO GPIO_NUM_43
#define WIRED_CAN_RX_GPIO GPIO_NUM_44
#define WIRED_CAN_PROBE_MS 1200
#define WIRED_CAN_REPLY_MS 120
#define WIRED_CAN_STALE_US 8000000LL
#define WIRED_CAN_FAILURE_LIMIT 50

static const char *TAG = "wired_can_obd";
static volatile bool s_active;
static bool s_extd;
static uint32_t s_request_id;
static volatile int64_t s_last_valid_us;
static volatile unsigned s_consecutive_failures;
static volatile TaskHandle_t s_task_handle;
static bool s_driver_started;

static bool response_id_matches(const twai_message_t *m)
{
    if (s_extd) {
        return m->extd && (m->identifier & 0x1FFFFF00U) == 0x18DAF100U;
    }
    return !m->extd && m->identifier >= 0x7E8U && m->identifier <= 0x7EFU;
}

static bool decode_mode01(const twai_message_t *m, uint8_t wanted_pid)
{
    if (!response_id_matches(m) || m->data_length_code < 4) return false;
    if ((m->data[0] & 0xF0U) != 0 || m->data[1] != 0x41 || m->data[2] != wanted_pid) return false;

    const uint8_t a = m->data[3];
    const uint8_t b = m->data_length_code > 4 ? m->data[4] : 0;
    switch (wanted_pid) {
    case 0x04: obd_data_set_load_pct((int16_t)((a * 100U + 127U) / 255U)); break;
    case 0x05: obd_data_set_coolant_temp((int16_t)a - 40); break;
    case 0x0C: obd_data_set_rpm((uint16_t)(((uint16_t)a << 8) | b) / 4U); break;
    case 0x0D: obd_data_set_speed(a); break;
    case 0x0F: obd_data_set_intake_temp((int16_t)a - 40); break;
    case 0x11: obd_data_set_tps((int16_t)((a * 100U + 127U) / 255U)); break;
    case 0x42: obd_data_set_bat_mv((int32_t)(((uint16_t)a << 8) | b)); break;
    case 0x44: {
        uint16_t raw = ((uint16_t)a << 8) | b;
        obd_data_set_afr_x100((int16_t)(((uint32_t)raw * 1470U + 16384U) / 32768U));
        break;
    }
    case 0x5C: obd_data_set_oil_temp((int16_t)a - 40); break;
    default: break;
    }
    return true;
}

static bool request_pid(uint8_t pid, uint32_t timeout_ms)
{
    twai_message_t tx = {
        .identifier = s_request_id,
        .data_length_code = 8,
        .extd = s_extd,
        .data = { 0x02, 0x01, pid, 0x55, 0x55, 0x55, 0x55, 0x55 },
    };
    if (twai_transmit(&tx, pdMS_TO_TICKS(20)) != ESP_OK) return false;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK && decode_mode01(&rx, pid)) {
            s_last_valid_us = esp_timer_get_time();
            return true;
        }
    } while (esp_timer_get_time() < deadline);
    return false;
}

static void wired_can_task(void *arg)
{
    (void)arg;
    static const uint8_t pids[] = { 0x0C, 0x0D, 0x05, 0x5C, 0x0F, 0x04, 0x11, 0x42, 0x44 };
    unsigned idx = 0;
    unsigned failures = 0;

    while (s_active) {
        if (request_pid(pids[idx], WIRED_CAN_REPLY_MS)) {
            failures = 0;
            s_consecutive_failures = 0;
        } else {
            failures++;
            s_consecutive_failures++;
        }
        if (failures >= WIRED_CAN_FAILURE_LIMIT) {
            twai_status_info_t status;
            if (twai_get_status_info(&status) == ESP_OK && status.state == TWAI_STATE_BUS_OFF) {
                ESP_LOGW(TAG, "TWAI bus-off; starting recovery");
                twai_initiate_recovery();
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            failures = 0;
        }
        idx = (idx + 1U) % (sizeof(pids) / sizeof(pids[0]));
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

bool wired_can_obd_start(void)
{
    s_last_valid_us = 0;
    s_consecutive_failures = 0;
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    uint8_t protocol = vp && vp->forced_protocol ? vp->forced_protocol : 6;
    s_extd = (protocol == 7 || protocol == 9);
    if (s_extd) {
        s_request_id = 0x18DB33F1U;
    } else {
        s_request_id = (!vp || vp->obd_functional_addr) ? 0x7DFU : 0x7E0U;
    }

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(WIRED_CAN_TX_GPIO,
                                                                 WIRED_CAN_RX_GPIO,
                                                                 TWAI_MODE_NORMAL);
    general.tx_queue_len = 8;
    general.rx_queue_len = 24;
    twai_timing_config_t timing = (protocol == 8 || protocol == 9)
                                    ? (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS()
                                    : (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&general, &timing, &filter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return false;
    }
    s_driver_started = true;

    ESP_LOGI(TAG, "Probing wired OBD: GPIO43=TX GPIO44=RX, %s-bit %u kbit/s, request=0x%" PRIX32,
             s_extd ? "29" : "11", (protocol == 8 || protocol == 9) ? 250U : 500U, s_request_id);
    if (!request_pid(0x0C, WIRED_CAN_PROBE_MS)) {
        ESP_LOGW(TAG, "No wired OBD response");
        twai_stop();
        twai_driver_uninstall();
        s_driver_started = false;
        return false;
    }

    s_active = true;
    if (xTaskCreate(wired_can_task, "wired_can_obd", 4096, NULL, 4,
                    (TaskHandle_t *)&s_task_handle) != pdPASS) {
        s_active = false;
        twai_stop();
        twai_driver_uninstall();
        s_driver_started = false;
        ESP_LOGE(TAG, "Could not create wired CAN polling task");
        return false;
    }
    ESP_LOGI(TAG, "Wired CAN selected; BLE ELM327 auto-connect skipped");
    return true;
}

void wired_can_obd_stop(void)
{
    s_active = false;
    for (int i = 0; i < 60 && s_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_driver_started) {
        twai_stop();
        twai_driver_uninstall();
        s_driver_started = false;
    }
    s_last_valid_us = 0;
    s_consecutive_failures = 0;
}

bool wired_can_obd_is_active(void)
{
    return s_active;
}

bool wired_can_obd_has_fresh_data(void)
{
    int64_t last = s_last_valid_us;
    return s_active && last > 0 && s_consecutive_failures < WIRED_CAN_FAILURE_LIMIT &&
           (esp_timer_get_time() - last) < WIRED_CAN_STALE_US;
}

