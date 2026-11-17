#pragma once
/* Minimum SDK ABI for compiling the real ESP-NOW link worker on the host. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
#define ESP_NOW_MAX_DATA_LEN 250
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 1
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PS_NONE 0
#define WIFI_IF_STA 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { uint8_t peer_addr[6]; int channel, ifidx; bool encrypt; } esp_now_peer_info_t;
typedef struct { const uint8_t *src_addr; } esp_now_recv_info_t;
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *, const uint8_t *, int);
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_get_mac(int ifidx, uint8_t mac[6]);
esp_err_t esp_wifi_set_channel(uint8_t channel, int secondary);
esp_err_t esp_wifi_set_ps(int ps);
esp_err_t esp_now_init(void);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer);
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb);
esp_err_t esp_now_unregister_recv_cb(void);
esp_err_t esp_now_unregister_send_cb(void);
esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *data, size_t len);
esp_err_t esp_now_deinit(void);
int64_t esp_timer_get_time(void);
void *heap_caps_malloc(size_t bytes, unsigned caps);
void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps);
const char *esp_err_to_name(esp_err_t err);
void link_test_log(const char *tag, const char *fmt, ...);
#define ESP_ERROR_CHECK(expr) do { if ((expr) != ESP_OK) abort(); } while (0)
#define ESP_LOGE(...) link_test_log(__VA_ARGS__)
#define ESP_LOGW(...) link_test_log(__VA_ARGS__)
#define ESP_LOGI(...) link_test_log(__VA_ARGS__)
#define ESP_LOGD(...) link_test_log(__VA_ARGS__)
