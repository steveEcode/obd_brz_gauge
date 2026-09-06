#include "rs485_brake_temp.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "app_obd_dsp/obd_data_cache.h"

#define BRAKE_TEMP_UART_PORT        UART_NUM_0
#define BRAKE_TEMP_UART_BUF_SIZE    256
#define BRAKE_TEMP_POLL_MS          500
#define BRAKE_TEMP_READ_TIMEOUT_MS  500
#define BRAKE_TEMP_TX_DONE_MS       50
#define BRAKE_TEMP_FAIL_RESET       6

static const char *TAG = "brake_temp";
static bool s_started = false;
static volatile bool s_paused = false;
static int s_uart_baud_cur = -1;
static int s_uart_parity_cur = -1;
static int s_uart_stop_bits_cur = -1;
static int s_fail_count = 0;

typedef struct {
    int baud;
    uint8_t addr;
    uint16_t reg;
    uint8_t func;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
} brake_modbus_cfg_t;

typedef enum {
    QUERY_OK = 0,
    QUERY_TIMEOUT,
    QUERY_PARSE_FAIL,
} query_status_t;

static const gpio_num_t s_tx_pin = (gpio_num_t)CONFIG_OBD_RS485_TX_GPIO;
static const gpio_num_t s_rx_pin = (gpio_num_t)CONFIG_OBD_RS485_RX_GPIO;
static const gpio_num_t s_de_re_gpio = (gpio_num_t)CONFIG_OBD_RS485_DE_RE_GPIO;

static const brake_modbus_cfg_t s_cfg = {
    .baud = 9600,
    .addr = 0x01,
    .reg = 0x0000,
    .func = 0x03,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
};

static inline bool rs485_has_de_re(void)
{
    return CONFIG_OBD_RS485_DE_RE_GPIO >= 0;
}

static void rs485_set_tx_mode(bool tx_mode)
{
    if (!rs485_has_de_re()) return;
    gpio_set_level(s_de_re_gpio, tx_mode ? 1 : 0);
}

static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void build_req_frame(const brake_modbus_cfg_t *cfg, uint8_t *out8)
{
    out8[0] = cfg->addr;
    out8[1] = cfg->func;
    out8[2] = (uint8_t)(cfg->reg >> 8);
    out8[3] = (uint8_t)(cfg->reg & 0xFF);
    out8[4] = 0x00;
    out8[5] = 0x01;

    uint16_t crc = modbus_crc16(out8, 6);
    out8[6] = (uint8_t)(crc & 0xFF);
    out8[7] = (uint8_t)(crc >> 8);
}

static bool parse_temp_from_rx(const uint8_t *rx, int rx_len, const brake_modbus_cfg_t *cfg, int16_t *temp_x10)
{
    if (!rx || rx_len < 7 || !temp_x10) return false;

    for (int i = 0; i <= rx_len - 7; i++) {
        const uint8_t *p = &rx[i];
        if (p[0] != cfg->addr) continue;
        if (p[1] != cfg->func) continue;
        
        uint8_t byte_count = p[2];  // data byte count; not forced to 0x02

        // Check minimum length: addr(1) + func(1) + byte_count(1) + data + CRC(2)
        if (i + 3 + byte_count + 2 > rx_len) continue;

        // Compute CRC (from addr through byte_count and the data, excluding the CRC itself)
        uint16_t crc_calc = modbus_crc16(p, 3 + byte_count);
        uint16_t crc_recv = (uint16_t)p[3 + byte_count] | ((uint16_t)p[4 + byte_count] << 8);
        
        if (crc_calc != crc_recv) continue;

        // Most temperature sensors return a single 16-bit register (2 bytes),
        // but some may return multiple registers; take the first register
        if (byte_count < 2) continue;

        int16_t raw = (int16_t)(((uint16_t)p[3] << 8) | p[4]);

        *temp_x10 = raw;
        return true;
    }

    return false;
}

static void ensure_uart_baud(int baud)
{
    if (s_uart_baud_cur == baud) return;
    if (uart_set_baudrate(BRAKE_TEMP_UART_PORT, baud) == ESP_OK) {
        s_uart_baud_cur = baud;
    }
}

static void ensure_uart_frame(uart_parity_t parity, uart_stop_bits_t stop_bits)
{
    if (s_uart_parity_cur != (int)parity) {
        if (uart_set_parity(BRAKE_TEMP_UART_PORT, parity) == ESP_OK) {
            s_uart_parity_cur = (int)parity;
        }
    }
    if (s_uart_stop_bits_cur != (int)stop_bits) {
        if (uart_set_stop_bits(BRAKE_TEMP_UART_PORT, stop_bits) == ESP_OK) {
            s_uart_stop_bits_cur = (int)stop_bits;
        }
    }
}

