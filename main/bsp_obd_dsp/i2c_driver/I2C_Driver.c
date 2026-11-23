#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <stdbool.h>
#include "freertos/semphr.h"

static const char *I2C_TAG = "I2C";
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* Small cache so we don't re-add devices on every transaction */
#define MAX_I2C_DEVICES 8
typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
} i2c_dev_entry_t;
static i2c_dev_entry_t s_dev_cache[MAX_I2C_DEVICES];
static int s_dev_count = 0;
static StaticSemaphore_t s_cache_lock_storage;
static SemaphoreHandle_t s_cache_lock;

/* Caller holds s_cache_lock. */
static esp_err_t get_device(uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (!out || addr > 0x7f) return ESP_ERR_INVALID_ARG;
    if (!s_i2c_bus || !s_cache_lock) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev_cache[i].addr == addr) {
            *out = s_dev_cache[i].handle;
            return ESP_OK;
        }
    }
    if (s_dev_count >= MAX_I2C_DEVICES) {
        ESP_LOGW(I2C_TAG, "I2C device cache full (%d)", MAX_I2C_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev_handle);
    if (err != ESP_OK) return err;

    // Only increment the count when actually stored in the cache; otherwise s_dev_count would
    // exceed the array size, the loop above would read out of bounds, and once the cache is full
    // the same address would re-add_device every time and abort at the next ESP_ERROR_CHECK.
    s_dev_cache[s_dev_count++] = (i2c_dev_entry_t){ .addr = addr, .handle = dev_handle };
    *out = dev_handle;
    return ESP_OK;
}

/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_SCL_IO,
        .sda_io_num = I2C_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}
void I2C_Init(void)
{
    /********************* I2C *********************/
    s_cache_lock = xSemaphoreCreateMutexStatic(&s_cache_lock_storage);
    assert(s_cache_lock);
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGD(I2C_TAG, "I2C initialized successfully");
}

i2c_master_bus_handle_t I2C_GetBusHandle(void)
{
    return s_i2c_bus;
}

esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    if (Length && !Reg_data) return ESP_ERR_INVALID_ARG;
    if (!s_i2c_bus || !s_cache_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cache_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(Driver_addr, &dev);
    if (err == ESP_OK) {
        i2c_master_transmit_multi_buffer_info_t buffers[] = {
            { .write_buffer = &Reg_addr, .buffer_size = 1 },
            { .write_buffer = Reg_data, .buffer_size = Length },
        };
        err = i2c_master_multi_buffer_transmit(dev, buffers, 2,
                                               (int)I2C_MASTER_TIMEOUT_MS);
    }
    xSemaphoreGive(s_cache_lock);
    return err;
}

esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    if (!Reg_data || !Length) return ESP_ERR_INVALID_ARG;
    if (!s_i2c_bus || !s_cache_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cache_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(Driver_addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length,
                                           (int)I2C_MASTER_TIMEOUT_MS);
    }
    xSemaphoreGive(s_cache_lock);
    return err;
}
