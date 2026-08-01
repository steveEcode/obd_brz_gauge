#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static i2c_master_dev_handle_t get_device(uint8_t addr)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev_cache[i].addr == addr) {
            return s_dev_cache[i].handle;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev_handle));

    // 只有真正存进缓存才递增计数; 否则 s_dev_count 会超过数组大小, 上面的遍历会越界读,
    // 且缓存满之后同一地址每次都会重新 add_device 从而在下一次 ESP_ERROR_CHECK 处直接 abort。
    if (s_dev_count < MAX_I2C_DEVICES) {
        s_dev_cache[s_dev_count].addr = addr;
        s_dev_cache[s_dev_count].handle = dev_handle;
        s_dev_count++;
    } else {
        ESP_LOGW(I2C_TAG, "I2C device cache full (%d), addr 0x%02X not cached", MAX_I2C_DEVICES, addr);
    }
    return dev_handle;
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
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(I2C_TAG, "I2C initialized successfully");
}

i2c_master_bus_handle_t I2C_GetBusHandle(void)
{
    return s_i2c_bus;
}

esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    uint8_t buf[Length + 1];
    buf[0] = Reg_addr;
    memcpy(&buf[1], Reg_data, Length);
    i2c_master_dev_handle_t dev = get_device(Driver_addr);
    return i2c_master_transmit(dev, buf, Length + 1, (int)I2C_MASTER_TIMEOUT_MS);
}

esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev = get_device(Driver_addr);
    return i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length, (int)I2C_MASTER_TIMEOUT_MS);
}