static query_status_t query_with_cfg(const brake_modbus_cfg_t *cfg, int16_t *temp_x10)
{
    uint8_t req[8];
    uint8_t rx[BRAKE_TEMP_UART_BUF_SIZE];
    int n;
    int total = 0;
    int wait_ms = BRAKE_TEMP_READ_TIMEOUT_MS;
    static int s_last_baud = -1;
    static int s_last_parity = -1;
    static int s_last_stop_bits = -1;

    // If UART parameters changed, refresh before operating
    int baud_changed = (s_last_baud != cfg->baud);
    int frame_changed = (s_last_parity != (int)cfg->parity) || (s_last_stop_bits != (int)cfg->stop_bits);

    ensure_uart_baud(cfg->baud);
    ensure_uart_frame(cfg->parity, cfg->stop_bits);

    // Add a debounce delay after a parameter change
    if (baud_changed || frame_changed) {
        vTaskDelay(pdMS_TO_TICKS(10));  // let the UART hardware settle
    }

    s_last_baud = cfg->baud;
    s_last_parity = (int)cfg->parity;
    s_last_stop_bits = (int)cfg->stop_bits;

    build_req_frame(cfg, req);

    rs485_set_tx_mode(true);
    uart_flush_input(BRAKE_TEMP_UART_PORT);
    uart_write_bytes(BRAKE_TEMP_UART_PORT, (const char *)req, sizeof(req));
    uart_wait_tx_done(BRAKE_TEMP_UART_PORT, pdMS_TO_TICKS(BRAKE_TEMP_TX_DONE_MS));
    rs485_set_tx_mode(false);
    if (rs485_has_de_re()) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Accumulate received data over a period to avoid getting only half a packet due to serial fragmentation.
    while (wait_ms > 0 && total < (int)sizeof(rx)) {
        int chunk_timeout = (wait_ms > 30) ? 30 : wait_ms;
        n = uart_read_bytes(BRAKE_TEMP_UART_PORT,
                            rx + total,
                            sizeof(rx) - total,
                            pdMS_TO_TICKS(chunk_timeout));
        if (n > 0) {
            total += n;
        }
        wait_ms -= chunk_timeout;
    }

    if (total <= 0) {
        if ((s_fail_count % 6) == 0) {
            ESP_LOGW(TAG, "RS485 RX timeout: 0 bytes in %d ms", BRAKE_TEMP_READ_TIMEOUT_MS);
        }
        return QUERY_TIMEOUT;
    }

    if (parse_temp_from_rx(rx, total, cfg, temp_x10)) {
        return QUERY_OK;
    }

    static uint32_t s_parse_log_count = 0;
    if ((s_parse_log_count++ % 20) == 0) {
        int dump_len = (total > 16) ? 16 : total;
        ESP_LOGW(TAG, "Parse fail: got %d bytes (show %d): %02X %02X %02X %02X %02X %02X %02X %02X ...",
                 total,
                 dump_len,
                 dump_len > 0 ? rx[0] : 0,
                 dump_len > 1 ? rx[1] : 0,
                 dump_len > 2 ? rx[2] : 0,
                 dump_len > 3 ? rx[3] : 0,
                 dump_len > 4 ? rx[4] : 0,
                 dump_len > 5 ? rx[5] : 0,
                 dump_len > 6 ? rx[6] : 0,
                 dump_len > 7 ? rx[7] : 0);
    }

    return QUERY_PARSE_FAIL;
}

static void rs485_temp_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate = s_cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(BRAKE_TEMP_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRAKE_TEMP_UART_PORT,
                                 s_tx_pin,
                                 s_rx_pin,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(BRAKE_TEMP_UART_PORT,
                                        BRAKE_TEMP_UART_BUF_SIZE,
                                        BRAKE_TEMP_UART_BUF_SIZE,
                                        0,
                                        NULL,
                                        0));

    // DE/RE is disabled when CONFIG_OBD_RS485_DE_RE_GPIO < 0 (e.g. -1). Guard at
    // compile time so the constant shift below is never evaluated with an invalid count.
#if CONFIG_OBD_RS485_DE_RE_GPIO >= 0
    if (rs485_has_de_re()) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << (uint64_t)s_de_re_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }
#endif
    rs485_set_tx_mode(false);

    s_uart_baud_cur = s_cfg.baud;
    s_uart_parity_cur = (int)s_cfg.parity;
    s_uart_stop_bits_cur = (int)s_cfg.stop_bits;
    obd_data_set_brake_rs485_status(BRAKE_RS485_PROBE);

    ESP_LOGD(TAG, "RS485 brake temp started: UART%d, TX=%d RX=%d DE/RE=%d",
             BRAKE_TEMP_UART_PORT,
             s_tx_pin,
             s_rx_pin,
             (int)CONFIG_OBD_RS485_DE_RE_GPIO);

    while (1) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int16_t temp_x10 = -1000;
        query_status_t st = query_with_cfg(&s_cfg, &temp_x10);
        if (st == QUERY_OK) {
            s_fail_count = 0;
            obd_data_set_brake_temp_x10(temp_x10);
            obd_data_set_brake_rs485_status(BRAKE_RS485_OK);
            ESP_LOGD(TAG, "Brake temp: %d.%dC", (int)(temp_x10 / 10), (int)(temp_x10 < 0 ? -temp_x10 : temp_x10) % 10);
        } else {
            s_fail_count++;
            obd_data_set_brake_rs485_status(st == QUERY_TIMEOUT ? BRAKE_RS485_TIMEOUT : BRAKE_RS485_PARSE_FAIL);
            if ((s_fail_count % 3) == 0) {
                ESP_LOGW(TAG, "Query fail x%d (st=%d): baud=%d addr=0x%02X reg=0x%04X func=0x%02X",
                         s_fail_count, st, s_cfg.baud, s_cfg.addr, s_cfg.reg, s_cfg.func);
            }
            if (s_fail_count >= BRAKE_TEMP_FAIL_RESET) {
                obd_data_set_brake_temp_x10(-1000);
                s_fail_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BRAKE_TEMP_POLL_MS));
    }
}

void rs485_brake_temp_start(void)
{
    if (s_started) return;
    s_started = true;
    xTaskCreatePinnedToCore(rs485_temp_task, "brake_temp", 3072, NULL, 4, NULL, 1);
}

void rs485_brake_temp_pause(void)
{
    s_paused = true;
}

void rs485_brake_temp_resume(void)
{
    s_paused = false;
}
